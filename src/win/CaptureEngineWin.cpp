#include "../CaptureEngine.h"
#include "../FileLog.h"

#include <unknwn.h>
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wg    = winrt::Windows::Graphics;
namespace wgc   = winrt::Windows::Graphics::Capture;
namespace wgd   = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
using ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

namespace
{
const wgd::DirectXPixelFormat PIXEL_FORMAT = wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized;
const int FRAME_BUFFERS                    = 2;
const DWORD IDLE_WAKE_MS                   = 100;
const DWORD SHUTDOWN_WAIT_MS               = 3000;
const std::chrono::milliseconds MINIMIZED_CHECK_INTERVAL( 250 );
const std::chrono::seconds RESTORE_RETRY_INTERVAL( 2 );

// Lets threads we don't own (Resolume's) make WinRT calls without initializing COM on them.
void EnsureWinRTUsable()
{
	static std::once_flag once;
	std::call_once( once, [] {
		CO_MTA_USAGE_COOKIE cookie{};
		CoIncrementMTAUsage( &cookie );//Intentionally never released.
	} );
}

// A capture thread may outlive its plugin instance if Windows is slow to stop, so the dll must
// never be unloaded underneath it.
void PinThisModule()
{
	static std::once_flag once;
	std::call_once( once, [] {
		HMODULE module = nullptr;
		GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		                    reinterpret_cast< LPCWSTR >( &PinThisModule ), &module );
	} );
}

bool SessionHasProperty( const wchar_t* property )
{
	try
	{
		return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
			L"Windows.Graphics.Capture.GraphicsCaptureSession", property );
	}
	catch( ... )
	{
		return false;
	}
}

wg::SizeInt32 AtLeastOnePixel( wg::SizeInt32 size )
{
	//Minimized windows report 0x0 which the frame pool refuses.
	if( size.Width < 1 )
		size.Width = 1;
	if( size.Height < 1 )
		size.Height = 1;
	return size;
}

std::string Describe( const winrt::hresult_error& error )
{
	char code[ 16 ];
	snprintf( code, sizeof( code ), "0x%08X", static_cast< unsigned int >( error.code() ) );
	return std::string( code ) + " " + winrt::to_string( error.message() );
}

enum class Phase
{
	Idle,
	Starting,
	Running,
	Closed,
	Failed,
};
}// namespace

struct CaptureEngine::Impl
{
	// ---- Shared between the render thread and the capture thread (guarded by mutex) ----
	std::mutex mutex;
	CaptureTarget wantedTarget;
	bool wantedCursor         = true;
	bool wantedRestore        = true;//Bring the captured window back (behind the others) when it's minimized.
	uint64_t wantedGeneration = 0;//Bumped by every Start/Stop.
	bool quit                 = false;

	std::vector< unsigned char > readyPixels;//Newest finished frame.
	int readyWidth            = 0;
	int readyHeight           = 0;
	uint64_t readyGeneration  = 0;
	bool readyIsNew           = false;
	std::string error;

	std::atomic< Phase > phase{ Phase::Idle };
	std::atomic< bool > itemClosed{ false };
	HANDLE wakeEvent = CreateEventW( nullptr, FALSE, FALSE, nullptr );
	HANDLE doneEvent = CreateEventW( nullptr, TRUE, FALSE, nullptr );

	// ---- Render thread only ----
	std::vector< unsigned char > frontPixels;

	// ---- Capture thread only ----
	winrt::com_ptr< ID3D11Device > d3dDevice;
	winrt::com_ptr< ID3D11DeviceContext > d3dContext;
	wgd3d::IDirect3DDevice device{ nullptr };
	wgc::GraphicsCaptureItem item{ nullptr };
	wgc::Direct3D11CaptureFramePool framePool{ nullptr };
	wgc::GraphicsCaptureSession session{ nullptr };
	wgc::GraphicsCaptureItem::Closed_revoker closedRevoker;
	wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrivedRevoker;
	wg::SizeInt32 poolSize{};
	uint64_t activeGeneration = 0;
	bool activeCursor         = true;
	HWND activeWindow         = nullptr;
	bool wasMinimized         = false;
	std::chrono::steady_clock::time_point lastMinimizedCheck;
	std::chrono::steady_clock::time_point lastRestore;
	std::string activeLabel;
	std::chrono::steady_clock::time_point startTime;
	bool gotFirstFrame = false;

	winrt::com_ptr< ID3D11Texture2D > staging;
	UINT stagingWidth  = 0;
	UINT stagingHeight = 0;
	std::vector< unsigned char > workPixels;

	~Impl()
	{
		CloseHandle( wakeEvent );
		CloseHandle( doneEvent );
	}

	void Fail( const std::string& message )
	{
		LogToFile( "ERROR " + activeLabel + ": " + message );
		{
			std::lock_guard< std::mutex > lock( mutex );
			error = "No se pudo capturar " + activeLabel + ": " + message;
		}
		phase = Phase::Failed;
	}

	void Run()
	{
		try
		{
			winrt::init_apartment( winrt::apartment_type::multi_threaded );
		}
		catch( ... )
		{
		}

		for( ;; )
		{
			WaitForSingleObject( wakeEvent, IDLE_WAKE_MS );

			CaptureTarget target;
			bool cursor;
			bool restore;
			uint64_t generation;
			{
				std::lock_guard< std::mutex > lock( mutex );
				if( quit )
					break;
				target     = wantedTarget;
				cursor     = wantedCursor;
				restore    = wantedRestore;
				generation = wantedGeneration;
			}

			if( generation != activeGeneration )
			{
				activeGeneration = generation;
				Close();
				if( target.kind == CaptureTarget::Kind::None )
					phase = Phase::Idle;
				else
					Open( target, cursor );
			}
			else if( session && cursor != activeCursor )
			{
				ApplyCursor( cursor );
			}

			if( session && itemClosed )
			{
				LogToFile( "La fuente se cerro: " + activeLabel );
				Close();
				phase = Phase::Closed;
			}

			if( session )
			{
				HandleMinimized( restore );
				ReadNewestFrame();
			}
		}

		Close();
		SetEvent( doneEvent );
	}

	bool EnsureDevice()
	{
		if( device )
			return true;

		HRESULT result = D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                                    nullptr, 0, D3D11_SDK_VERSION, d3dDevice.put(), nullptr, d3dContext.put() );
		if( FAILED( result ) )
		{
			Fail( "no se pudo crear el dispositivo Direct3D 11" );
			return false;
		}

		auto dxgiDevice = d3dDevice.as< IDXGIDevice >();
		winrt::com_ptr< ::IInspectable > inspectable;
		winrt::check_hresult( CreateDirect3D11DeviceFromDXGIDevice( dxgiDevice.get(), inspectable.put() ) );
		device = inspectable.as< wgd3d::IDirect3DDevice >();
		return true;
	}

	// Windows stops drawing minimized windows, so there is nothing to capture. Showing the window
	// again at the very bottom of the stack keeps it live in Arena without covering anything.
	void HandleMinimized( bool restore )
	{
		if( activeWindow == nullptr )
			return;
		auto now = std::chrono::steady_clock::now();
		if( now - lastMinimizedCheck < MINIMIZED_CHECK_INTERVAL )
			return;
		lastMinimizedCheck = now;

		bool minimized = IsIconic( activeWindow ) != FALSE;
		if( minimized && !wasMinimized )
			LogToFile( restore ? "Ventana minimizada, se restaura detras de las demas: " + activeLabel
			                   : "Ventana minimizada, se mantiene la ultima imagen: " + activeLabel );
		wasMinimized = minimized;
		if( !minimized || !restore || now - lastRestore < RESTORE_RETRY_INTERVAL )
			return;
		lastRestore = now;

		//Async versions: never wait on the other program, it might be busy or frozen.
		ShowWindowAsync( activeWindow, SW_SHOWNOACTIVATE );
		SetWindowPos( activeWindow, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS );
	}

	void Open( const CaptureTarget& target, bool cursor )
	{
		activeWindow  = target.kind == CaptureTarget::Kind::Window ? reinterpret_cast< HWND >( target.id ) : nullptr;
		wasMinimized  = false;
		activeLabel   = target.label;
		gotFirstFrame = false;
		startTime     = std::chrono::steady_clock::now();
		phase         = Phase::Starting;
		LogToFile( "Iniciando captura: " + activeLabel );

		if( !IsTargetAlive( target ) )
		{
			Fail( "la ventana o pantalla ya no existe (pulsa 'Actualizar lista')" );
			return;
		}

		try
		{
			if( !EnsureDevice() )
				return;

			auto interop = winrt::get_activation_factory< wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop >();
			wgc::GraphicsCaptureItem newItem{ nullptr };
			if( target.kind == CaptureTarget::Kind::Window )
				winrt::check_hresult( interop->CreateForWindow( reinterpret_cast< HWND >( target.id ), winrt::guid_of< wgc::IGraphicsCaptureItem >(), winrt::put_abi( newItem ) ) );
			else
				winrt::check_hresult( interop->CreateForMonitor( reinterpret_cast< HMONITOR >( target.id ), winrt::guid_of< wgc::IGraphicsCaptureItem >(), winrt::put_abi( newItem ) ) );

			item       = newItem;
			poolSize   = AtLeastOnePixel( item.Size() );
			framePool  = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded( device, PIXEL_FORMAT, FRAME_BUFFERS, poolSize );
			session    = framePool.CreateCaptureSession( item );
			itemClosed = false;

			HANDLE wake         = wakeEvent;
			std::atomic< bool >* closedFlag = &itemClosed;
			closedRevoker       = item.Closed( winrt::auto_revoke, [ closedFlag, wake ]( auto&&, auto&& ) {
                *closedFlag = true;
                SetEvent( wake );
            } );
			frameArrivedRevoker = framePool.FrameArrived( winrt::auto_revoke, [ wake ]( auto&&, auto&& ) { SetEvent( wake ); } );

			activeCursor = !cursor;//Force ApplyCursor to set it.
			ApplyCursor( cursor );

#if defined( NTDDI_WIN10_FE )
			//Windows 11 lets us hide the yellow "being captured" border, which would otherwise
			//end up on the projector. Older systems simply keep the border.
			static const bool canHideBorder = SessionHasProperty( L"IsBorderRequired" );
			if( canHideBorder )
			{
				try
				{
					session.IsBorderRequired( false );
				}
				catch( ... )
				{
				}
			}
#endif

			session.StartCapture();
			LogToFile( "Captura iniciada (" + std::to_string( poolSize.Width ) + "x" + std::to_string( poolSize.Height ) + "), esperando la primera imagen" );
		}
		catch( const winrt::hresult_error& e )
		{
			Close();
			Fail( Describe( e ) );
		}
		catch( const std::exception& e )
		{
			Close();
			Fail( e.what() );
		}
	}

	void Close()
	{
		frameArrivedRevoker.revoke();
		closedRevoker.revoke();
		try
		{
			if( session )
				session.Close();
			if( framePool )
				framePool.Close();
		}
		catch( ... )
		{
		}
		session      = nullptr;
		framePool    = nullptr;
		item         = nullptr;
		activeWindow = nullptr;
	}

	void ApplyCursor( bool cursor )
	{
		if( cursor == activeCursor )
			return;
		activeCursor = cursor;
#if defined( NTDDI_WIN10_VB )
		static const bool canToggleCursor = SessionHasProperty( L"IsCursorCaptureEnabled" );
		if( !session || !canToggleCursor )
			return;
		try
		{
			session.IsCursorCaptureEnabled( cursor );
		}
		catch( ... )
		{
		}
#endif
	}

	void EnsureStaging( UINT width, UINT height )
	{
		if( staging && stagingWidth == width && stagingHeight == height )
			return;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width            = width;
		desc.Height           = height;
		desc.MipLevels        = 1;
		desc.ArraySize        = 1;
		desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage            = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

		staging = nullptr;
		winrt::check_hresult( d3dDevice->CreateTexture2D( &desc, nullptr, staging.put() ) );
		stagingWidth  = width;
		stagingHeight = height;
	}

	void ReadNewestFrame()
	{
		try
		{
			//Windows only sends frames when something changes; keep just the newest one.
			wgc::Direct3D11CaptureFrame frame{ nullptr };
			while( auto next = framePool.TryGetNextFrame() )
			{
				if( frame )
					frame.Close();
				frame = next;
			}
			if( !frame )
				return;

			wg::SizeInt32 contentSize = frame.ContentSize();
			winrt::com_ptr< ID3D11Texture2D > texture;
			auto access = frame.Surface().as< IDirect3DDxgiInterfaceAccess >();
			winrt::check_hresult( access->GetInterface( __uuidof( ID3D11Texture2D ), texture.put_void() ) );

			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc( &desc );
			UINT width  = contentSize.Width > 0 ? static_cast< UINT >( contentSize.Width ) : 0;
			UINT height = contentSize.Height > 0 ? static_cast< UINT >( contentSize.Height ) : 0;
			//A minimized window reports a tiny size, and right after it's restored the buffers are still
			//too small. Skip those frames so Arena keeps showing the last good image instead.
			bool usable = width > 1 && height > 1 && width <= desc.Width && height <= desc.Height;

			if( usable )
			{
				EnsureStaging( width, height );
				D3D11_BOX box{ 0, 0, 0, width, height, 1 };
				d3dContext->CopySubresourceRegion( staging.get(), 0, 0, 0, 0, texture.get(), 0, &box );

				//This waits for the GPU copy, which is fine here: we're not on Resolume's thread.
				D3D11_MAPPED_SUBRESOURCE mapped{};
				winrt::check_hresult( d3dContext->Map( staging.get(), 0, D3D11_MAP_READ, 0, &mapped ) );
				size_t rowBytes = static_cast< size_t >( width ) * 4;
				workPixels.resize( rowBytes * height );
				for( UINT row = 0; row < height; ++row )
					memcpy( workPixels.data() + row * rowBytes, static_cast< const unsigned char* >( mapped.pData ) + row * mapped.RowPitch, rowBytes );
				d3dContext->Unmap( staging.get(), 0 );

				{
					std::lock_guard< std::mutex > lock( mutex );
					readyPixels.swap( workPixels );
					readyWidth      = static_cast< int >( width );
					readyHeight     = static_cast< int >( height );
					readyGeneration = activeGeneration;
					readyIsNew      = true;
				}
				phase = Phase::Running;

				if( !gotFirstFrame )
				{
					gotFirstFrame = true;
					auto elapsed  = std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now() - startTime ).count();
					LogToFile( "Primera imagen recibida: " + std::to_string( width ) + "x" + std::to_string( height ) + " en " + std::to_string( elapsed ) + " ms" );
				}
			}
			frame.Close();

			//The window was resized: let Windows allocate buffers of the new size.
			wg::SizeInt32 newSize = AtLeastOnePixel( contentSize );
			if( newSize.Width != poolSize.Width || newSize.Height != poolSize.Height )
			{
				poolSize = newSize;
				framePool.Recreate( device, PIXEL_FORMAT, FRAME_BUFFERS, newSize );
			}
		}
		catch( const winrt::hresult_error& e )
		{
			Close();
			Fail( Describe( e ) );
		}
	}
};

CaptureEngine::CaptureEngine() :
	impl( std::make_shared< Impl >() )
{
	PinThisModule();
	std::shared_ptr< Impl > state = impl;
	worker                        = std::thread( [ state ] { state->Run(); } );
}

CaptureEngine::~CaptureEngine()
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->quit = true;
	}
	SetEvent( impl->wakeEvent );

	//If Windows is stuck we'd rather leave the thread behind than freeze Resolume.
	if( WaitForSingleObject( impl->doneEvent, SHUTDOWN_WAIT_MS ) == WAIT_OBJECT_0 )
	{
		worker.join();
	}
	else
	{
		LogToFile( "El hilo de captura no se detuvo a tiempo; se deja terminar solo" );
		worker.detach();
	}
}

bool CaptureEngine::IsSupported()
{
	EnsureWinRTUsable();
	try
	{
		return wgc::GraphicsCaptureSession::IsSupported();
	}
	catch( ... )
	{
		return false;
	}
}

bool CaptureEngine::HasPermission()
{
	return true;
}

void CaptureEngine::Start( const CaptureTarget& target, bool showCursor )
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->wantedTarget = target;
		impl->wantedCursor = showCursor;
		++impl->wantedGeneration;
		impl->error.clear();
	}
	impl->phase = target.kind == CaptureTarget::Kind::None ? Phase::Idle : Phase::Starting;
	SetEvent( impl->wakeEvent );
}

void CaptureEngine::Stop()
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->wantedTarget = CaptureTarget();
		++impl->wantedGeneration;
	}
	impl->phase = Phase::Idle;
	SetEvent( impl->wakeEvent );
}

void CaptureEngine::SetRestoreMinimized( bool restore )
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->wantedRestore = restore;
	}
	SetEvent( impl->wakeEvent );
}

void CaptureEngine::SetCursorVisible( bool visible )
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->wantedCursor = visible;
	}
	SetEvent( impl->wakeEvent );
}

bool CaptureEngine::IsActive() const
{
	Phase phase = impl->phase;
	return phase == Phase::Starting || phase == Phase::Running;
}

bool CaptureEngine::WasClosed() const
{
	return impl->phase == Phase::Closed;
}

bool CaptureEngine::TakeFrame( const FrameCallback& onFrame )
{
	int width, height;
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		//Ignore frames that still belong to the previous source.
		if( !impl->readyIsNew || impl->readyGeneration != impl->wantedGeneration )
			return false;
		impl->frontPixels.swap( impl->readyPixels );
		width            = impl->readyWidth;
		height           = impl->readyHeight;
		impl->readyIsNew = false;
	}
	onFrame( impl->frontPixels.data(), width, height );
	return true;
}

std::string CaptureEngine::TakeError()
{
	std::lock_guard< std::mutex > lock( impl->mutex );
	std::string error;
	error.swap( impl->error );
	return error;
}

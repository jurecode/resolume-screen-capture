#include "WgcCapture.h"

#include <unknwn.h>
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdio>
#include <mutex>

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

// Resolume owns its threads, so we can't call CoInitializeEx on them without risking
// breaking the host's own COM usage. Keeping the process-wide MTA alive instead lets any
// thread without an apartment make WinRT calls.
void EnsureWinRTUsable()
{
	static std::once_flag once;
	std::call_once( once, [] {
		CO_MTA_USAGE_COOKIE cookie{};
		CoIncrementMTAUsage( &cookie );//Intentionally never released.
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
}// namespace

struct WgcCapture::Impl
{
	winrt::com_ptr< ID3D11Device > d3dDevice;
	winrt::com_ptr< ID3D11DeviceContext > d3dContext;
	wgd3d::IDirect3DDevice device{ nullptr };

	wgc::GraphicsCaptureItem item{ nullptr };
	wgc::Direct3D11CaptureFramePool framePool{ nullptr };
	wgc::GraphicsCaptureSession session{ nullptr };
	wgc::GraphicsCaptureItem::Closed_revoker closedRevoker;
	wg::SizeInt32 poolSize{};
	std::atomic< bool > closed{ false };

	//CPU readable copy of the last frame. Copied into on one Poll, read on the next.
	winrt::com_ptr< ID3D11Texture2D > staging;
	UINT stagingWidth  = 0;
	UINT stagingHeight = 0;
	bool copyPending   = false;
	UINT pendingWidth  = 0;
	UINT pendingHeight = 0;

	std::string lastError;

	bool EnsureDevice()
	{
		if( device )
			return true;

		HRESULT result = D3D11CreateDevice( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                                    nullptr, 0, D3D11_SDK_VERSION, d3dDevice.put(), nullptr, d3dContext.put() );
		if( FAILED( result ) )
		{
			lastError = "No se pudo crear el dispositivo Direct3D 11";
			return false;
		}

		auto dxgiDevice = d3dDevice.as< IDXGIDevice >();
		winrt::com_ptr< ::IInspectable > inspectable;
		winrt::check_hresult( CreateDirect3D11DeviceFromDXGIDevice( dxgiDevice.get(), inspectable.put() ) );
		device = inspectable.as< wgd3d::IDirect3DDevice >();
		return true;
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
};

WgcCapture::WgcCapture() :
	impl( std::make_unique< Impl >() )
{
}

WgcCapture::~WgcCapture()
{
	Stop();
}

bool WgcCapture::IsSupported()
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

bool WgcCapture::Start( const CaptureTarget& target, bool showCursor )
{
	Stop();
	EnsureWinRTUsable();

	if( target.kind == CaptureTarget::Kind::None )
		return false;

	try
	{
		if( !impl->EnsureDevice() )
			return false;

		auto interop = winrt::get_activation_factory< wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop >();
		wgc::GraphicsCaptureItem item{ nullptr };
		if( target.kind == CaptureTarget::Kind::Window )
			winrt::check_hresult( interop->CreateForWindow( target.window, winrt::guid_of< wgc::IGraphicsCaptureItem >(), winrt::put_abi( item ) ) );
		else
			winrt::check_hresult( interop->CreateForMonitor( target.monitor, winrt::guid_of< wgc::IGraphicsCaptureItem >(), winrt::put_abi( item ) ) );

		impl->item      = item;
		impl->poolSize  = AtLeastOnePixel( item.Size() );
		impl->framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded( impl->device, PIXEL_FORMAT, FRAME_BUFFERS, impl->poolSize );
		impl->session   = impl->framePool.CreateCaptureSession( item );

		impl->closed        = false;
		Impl* state         = impl.get();
		impl->closedRevoker = item.Closed( winrt::auto_revoke, [ state ]( auto&&, auto&& ) { state->closed = true; } );

		SetCursorVisible( showCursor );

#if defined( NTDDI_WIN10_FE )
		//Windows 11 lets us hide the yellow "being captured" border, which would otherwise
		//end up on the projector. Older systems simply keep the border.
		static const bool canHideBorder = SessionHasProperty( L"IsBorderRequired" );
		if( canHideBorder )
		{
			try
			{
				impl->session.IsBorderRequired( false );
			}
			catch( ... )
			{
			}
		}
#endif

		impl->session.StartCapture();
		impl->lastError.clear();
		return true;
	}
	catch( const winrt::hresult_error& error )
	{
		impl->lastError = Describe( error );
	}
	catch( const std::exception& error )
	{
		impl->lastError = error.what();
	}

	Stop();
	return false;
}

void WgcCapture::Stop()
{
	impl->closedRevoker.revoke();
	try
	{
		if( impl->session )
			impl->session.Close();
		if( impl->framePool )
			impl->framePool.Close();
	}
	catch( ... )
	{
	}
	impl->session     = nullptr;
	impl->framePool   = nullptr;
	impl->item        = nullptr;
	impl->copyPending = false;
}

bool WgcCapture::IsActive() const
{
	return impl->session != nullptr;
}

bool WgcCapture::WasClosed() const
{
	return impl->closed;
}

void WgcCapture::SetCursorVisible( bool visible )
{
#if defined( NTDDI_WIN10_VB )
	static const bool canToggleCursor = SessionHasProperty( L"IsCursorCaptureEnabled" );
	if( !impl->session || !canToggleCursor )
		return;
	try
	{
		impl->session.IsCursorCaptureEnabled( visible );
	}
	catch( ... )
	{
	}
#else
	(void)visible;
#endif
}

bool WgcCapture::Poll( const FrameCallback& onFrame )
{
	if( !impl->framePool )
		return false;

	bool delivered = false;
	try
	{
		//1. Hand over the frame we asked the GPU to copy on the previous call. That copy has
		//   had a whole render tick to finish, so Map() normally returns without waiting.
		if( impl->copyPending )
		{
			impl->copyPending = false;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if( SUCCEEDED( impl->d3dContext->Map( impl->staging.get(), 0, D3D11_MAP_READ, 0, &mapped ) ) )
			{
				onFrame( static_cast< const unsigned char* >( mapped.pData ),
				         static_cast< int >( impl->pendingWidth ),
				         static_cast< int >( impl->pendingHeight ),
				         static_cast< int >( mapped.RowPitch ) );
				impl->d3dContext->Unmap( impl->staging.get(), 0 );
				delivered = true;
			}
		}

		//2. Take the newest frame Windows produced and drop any older ones. Windows only sends
		//   frames when something changes, so a still slide may produce none for a long time.
		wgc::Direct3D11CaptureFrame frame{ nullptr };
		while( auto next = impl->framePool.TryGetNextFrame() )
		{
			if( frame )
				frame.Close();
			frame = next;
		}
		if( !frame )
			return delivered;

		wg::SizeInt32 contentSize = frame.ContentSize();

		winrt::com_ptr< ID3D11Texture2D > texture;
		auto access = frame.Surface().as< IDirect3DDxgiInterfaceAccess >();
		winrt::check_hresult( access->GetInterface( __uuidof( ID3D11Texture2D ), texture.put_void() ) );

		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc( &desc );
		UINT width  = contentSize.Width > 0 ? static_cast< UINT >( contentSize.Width ) : 0;
		UINT height = contentSize.Height > 0 ? static_cast< UINT >( contentSize.Height ) : 0;
		width       = width < desc.Width ? width : desc.Width;
		height      = height < desc.Height ? height : desc.Height;

		if( width > 0 && height > 0 )
		{
			impl->EnsureStaging( width, height );
			D3D11_BOX box{ 0, 0, 0, width, height, 1 };
			impl->d3dContext->CopySubresourceRegion( impl->staging.get(), 0, 0, 0, 0, texture.get(), 0, &box );
			impl->d3dContext->Flush();
			impl->copyPending   = true;
			impl->pendingWidth  = width;
			impl->pendingHeight = height;
		}
		frame.Close();

		//The window was resized: let Windows allocate buffers of the new size.
		wg::SizeInt32 newSize = AtLeastOnePixel( contentSize );
		if( newSize.Width != impl->poolSize.Width || newSize.Height != impl->poolSize.Height )
		{
			impl->poolSize = newSize;
			impl->framePool.Recreate( impl->device, PIXEL_FORMAT, FRAME_BUFFERS, newSize );
		}
	}
	catch( const winrt::hresult_error& error )
	{
		impl->lastError = Describe( error );
		Stop();
	}

	return delivered;
}

const std::string& WgcCapture::GetLastError() const
{
	return impl->lastError;
}

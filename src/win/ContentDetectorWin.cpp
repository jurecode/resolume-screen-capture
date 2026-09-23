#include "ContentDetectorWin.h"
#include "../FileLog.h"
#include "../Platform.h"

#include <dwmapi.h>
#include <uiautomation.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace
{
const DWORD SCAN_INTERVAL_MS   = 500;
const DWORD SHUTDOWN_WAIT_MS   = 3000;
const DWORD UIA_TIMEOUT_MS     = 1000;
const double MIN_AREA_FRACTION = 0.10;//Ignore thumbnails and icons: the content is big.
const size_t MAX_DUMP_LINES    = 30;

// Classes the Windows media apps use for their video surface. Photos (and most apps) expose the
// picture as a plain Image control, which the search also accepts.
const wchar_t* const MEDIA_CLASS_NAMES[] = {
	L"MediaPlayerElement",
	L"MediaPlayerPresenter",
	L"MediaElement",
};

struct Candidate
{
	RECT rect{};
	double area = 0;
	std::string description;
};

std::string Narrow( BSTR text )
{
	return text != nullptr ? ToUtf8( std::wstring( text, SysStringLen( text ) ) ) : std::string();
}

std::string Describe( IUIAutomationElement* element, const RECT& rect )
{
	CONTROLTYPEID type = 0;
	BSTR className = nullptr, automationId = nullptr, name = nullptr;
	element->get_CachedControlType( &type );
	element->get_CachedClassName( &className );
	element->get_CachedAutomationId( &automationId );
	element->get_CachedName( &name );

	std::string title = Narrow( name );
	if( title.size() > 40 )
		title = title.substr( 0, 40 ) + "...";
	char text[ 512 ];
	snprintf( text, sizeof( text ), "tipo=%d clase='%s' id='%s' nombre='%s' rect=(%ld,%ld)-(%ld,%ld)", type,
	          Narrow( className ).c_str(), Narrow( automationId ).c_str(), title.c_str(), rect.left, rect.top, rect.right, rect.bottom );
	SysFreeString( className );
	SysFreeString( automationId );
	SysFreeString( name );
	return text;
}

// Visible part of the element inside the window.
bool VisibleRect( IUIAutomationElement* element, const RECT& window, RECT& visible, double& area )
{
	RECT bounds{};
	if( FAILED( element->get_CachedBoundingRectangle( &bounds ) ) )
		return false;
	if( !IntersectRect( &visible, &bounds, &window ) )
		return false;
	area = static_cast< double >( visible.right - visible.left ) * static_cast< double >( visible.bottom - visible.top );
	return true;
}
}// namespace

struct ContentDetector::Impl
{
	mutable std::mutex mutex;
	HWND window  = nullptr;
	bool enabled = false;
	bool quit    = false;
	bool found   = false;
	float rect[ 4 ] = { 0, 0, 1, 1 };

	HANDLE wakeEvent = CreateEventW( nullptr, FALSE, FALSE, nullptr );
	HANDLE doneEvent = CreateEventW( nullptr, TRUE, FALSE, nullptr );

	// ---- Detector thread only ----
	winrt::com_ptr< IUIAutomation > automation;
	winrt::com_ptr< IUIAutomationCondition > contentCondition;
	winrt::com_ptr< IUIAutomationCondition > everything;
	winrt::com_ptr< IUIAutomationCacheRequest > cache;
	HWND dumpedWindow = nullptr;//Tree already written to the log for this window.
	std::string lastLogged;

	~Impl()
	{
		CloseHandle( wakeEvent );
		CloseHandle( doneEvent );
	}

	bool Setup()
	{
		if( FAILED( CoCreateInstance( __uuidof( CUIAutomation ), nullptr, CLSCTX_INPROC_SERVER, __uuidof( IUIAutomation ), automation.put_void() ) ) )
		{
			LogToFile( "Solo contenido: UI Automation no esta disponible" );
			return false;
		}
		//Never wait long on a busy or frozen program.
		if( auto automation2 = automation.try_as< IUIAutomation2 >() )
		{
			automation2->put_ConnectionTimeout( UIA_TIMEOUT_MS );
			automation2->put_TransactionTimeout( UIA_TIMEOUT_MS );
		}

		std::vector< IUIAutomationCondition* > conditions;
		VARIANT value;
		VariantInit( &value );
		value.vt   = VT_I4;
		value.lVal = UIA_ImageControlTypeId;
		IUIAutomationCondition* condition = nullptr;
		if( SUCCEEDED( automation->CreatePropertyCondition( UIA_ControlTypePropertyId, value, &condition ) ) )
			conditions.push_back( condition );
		for( const wchar_t* className : MEDIA_CLASS_NAMES )
		{
			VariantInit( &value );
			value.vt      = VT_BSTR;
			value.bstrVal = SysAllocString( className );
			condition     = nullptr;
			if( SUCCEEDED( automation->CreatePropertyCondition( UIA_ClassNamePropertyId, value, &condition ) ) )
				conditions.push_back( condition );
			VariantClear( &value );
		}
		automation->CreateOrConditionFromNativeArray( conditions.data(), static_cast< int >( conditions.size() ), contentCondition.put() );
		for( IUIAutomationCondition* item : conditions )
			item->Release();
		automation->CreateTrueCondition( everything.put() );

		automation->CreateCacheRequest( cache.put() );
		for( PROPERTYID property : { UIA_BoundingRectanglePropertyId, UIA_ControlTypePropertyId, UIA_ClassNamePropertyId,
		                             UIA_AutomationIdPropertyId, UIA_NamePropertyId } )
			cache->AddProperty( property );
		return contentCondition != nullptr;
	}

	void Run()
	{
		CoInitializeEx( nullptr, COINIT_MULTITHREADED );
		//Physical pixels, like the captured frames, whatever Resolume's own DPI mode is.
		SetThreadDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 );
		bool ready = Setup();

		for( ;; )
		{
			WaitForSingleObject( wakeEvent, SCAN_INTERVAL_MS );
			HWND target;
			bool active;
			{
				std::lock_guard< std::mutex > lock( mutex );
				if( quit )
					break;
				target = window;
				active = enabled && window != nullptr && ready;
			}

			float area[ 4 ] = { 0, 0, 1, 1 };
			bool hit        = active && Detect( target, area );

			std::lock_guard< std::mutex > lock( mutex );
			if( target != window )
				continue;//The source changed while we were looking.
			found = hit;
			std::copy( area, area + 4, rect );
		}

		automation   = nullptr;
		contentCondition = nullptr;
		everything   = nullptr;
		cache        = nullptr;
		CoUninitialize();
		SetEvent( doneEvent );
	}

	bool Detect( HWND target, float result[ 4 ] )
	{
		//The captured image covers the window's visible frame (no invisible resize borders).
		RECT frame{};
		if( FAILED( DwmGetWindowAttribute( target, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof( frame ) ) ) )
			GetWindowRect( target, &frame );
		double width  = frame.right - frame.left;
		double height = frame.bottom - frame.top;
		if( width < 2 || height < 2 )
			return false;

		winrt::com_ptr< IUIAutomationElement > root;
		if( FAILED( automation->ElementFromHandle( target, root.put() ) ) || !root )
			return false;

		winrt::com_ptr< IUIAutomationElementArray > matches;
		root->FindAllBuildCache( TreeScope_Descendants, contentCondition.get(), cache.get(), matches.put() );
		Candidate best;
		int count = 0;
		if( matches )
			matches->get_Length( &count );
		for( int index = 0; index < count; ++index )
		{
			winrt::com_ptr< IUIAutomationElement > element;
			RECT visible{};
			double area = 0;
			if( SUCCEEDED( matches->GetElement( index, element.put() ) ) && VisibleRect( element.get(), frame, visible, area ) && area > best.area )
			{
				best.rect        = visible;
				best.area        = area;
				best.description = Describe( element.get(), visible );
			}
		}

		if( best.area < MIN_AREA_FRACTION * width * height )
		{
			Report( "Solo contenido: UI Automation no describe la foto/video de esta ventana; se busca por el fondo de la imagen" );
			DumpTree( target, root.get(), frame );
			return false;
		}

		result[ 0 ] = static_cast< float >( ( best.rect.left - frame.left ) / width );
		result[ 1 ] = static_cast< float >( ( best.rect.top - frame.top ) / height );
		result[ 2 ] = static_cast< float >( ( best.rect.right - frame.left ) / width );
		result[ 3 ] = static_cast< float >( ( best.rect.bottom - frame.top ) / height );
		Report( "Solo contenido: " + best.description );
		return true;
	}

	void Report( const std::string& message )
	{
		if( message == lastLogged )
			return;
		lastLogged = message;
		LogToFile( message );
	}

	// Writes the window's large UI elements to the log once, so an app we don't recognise yet can
	// be supported quickly from a user's log file.
	void DumpTree( HWND target, IUIAutomationElement* root, const RECT& frame )
	{
		if( target == dumpedWindow )
			return;
		dumpedWindow = target;

		winrt::com_ptr< IUIAutomationElementArray > all;
		root->FindAllBuildCache( TreeScope_Descendants, everything.get(), cache.get(), all.put() );
		int count = 0;
		if( all )
			all->get_Length( &count );

		double windowArea = static_cast< double >( frame.right - frame.left ) * ( frame.bottom - frame.top );
		size_t lines      = 0;
		LogToFile( "Solo contenido: elementos grandes de la ventana (" + std::to_string( count ) + " en total):" );
		for( int index = 0; index < count && lines < MAX_DUMP_LINES; ++index )
		{
			winrt::com_ptr< IUIAutomationElement > element;
			RECT visible{};
			double area = 0;
			if( SUCCEEDED( all->GetElement( index, element.put() ) ) && VisibleRect( element.get(), frame, visible, area ) &&
			    area >= MIN_AREA_FRACTION * windowArea )
			{
				LogToFile( "    " + Describe( element.get(), visible ) );
				++lines;
			}
		}
	}
};

ContentDetector::ContentDetector() :
	impl( std::make_shared< Impl >() )
{
	std::shared_ptr< Impl > state = impl;
	worker                        = std::thread( [ state ] { state->Run(); } );
}

ContentDetector::~ContentDetector()
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		impl->quit = true;
	}
	SetEvent( impl->wakeEvent );
	//If another program keeps UI Automation busy we'd rather leave the thread behind than freeze Resolume.
	if( WaitForSingleObject( impl->doneEvent, SHUTDOWN_WAIT_MS ) == WAIT_OBJECT_0 )
		worker.join();
	else
		worker.detach();
}

void ContentDetector::SetWindow( HWND window )
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		if( impl->window == window )
			return;
		impl->window = window;
		impl->found  = false;
	}
	SetEvent( impl->wakeEvent );
}

void ContentDetector::SetEnabled( bool enabled )
{
	{
		std::lock_guard< std::mutex > lock( impl->mutex );
		if( impl->enabled == enabled )
			return;
		impl->enabled = enabled;
		impl->found   = false;
	}
	SetEvent( impl->wakeEvent );
}

bool ContentDetector::GetRect( float rect[ 4 ] ) const
{
	std::lock_guard< std::mutex > lock( impl->mutex );
	if( !impl->enabled || !impl->found )
		return false;
	std::copy( impl->rect, impl->rect + 4, rect );
	return true;
}

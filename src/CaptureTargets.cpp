#include "CaptureTargets.h"
#include <dwmapi.h>

namespace
{
const size_t MAX_TITLE_CHARS = 48;

std::wstring ToLower( std::wstring text )
{
	if( !text.empty() )
		CharLowerBuffW( &text[ 0 ], static_cast< DWORD >( text.size() ) );
	return text;
}

std::wstring Truncate( const std::wstring& text, size_t maxChars )
{
	if( text.size() <= maxChars )
		return text;
	size_t cut = maxChars - 1;
	if( cut > 0 && IS_HIGH_SURROGATE( text[ cut - 1 ] ) )
		--cut;//Don't split a surrogate pair (emoji etc.)
	return text.substr( 0, cut ) + L"\x2026";
}

std::wstring GetWindowTitle( HWND hwnd )
{
	int length = GetWindowTextLengthW( hwnd );
	if( length <= 0 )
		return {};
	std::wstring title( static_cast< size_t >( length ) + 1, L'\0' );
	length = GetWindowTextW( hwnd, &title[ 0 ], length + 1 );
	title.resize( static_cast< size_t >( length > 0 ? length : 0 ) );
	return title;
}

std::wstring GetProcessName( HWND hwnd )
{
	DWORD processId = 0;
	GetWindowThreadProcessId( hwnd, &processId );
	HANDLE process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId );
	if( process == nullptr )
		return {};

	std::wstring name;
	wchar_t path[ MAX_PATH ];
	DWORD length = MAX_PATH;
	if( QueryFullProcessImageNameW( process, 0, path, &length ) )
	{
		name        = std::wstring( path, length );
		size_t slash = name.find_last_of( L"\\/" );
		if( slash != std::wstring::npos )
			name = name.substr( slash + 1 );
	}
	CloseHandle( process );
	return name;
}

// Same filtering Microsoft uses in its Win32CaptureSample, so the list matches what
// the user sees in the taskbar / Alt+Tab.
bool IsCapturableWindow( HWND hwnd )
{
	if( hwnd == GetShellWindow() || hwnd == GetDesktopWindow() )
		return false;
	if( !IsWindowVisible( hwnd ) )
		return false;
	if( GetAncestor( hwnd, GA_ROOT ) != hwnd )
		return false;
	if( GetWindowLongW( hwnd, GWL_STYLE ) & WS_DISABLED )
		return false;
	if( GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_TOOLWINDOW )
		return false;
	if( GetWindowTextLengthW( hwnd ) == 0 )
		return false;

	DWORD cloaked = 0;
	if( SUCCEEDED( DwmGetWindowAttribute( hwnd, DWMWA_CLOAKED, &cloaked, sizeof( cloaked ) ) ) && cloaked )
		return false;//Hidden UWP apps / windows on other virtual desktops.

	return true;
}

BOOL CALLBACK AddMonitor( HMONITOR monitor, HDC, LPRECT, LPARAM param )
{
	auto& targets = *reinterpret_cast< std::vector< CaptureTarget >* >( param );

	MONITORINFOEXW info{};
	info.cbSize = sizeof( info );
	if( !GetMonitorInfoW( monitor, &info ) )
		return TRUE;

	int number = 1;
	for( const CaptureTarget& target : targets )
		if( target.kind == CaptureTarget::Kind::Monitor )
			++number;

	std::wstring label = L"Pantalla " + std::to_wstring( number ) + L" (" +
	                     std::to_wstring( info.rcMonitor.right - info.rcMonitor.left ) + L"x" +
	                     std::to_wstring( info.rcMonitor.bottom - info.rcMonitor.top ) + L")";
	if( info.dwFlags & MONITORINFOF_PRIMARY )
		label += L" principal";

	CaptureTarget target;
	target.kind      = CaptureTarget::Kind::Monitor;
	target.monitor   = monitor;
	target.label     = ToUtf8( label );
	target.searchKey = ToLower( label + L" " + info.szDevice );
	targets.push_back( target );
	return TRUE;
}

BOOL CALLBACK AddWindow( HWND hwnd, LPARAM param )
{
	if( !IsCapturableWindow( hwnd ) )
		return TRUE;

	auto& targets             = *reinterpret_cast< std::vector< CaptureTarget >* >( param );
	std::wstring title        = GetWindowTitle( hwnd );
	std::wstring processName  = GetProcessName( hwnd );
	std::wstring label        = L"Ventana: " + Truncate( title, MAX_TITLE_CHARS );
	if( !processName.empty() )
		label += L" [" + processName + L"]";

	CaptureTarget target;
	target.kind      = CaptureTarget::Kind::Window;
	target.window    = hwnd;
	target.label     = ToUtf8( label );
	target.searchKey = ToLower( title + L" " + processName + L" " + label );
	targets.push_back( target );
	return TRUE;
}
}// namespace

std::vector< CaptureTarget > EnumerateCaptureTargets()
{
	std::vector< CaptureTarget > targets;

	CaptureTarget none;
	none.label = "(ninguna)";
	targets.push_back( none );

	EnumDisplayMonitors( nullptr, nullptr, AddMonitor, reinterpret_cast< LPARAM >( &targets ) );
	EnumWindows( AddWindow, reinterpret_cast< LPARAM >( &targets ) );
	return targets;
}

bool IsTargetAlive( const CaptureTarget& target )
{
	switch( target.kind )
	{
	case CaptureTarget::Kind::Window:
		return IsWindow( target.window ) != FALSE;
	case CaptureTarget::Kind::Monitor:
	{
		MONITORINFO info{};
		info.cbSize = sizeof( info );
		return GetMonitorInfoW( target.monitor, &info ) != FALSE;
	}
	default:
		return false;
	}
}

int FindTarget( const std::vector< CaptureTarget >& targets, const std::string& utf8Text )
{
	std::wstring needle = ToLower( FromUtf8( utf8Text ) );
	if( needle.empty() )
		return -1;

	//Windows first: "chrome" should find the browser, not a monitor that happens to match.
	for( CaptureTarget::Kind kind : { CaptureTarget::Kind::Window, CaptureTarget::Kind::Monitor } )
	{
		for( size_t index = 0; index < targets.size(); ++index )
		{
			if( targets[ index ].kind == kind && targets[ index ].searchKey.find( needle ) != std::wstring::npos )
				return static_cast< int >( index );
		}
	}
	return -1;
}

std::string ToUtf8( const std::wstring& text )
{
	if( text.empty() )
		return {};
	int size = WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast< int >( text.size() ), nullptr, 0, nullptr, nullptr );
	std::string result( static_cast< size_t >( size ), '\0' );
	WideCharToMultiByte( CP_UTF8, 0, text.data(), static_cast< int >( text.size() ), &result[ 0 ], size, nullptr, nullptr );
	return result;
}

std::wstring FromUtf8( const std::string& text )
{
	if( text.empty() )
		return {};
	int size = MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast< int >( text.size() ), nullptr, 0 );
	std::wstring result( static_cast< size_t >( size ), L'\0' );
	MultiByteToWideChar( CP_UTF8, 0, text.data(), static_cast< int >( text.size() ), &result[ 0 ], size );
	return result;
}

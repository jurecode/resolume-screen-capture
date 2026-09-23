#include "../CaptureTargets.h"
#include "../Platform.h"
#include <windows.h>
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

// InternalGetWindowText reads the title Windows already stores, unlike GetWindowText(Length)
// which sends a message to the window and waits for its thread to answer. Waiting like that
// can hang on a frozen app, or deadlock against Resolume's own UI thread while it waits for us.
std::wstring GetWindowTitle( HWND hwnd )
{
	wchar_t title[ 512 ];
	int length = InternalGetWindowText( hwnd, title, ARRAYSIZE( title ) );
	return std::wstring( title, length > 0 ? static_cast< size_t >( length ) : 0 );
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
	DWORD processId = 0;
	GetWindowThreadProcessId( hwnd, &processId );
	if( processId == GetCurrentProcessId() )
		return false;//Resolume itself: capturing it only makes an infinite mirror.
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
	target.id        = reinterpret_cast< uint64_t >( monitor );
	target.label     = ToUtf8( label );
	target.searchKey = ToUtf8( ToLower( label + L" " + info.szDevice ) );
	targets.push_back( target );
	return TRUE;
}

BOOL CALLBACK AddWindow( HWND hwnd, LPARAM param )
{
	if( !IsCapturableWindow( hwnd ) )
		return TRUE;

	auto& targets             = *reinterpret_cast< std::vector< CaptureTarget >* >( param );
	std::wstring title        = GetWindowTitle( hwnd );
	if( title.empty() )
		return TRUE;
	std::wstring processName  = GetProcessName( hwnd );
	std::wstring label        = L"Ventana: " + Truncate( title, MAX_TITLE_CHARS );
	if( !processName.empty() )
		label += L" [" + processName + L"]";

	CaptureTarget target;
	target.kind      = CaptureTarget::Kind::Window;
	target.id        = reinterpret_cast< uint64_t >( hwnd );
	target.label     = ToUtf8( label );
	target.searchKey = ToUtf8( ToLower( title + L" " + processName + L" " + label ) );
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
		return IsWindow( reinterpret_cast< HWND >( target.id ) ) != FALSE;
	case CaptureTarget::Kind::Monitor:
	{
		MONITORINFO info{};
		info.cbSize = sizeof( info );
		return GetMonitorInfoW( reinterpret_cast< HMONITOR >( target.id ), &info ) != FALSE;
	}
	default:
		return false;
	}
}

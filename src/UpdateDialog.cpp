#include "UpdateDialog.h"
#include "CaptureTargets.h"//FromUtf8
#include "Updater.h"

#include <windows.h>
#include <atomic>
#include <thread>

namespace
{
enum ControlId
{
	ID_INSTALL = 101,
	ID_LATER,
	ID_SKIP,
	ID_CLOSE,
};

const UINT_PTR POLL_TIMER    = 1;
const wchar_t CLASS_NAME[]   = L"CapturaPantallaUpdateWindow";
const int CLIENT_WIDTH       = 470;
const int CLIENT_HEIGHT      = 290;
const int MARGIN             = 22;
const int BUTTON_HEIGHT      = 32;

std::atomic< bool > dialogOpen{ false };

struct DialogState
{
	std::wstring newVersion;
	HWND title   = nullptr;
	HWND text    = nullptr;
	HWND install = nullptr;
	HWND later   = nullptr;
	HWND skip    = nullptr;
	HWND close   = nullptr;
	HFONT font      = nullptr;
	HFONT titleFont = nullptr;
	bool installing = false;
};

HMODULE ThisModule()
{
	HMODULE module = nullptr;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    reinterpret_cast< LPCWSTR >( &ThisModule ), &module );
	return module;
}

void ShowButtons( DialogState& state, bool install, bool later, bool skip, bool close )
{
	ShowWindow( state.install, install ? SW_SHOW : SW_HIDE );
	ShowWindow( state.later, later ? SW_SHOW : SW_HIDE );
	ShowWindow( state.skip, skip ? SW_SHOW : SW_HIDE );
	ShowWindow( state.close, close ? SW_SHOW : SW_HIDE );
}

void StartInstall( HWND window, DialogState& state )
{
	state.installing = true;
	Updater::Get().PressAction();
	SetWindowTextW( state.title, L"Actualizando…" );
	SetWindowTextW( state.text, ( L"Descargando e instalando la versión " + state.newVersion +
	                              L".\r\n\r\nPuedes seguir usando Arena mientras tanto." ).c_str() );
	ShowButtons( state, false, false, false, false );
	SetTimer( window, POLL_TIMER, 300, nullptr );
}

void OnTimer( HWND window, DialogState& state )
{
	Updater::Status status = Updater::Get().GetStatus();
	switch( status.state )
	{
	case Updater::State::Installed:
		KillTimer( window, POLL_TIMER );
		state.installing = false;
		SetWindowTextW( state.title, L"¡Listo!" );
		SetWindowTextW( state.text, ( L"La versión " + state.newVersion + L" quedó instalada.\r\n\r\n"
		                              L"Cierra Resolume Arena y vuelve a abrirlo para empezar a usarla." ).c_str() );
		SetWindowTextW( state.close, L"Entendido" );
		ShowButtons( state, false, false, false, true );
		SetFocus( state.close );
		break;

	case Updater::State::Failed:
		KillTimer( window, POLL_TIMER );
		state.installing = false;
		SetWindowTextW( state.title, L"No se pudo actualizar" );
		SetWindowTextW( state.text, ( FromUtf8( status.error ) + L"\r\n\r\nTu versión actual sigue funcionando normalmente." ).c_str() );
		SetWindowTextW( state.install, L"Reintentar" );
		SetWindowTextW( state.close, L"Cerrar" );
		ShowButtons( state, true, false, false, true );
		break;

	case Updater::State::Available:
	case Updater::State::Postponed:
		//"Reintentar" first checks again; once the update is found, go on and install it.
		if( state.installing )
			Updater::Get().PressAction();
		break;

	default:
		break;
	}
}

LRESULT CALLBACK WindowProc( HWND window, UINT message, WPARAM wParam, LPARAM lParam )
{
	auto* state = reinterpret_cast< DialogState* >( GetWindowLongPtrW( window, GWLP_USERDATA ) );
	switch( message )
	{
	case WM_COMMAND:
		if( state == nullptr )
			break;
		switch( LOWORD( wParam ) )
		{
		case ID_INSTALL:
			StartInstall( window, *state );
			return 0;
		case ID_LATER:
			Updater::Get().RemindLater();
			DestroyWindow( window );
			return 0;
		case ID_SKIP:
			Updater::Get().SkipVersion();
			DestroyWindow( window );
			return 0;
		case ID_CLOSE:
			DestroyWindow( window );
			return 0;
		}
		break;

	case WM_TIMER:
		if( state != nullptr && wParam == POLL_TIMER )
			OnTimer( window, *state );
		return 0;

	case WM_CTLCOLORSTATIC:
		SetBkColor( reinterpret_cast< HDC >( wParam ), GetSysColor( COLOR_WINDOW ) );
		SetTextColor( reinterpret_cast< HDC >( wParam ), GetSysColor( COLOR_WINDOWTEXT ) );
		return reinterpret_cast< LRESULT >( GetSysColorBrush( COLOR_WINDOW ) );

	case WM_CLOSE:
		//Closing the window while it downloads is fine: the install carries on in the background.
		DestroyWindow( window );
		return 0;

	case WM_DESTROY:
		PostQuitMessage( 0 );
		return 0;
	}
	return DefWindowProcW( window, message, wParam, lParam );
}

HWND AddControl( HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height,
                 int id, HFONT font )
{
	HWND control = CreateWindowExW( 0, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
	                                reinterpret_cast< HMENU >( static_cast< INT_PTR >( id ) ), ThisModule(), nullptr );
	SendMessageW( control, WM_SETFONT, reinterpret_cast< WPARAM >( font ), TRUE );
	return control;
}

void RunDialog( std::wstring currentVersion, std::wstring newVersion, std::wstring notes )
{
	//Use the dll's embedded manifest so the buttons get the modern Windows look.
	ACTCTXW activation{};
	activation.cbSize         = sizeof( activation );
	activation.dwFlags        = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
	activation.hModule        = ThisModule();
	activation.lpResourceName = MAKEINTRESOURCEW( 2 );
	HANDLE context            = CreateActCtxW( &activation );
	ULONG_PTR cookie          = 0;
	if( context != INVALID_HANDLE_VALUE )
		ActivateActCtx( context, &cookie );

	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc   = WindowProc;
	windowClass.hInstance     = ThisModule();
	windowClass.hCursor       = LoadCursorW( nullptr, MAKEINTRESOURCEW( 32512 ) );//IDC_ARROW
	windowClass.hIcon         = LoadIconW( nullptr, MAKEINTRESOURCEW( 32516 ) );  //IDI_INFORMATION
	windowClass.hbrBackground = GetSysColorBrush( COLOR_WINDOW );
	windowClass.lpszClassName = CLASS_NAME;
	RegisterClassW( &windowClass );//Fails harmlessly when already registered.

	NONCLIENTMETRICSW metrics{};
	metrics.cbSize = sizeof( metrics );
	SystemParametersInfoW( SPI_GETNONCLIENTMETRICS, sizeof( metrics ), &metrics, 0 );
	DialogState state;
	state.newVersion      = newVersion;
	state.font            = CreateFontIndirectW( &metrics.lfMessageFont );
	LOGFONTW titleLogFont = metrics.lfMessageFont;
	titleLogFont.lfHeight = titleLogFont.lfHeight * 3 / 2;
	titleLogFont.lfWeight = FW_SEMIBOLD;
	state.titleFont       = CreateFontIndirectW( &titleLogFont );

	//Open on the screen the user is working on (Resolume's window), never on the projector.
	const DWORD style   = WS_CAPTION | WS_SYSMENU;
	const DWORD exStyle = WS_EX_DLGMODALFRAME;
	RECT frame          = { 0, 0, CLIENT_WIDTH, CLIENT_HEIGHT };
	AdjustWindowRectEx( &frame, style, FALSE, exStyle );
	int windowWidth  = frame.right - frame.left;
	int windowHeight = frame.bottom - frame.top;
	MONITORINFO monitor{};
	monitor.cbSize = sizeof( monitor );
	GetMonitorInfoW( MonitorFromWindow( GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY ), &monitor );
	int x = ( monitor.rcWork.left + monitor.rcWork.right - windowWidth ) / 2;
	int y = ( monitor.rcWork.top + monitor.rcWork.bottom - windowHeight ) / 2;

	HWND window = CreateWindowExW( exStyle, CLASS_NAME, L"Captura Pantalla - Actualización", style, x, y, windowWidth,
	                               windowHeight, nullptr, nullptr, ThisModule(), nullptr );
	if( window != nullptr )
	{
		SetWindowLongPtrW( window, GWLP_USERDATA, reinterpret_cast< LONG_PTR >( &state ) );

		std::wstring body = L"Versión instalada: " + currentVersion + L"\r\nVersión nueva: " + newVersion;
		if( !notes.empty() )
			body += L"\r\n\r\nNovedades:\r\n" + notes;

		int contentWidth = CLIENT_WIDTH - 2 * MARGIN;
		int buttonsY     = CLIENT_HEIGHT - MARGIN - BUTTON_HEIGHT;
		state.title   = AddControl( window, L"STATIC", L"Hay una nueva versión del plugin", SS_LEFT, MARGIN, MARGIN, contentWidth, 34, 0, state.titleFont );
		state.text    = AddControl( window, L"STATIC", body.c_str(), SS_LEFT | SS_NOPREFIX, MARGIN, MARGIN + 46, contentWidth, buttonsY - MARGIN - 56, 0, state.font );
		state.install = AddControl( window, L"BUTTON", L"Actualizar ahora", WS_TABSTOP | BS_DEFPUSHBUTTON, MARGIN, buttonsY, 150, BUTTON_HEIGHT, ID_INSTALL, state.font );
		state.later   = AddControl( window, L"BUTTON", L"Más tarde", WS_TABSTOP | BS_PUSHBUTTON, MARGIN + 160, buttonsY, 120, BUTTON_HEIGHT, ID_LATER, state.font );
		state.skip    = AddControl( window, L"BUTTON", L"Omitir esta versión", WS_TABSTOP | BS_PUSHBUTTON, CLIENT_WIDTH - MARGIN - 136, buttonsY, 136, BUTTON_HEIGHT, ID_SKIP, state.font );
		state.close   = AddControl( window, L"BUTTON", L"Cerrar", WS_TABSTOP | BS_PUSHBUTTON, CLIENT_WIDTH - MARGIN - 136, buttonsY, 136, BUTTON_HEIGHT, ID_CLOSE, state.font );
		ShowWindow( state.close, SW_HIDE );

		ShowWindow( window, SW_SHOWNORMAL );
		SetForegroundWindow( window );
		SetFocus( state.install );

		MSG message;
		while( GetMessageW( &message, nullptr, 0, 0 ) > 0 )
		{
			if( !IsDialogMessageW( window, &message ) )
			{
				TranslateMessage( &message );
				DispatchMessageW( &message );
			}
		}
	}

	DeleteObject( state.font );
	DeleteObject( state.titleFont );
	if( context != INVALID_HANDLE_VALUE )
	{
		DeactivateActCtx( 0, cookie );
		ReleaseActCtx( context );
	}
}
}// namespace

void ShowUpdateDialog( const std::string& currentVersion, const std::string& newVersion, const std::string& notes )
{
	if( dialogOpen.exchange( true ) )
		return;

	std::wstring current = FromUtf8( currentVersion );
	std::wstring fresh   = FromUtf8( newVersion );
	std::wstring text    = FromUtf8( notes );
	std::thread( [ current, fresh, text ] {
		RunDialog( current, fresh, text );
		dialogOpen = false;
	} ).detach();
}

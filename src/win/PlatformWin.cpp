#include "../Platform.h"
#include "../PluginIdentity.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifndef PLUGIN_VERSION_STRING
#	define PLUGIN_VERSION_STRING "0.0.0"
#endif

namespace
{
const size_t MAX_DOWNLOAD_BYTES = 64 * 1024 * 1024;
const wchar_t REGISTRY_KEY[]    = L"Software\\ResolumeScreenCapture";

std::wstring ModulePath()
{
	HMODULE module = nullptr;
	GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                    reinterpret_cast< LPCWSTR >( &ModulePath ), &module );
	std::wstring path( 2048, L'\0' );
	DWORD length = GetModuleFileNameW( module, &path[ 0 ], static_cast< DWORD >( path.size() ) );
	path.resize( length );
	return path;
}

HMODULE PinnedModule()
{
	//Keep our dll loaded until the process exits, so background threads can never run
	//code that was unloaded underneath them.
	static HMODULE module = [] {
		HMODULE handle = nullptr;
		GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		                    reinterpret_cast< LPCWSTR >( &ModulePath ), &handle );
		return handle;
	}();
	return module;
}

struct InternetHandle
{
	HINTERNET handle = nullptr;
	InternetHandle( HINTERNET h ) :
		handle( h )
	{
	}
	~InternetHandle()
	{
		if( handle )
			WinHttpCloseHandle( handle );
	}
	operator HINTERNET() const
	{
		return handle;
	}
};
}// namespace

namespace platform
{
// HTTPS download with WinHTTP (built into Windows, follows GitHub's redirects).
bool HttpsGet( const std::string& url, std::string& body, std::string& error )
{
	std::wstring wideUrl = FromUtf8( url );
	wchar_t host[ 256 ];
	std::vector< wchar_t > path( 4096 );
	URL_COMPONENTS parts{};
	parts.dwStructSize     = sizeof( parts );
	parts.lpszHostName     = host;
	parts.dwHostNameLength = ARRAYSIZE( host );
	parts.lpszUrlPath      = path.data();
	parts.dwUrlPathLength  = static_cast< DWORD >( path.size() );
	if( !WinHttpCrackUrl( wideUrl.c_str(), 0, 0, &parts ) )
	{
		error = "URL invalida: " + url;
		return false;
	}
	if( parts.nScheme != INTERNET_SCHEME_HTTPS )
	{
		error = "Solo se permiten URLs https";
		return false;
	}

	std::wstring agent = L"ResolumeScreenCapture/" + FromUtf8( PLUGIN_VERSION_STRING );
	InternetHandle session( WinHttpOpen( agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 ) );
	if( !session )
	{
		error = "No se pudo iniciar WinHTTP";
		return false;
	}
	WinHttpSetTimeouts( session, 10000, 10000, 15000, 60000 );

	InternetHandle connection( WinHttpConnect( session, host, parts.nPort, 0 ) );
	InternetHandle request( connection ? WinHttpOpenRequest( connection, L"GET", path.data(), nullptr, WINHTTP_NO_REFERER,
	                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE )
	                                   : nullptr );
	const wchar_t headers[] = L"Cache-Control: no-cache\r\n";
	if( !request ||
	    !WinHttpSendRequest( request, headers, static_cast< DWORD >( -1L ), WINHTTP_NO_REQUEST_DATA, 0, 0, 0 ) ||
	    !WinHttpReceiveResponse( request, nullptr ) )
	{
		error = "Sin conexion con el servidor de actualizaciones (error " + std::to_string( GetLastError() ) + ")";
		return false;
	}

	DWORD statusCode = 0;
	DWORD size       = sizeof( statusCode );
	WinHttpQueryHeaders( request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
	                     &statusCode, &size, WINHTTP_NO_HEADER_INDEX );
	if( statusCode != 200 )
	{
		error = "El servidor respondio HTTP " + std::to_string( statusCode );
		return false;
	}

	body.clear();
	std::vector< char > buffer( 64 * 1024 );
	for( ;; )
	{
		DWORD read = 0;
		if( !WinHttpReadData( request, buffer.data(), static_cast< DWORD >( buffer.size() ), &read ) )
		{
			error = "La descarga se corto";
			return false;
		}
		if( read == 0 )
			return true;
		body.append( buffer.data(), read );
		if( body.size() > MAX_DOWNLOAD_BYTES )
		{
			error = "Archivo demasiado grande";
			return false;
		}
	}
}

std::string Sha256Hex( const std::string& data )
{
	UCHAR hash[ 32 ] = {};
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if( !BCRYPT_SUCCESS( BCryptOpenAlgorithmProvider( &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0 ) ) )
		return {};
	NTSTATUS result = BCryptHash( algorithm, nullptr, 0, reinterpret_cast< PUCHAR >( const_cast< char* >( data.data() ) ),
	                              static_cast< ULONG >( data.size() ), hash, sizeof( hash ) );
	BCryptCloseAlgorithmProvider( algorithm, 0 );
	if( !BCRYPT_SUCCESS( result ) )
		return {};

	std::string hex;
	char byte[ 3 ];
	for( UCHAR value : hash )
	{
		snprintf( byte, sizeof( byte ), "%02x", value );
		hex += byte;
	}
	return hex;
}

std::string ReadSetting( const char* name )
{
	wchar_t value[ 128 ] = {};
	DWORD size           = sizeof( value );
	if( RegGetValueW( HKEY_CURRENT_USER, REGISTRY_KEY, FromUtf8( name ).c_str(), RRF_RT_REG_SZ, nullptr, value, &size ) != ERROR_SUCCESS )
		return {};
	return ToUtf8( value );
}

void WriteSetting( const char* name, const std::string& value )
{
	std::wstring wide = FromUtf8( value );
	RegSetKeyValueW( HKEY_CURRENT_USER, REGISTRY_KEY, FromUtf8( name ).c_str(), REG_SZ, wide.c_str(),
	                 static_cast< DWORD >( ( wide.size() + 1 ) * sizeof( wchar_t ) ) );
}

// Windows notification (bottom-right toast). It doesn't take focus or cover the projector output,
// and Windows holds it back while a fullscreen app is running.
void ShowNotification( const std::string& title, const std::string& text )
{
	std::thread( [ title, text ] {
		HINSTANCE instance        = PinnedModule();
		const wchar_t className[] = L"ResolumeScreenCaptureNotifier";
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc   = DefWindowProcW;
		windowClass.hInstance     = instance;
		windowClass.lpszClassName = className;
		RegisterClassW( &windowClass );//Fails harmlessly if already registered.

		HWND window = CreateWindowExW( 0, className, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr );
		if( !window )
			return;

		NOTIFYICONDATAW icon{};
		icon.cbSize      = sizeof( icon );
		icon.hWnd        = window;
		icon.uID         = 1;
		icon.uFlags      = NIF_ICON | NIF_TIP | NIF_INFO;
		icon.hIcon       = LoadIconW( nullptr, MAKEINTRESOURCEW( 32516 ) );//IDI_INFORMATION
		icon.dwInfoFlags = NIIF_INFO;
		wcsncpy_s( icon.szTip, FromUtf8( PLUGIN_DISPLAY_NAME " (Resolume)" ).c_str(), _TRUNCATE );
		wcsncpy_s( icon.szInfoTitle, FromUtf8( title ).c_str(), _TRUNCATE );
		wcsncpy_s( icon.szInfo, FromUtf8( text ).c_str(), _TRUNCATE );
		Shell_NotifyIconW( NIM_ADD, &icon );

		//Keep the tray icon alive long enough for the notification to be seen.
		SetTimer( window, 1, 15000, nullptr );
		MSG message;
		while( GetMessageW( &message, nullptr, 0, 0 ) > 0 )
		{
			if( message.message == WM_TIMER )
				break;
			DispatchMessageW( &message );
		}

		Shell_NotifyIconW( NIM_DELETE, &icon );
		DestroyWindow( window );
	} ).detach();
}

void KeepPluginLoaded()
{
	PinnedModule();
}

void CleanupPreviousUpdate()
{
	//Leftover from the previous update, no longer loaded now.
	DeleteFileW( ( ModulePath() + L".old" ).c_str() );
}

std::string InstallUpdate( const std::string& dll )
{
	if( dll.size() < 2 || dll[ 0 ] != 'M' || dll[ 1 ] != 'Z' )
		return "El archivo descargado no es un dll";

	std::wstring current = ModulePath();
	std::wstring fresh   = current + L".new";
	std::wstring old     = current + L".old";

	HANDLE file = CreateFileW( fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
	if( file == INVALID_HANDLE_VALUE )
		return "No se puede escribir en la carpeta del plugin";
	DWORD written = 0;
	BOOL wrote    = WriteFile( file, dll.data(), static_cast< DWORD >( dll.size() ), &written, nullptr );
	CloseHandle( file );
	if( !wrote || written != dll.size() )
	{
		DeleteFileW( fresh.c_str() );
		return "No se pudo guardar la actualizacion";
	}

	//Windows won't let us overwrite a loaded dll, but it does let us rename it.
	DeleteFileW( old.c_str() );
	if( !MoveFileExW( current.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING ) )
	{
		DeleteFileW( fresh.c_str() );
		return "No se pudo apartar la version actual del plugin";
	}
	if( !MoveFileExW( fresh.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING ) )
	{
		MoveFileExW( old.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING );
		DeleteFileW( fresh.c_str() );
		return "No se pudo colocar la nueva version del plugin";
	}
	return {};
}

std::string LogFilePath()
{
	PWSTR documents = nullptr;
	if( FAILED( SHGetKnownFolderPath( FOLDERID_Documents, 0, nullptr, &documents ) ) )
		return {};
	std::wstring path = std::wstring( documents ) + L"\\" + FromUtf8( LOG_FILE_NAME );
	CoTaskMemFree( documents );
	return ToUtf8( path );
}

std::string LowercaseUtf8( const std::string& text )
{
	std::wstring wide = FromUtf8( text );
	if( !wide.empty() )
		CharLowerBuffW( &wide[ 0 ], static_cast< DWORD >( wide.size() ) );
	return ToUtf8( wide );
}
}// namespace platform

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

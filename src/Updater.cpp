#include "Updater.h"
#include "CaptureTargets.h"//ToUtf8 / FromUtf8
#include "UpdateDialog.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifndef PLUGIN_VERSION_STRING
#	define PLUGIN_VERSION_STRING "0.0.0"
#endif
#ifndef UPDATE_MANIFEST_URL
#	define UPDATE_MANIFEST_URL ""
#endif

namespace
{
const std::chrono::seconds FIRST_CHECK_DELAY( 20 );//Don't compete with Resolume while it starts up.
const std::chrono::hours CHECK_INTERVAL( 6 );
const std::chrono::hours REMIND_LATER( 24 );
const size_t MAX_DOWNLOAD_BYTES = 64 * 1024 * 1024;
const wchar_t REGISTRY_KEY[]    = L"Software\\ResolumeScreenCapture";

// ---------------------------------------------------------------------------------------------
// Small helpers

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
	//Keep our dll loaded until the process exits, so the background thread can never run
	//code that was unloaded underneath it.
	static HMODULE module = [] {
		HMODULE handle = nullptr;
		GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		                    reinterpret_cast< LPCWSTR >( &ModulePath ), &handle );
		return handle;
	}();
	return module;
}

std::vector< int > ParseVersion( std::string text )
{
	if( !text.empty() && ( text[ 0 ] == 'v' || text[ 0 ] == 'V' ) )
		text.erase( 0, 1 );
	std::vector< int > parts;
	size_t pos = 0;
	while( pos < text.size() && isdigit( static_cast< unsigned char >( text[ pos ] ) ) )
	{
		int value = 0;
		while( pos < text.size() && isdigit( static_cast< unsigned char >( text[ pos ] ) ) )
			value = value * 10 + ( text[ pos++ ] - '0' );
		parts.push_back( value );
		if( pos < text.size() && text[ pos ] == '.' )
			++pos;
		else
			break;
	}
	return parts;
}

bool IsNewer( const std::string& candidate, const std::string& current )
{
	std::vector< int > a = ParseVersion( candidate );
	std::vector< int > b = ParseVersion( current );
	if( a.empty() )
		return false;
	size_t count = a.size() > b.size() ? a.size() : b.size();
	a.resize( count, 0 );
	b.resize( count, 0 );
	return a > b;
}

// Reads a string field from the flat JSON manifest we generate ourselves.
std::string JsonString( const std::string& json, const char* key )
{
	std::string pattern = std::string( "\"" ) + key + "\"";
	size_t pos          = json.find( pattern );
	if( pos == std::string::npos )
		return {};
	pos = json.find( ':', pos + pattern.size() );
	if( pos == std::string::npos )
		return {};
	pos = json.find( '"', pos );
	if( pos == std::string::npos )
		return {};

	std::string value;
	for( ++pos; pos < json.size(); ++pos )
	{
		char c = json[ pos ];
		if( c == '"' )
			return value;
		if( c != '\\' || pos + 1 >= json.size() )
		{
			value += c;
			continue;
		}
		char escaped = json[ ++pos ];
		switch( escaped )
		{
		case 'n': value += '\n'; break;
		case 'r': break;
		case 't': value += '\t'; break;
		case 'u':
			if( pos + 4 < json.size() )
			{
				wchar_t character = static_cast< wchar_t >( strtoul( json.substr( pos + 1, 4 ).c_str(), nullptr, 16 ) );
				value += ToUtf8( std::wstring( 1, character ) );
				pos += 4;
			}
			break;
		default: value += escaped; break;
		}
	}
	return {};
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

std::string Lowercase( std::string text )
{
	for( char& c : text )
		c = static_cast< char >( tolower( static_cast< unsigned char >( c ) ) );
	return text;
}

// ---------------------------------------------------------------------------------------------
// HTTPS download with WinHTTP (built into Windows, follows GitHub's redirects).

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

// ---------------------------------------------------------------------------------------------
// User choices ("later" / "skip") survive Resolume restarts in the registry.

std::string ReadRegistryString( const wchar_t* name )
{
	wchar_t value[ 128 ] = {};
	DWORD size           = sizeof( value );
	if( RegGetValueW( HKEY_CURRENT_USER, REGISTRY_KEY, name, RRF_RT_REG_SZ, nullptr, value, &size ) != ERROR_SUCCESS )
		return {};
	return ToUtf8( value );
}

void WriteRegistryString( const wchar_t* name, const std::string& value )
{
	std::wstring wide = FromUtf8( value );
	RegSetKeyValueW( HKEY_CURRENT_USER, REGISTRY_KEY, name, REG_SZ, wide.c_str(), static_cast< DWORD >( ( wide.size() + 1 ) * sizeof( wchar_t ) ) );
}

long long UnixNow()
{
	return std::chrono::duration_cast< std::chrono::seconds >( std::chrono::system_clock::now().time_since_epoch() ).count();
}

// ---------------------------------------------------------------------------------------------
// Windows notification (bottom-right toast). It doesn't take focus or cover the projector output,
// and Windows holds it back while a fullscreen app is running.

void ShowNotification( std::string title, std::string text )
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
		wcsncpy_s( icon.szTip, L"Captura Pantalla (Resolume)", _TRUNCATE );
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
}// namespace

// =============================================================================================

struct Updater::Impl
{
	enum class Command
	{
		None,
		Check,
		Install,
	};

	std::mutex mutex;
	std::condition_variable wake;
	Command pending = Command::None;
	Status status;
	std::string downloadUrl;
	std::string downloadSha256;
	std::string alertedVersion;//Alert only once per version per Resolume session.
	bool startupCheck = true;  //The first check after Arena opens gets a window, later ones a notification.

	void SetState( State state )//Caller holds the mutex.
	{
		status.state = state;
		++status.revision;
	}

	bool IsPostponed( const std::string& version )
	{
		if( ReadRegistryString( L"SkipVersion" ) == version )
			return true;
		std::string remindAfter = ReadRegistryString( L"RemindAfter" );
		return !remindAfter.empty() && UnixNow() < atoll( remindAfter.c_str() );
	}

	void Run()
	{
		//Leftover from the previous update, no longer loaded now.
		DeleteFileW( ( ModulePath() + L".old" ).c_str() );

		auto nextCheck = std::chrono::steady_clock::now() + FIRST_CHECK_DELAY;
		for( ;; )
		{
			Command command;
			{
				std::unique_lock< std::mutex > lock( mutex );
				wake.wait_until( lock, nextCheck, [ this ] { return pending != Command::None; } );
				command = pending != Command::None ? pending : Command::Check;
				pending = Command::None;
			}

			if( command == Command::Install )
				Install();
			else
				Check();
			nextCheck = std::chrono::steady_clock::now() + CHECK_INTERVAL;
		}
	}

	void Check()
	{
		{
			std::lock_guard< std::mutex > lock( mutex );
			if( status.state == State::Installed || status.state == State::Downloading )
				return;
			SetState( State::Checking );
		}

		std::string manifest, error;
		bool ok = HttpsGet( UPDATE_MANIFEST_URL, manifest, error );
		std::string version = JsonString( manifest, "version" );
		if( ok && version.empty() )
		{
			ok    = false;
			error = "El manifiesto de actualizacion no tiene 'version'";
		}

		bool alert = false;
		bool showWindow = false;
		std::string notes, currentVersion;
		{
			std::lock_guard< std::mutex > lock( mutex );
			if( !ok )
			{
				status.error = error;
				SetState( State::Failed );
				return;
			}
			//A popup in the middle of a show would be bad: only the check right after Arena opens gets one.
			showWindow     = startupCheck;
			startupCheck   = false;
			currentVersion = status.currentVersion;

			status.error.clear();
			if( !IsNewer( version, status.currentVersion ) )
			{
				SetState( State::UpToDate );
				return;
			}

			status.latestVersion = version;
			status.notes         = JsonString( manifest, "notes" );
			notes                = status.notes;
			downloadUrl          = JsonString( manifest, "url" );
			downloadSha256       = Lowercase( JsonString( manifest, "sha256" ) );
			if( IsPostponed( version ) )
			{
				SetState( State::Postponed );
				return;
			}
			SetState( State::Available );
			alert          = alertedVersion != version;
			alertedVersion = version;
		}

		if( alert && showWindow )
		{
			ShowUpdateDialog( currentVersion, version, notes );
		}
		else if( alert )
		{
			std::string text = "Abre un clip de Captura Pantalla en Arena para instalarla.";
			if( !notes.empty() )
				text = notes.substr( 0, 180 ) + "\n" + text;
			ShowNotification( "Actualizacion disponible: v" + version, text );
		}
	}

	void Install()
	{
		std::string url, expectedHash, version;
		{
			std::lock_guard< std::mutex > lock( mutex );
			if( status.state != State::Available && status.state != State::Postponed )
				return;
			url          = downloadUrl;
			expectedHash = downloadSha256;
			version      = status.latestVersion;
			SetState( State::Downloading );
		}

		std::string error = InstallFrom( url, expectedHash );

		std::lock_guard< std::mutex > lock( mutex );
		if( !error.empty() )
		{
			status.error = error;
			SetState( State::Failed );
			return;
		}
		status.error.clear();
		SetState( State::Installed );
		ShowNotification( "v" + version + " instalada", "Reinicia Resolume Arena para usar la nueva version." );
	}

	// Returns an empty string on success.
	static std::string InstallFrom( const std::string& url, const std::string& expectedHash )
	{
		if( url.empty() || expectedHash.size() != 64 )
			return "El manifiesto no tiene 'url' o 'sha256'";

		std::string dll, error;
		if( !HttpsGet( url, dll, error ) )
			return error;
		if( Sha256Hex( dll ) != expectedHash )
			return "El archivo descargado no coincide con su sha256; no se instalo";
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
};

Updater& Updater::Get()
{
	//Intentionally leaked: the background thread may still be running at process exit.
	static Updater* instance = new Updater();
	return *instance;
}

void Updater::Start()
{
	static std::once_flag once;
	std::call_once( once, [ this ] {
		impl                        = new Impl();
		impl->status.currentVersion = PLUGIN_VERSION_STRING;
		if( std::string( UPDATE_MANIFEST_URL ).empty() )
			return;//Local builds without a manifest URL never update.

		PinnedModule();
		impl->status.state = State::UpToDate;
		++impl->status.revision;
		Impl* state = impl;
		std::thread( [ state ] { state->Run(); } ).detach();
	} );
}

Updater::Status Updater::GetStatus()
{
	if( impl == nullptr )
		return {};
	std::lock_guard< std::mutex > lock( impl->mutex );
	return impl->status;
}

void Updater::PressAction()
{
	if( impl == nullptr )
		return;
	std::lock_guard< std::mutex > lock( impl->mutex );
	switch( impl->status.state )
	{
	case State::Available:
	case State::Postponed:
		impl->pending = Impl::Command::Install;
		break;
	case State::UpToDate:
	case State::Failed:
		impl->pending = Impl::Command::Check;
		break;
	default:
		return;//Busy, or waiting for a restart.
	}
	impl->wake.notify_one();
}

void Updater::RemindLater()
{
	if( impl == nullptr )
		return;
	std::lock_guard< std::mutex > lock( impl->mutex );
	if( impl->status.state != State::Available )
		return;
	auto later = UnixNow() + std::chrono::duration_cast< std::chrono::seconds >( REMIND_LATER ).count();
	WriteRegistryString( L"RemindAfter", std::to_string( later ) );
	impl->SetState( State::Postponed );
}

void Updater::SkipVersion()
{
	if( impl == nullptr )
		return;
	std::lock_guard< std::mutex > lock( impl->mutex );
	if( impl->status.state != State::Available )
		return;
	WriteRegistryString( L"SkipVersion", impl->status.latestVersion );
	impl->SetState( State::Postponed );
}

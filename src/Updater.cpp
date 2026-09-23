#include "Updater.h"
#include "Platform.h"
#include "UpdateDialog.h"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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

void AppendUtf8( std::string& out, unsigned int codePoint )
{
	if( codePoint < 0x80 )
		out += static_cast< char >( codePoint );
	else if( codePoint < 0x800 )
	{
		out += static_cast< char >( 0xC0 | ( codePoint >> 6 ) );
		out += static_cast< char >( 0x80 | ( codePoint & 0x3F ) );
	}
	else
	{
		out += static_cast< char >( 0xE0 | ( codePoint >> 12 ) );
		out += static_cast< char >( 0x80 | ( ( codePoint >> 6 ) & 0x3F ) );
		out += static_cast< char >( 0x80 | ( codePoint & 0x3F ) );
	}
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
				AppendUtf8( value, static_cast< unsigned int >( strtoul( json.substr( pos + 1, 4 ).c_str(), nullptr, 16 ) ) );
				pos += 4;
			}
			break;
		default: value += escaped; break;
		}
	}
	return {};
}

std::string Lowercase( std::string text )
{
	for( char& c : text )
		c = static_cast< char >( tolower( static_cast< unsigned char >( c ) ) );
	return text;
}

long long UnixNow()
{
	return std::chrono::duration_cast< std::chrono::seconds >( std::chrono::system_clock::now().time_since_epoch() ).count();
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
		if( platform::ReadSetting( "SkipVersion" ) == version )
			return true;
		std::string remindAfter = platform::ReadSetting( "RemindAfter" );
		return !remindAfter.empty() && UnixNow() < atoll( remindAfter.c_str() );
	}

	void Run()
	{
		platform::CleanupPreviousUpdate();

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
		bool ok = platform::HttpsGet( UPDATE_MANIFEST_URL, manifest, error );
		std::string version = JsonString( manifest, "version" );
		if( ok && version.empty() )
		{
			ok    = false;
			error = "El manifiesto de actualizacion no tiene 'version'";
		}
		//An older release may only have been published for the other platform.
		if( ok && JsonString( manifest, platform::UPDATE_URL_KEY ).empty() )
			version = PLUGIN_VERSION_STRING;

		bool alert      = false;
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
			downloadUrl          = JsonString( manifest, platform::UPDATE_URL_KEY );
			downloadSha256       = Lowercase( JsonString( manifest, platform::UPDATE_SHA256_KEY ) );
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
			platform::ShowNotification( "Actualizacion disponible: v" + version, text );
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
		{
			std::lock_guard< std::mutex > lock( mutex );
			if( !error.empty() )
			{
				status.error = error;
				SetState( State::Failed );
				return;
			}
			status.error.clear();
			SetState( State::Installed );
		}
		platform::ShowNotification( "v" + version + " instalada", "Reinicia Resolume Arena para usar la nueva version." );
	}

	// Returns an empty string on success.
	static std::string InstallFrom( const std::string& url, const std::string& expectedHash )
	{
		if( url.empty() || expectedHash.size() != 64 )
			return "El manifiesto no tiene la descarga para este sistema";

		std::string package, error;
		if( !platform::HttpsGet( url, package, error ) )
			return error;
		if( platform::Sha256Hex( package ) != expectedHash )
			return "El archivo descargado no coincide con su sha256; no se instalo";
		return platform::InstallUpdate( package );
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

		platform::KeepPluginLoaded();
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
	platform::WriteSetting( "RemindAfter", std::to_string( later ) );
	impl->SetState( State::Postponed );
}

void Updater::SkipVersion()
{
	if( impl == nullptr )
		return;
	std::lock_guard< std::mutex > lock( impl->mutex );
	if( impl->status.state != State::Available )
		return;
	platform::WriteSetting( "SkipVersion", impl->status.latestVersion );
	impl->SetState( State::Postponed );
}

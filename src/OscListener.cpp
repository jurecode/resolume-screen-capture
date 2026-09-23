#include "OscListener.h"
#include "FileLog.h"
#include "Platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#ifdef _WIN32
#	include <winsock2.h>
#	include <ws2tcpip.h>
using Socket                        = SOCKET;
static const Socket NO_SOCKET       = INVALID_SOCKET;
static void CloseSocket( Socket s ) { closesocket( s ); }
#else
#	include <arpa/inet.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <unistd.h>
using Socket                        = int;
static const Socket NO_SOCKET       = -1;
static void CloseSocket( Socket s ) { close( s ); }
#endif

namespace
{
const size_t MAX_KEPT_TRIGGERS  = 64;
const size_t MAX_LOGGED_ADDRESSES = 40;
const int RECEIVE_TIMEOUT_MS    = 500;//How quickly the thread notices a port change.

std::mutex mutex;
std::deque< osc::ClipTrigger > triggers;
uint64_t lastId = 0;
std::atomic< int > wantedPort{ 0 };
std::map< std::pair< int, int >, int > clipStates;//Last "connected" state per layer/clip.
bool sawConnectPresses = false;                  //Resolume sends button presses: prefer those.
std::once_flag startOnce;

uint32_t ReadBigEndian( const char* data )
{
	const unsigned char* bytes = reinterpret_cast< const unsigned char* >( data );
	return ( uint32_t( bytes[ 0 ] ) << 24 ) | ( uint32_t( bytes[ 1 ] ) << 16 ) | ( uint32_t( bytes[ 2 ] ) << 8 ) | bytes[ 3 ];
}

// OSC strings are null terminated and padded to a multiple of 4 bytes.
bool ReadString( const char* data, size_t size, size_t& offset, std::string& out )
{
	size_t end = offset;
	while( end < size && data[ end ] != '\0' )
		++end;
	if( end >= size )
		return false;
	out.assign( data + offset, end - offset );
	offset = ( end + 4 ) & ~size_t( 3 );
	return true;
}

// Reads "/composition/layers/<layer>/clips/<clip>/<last>" (or ".../columns/<n>/<last>").
bool ParseClipAddress( const std::string& address, int& layer, int& clip, std::string& last )
{
	int a = 0, b = 0, consumed = 0;
	char tail[ 32 ] = {};
	if( sscanf( address.c_str(), "/composition/layers/%d/clips/%d/%31s%n", &a, &b, tail, &consumed ) == 3 && consumed == static_cast< int >( address.size() ) )
	{
		layer = a;
		clip  = b;
		last  = tail;
		return true;
	}
	if( sscanf( address.c_str(), "/composition/columns/%d/%31s%n", &b, tail, &consumed ) == 2 && consumed == static_cast< int >( address.size() ) )
	{
		layer = -1;
		clip  = b;
		last  = tail;
		return true;
	}
	return false;
}

void LogAddressOnce( const std::string& address, float value )
{
	//A sample of what Resolume sends, to adapt the plugin if an Arena version names things differently.
	static std::set< std::string > logged;
	if( logged.size() >= MAX_LOGGED_ADDRESSES || !logged.insert( address ).second )
		return;
	LogToFile( "OSC recibido: " + address + " = " + std::to_string( value ) );
}

void HandleMessage( const char* data, size_t size )
{
	size_t offset = 0;
	std::string address, types;
	if( !ReadString( data, size, offset, address ) || !ReadString( data, size, offset, types ) || types.empty() || types[ 0 ] != ',' )
		return;

	//Only the first argument matters: 1/0 for "connect", a state number for "connected".
	float value = 1.0f;
	if( types.size() > 1 )
	{
		char type = types[ 1 ];
		if( ( type == 'i' || type == 'f' ) && offset + 4 <= size )
		{
			uint32_t raw = ReadBigEndian( data + offset );
			if( type == 'i' )
				value = static_cast< float >( static_cast< int32_t >( raw ) );
			else
				memcpy( &value, &raw, 4 );
		}
		else if( type == 'F' )
			value = 0.0f;
	}

	int layer = 0, clip = 0;
	std::string last;
	if( !ParseClipAddress( address, layer, clip, last ) )
		return;
	if( last.compare( 0, 7, "connect" ) == 0 )
		LogAddressOnce( address, value );

	//"connect 1" when a clip's button is pressed. "connected" is its state: 1 stopped,
	//2 previewing, 3 playing, 4 playing and previewing. A clip starts playing when its state
	//goes from below 3 to 3 or more (3 <-> 4 is only the preview being toggled).
	std::lock_guard< std::mutex > lock( mutex );
	bool triggered = false;
	if( last == "connect" && value > 0.5f )
	{
		sawConnectPresses = true;
		triggered         = true;
	}
	else if( last == "connected" )
	{
		int state    = static_cast< int >( value + 0.5f );
		auto key     = std::make_pair( layer, clip );
		auto known   = clipStates.find( key );
		int previous = known != clipStates.end() ? known->second : 1;
		clipStates[ key ] = state;
		triggered = !sawConnectPresses && previous < 3 && state >= 3;
	}
	if( !triggered )
		return;

	triggers.push_back( { ++lastId, std::chrono::steady_clock::now(), layer, clip } );
	while( triggers.size() > MAX_KEPT_TRIGGERS )
		triggers.pop_front();
}

void HandlePacket( const char* data, size_t size, int depth = 0 )
{
	if( size >= 16 && depth < 4 && memcmp( data, "#bundle", 8 ) == 0 )
	{
		//"#bundle\0", 8-byte time tag, then size-prefixed elements.
		for( size_t offset = 16; offset + 4 <= size; )
		{
			uint32_t length = ReadBigEndian( data + offset );
			offset += 4;
			if( length == 0 || offset + length > size )
				break;
			HandlePacket( data + offset, length, depth + 1 );
			offset += length;
		}
		return;
	}
	HandleMessage( data, size );
}

Socket OpenSocket( int port )
{
	Socket s = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( s == NO_SOCKET )
		return NO_SOCKET;
#ifdef _WIN32
	DWORD timeout = RECEIVE_TIMEOUT_MS;
	setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast< const char* >( &timeout ), sizeof( timeout ) );
#else
	timeval timeout{ 0, RECEIVE_TIMEOUT_MS * 1000 };
	setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof( timeout ) );
#endif
	//Only this computer: no firewall prompt, nothing reachable from the network.
	sockaddr_in address{};
	address.sin_family      = AF_INET;
	address.sin_port        = htons( static_cast< uint16_t >( port ) );
	address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
	if( bind( s, reinterpret_cast< sockaddr* >( &address ), sizeof( address ) ) != 0 )
	{
		CloseSocket( s );
		return NO_SOCKET;
	}
	return s;
}

void Run()
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup( MAKEWORD( 2, 2 ), &wsa );
#endif
	Socket s        = NO_SOCKET;
	int boundPort   = 0;
	int failedPort  = 0;
	bool heardAnything = false;
	std::vector< char > buffer( 65536 );

	for( ;; )
	{
		int port = wantedPort;
		if( port != boundPort )
		{
			if( s != NO_SOCKET )
				CloseSocket( s );
			s         = OpenSocket( port );
			boundPort = s != NO_SOCKET ? port : 0;
			if( s != NO_SOCKET )
				LogToFile( "OSC: escuchando a Resolume en 127.0.0.1:" + std::to_string( port ) );
			else if( failedPort != port )
			{
				failedPort = port;
				LogToFile( "OSC: no se pudo usar el puerto " + std::to_string( port ) + " (otro programa lo usa?)" );
			}
		}
		if( s == NO_SOCKET )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( RECEIVE_TIMEOUT_MS ) );
			continue;
		}

		int received = static_cast< int >( recv( s, buffer.data(), static_cast< int >( buffer.size() ), 0 ) );
		if( received <= 0 )
			continue;
		if( !heardAnything )
		{
			heardAnything = true;
			LogToFile( "OSC: llegan mensajes de Resolume" );
		}
		HandlePacket( buffer.data(), static_cast< size_t >( received ) );
	}
}
}// namespace

namespace osc
{
void Listen( int port )
{
	wantedPort = port;
	std::call_once( startOnce, [] {
		platform::KeepPluginLoaded();//The thread lives as long as Resolume.
		std::thread( Run ).detach();
	} );
}

std::vector< ClipTrigger > TriggersSince( uint64_t afterId )
{
	std::lock_guard< std::mutex > lock( mutex );
	std::vector< ClipTrigger > result;
	for( const ClipTrigger& trigger : triggers )
		if( trigger.id > afterId )
			result.push_back( trigger );
	return result;
}

uint64_t LatestTriggerId()
{
	std::lock_guard< std::mutex > lock( mutex );
	return lastId;
}
}// namespace osc

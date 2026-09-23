#include "FileLog.h"
#include "Platform.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace
{
const long MAX_LOG_BYTES = 1024 * 1024;

std::mutex logMutex;
FILE* logFile  = nullptr;
bool triedOpen = false;

FILE* OpenFile( const std::string& path, const char* mode )
{
#ifdef _WIN32
	return _wfopen( FromUtf8( path ).c_str(), FromUtf8( mode ).c_str() );
#else
	return fopen( path.c_str(), mode );
#endif
}

FILE* OpenLog()
{
	if( triedOpen )
		return logFile;
	triedOpen = true;

	std::string path = platform::LogFilePath();
	if( path.empty() )
		return nullptr;

	//Start over when the log gets big, so it never grows without limit.
	logFile = OpenFile( path, "ab" );
	if( logFile != nullptr && ftell( logFile ) > MAX_LOG_BYTES )
	{
		fclose( logFile );
		logFile = OpenFile( path, "wb" );
	}
	return logFile;
}
}// namespace

void LogToFile( const std::string& message )
{
	std::lock_guard< std::mutex > lock( logMutex );
	FILE* file = OpenLog();
	if( file == nullptr )
		return;

	auto now          = std::chrono::system_clock::now();
	std::time_t clock = std::chrono::system_clock::to_time_t( now );
	int milliseconds  = static_cast< int >( std::chrono::duration_cast< std::chrono::milliseconds >( now.time_since_epoch() ).count() % 1000 );
	std::tm local{};
#ifdef _WIN32
	localtime_s( &local, &clock );
#else
	localtime_r( &clock, &local );
#endif
	char stamp[ 32 ];
	strftime( stamp, sizeof( stamp ), "%Y-%m-%d %H:%M:%S", &local );
	fprintf( file, "%s.%03d  %s\r\n", stamp, milliseconds, message.c_str() );
	fflush( file );
}

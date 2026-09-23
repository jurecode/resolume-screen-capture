#include "FileLog.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <mutex>

namespace
{
const long MAX_LOG_BYTES = 1024 * 1024;

std::mutex logMutex;
FILE* logFile = nullptr;
bool triedOpen = false;

FILE* OpenLog()
{
	if( triedOpen )
		return logFile;
	triedOpen = true;

	PWSTR documents = nullptr;
	if( FAILED( SHGetKnownFolderPath( FOLDERID_Documents, 0, nullptr, &documents ) ) )
		return nullptr;
	std::wstring path = std::wstring( documents ) + L"\\CapturaPantalla-log.txt";
	CoTaskMemFree( documents );

	//Start over when the log gets big, so it never grows without limit.
	logFile = _wfopen( path.c_str(), L"ab" );
	if( logFile != nullptr && ftell( logFile ) > MAX_LOG_BYTES )
	{
		fclose( logFile );
		logFile = _wfopen( path.c_str(), L"wb" );
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

	SYSTEMTIME time;
	GetLocalTime( &time );
	fprintf( file, "%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\r\n", time.wYear, time.wMonth, time.wDay, time.wHour,
	         time.wMinute, time.wSecond, time.wMilliseconds, message.c_str() );
	fflush( file );
}

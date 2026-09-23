#include "../Platform.h"
#include "../FileLog.h"
#include "../PluginIdentity.h"

#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <vector>

#ifndef PLUGIN_VERSION_STRING
#	define PLUGIN_VERSION_STRING "0.0.0"
#endif

extern char** environ;

namespace
{
const size_t MAX_DOWNLOAD_BYTES     = 64 * 1024 * 1024;
const CFStringRef PREFERENCES_DOMAIN = CFSTR( "com.jurecode.resolume-screen-capture" );

NSString* ToNSString( const std::string& text )
{
	return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

// .../Extra Effects/ScreenCapture.bundle
NSString* BundlePath()
{
	Dl_info info{};
	if( dladdr( reinterpret_cast< const void* >( &BundlePath ), &info ) == 0 || info.dli_fname == nullptr )
		return nil;
	//The binary lives at ScreenCapture.bundle/Contents/MacOS/ScreenCapture.
	NSString* path = [NSString stringWithUTF8String:info.dli_fname];
	for( int level = 0; level < 3; ++level )
		path = [path stringByDeletingLastPathComponent];
	return [path.pathExtension isEqualToString:@"bundle"] ? path : nil;
}

int RunTool( const char* tool, std::vector< const char* > arguments )
{
	arguments.insert( arguments.begin(), tool );
	arguments.push_back( nullptr );
	pid_t pid = 0;
	if( posix_spawn( &pid, tool, nullptr, nullptr, const_cast< char* const* >( arguments.data() ), environ ) != 0 )
		return -1;
	int status = 0;
	waitpid( pid, &status, 0 );
	return WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
}
}// namespace

namespace platform
{

bool HttpsGet( const std::string& url, std::string& body, std::string& error )
{
	@autoreleasepool
	{
		NSURL* address = [NSURL URLWithString:ToNSString( url )];
		if( address == nil || ![address.scheme isEqualToString:@"https"] )
		{
			error = "Solo se permiten URLs https";
			return false;
		}

		NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:address
		                                                       cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
		                                                   timeoutInterval:60];
		[request setValue:@"ResolumeScreenCapture/" PLUGIN_VERSION_STRING forHTTPHeaderField:@"User-Agent"];

		__block NSData* received        = nil;
		__block NSInteger statusCode    = 0;
		__block NSString* failure       = nil;
		dispatch_semaphore_t finished   = dispatch_semaphore_create( 0 );
		NSURLSessionDataTask* task      = [NSURLSession.sharedSession
            dataTaskWithRequest:request
              completionHandler:^( NSData* data, NSURLResponse* response, NSError* taskError ) {
                  received = data;
                  if( [response isKindOfClass:NSHTTPURLResponse.class] )
                      statusCode = ( (NSHTTPURLResponse*)response ).statusCode;
                  if( taskError != nil )
                      failure = taskError.localizedDescription;
                  dispatch_semaphore_signal( finished );
              }];
		[task resume];
		dispatch_semaphore_wait( finished, DISPATCH_TIME_FOREVER );

		if( failure != nil )
		{
			error = std::string( "Sin conexion con el servidor de actualizaciones: " ) + failure.UTF8String;
			return false;
		}
		if( statusCode != 200 )
		{
			error = "El servidor respondio HTTP " + std::to_string( statusCode );
			return false;
		}
		if( received.length > MAX_DOWNLOAD_BYTES )
		{
			error = "Archivo demasiado grande";
			return false;
		}
		body.assign( static_cast< const char* >( received.bytes ), received.length );
		return true;
	}
}

std::string Sha256Hex( const std::string& data )
{
	unsigned char hash[ CC_SHA256_DIGEST_LENGTH ];
	CC_SHA256( data.data(), static_cast< CC_LONG >( data.size() ), hash );
	std::string hex;
	char byte[ 3 ];
	for( unsigned char value : hash )
	{
		snprintf( byte, sizeof( byte ), "%02x", value );
		hex += byte;
	}
	return hex;
}

std::string ReadSetting( const char* name )
{
	@autoreleasepool
	{
		CFPropertyListRef value = CFPreferencesCopyAppValue( (__bridge CFStringRef)ToNSString( name ), PREFERENCES_DOMAIN );
		NSString* text          = CFBridgingRelease( value );
		return [text isKindOfClass:NSString.class] ? std::string( text.UTF8String ) : std::string();
	}
}

void WriteSetting( const char* name, const std::string& value )
{
	@autoreleasepool
	{
		CFPreferencesSetAppValue( (__bridge CFStringRef)ToNSString( name ), (__bridge CFStringRef)ToNSString( value ), PREFERENCES_DOMAIN );
		CFPreferencesAppSynchronize( PREFERENCES_DOMAIN );
	}
}

void ShowNotification( const std::string& title, const std::string& text )
{
	//macOS only lets an app's own bundle post notifications, and we live inside Resolume's.
	//The update window at startup and the buttons in the clip cover this instead.
	LogToFile( title + ": " + text );
}

void KeepPluginLoaded()
{
	static void* handle = [] {
		Dl_info info{};
		dladdr( reinterpret_cast< const void* >( &BundlePath ), &info );
		return info.dli_fname != nullptr ? dlopen( info.dli_fname, RTLD_NOW | RTLD_NODELETE ) : nullptr;
	}();
	(void)handle;
}

void CleanupPreviousUpdate()
{
	@autoreleasepool
	{
		NSString* bundle = BundlePath();
		if( bundle != nil )
			[NSFileManager.defaultManager removeItemAtPath:[bundle stringByAppendingString:@".old"] error:nil];
	}
}

std::string InstallUpdate( const std::string& zip )
{
	@autoreleasepool
	{
		NSFileManager* files = NSFileManager.defaultManager;
		NSString* bundle     = BundlePath();
		if( bundle == nil )
			return "No se encontro la carpeta del plugin";

		//Work next to the plugin so the final move is a simple rename on the same disk.
		NSString* folder  = [bundle stringByDeletingLastPathComponent];
		NSString* staging = [folder stringByAppendingPathComponent:@".ScreenCapture-update"];
		[files removeItemAtPath:staging error:nil];
		if( ![files createDirectoryAtPath:staging withIntermediateDirectories:YES attributes:nil error:nil] )
			return "No se puede escribir en la carpeta del plugin";

		NSString* zipPath = [staging stringByAppendingPathComponent:@"update.zip"];
		if( ![[NSData dataWithBytes:zip.data() length:zip.size()] writeToFile:zipPath atomically:YES] )
			return "No se pudo guardar la actualizacion";
		if( RunTool( "/usr/bin/ditto", { "-x", "-k", zipPath.fileSystemRepresentation, staging.fileSystemRepresentation } ) != 0 )
		{
			[files removeItemAtPath:staging error:nil];
			return "No se pudo descomprimir la actualizacion";
		}

		NSString* fresh = [staging stringByAppendingPathComponent:bundle.lastPathComponent];
		if( ![files fileExistsAtPath:[fresh stringByAppendingPathComponent:@"Contents/MacOS"]] )
		{
			[files removeItemAtPath:staging error:nil];
			return "La actualizacion descargada no contiene el plugin";
		}
		RunTool( "/usr/bin/xattr", { "-dr", "com.apple.quarantine", fresh.fileSystemRepresentation } );

		//Moving the loaded bundle aside is safe: Resolume keeps using the copy it already loaded.
		NSString* old = [bundle stringByAppendingString:@".old"];
		[files removeItemAtPath:old error:nil];
		if( ![files moveItemAtPath:bundle toPath:old error:nil] )
		{
			[files removeItemAtPath:staging error:nil];
			return "No se pudo apartar la version actual del plugin";
		}
		if( ![files moveItemAtPath:fresh toPath:bundle error:nil] )
		{
			[files moveItemAtPath:old toPath:bundle error:nil];
			[files removeItemAtPath:staging error:nil];
			return "No se pudo colocar la nueva version del plugin";
		}
		[files removeItemAtPath:staging error:nil];
		return {};
	}
}

std::string LogFilePath()
{
	@autoreleasepool
	{
		NSString* documents = NSSearchPathForDirectoriesInDomains( NSDocumentDirectory, NSUserDomainMask, YES ).firstObject;
		return documents != nil ? std::string( [documents stringByAppendingPathComponent:@LOG_FILE_NAME].UTF8String ) : std::string();
	}
}

std::string LowercaseUtf8( const std::string& text )
{
	@autoreleasepool
	{
		return std::string( ToNSString( text ).lowercaseString.UTF8String );
	}
}
}// namespace platform

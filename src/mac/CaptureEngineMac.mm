#include "../CaptureEngine.h"
#include "../FileLog.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
const int FRAMES_PER_SECOND = 60;
const int QUEUE_DEPTH       = 5;

enum class Phase
{
	Idle,
	Starting,
	Running,
	Closed,
	Failed,
};

// Everything ScreenCaptureKit's callbacks touch. They hold it weakly, so a callback arriving after
// the plugin instance is gone simply does nothing.
struct CaptureState
{
	std::mutex mutex;
	uint64_t wantedGeneration = 0;//Bumped by every Start/Stop; callbacks of older streams are ignored.
	bool wantedCursor         = true;
	id stream                 = nil;//SCStream of the current generation.
	id configuration          = nil;//SCStreamConfiguration, kept to toggle the cursor.
	id receiver               = nil;//Keeps the stream's delegate alive.

	std::vector< unsigned char > readyPixels;
	int readyWidth           = 0;
	int readyHeight          = 0;
	uint64_t readyGeneration = 0;
	bool readyIsNew          = false;
	std::string error;
	std::string activeLabel;
	std::chrono::steady_clock::time_point startTime;
	bool gotFirstFrame = false;

	std::atomic< Phase > phase{ Phase::Idle };
	dispatch_queue_t queue = dispatch_queue_create( "com.jurecode.screencapture.frames", DISPATCH_QUEUE_SERIAL );
	std::vector< unsigned char > workPixels;//Only touched on `queue`.

	bool IsCurrent( uint64_t generation )//Caller holds the mutex.
	{
		return generation == wantedGeneration;
	}

	void Fail( uint64_t generation, const std::string& message )
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( !IsCurrent( generation ) )
			return;
		LogToFile( "ERROR " + activeLabel + ": " + message );
		error = "No se pudo capturar " + activeLabel + ": " + message;
		phase = Phase::Failed;
	}

	void Deliver( uint64_t generation, CVPixelBufferRef pixels )
	{
		if( CVPixelBufferLockBaseAddress( pixels, kCVPixelBufferLock_ReadOnly ) != kCVReturnSuccess )
			return;
		size_t width    = CVPixelBufferGetWidth( pixels );
		size_t height   = CVPixelBufferGetHeight( pixels );
		size_t pitch    = CVPixelBufferGetBytesPerRow( pixels );
		auto* source    = static_cast< const unsigned char* >( CVPixelBufferGetBaseAddress( pixels ) );
		size_t rowBytes = width * 4;
		workPixels.resize( rowBytes * height );
		for( size_t row = 0; row < height; ++row )
			memcpy( workPixels.data() + row * rowBytes, source + row * pitch, rowBytes );
		CVPixelBufferUnlockBaseAddress( pixels, kCVPixelBufferLock_ReadOnly );

		std::lock_guard< std::mutex > lock( mutex );
		if( !IsCurrent( generation ) )
			return;
		readyPixels.swap( workPixels );
		readyWidth      = static_cast< int >( width );
		readyHeight     = static_cast< int >( height );
		readyGeneration = generation;
		readyIsNew      = true;
		phase           = Phase::Running;
		if( !gotFirstFrame )
		{
			gotFirstFrame = true;
			auto elapsed  = std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::steady_clock::now() - startTime ).count();
			LogToFile( "Primera imagen recibida: " + std::to_string( width ) + "x" + std::to_string( height ) + " en " + std::to_string( elapsed ) + " ms" );
		}
	}

	void Stopped( uint64_t generation, NSError* stopError )
	{
		std::lock_guard< std::mutex > lock( mutex );
		if( !IsCurrent( generation ) )
			return;
		LogToFile( "La captura se detuvo (" + activeLabel + "): " + std::string( stopError.localizedDescription.UTF8String ?: "" ) );
		stream = nil;
		phase  = Phase::Closed;//Usually the window was closed.
	}
};

std::string PermissionHelp()
{
	return "Resolume Arena necesita permiso de Grabacion de pantalla: Ajustes del Sistema > Privacidad y seguridad > "
	       "Grabacion de pantalla y audio del sistema. Activalo para Arena y reinicia Arena.";
}
}// namespace

API_AVAILABLE( macos( 12.3 ) )
@interface CSFrameReceiver : NSObject < SCStreamOutput, SCStreamDelegate >
- (instancetype)initWithState:(std::weak_ptr< CaptureState >)state generation:(uint64_t)generation;
@end

@implementation CSFrameReceiver
{
	std::weak_ptr< CaptureState > _state;
	uint64_t _generation;
}

- (instancetype)initWithState:(std::weak_ptr< CaptureState >)state generation:(uint64_t)generation
{
	if( ( self = [super init] ) )
	{
		_state      = state;
		_generation = generation;
	}
	return self;
}

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type
{
	if( type != SCStreamOutputTypeScreen )
		return;
	std::shared_ptr< CaptureState > state = _state.lock();
	if( !state )
		return;

	//"Idle" frames mean nothing changed (or the window is minimized): keep showing the last image.
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray( sampleBuffer, false );
	if( attachments != nullptr && CFArrayGetCount( attachments ) > 0 )
	{
		NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex( attachments, 0 );
		NSNumber* status   = info[ SCStreamFrameInfoStatus ];
		if( status != nil && status.integerValue != SCFrameStatusComplete )
			return;
	}

	CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer( sampleBuffer );
	if( pixels != nullptr )
		state->Deliver( _generation, pixels );
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
	if( std::shared_ptr< CaptureState > state = _state.lock() )
		state->Stopped( _generation, error );
}
@end

struct CaptureEngine::Impl
{
	std::shared_ptr< CaptureState > state = std::make_shared< CaptureState >();
	std::vector< unsigned char > frontPixels;//Render thread only.
};

namespace
{
API_AVAILABLE( macos( 12.3 ) )
void StopStream( id stream )
{
	if( stream != nil )
		[(SCStream*)stream stopCaptureWithCompletionHandler:^( NSError* ) {
		}];
}

API_AVAILABLE( macos( 12.3 ) )
void OpenStream( std::shared_ptr< CaptureState > state, CaptureTarget target, bool cursor, uint64_t generation )
{
	std::weak_ptr< CaptureState > weakState = state;
	[SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:NO
                                 completionHandler:^( SCShareableContent* content, NSError* contentError ) {
                                     std::shared_ptr< CaptureState > state = weakState.lock();
                                     if( !state )
                                         return;
                                     if( contentError != nil || content == nil )
                                     {
                                         state->Fail( generation, CGPreflightScreenCaptureAccess() ? std::string( contentError.localizedDescription.UTF8String ?: "error" ) : PermissionHelp() );
                                         return;
                                     }

                                     SCContentFilter* filter = nil;
                                     CGSize points           = CGSizeZero;
                                     if( target.kind == CaptureTarget::Kind::Window )
                                     {
                                         for( SCWindow* window in content.windows )
                                         {
                                             if( window.windowID == target.id )
                                             {
                                                 filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
                                                 points = window.frame.size;
                                             }
                                         }
                                     }
                                     else
                                     {
                                         for( SCDisplay* display in content.displays )
                                         {
                                             if( display.displayID == target.id )
                                             {
                                                 filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                                                 points = CGSizeMake( display.width, display.height );
                                             }
                                         }
                                     }
                                     if( filter == nil )
                                     {
                                         state->Fail( generation, "la ventana o pantalla ya no existe (pulsa 'Actualizar lista')" );
                                         return;
                                     }

                                     //Capture at full Retina resolution.
                                     CGFloat scale = NSScreen.mainScreen.backingScaleFactor;
                                     if( @available( macOS 14.0, * ) )
                                         scale = filter.pointPixelScale;
                                     if( scale < 1 )
                                         scale = 1;

                                     SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
                                     configuration.width                  = static_cast< size_t >( points.width * scale );
                                     configuration.height                 = static_cast< size_t >( points.height * scale );
                                     configuration.pixelFormat            = kCVPixelFormatType_32BGRA;
                                     configuration.showsCursor            = cursor;
                                     configuration.minimumFrameInterval   = CMTimeMake( 1, FRAMES_PER_SECOND );
                                     configuration.queueDepth             = QUEUE_DEPTH;

                                     CSFrameReceiver* receiver = [[CSFrameReceiver alloc] initWithState:weakState generation:generation];
                                     SCStream* stream          = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:receiver];
                                     NSError* outputError      = nil;
                                     if( ![stream addStreamOutput:receiver type:SCStreamOutputTypeScreen sampleHandlerQueue:state->queue error:&outputError] )
                                     {
                                         state->Fail( generation, outputError.localizedDescription.UTF8String ?: "error" );
                                         return;
                                     }

                                     {
                                         std::lock_guard< std::mutex > lock( state->mutex );
                                         if( !state->IsCurrent( generation ) )
                                             return;//The user already picked something else.
                                         state->stream        = stream;
                                         state->configuration = configuration;
                                         state->receiver      = receiver;
                                     }

                                     LogToFile( "Captura iniciada (" + std::to_string( configuration.width ) + "x" + std::to_string( configuration.height ) + "), esperando la primera imagen" );
                                     [stream startCaptureWithCompletionHandler:^( NSError* startError ) {
                                         if( startError != nil )
                                             if( std::shared_ptr< CaptureState > state = weakState.lock() )
                                                 state->Fail( generation, CGPreflightScreenCaptureAccess() ? std::string( startError.localizedDescription.UTF8String ?: "error" ) : PermissionHelp() );
                                     }];
                                 }];
}
}// namespace

CaptureEngine::CaptureEngine() :
	impl( std::make_shared< Impl >() )
{
}

CaptureEngine::~CaptureEngine()
{
	Stop();
}

bool CaptureEngine::IsSupported()
{
	if( @available( macOS 12.3, * ) )
		return true;
	return false;
}

bool CaptureEngine::HasPermission()
{
	if( CGPreflightScreenCaptureAccess() )
		return true;
	//Shows the system prompt (only the first time) and adds Resolume to the settings list.
	static std::once_flag once;
	std::call_once( once, [] { CGRequestScreenCaptureAccess(); } );
	LogToFile( PermissionHelp() );
	return false;
}

void CaptureEngine::Start( const CaptureTarget& target, bool showCursor )
{
	std::shared_ptr< CaptureState > state = impl->state;
	id previous                           = nil;
	uint64_t generation;
	{
		std::lock_guard< std::mutex > lock( state->mutex );
		generation            = ++state->wantedGeneration;
		previous              = state->stream;
		state->stream         = nil;
		state->configuration  = nil;
		state->receiver       = nil;
		state->wantedCursor   = showCursor;
		state->error.clear();
		state->activeLabel    = target.label;
		state->gotFirstFrame  = false;
		state->startTime      = std::chrono::steady_clock::now();
	}

	if( @available( macOS 12.3, * ) )
	{
		StopStream( previous );
		if( target.kind == CaptureTarget::Kind::None )
		{
			state->phase = Phase::Idle;
			return;
		}
		state->phase = Phase::Starting;
		LogToFile( "Iniciando captura: " + target.label );
		OpenStream( state, target, showCursor, generation );
	}
	else
	{
		state->Fail( generation, "se necesita macOS 12.3 o superior" );
	}
}

void CaptureEngine::Stop()
{
	std::shared_ptr< CaptureState > state = impl->state;
	id previous                           = nil;
	{
		std::lock_guard< std::mutex > lock( state->mutex );
		++state->wantedGeneration;
		previous             = state->stream;
		state->stream        = nil;
		state->configuration = nil;
		state->receiver      = nil;
	}
	state->phase = Phase::Idle;
	if( @available( macOS 12.3, * ) )
		StopStream( previous );
}

void CaptureEngine::SetCursorVisible( bool visible )
{
	std::shared_ptr< CaptureState > state = impl->state;
	std::lock_guard< std::mutex > lock( state->mutex );
	state->wantedCursor = visible;
	if( @available( macOS 12.3, * ) )
	{
		SCStreamConfiguration* configuration = state->configuration;
		if( state->stream == nil || configuration == nil || configuration.showsCursor == visible )
			return;
		configuration.showsCursor = visible;
		[(SCStream*)state->stream updateConfiguration:configuration completionHandler:^( NSError* ) {
		}];
	}
}

void CaptureEngine::SetRestoreMinimized( bool )
{
	//macOS can't capture minimized windows either, and restoring another app's window needs the
	//Accessibility permission. The last image stays on screen instead (idle frames are skipped).
}

bool CaptureEngine::IsActive() const
{
	Phase phase = impl->state->phase;
	return phase == Phase::Starting || phase == Phase::Running;
}

bool CaptureEngine::WasClosed() const
{
	return impl->state->phase == Phase::Closed;
}

bool CaptureEngine::TakeFrame( const FrameCallback& onFrame )
{
	std::shared_ptr< CaptureState > state = impl->state;
	int width, height;
	{
		std::lock_guard< std::mutex > lock( state->mutex );
		//Ignore frames that still belong to the previous source.
		if( !state->readyIsNew || state->readyGeneration != state->wantedGeneration )
			return false;
		impl->frontPixels.swap( state->readyPixels );
		width             = state->readyWidth;
		height            = state->readyHeight;
		state->readyIsNew = false;
	}
	onFrame( impl->frontPixels.data(), width, height );
	return true;
}

std::string CaptureEngine::TakeError()
{
	std::shared_ptr< CaptureState > state = impl->state;
	std::lock_guard< std::mutex > lock( state->mutex );
	std::string error;
	error.swap( state->error );
	return error;
}

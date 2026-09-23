#include "../CaptureTargets.h"
#include "../Platform.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

namespace
{
const NSUInteger MAX_TITLE_CHARS = 48;
const CGFloat MIN_WINDOW_WIDTH   = 100;
const CGFloat MIN_WINDOW_HEIGHT  = 60;

std::string ToStd( NSString* text )
{
	return text != nil ? std::string( text.UTF8String ) : std::string();
}

NSString* Truncate( NSString* text, NSUInteger maxChars )
{
	if( text.length <= maxChars )
		return text;
	//Cut on a whole character so emoji and accents survive.
	NSRange whole = [text rangeOfComposedCharacterSequencesForRange:NSMakeRange( 0, maxChars - 1 )];
	return [[text substringWithRange:whole] stringByAppendingString:@"…"];
}

NSString* ScreenName( CGDirectDisplayID display )
{
	if( @available( macOS 10.15, * ) )
	{
		for( NSScreen* screen in NSScreen.screens )
		{
			NSNumber* number = screen.deviceDescription[ @"NSScreenNumber" ];
			if( number.unsignedIntValue == display )
				return screen.localizedName;
		}
	}
	return @"";
}

void AddMonitors( std::vector< CaptureTarget >& targets )
{
	CGDirectDisplayID displays[ 16 ];
	uint32_t count = 0;
	if( CGGetActiveDisplayList( 16, displays, &count ) != kCGErrorSuccess )
		return;

	for( uint32_t index = 0; index < count; ++index )
	{
		CGDirectDisplayID display = displays[ index ];
		size_t width              = CGDisplayPixelsWide( display );
		size_t height             = CGDisplayPixelsHigh( display );
		if( CGDisplayModeRef mode = CGDisplayCopyDisplayMode( display ) )
		{
			width  = CGDisplayModeGetPixelWidth( mode );
			height = CGDisplayModeGetPixelHeight( mode );
			CGDisplayModeRelease( mode );
		}

		NSString* label = [NSString stringWithFormat:@"Pantalla %u (%zux%zu)%@", index + 1, width, height,
		                                              CGDisplayIsMain( display ) ? @" principal" : @""];
		CaptureTarget target;
		target.kind      = CaptureTarget::Kind::Monitor;
		target.id        = display;
		target.label     = ToStd( label );
		target.searchKey = ToStd( [NSString stringWithFormat:@"%@ %@", label, ScreenName( display )].lowercaseString );
		targets.push_back( target );
	}
}

void AddWindows( std::vector< CaptureTarget >& targets )
{
	//Front to back, including minimized windows and the ones on other Spaces.
	NSArray* windows = CFBridgingRelease( CGWindowListCopyWindowInfo( kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID ) );
	pid_t ourProcess = getpid();

	for( NSDictionary* window in windows )
	{
		if( [window[ (id)kCGWindowLayer ] intValue] != 0 )
			continue;//Menu bar, Dock, overlays...
		if( [window[ (id)kCGWindowOwnerPID ] intValue] == ourProcess )
			continue;//Resolume itself: capturing it only makes an infinite mirror.
		if( [window[ (id)kCGWindowAlpha ] doubleValue] <= 0.0 )
			continue;

		CGRect bounds;
		if( !CGRectMakeWithDictionaryRepresentation( (__bridge CFDictionaryRef)window[ (id)kCGWindowBounds ], &bounds ) ||
		    bounds.size.width < MIN_WINDOW_WIDTH || bounds.size.height < MIN_WINDOW_HEIGHT )
			continue;

		NSString* owner  = window[ (id)kCGWindowOwnerName ] ?: @"";
		NSString* title  = window[ (id)kCGWindowName ] ?: @"";//Empty without the Screen Recording permission.
		BOOL onScreen    = [window[ (id)kCGWindowIsOnscreen ] boolValue];
		if( title.length == 0 && !onScreen )
			continue;//Hidden helper windows every app keeps around.
		if( owner.length == 0 && title.length == 0 )
			continue;

		NSString* label = title.length > 0
		                      ? [NSString stringWithFormat:@"Ventana: %@ [%@]", Truncate( title, MAX_TITLE_CHARS ), owner]
		                      : [NSString stringWithFormat:@"Ventana: %@", owner];
		CaptureTarget target;
		target.kind      = CaptureTarget::Kind::Window;
		target.id        = [window[ (id)kCGWindowNumber ] unsignedIntValue];
		target.label     = ToStd( label );
		target.searchKey = ToStd( [NSString stringWithFormat:@"%@ %@ %@", title, owner, label].lowercaseString );
		targets.push_back( target );
	}
}
}// namespace

std::vector< CaptureTarget > EnumerateCaptureTargets()
{
	@autoreleasepool
	{
		std::vector< CaptureTarget > targets;
		CaptureTarget none;
		none.label = "(ninguna)";
		targets.push_back( none );

		AddMonitors( targets );
		AddWindows( targets );

		//Without the Screen Recording permission macOS hides window titles, so several windows of
		//one app would look identical: number the repeats.
		for( size_t index = 0; index < targets.size(); ++index )
		{
			int copies = 1;
			for( size_t other = index + 1; other < targets.size(); ++other )
			{
				if( targets[ other ].label == targets[ index ].label )
					targets[ other ].label += " (" + std::to_string( ++copies ) + ")";
			}
		}
		return targets;
	}
}

bool IsTargetAlive( const CaptureTarget& target )
{
	@autoreleasepool
	{
		switch( target.kind )
		{
		case CaptureTarget::Kind::Window:
		{
			NSArray* info = CFBridgingRelease( CGWindowListCopyWindowInfo( kCGWindowListOptionIncludingWindow, static_cast< CGWindowID >( target.id ) ) );
			return info.count > 0;
		}
		case CaptureTarget::Kind::Monitor:
			return CGDisplayIsActive( static_cast< CGDirectDisplayID >( target.id ) ) != 0;
		default:
			return false;
		}
	}
}

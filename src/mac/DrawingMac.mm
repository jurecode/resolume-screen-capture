#include "../Drawing.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include <cmath>

namespace
{
NSString* ToNSString( const std::string& text )
{
	return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

// Draws into an RGBA bitmap (premultiplied, as CoreGraphics requires) and hands back the pixels.
std::vector< unsigned char > DrawRGBA( int width, int height, void ( ^draw )( CGContextRef ) )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4, 0 );
	CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName( kCGColorSpaceSRGB );
	CGContextRef context       = CGBitmapContextCreate( pixels.data(), width, height, 8, width * 4, colorSpace,
                                                  kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big );
	if( context != nullptr )
	{
		draw( context );
		CGContextRelease( context );
	}
	CGColorSpaceRelease( colorSpace );
	return pixels;
}
}// namespace

namespace drawing
{
Bitmap RenderText( const std::string& utf8, int fontPixels, bool bold )
{
	@autoreleasepool
	{
		Bitmap result;
		NSString* text = ToNSString( utf8 );
		if( text.length == 0 || fontPixels <= 0 )
			return result;

		NSFont* font            = [NSFont systemFontOfSize:fontPixels weight:bold ? NSFontWeightSemibold : NSFontWeightRegular];
		NSDictionary* style     = @{ (id)kCTFontAttributeName : font, (id)kCTForegroundColorAttributeName : (id)CGColorGetConstantColor( kCGColorWhite ) };
		NSAttributedString* run = [[NSAttributedString alloc] initWithString:text attributes:style];
		CTLineRef line          = CTLineCreateWithAttributedString( (__bridge CFAttributedStringRef)run );

		CGFloat ascent = 0, descent = 0, leading = 0;
		double width  = CTLineGetTypographicBounds( line, &ascent, &descent, &leading );
		int margin    = fontPixels / 8 + 2;
		result.width  = static_cast< int >( std::ceil( width ) ) + 2 * margin;
		result.height = static_cast< int >( std::ceil( ascent + descent ) ) + 2 * margin;

		std::vector< unsigned char > pixels = DrawRGBA( result.width, result.height, ^( CGContextRef context ) {
			CGContextSetTextPosition( context, margin, margin + descent );
			CTLineDraw( line, context );
		} );
		CFRelease( line );

		//White glyphs: keep only how much each pixel is covered.
		result.rgba.resize( pixels.size() );
		for( size_t pixel = 0; pixel < pixels.size(); pixel += 4 )
		{
			result.rgba[ pixel ]     = 255;
			result.rgba[ pixel + 1 ] = 255;
			result.rgba[ pixel + 2 ] = 255;
			result.rgba[ pixel + 3 ] = pixels[ pixel + 3 ];
		}
		return result;
	}
}

bool LoadPhoto( const std::string& utf8Path, int maxSide, Bitmap& out, std::string& error )
{
	@autoreleasepool
	{
		NSURL* url                = [NSURL fileURLWithPath:ToNSString( utf8Path )];
		CGImageSourceRef source   = CGImageSourceCreateWithURL( (__bridge CFURLRef)url, nullptr );
		if( source == nullptr )
		{
			error = "No se pudo abrir la foto: " + utf8Path;
			return false;
		}
		//A "thumbnail" at our size, with the camera's rotation already applied.
		NSDictionary* options = @{
			(id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
			(id)kCGImageSourceCreateThumbnailWithTransform : @YES,
			(id)kCGImageSourceThumbnailMaxPixelSize : @( maxSide ),
		};
		CGImageRef image = CGImageSourceCreateThumbnailAtIndex( source, 0, (__bridge CFDictionaryRef)options );
		CFRelease( source );
		if( image == nullptr )
		{
			error = "Formato de foto no soportado: " + utf8Path;
			return false;
		}

		out.width                           = static_cast< int >( CGImageGetWidth( image ) );
		out.height                          = static_cast< int >( CGImageGetHeight( image ) );
		int width = out.width, height = out.height;
		std::vector< unsigned char > pixels = DrawRGBA( width, height, ^( CGContextRef context ) {
			CGContextDrawImage( context, CGRectMake( 0, 0, width, height ), image );
		} );
		CGImageRelease( image );

		//Back to straight alpha.
		for( size_t pixel = 0; pixel < pixels.size(); pixel += 4 )
		{
			unsigned int alpha = pixels[ pixel + 3 ];
			if( alpha > 0 && alpha < 255 )
				for( int channel = 0; channel < 3; ++channel )
					pixels[ pixel + channel ] = static_cast< unsigned char >( std::min( 255u, pixels[ pixel + channel ] * 255u / alpha ) );
		}
		out.rgba.swap( pixels );
		return true;
	}
}
}// namespace drawing

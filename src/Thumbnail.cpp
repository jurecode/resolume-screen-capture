// Icon Resolume shows for the plugin in the Sources browser and on clips.
// Drawn in code (a monitor with a red "recording" dot) so we don't need an image decoder.
#include <FFGLSDK.h>
#include <algorithm>
#include <cmath>

namespace
{
const FFUInt32 WIDTH  = 160;
const FFUInt32 HEIGHT = 120;

struct Rgb
{
	float r, g, b;
};

Rgb Mix( Rgb a, Rgb b, float t )
{
	return { a.r + ( b.r - a.r ) * t, a.g + ( b.g - a.g ) * t, a.b + ( b.b - a.b ) * t };
}

// Coverage (0..1) of pixel (x, y) by a rounded rectangle, with a one pixel soft edge.
float RoundedRect( float x, float y, float left, float top, float right, float bottom, float radius )
{
	float cx = std::max( left + radius, std::min( x, right - radius ) );
	float cy = std::max( top + radius, std::min( y, bottom - radius ) );
	float distance = std::sqrt( ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy ) );
	return std::max( 0.0f, std::min( 1.0f, radius - distance + 0.5f ) );
}

float Circle( float x, float y, float cx, float cy, float radius )
{
	float distance = std::sqrt( ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy ) );
	return std::max( 0.0f, std::min( 1.0f, radius - distance + 0.5f ) );
}

std::vector< CFFGLColor > DrawThumbnail()
{
	const Rgb backgroundTop    = { 0.11f, 0.16f, 0.20f };
	const Rgb backgroundBottom = { 0.05f, 0.08f, 0.10f };
	const Rgb frame            = { 0.85f, 0.89f, 0.91f };
	const Rgb screenTop        = { 0.09f, 0.78f, 0.66f };
	const Rgb screenBottom     = { 0.16f, 0.44f, 0.86f };
	const Rgb window           = { 0.95f, 0.97f, 0.98f };
	const Rgb record           = { 0.93f, 0.22f, 0.25f };

	std::vector< CFFGLColor > pixels( WIDTH * HEIGHT );
	for( FFUInt32 y = 0; y < HEIGHT; ++y )
	{
		for( FFUInt32 x = 0; x < WIDTH; ++x )
		{
			float px = x + 0.5f;
			float py = y + 0.5f;
			Rgb color = Mix( backgroundTop, backgroundBottom, py / HEIGHT );

			//Monitor: frame, screen, stand.
			color = Mix( color, frame, RoundedRect( px, py, 26, 16, 134, 88, 6 ) );
			float screen = RoundedRect( px, py, 31, 21, 129, 83, 3 );
			color = Mix( color, Mix( screenTop, screenBottom, ( py - 21 ) / 62 ), screen );
			color = Mix( color, frame, RoundedRect( px, py, 72, 88, 88, 99, 1 ) );
			color = Mix( color, frame, RoundedRect( px, py, 54, 97, 106, 103, 3 ) );

			//A window being captured, with its title bar.
			color = Mix( color, window, 0.9f * RoundedRect( px, py, 44, 34, 104, 72, 3 ) );
			color = Mix( color, screenBottom, 0.9f * RoundedRect( px, py, 44, 34, 104, 41, 2 ) );

			//Recording dot with a white ring.
			color = Mix( color, window, Circle( px, py, 118, 30, 8 ) );
			color = Mix( color, record, Circle( px, py, 118, 30, 6 ) );

			auto channel = []( float value ) { return static_cast< unsigned char >( std::max( 0.0f, std::min( 1.0f, value ) ) * 255.0f + 0.5f ); };
			pixels[ y * WIDTH + x ] = CFFGLColor( channel( color.r ), channel( color.g ), channel( color.b ), 255 );
		}
	}
	return pixels;
}
}// namespace

static CFFGLThumbnailInfo ThumbnailInfo( WIDTH, HEIGHT, DrawThumbnail() );

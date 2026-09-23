// Icon Resolume shows for the plugin in the Sources browser and on clips: a miniature lower third.
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

float RoundedRect( float x, float y, float left, float top, float right, float bottom, float radius )
{
	float cx = std::max( left + radius, std::min( x, right - radius ) );
	float cy = std::max( top + radius, std::min( y, bottom - radius ) );
	return std::max( 0.0f, std::min( 1.0f, radius - std::hypot( x - cx, y - cy ) + 0.5f ) );
}

float Circle( float x, float y, float cx, float cy, float radius )
{
	return std::max( 0.0f, std::min( 1.0f, radius - std::hypot( x - cx, y - cy ) + 0.5f ) );
}

std::vector< CFFGLColor > DrawThumbnail()
{
	const Rgb backgroundTop    = { 0.20f, 0.27f, 0.36f };
	const Rgb backgroundBottom = { 0.08f, 0.10f, 0.14f };
	const Rgb bar              = { 0.05f, 0.09f, 0.20f };
	const Rgb gold             = { 1.00f, 0.72f, 0.15f };
	const Rgb white            = { 0.96f, 0.97f, 0.98f };
	const Rgb silhouette       = { 0.62f, 0.68f, 0.78f };
	const Rgb photo            = { 0.30f, 0.36f, 0.48f };

	std::vector< CFFGLColor > pixels( WIDTH * HEIGHT );
	for( FFUInt32 y = 0; y < HEIGHT; ++y )
	{
		for( FFUInt32 x = 0; x < WIDTH; ++x )
		{
			float px = x + 0.5f, py = y + 0.5f;
			Rgb color = Mix( backgroundTop, backgroundBottom, py / HEIGHT );

			//Bar with its accent stripe and two lines of "text".
			color = Mix( color, bar, RoundedRect( px, py, 40, 70, 150, 102, 5 ) );
			color = Mix( color, gold, RoundedRect( px, py, 40, 99, 150, 102, 1 ) );
			color = Mix( color, white, RoundedRect( px, py, 66, 77, 132, 85, 3 ) );
			color = Mix( color, silhouette, RoundedRect( px, py, 66, 89, 116, 94, 2.5f ) );

			//Round photo with a gold ring and a person's silhouette.
			float ring  = Circle( px, py, 40, 86, 27 );
			float inner = Circle( px, py, 40, 86, 24 );
			color = Mix( color, gold, ring );
			Rgb face = photo;
			face     = Mix( face, silhouette, Circle( px, py, 40, 80, 8.5f ) );
			face     = Mix( face, silhouette, RoundedRect( px, py, 26, 91, 54, 118, 12 ) );
			color    = Mix( color, face, inner );

			auto channel = []( float value ) { return static_cast< unsigned char >( std::max( 0.0f, std::min( 1.0f, value ) ) * 255.0f + 0.5f ); };
			pixels[ y * WIDTH + x ] = CFFGLColor( channel( color.r ), channel( color.g ), channel( color.b ), 255 );
		}
	}
	return pixels;
}
}// namespace

static CFFGLThumbnailInfo ThumbnailInfo( WIDTH, HEIGHT, DrawThumbnail() );

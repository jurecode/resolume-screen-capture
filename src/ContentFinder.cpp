#include "ContentFinder.h"
#include <algorithm>
#include <cstdlib>

namespace
{
const int TOLERANCE        = 24;   //Sum of the B, G, R differences still counted as "background".
const float MIN_LINE_FILL  = 0.5f; //A row/column belongs to the content when half of it isn't background.
const float MIN_AREA       = 0.10f;//Of the searched region.
const int SAMPLE_STEP      = 4;    //Pixels skipped when measuring how full a row/column is.
const int PROBE_INSET_PERCENT = 2; //Background probes sit just inside the region's sides...
const int PROBE_BAND_PERCENT  = 20;//...or 20% from its top and bottom, below/above toolbars.
const int EDGE_DEPTH          = 8; //How far in from the content's sides a row must already be content.

struct Frame
{
	const unsigned char* pixels;
	int width;

	const unsigned char* At( int x, int y ) const
	{
		return pixels + ( static_cast< size_t >( y ) * width + x ) * 4;
	}
};

bool Same( const unsigned char* a, const unsigned char* b )
{
	return std::abs( a[ 0 ] - b[ 0 ] ) + std::abs( a[ 1 ] - b[ 1 ] ) + std::abs( a[ 2 ] - b[ 2 ] ) <= TOLERANCE;
}

// Fraction of a row (or column) segment that is not background.
float Fill( const Frame& frame, const unsigned char* background, bool row, int line, int from, int to )
{
	int samples = 0, content = 0;
	for( int position = from; position <= to; position += SAMPLE_STEP, ++samples )
	{
		const unsigned char* pixel = row ? frame.At( position, line ) : frame.At( line, position );
		if( !Same( pixel, background ) )
			++content;
	}
	return samples > 0 ? static_cast< float >( content ) / samples : 0.0f;
}

// A row (or column) of the content covers the whole span between the content's two edges, which
// toolbar text and icons never do: require content right at both ends and half of it overall.
bool IsContentLine( const Frame& frame, const unsigned char* background, bool row, int line, int from, int to )
{
	auto touches = [ & ]( int start, int step ) {
		for( int offset = 0; offset < EDGE_DEPTH; ++offset )
		{
			int position               = start + offset * step;
			const unsigned char* pixel = row ? frame.At( position, line ) : frame.At( line, position );
			if( !Same( pixel, background ) )
				return true;
		}
		return false;
	};
	return touches( from, 1 ) && touches( to, -1 ) && Fill( frame, background, row, line, from, to ) >= MIN_LINE_FILL;
}

int ToPixel( float fraction, int size )
{
	return std::max( 0, std::min( static_cast< int >( fraction * size + 0.5f ), size ) );
}
}// namespace

bool FindContentInFrame( const unsigned char* bgra, int width, int height, const float region[ 4 ], float result[ 4 ] )
{
	int x0 = ToPixel( region[ 0 ], width ), x1 = ToPixel( region[ 2 ], width ) - 1;
	int y0 = ToPixel( region[ 1 ], height ), y1 = ToPixel( region[ 3 ], height ) - 1;
	int regionWidth = x1 - x0 + 1, regionHeight = y1 - y0 + 1;
	if( regionWidth < 32 || regionHeight < 32 )
		return false;

	Frame frame{ bgra, width };
	int centerX = x0 + regionWidth / 2;
	int centerY = y0 + regionHeight / 2;
	int leftProbe   = x0 + std::max( 2, regionWidth * PROBE_INSET_PERCENT / 100 );
	int rightProbe  = x1 - std::max( 2, regionWidth * PROBE_INSET_PERCENT / 100 );
	int topProbe    = y0 + regionHeight * PROBE_BAND_PERCENT / 100;
	int bottomProbe = y1 - regionHeight * PROBE_BAND_PERCENT / 100;

	int left, right, top, bottom;
	if( Same( frame.At( leftProbe, centerY ), frame.At( rightProbe, centerY ) ) )
	{
		//Background on both sides: walk in from the sides to the content's edges, then keep the
		//rows that are mostly content (toolbars are mostly background, so they drop out).
		const unsigned char* background = frame.At( leftProbe, centerY );
		for( left = leftProbe; left < centerX && Same( frame.At( left, centerY ), background ); ++left )
		{
		}
		for( right = rightProbe; right > centerX && Same( frame.At( right, centerY ), background ); --right )
		{
		}
		if( right - left < std::max( regionWidth / 10, 2 * EDGE_DEPTH ) )
			return false;
		top = bottom = -1;
		for( int y = y0; y <= y1; ++y )
		{
			if( IsContentLine( frame, background, true, y, left, right ) )
			{
				if( top < 0 )
					top = y;
				bottom = y;
			}
		}
	}
	else if( Same( frame.At( centerX, topProbe ), frame.At( centerX, bottomProbe ) ) )
	{
		//Background above and below: the same walk, turned sideways.
		const unsigned char* background = frame.At( centerX, topProbe );
		for( top = topProbe; top < centerY && Same( frame.At( centerX, top ), background ); ++top )
		{
		}
		for( bottom = bottomProbe; bottom > centerY && Same( frame.At( centerX, bottom ), background ); --bottom )
		{
		}
		if( bottom - top < std::max( regionHeight / 10, 2 * EDGE_DEPTH ) )
			return false;
		left = right = -1;
		for( int x = x0; x <= x1; ++x )
		{
			if( IsContentLine( frame, background, false, x, top, bottom ) )
			{
				if( left < 0 )
					left = x;
				right = x;
			}
		}
	}
	else
	{
		return false;//The content fills the region: nothing to trim.
	}

	if( left < 0 || top < 0 || right <= left || bottom <= top )
		return false;
	float area = static_cast< float >( right - left + 1 ) * ( bottom - top + 1 );
	if( area < MIN_AREA * regionWidth * regionHeight )
		return false;

	result[ 0 ] = static_cast< float >( left ) / width;
	result[ 1 ] = static_cast< float >( top ) / height;
	result[ 2 ] = static_cast< float >( right + 1 ) / width;
	result[ 3 ] = static_cast< float >( bottom + 1 ) / height;
	return true;
}

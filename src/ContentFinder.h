#pragma once

// Finds the photo or video inside a captured app window by looking at the pixels: viewers like
// Windows Photos or a video player show the content centred on a flat background, with toolbars
// that are mostly background too.
//
// `region` (x0, y0, x1, y1 fractions, top-left origin) limits the search, e.g. to the area UI
// Automation reported. On success `result` holds the content in the same kind of fractions of
// the whole frame. Returns false when there is no flat background around the content.
bool FindContentInFrame( const unsigned char* bgra, int width, int height, const float region[ 4 ], float result[ 4 ] );

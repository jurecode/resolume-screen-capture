#pragma once
#include <string>
#include <vector>

// An image for the plugins' own drawing: RGBA, straight alpha, top row first.
struct Bitmap
{
	int width  = 0;
	int height = 0;
	std::vector< unsigned char > rgba;

	bool Empty() const
	{
		return width == 0 || height == 0;
	}
};

// Text and photos, drawn with each system's own tools.
// Implemented in win/DrawingWin.cpp (GDI + WIC) and mac/DrawingMac.mm (CoreText + ImageIO).
namespace drawing
{
// One line of white text: the alpha channel holds the anti-aliased glyphs. `fontPixels` is the font
// size; the bitmap includes a small transparent margin all around.
Bitmap RenderText( const std::string& utf8, int fontPixels, bool bold );

// Loads a photo (jpg, png, ...) the right way up (camera rotation applied), shrunk to fit `maxSide`.
bool LoadPhoto( const std::string& utf8Path, int maxSide, Bitmap& out, std::string& error );
}// namespace drawing

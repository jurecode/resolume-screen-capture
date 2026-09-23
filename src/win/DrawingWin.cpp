#include "../Drawing.h"
#include "../Platform.h"

#include <windows.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace
{
// WIC is free-threaded; keeping the process-wide MTA alive lets Resolume's threads use it without
// us initializing COM on threads we don't own.
void EnsureComUsable()
{
	static std::once_flag once;
	std::call_once( once, [] {
		CO_MTA_USAGE_COOKIE cookie{};
		CoIncrementMTAUsage( &cookie );//Intentionally never released.
	} );
}

WICBitmapTransformOptions TransformForOrientation( USHORT orientation )
{
	//EXIF orientation: how the camera was held.
	switch( orientation )
	{
	case 2: return WICBitmapTransformFlipHorizontal;
	case 3: return WICBitmapTransformRotate180;
	case 4: return WICBitmapTransformFlipVertical;
	case 5: return static_cast< WICBitmapTransformOptions >( WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal );
	case 6: return WICBitmapTransformRotate90;
	case 7: return static_cast< WICBitmapTransformOptions >( WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal );
	case 8: return WICBitmapTransformRotate270;
	default: return WICBitmapTransformRotate0;
	}
}

USHORT ReadOrientation( IWICBitmapFrameDecode* frame )
{
	winrt::com_ptr< IWICMetadataQueryReader > reader;
	if( FAILED( frame->GetMetadataQueryReader( reader.put() ) ) )
		return 1;
	for( const wchar_t* query : { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}" } )
	{
		PROPVARIANT value;
		PropVariantInit( &value );
		USHORT orientation = 0;
		if( SUCCEEDED( reader->GetMetadataByName( query, &value ) ) && value.vt == VT_UI2 )
			orientation = value.uiVal;
		PropVariantClear( &value );
		if( orientation != 0 )
			return orientation;
	}
	return 1;
}
}// namespace

namespace drawing
{
Bitmap RenderText( const std::string& utf8, int fontPixels, bool bold )
{
	Bitmap result;
	std::wstring text = FromUtf8( utf8 );
	if( text.empty() || fontPixels <= 0 )
		return result;

	HDC dc     = CreateCompatibleDC( nullptr );
	HFONT font = CreateFontW( -fontPixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
	                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
	                          bold ? L"Segoe UI Semibold" : L"Segoe UI" );
	HGDIOBJ previousFont = SelectObject( dc, font );

	SIZE size{};
	GetTextExtentPoint32W( dc, text.c_str(), static_cast< int >( text.size() ), &size );
	TEXTMETRICW metrics{};
	GetTextMetricsW( dc, &metrics );
	int margin    = fontPixels / 8 + 2;
	result.width  = size.cx + 2 * margin + metrics.tmOverhang;
	result.height = metrics.tmHeight + 2 * margin;

	BITMAPINFO info{};
	info.bmiHeader.biSize        = sizeof( BITMAPINFOHEADER );
	info.bmiHeader.biWidth       = result.width;
	info.bmiHeader.biHeight      = -result.height;//Top row first.
	info.bmiHeader.biPlanes      = 1;
	info.bmiHeader.biBitCount    = 32;
	info.bmiHeader.biCompression = BI_RGB;
	void* bits                   = nullptr;
	HBITMAP canvas               = CreateDIBSection( dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0 );
	if( canvas == nullptr )
	{
		SelectObject( dc, previousFont );
		DeleteObject( font );
		DeleteDC( dc );
		return Bitmap();
	}
	HGDIOBJ previousBitmap = SelectObject( dc, canvas );
	memset( bits, 0, static_cast< size_t >( result.width ) * result.height * 4 );

	//White on black: the grey level of each pixel is how much the glyph covers it.
	SetBkMode( dc, TRANSPARENT );
	SetTextColor( dc, RGB( 255, 255, 255 ) );
	TextOutW( dc, margin, margin, text.c_str(), static_cast< int >( text.size() ) );
	GdiFlush();

	const unsigned char* source = static_cast< const unsigned char* >( bits );
	result.rgba.resize( static_cast< size_t >( result.width ) * result.height * 4 );
	for( size_t pixel = 0; pixel < result.rgba.size(); pixel += 4 )
	{
		unsigned char coverage     = std::max( { source[ pixel ], source[ pixel + 1 ], source[ pixel + 2 ] } );
		result.rgba[ pixel ]     = 255;
		result.rgba[ pixel + 1 ] = 255;
		result.rgba[ pixel + 2 ] = 255;
		result.rgba[ pixel + 3 ] = coverage;
	}

	SelectObject( dc, previousBitmap );
	SelectObject( dc, previousFont );
	DeleteObject( canvas );
	DeleteObject( font );
	DeleteDC( dc );
	return result;
}

bool LoadPhoto( const std::string& utf8Path, int maxSide, Bitmap& out, std::string& error )
{
	EnsureComUsable();
	winrt::com_ptr< IWICImagingFactory > factory;
	if( FAILED( CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( factory.put() ) ) ) )
	{
		error = "Windows Imaging Component no esta disponible";
		return false;
	}

	winrt::com_ptr< IWICBitmapDecoder > decoder;
	winrt::com_ptr< IWICBitmapFrameDecode > frame;
	if( FAILED( factory->CreateDecoderFromFilename( FromUtf8( utf8Path ).c_str(), nullptr, GENERIC_READ,
	                                                WICDecodeMetadataCacheOnDemand, decoder.put() ) ) ||
	    FAILED( decoder->GetFrame( 0, frame.put() ) ) )
	{
		error = "No se pudo abrir la foto: " + utf8Path;
		return false;
	}

	UINT width = 0, height = 0;
	frame->GetSize( &width, &height );
	if( width == 0 || height == 0 )
	{
		error = "La foto esta vacia: " + utf8Path;
		return false;
	}

	winrt::com_ptr< IWICBitmapSource > source = frame.as< IWICBitmapSource >();
	double scale = std::min( 1.0, static_cast< double >( maxSide ) / std::max( width, height ) );
	if( scale < 1.0 )
	{
		winrt::com_ptr< IWICBitmapScaler > scaler;
		factory->CreateBitmapScaler( scaler.put() );
		scaler->Initialize( source.get(), std::max( 1u, static_cast< UINT >( width * scale ) ), std::max( 1u, static_cast< UINT >( height * scale ) ),
		                    WICBitmapInterpolationModeHighQualityCubic );
		source = scaler.as< IWICBitmapSource >();
	}

	WICBitmapTransformOptions transform = TransformForOrientation( ReadOrientation( frame.get() ) );
	if( transform != WICBitmapTransformRotate0 )
	{
		winrt::com_ptr< IWICBitmapFlipRotator > rotator;
		factory->CreateBitmapFlipRotator( rotator.put() );
		rotator->Initialize( source.get(), transform );
		source = rotator.as< IWICBitmapSource >();
	}

	winrt::com_ptr< IWICFormatConverter > converter;
	factory->CreateFormatConverter( converter.put() );
	if( FAILED( converter->Initialize( source.get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
	                                   WICBitmapPaletteTypeCustom ) ) )
	{
		error = "Formato de foto no soportado: " + utf8Path;
		return false;
	}
	UINT finalWidth = 0, finalHeight = 0;
	converter->GetSize( &finalWidth, &finalHeight );
	out.width  = static_cast< int >( finalWidth );
	out.height = static_cast< int >( finalHeight );
	out.rgba.resize( static_cast< size_t >( finalWidth ) * finalHeight * 4 );
	if( FAILED( converter->CopyPixels( nullptr, finalWidth * 4, static_cast< UINT >( out.rgba.size() ), out.rgba.data() ) ) )
	{
		error = "No se pudo leer la foto: " + utf8Path;
		out   = Bitmap();
		return false;
	}
	return true;
}
}// namespace drawing

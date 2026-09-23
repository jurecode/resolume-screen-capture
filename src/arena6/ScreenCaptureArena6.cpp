// "Captura Pantalla" for Resolume Arena 6 (FFGL 1.6, legacy OpenGL).
//
// Arena 6 has no dropdown parameters and can't be told about parameter changes made by the
// plugin, so the source is picked with a text field ("pantalla 1", "chrome", "keynote"...).
// Capture, window list and updates are the same code the Arena 7 plugin uses.
#include <TargetConditionals.h>
#include "FFGLPluginSDK.h"

#include "../CaptureEngine.h"
#include "../CaptureTargets.h"
#include "../FileLog.h"
#include "../Updater.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
enum ParamType : unsigned int
{
	PT_SOURCE,
	PT_KEEP_ASPECT,
	PT_CURSOR,
	PT_CROP_LEFT,
	PT_CROP_RIGHT,
	PT_CROP_TOP,
	PT_CROP_BOTTOM,
};

const char DEFAULT_SOURCE[] = "pantalla 1";
const float MAX_CROP        = 0.5f;//Each crop slider goes up to half of the image.
const std::chrono::seconds SEARCH_RETRY_INTERVAL( 2 );

class ScreenCaptureArena6 : public CFreeFrameGLPlugin
{
public:
	ScreenCaptureArena6()
	{
		SetMinInputs( 0 );
		SetMaxInputs( 0 );

		//FFGL 1.6 names are limited to 16 bytes (without a terminator): keep them to 15 characters.
		SetParamInfo( PT_SOURCE, "Fuente", FF_TYPE_TEXT, DEFAULT_SOURCE );
		SetParamInfo( PT_KEEP_ASPECT, "Proporcion", FF_TYPE_BOOLEAN, true );
		SetParamInfo( PT_CURSOR, "Mostrar cursor", FF_TYPE_BOOLEAN, true );
		SetParamInfo( PT_CROP_LEFT, "Recorte izq", FF_TYPE_STANDARD, 0.0f );
		SetParamInfo( PT_CROP_RIGHT, "Recorte der", FF_TYPE_STANDARD, 0.0f );
		SetParamInfo( PT_CROP_TOP, "Recorte arriba", FF_TYPE_STANDARD, 0.0f );
		SetParamInfo( PT_CROP_BOTTOM, "Recorte abajo", FF_TYPE_STANDARD, 0.0f );

		Updater::Get().Start();
	}

	static FFResult __stdcall CreateInstance( CFreeFrameGLPlugin** instance )
	{
		*instance = new ScreenCaptureArena6();
		return *instance != nullptr ? FF_SUCCESS : FF_FAIL;
	}

	FFResult InitGL( const FFGLViewportStruct* vp ) override
	{
		static std::once_flag logVersionOnce;
		std::call_once( logVersionOnce, [] { LogToFile( "Captura Pantalla (Arena 6) v" PLUGIN_VERSION_STRING " cargado" ); } );

		if( !CaptureEngine::IsSupported() )
		{
			LogToFile( "La captura necesita macOS 12.3 o superior." );
			return FF_FAIL;
		}
		CaptureEngine::HasPermission();//Asks for Screen Recording the first time.

		viewportWidth  = vp->width;
		viewportHeight = vp->height;

		glGenTextures( 1, &texture );
		glBindTexture( GL_TEXTURE_2D, texture );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glBindTexture( GL_TEXTURE_2D, 0 );
		textureWidth  = 0;
		textureHeight = 0;
		hasFrame      = false;
		searchPending = true;
		return FF_SUCCESS;
	}

	FFResult DeInitGL() override
	{
		capture.Stop();
		if( texture != 0 )
			glDeleteTextures( 1, &texture );
		texture  = 0;
		hasFrame = false;
		return FF_SUCCESS;
	}

	unsigned int Resize( const FFGLViewportStruct* vp ) override
	{
		viewportWidth  = vp->width;
		viewportHeight = vp->height;
		return FF_SUCCESS;
	}

	FFResult ProcessOpenGL( ProcessOpenGLStruct* ) override
	{
		UpdateSource();

		capture.TakeFrame( [ this ]( const unsigned char* bgra, int width, int height ) {
			glBindTexture( GL_TEXTURE_2D, texture );
			if( width != textureWidth || height != textureHeight )
			{
				glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bgra );
				textureWidth  = width;
				textureHeight = height;
			}
			else
			{
				glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bgra );
			}
			glBindTexture( GL_TEXTURE_2D, 0 );
			hasFrame = true;
		} );

		if( !hasFrame )
			return FF_SUCCESS;//Leave the host's transparent frame untouched.

		//Crop, in texture coordinates. Row 0 of the capture is the top of the image.
		float u0 = crop[ 0 ] * MAX_CROP;
		float u1 = 1.0f - crop[ 1 ] * MAX_CROP;
		float v0 = crop[ 2 ] * MAX_CROP;
		float v1 = 1.0f - crop[ 3 ] * MAX_CROP;

		//Quad size: full output, or letterboxed to keep the image's proportions.
		float sx = 1.0f;
		float sy = 1.0f;
		if( keepAspect && viewportHeight > 0 && textureHeight > 0 )
		{
			float sourceAspect = ( textureWidth * ( u1 - u0 ) ) / ( textureHeight * ( v1 - v0 ) );
			float outputAspect = static_cast< float >( viewportWidth ) / static_cast< float >( viewportHeight );
			if( sourceAspect > outputAspect )
				sy = outputAspect / sourceAspect;
			else
				sx = sourceAspect / outputAspect;
		}

		glEnable( GL_TEXTURE_2D );
		glBindTexture( GL_TEXTURE_2D, texture );
		glColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
		glBegin( GL_QUADS );
		glTexCoord2f( u0, v1 );
		glVertex2f( -sx, -sy );
		glTexCoord2f( u1, v1 );
		glVertex2f( sx, -sy );
		glTexCoord2f( u1, v0 );
		glVertex2f( sx, sy );
		glTexCoord2f( u0, v0 );
		glVertex2f( -sx, sy );
		glEnd();
		glBindTexture( GL_TEXTURE_2D, 0 );
		glDisable( GL_TEXTURE_2D );
		return FF_SUCCESS;
	}

	FFResult SetTextParameter( unsigned int index, const char* value ) override
	{
		if( index != PT_SOURCE )
			return FF_FAIL;
		std::string text = value != nullptr ? value : "";
		if( text != sourceText )
		{
			sourceText    = text;
			searchPending = true;
		}
		return FF_SUCCESS;
	}

	char* GetTextParameter( unsigned int index ) override
	{
		static char empty[] = "";
		return index == PT_SOURCE ? const_cast< char* >( sourceText.c_str() ) : empty;
	}

	FFResult SetFloatParameter( unsigned int index, float value ) override
	{
		switch( index )
		{
		case PT_KEEP_ASPECT:
			keepAspect = value > 0.5f;
			break;
		case PT_CURSOR:
			showCursor = value > 0.5f;
			capture.SetCursorVisible( showCursor );
			break;
		case PT_CROP_LEFT:
		case PT_CROP_RIGHT:
		case PT_CROP_TOP:
		case PT_CROP_BOTTOM:
			crop[ index - PT_CROP_LEFT ] = std::max( 0.0f, std::min( value, 1.0f ) );
			break;
		default:
			return FF_FAIL;
		}
		return FF_SUCCESS;
	}

	float GetFloatParameter( unsigned int index ) override
	{
		switch( index )
		{
		case PT_KEEP_ASPECT:
			return keepAspect ? 1.0f : 0.0f;
		case PT_CURSOR:
			return showCursor ? 1.0f : 0.0f;
		case PT_CROP_LEFT:
		case PT_CROP_RIGHT:
		case PT_CROP_TOP:
		case PT_CROP_BOTTOM:
			return crop[ index - PT_CROP_LEFT ];
		default:
			return 0.0f;
		}
	}

private:
	// Finds what "Fuente" describes and (re)starts the capture. Also brings the capture back by
	// itself when the window is closed and opened again.
	void UpdateSource()
	{
		if( capture.WasClosed() )
		{
			LogToFile( "La fuente se cerro: " + current.label );
			capture.Stop();
			hasFrame      = false;
			current       = CaptureTarget();
			searchPending = true;
		}

		std::string error = capture.TakeError();
		if( !error.empty() )
		{
			if( error != lastError )
				LogToFile( error );//The retry can hit the same error every 2 seconds, log it once.
			lastError = error;
			current   = CaptureTarget();
		}

		auto now = std::chrono::steady_clock::now();
		bool retry = !capture.IsActive() && !sourceText.empty() && now - lastSearch >= SEARCH_RETRY_INTERVAL;
		if( !searchPending && !retry )
			return;
		searchPending = false;
		lastSearch    = now;

		if( sourceText.empty() )
		{
			capture.Stop();
			hasFrame = false;
			current  = CaptureTarget();
			return;
		}

		std::vector< CaptureTarget > targets = EnumerateCaptureTargets();
		int index                            = FindTarget( targets, sourceText );
		if( index < 0 )
		{
			if( lastMissing != sourceText )
			{
				lastMissing          = sourceText;
				std::string choices;
				for( size_t option = 1; option < targets.size(); ++option )
					choices += "\n    " + targets[ option ].label;
				LogToFile( "No se encontro '" + sourceText + "'. Fuentes disponibles:" + choices );
			}
			return;
		}
		lastMissing.clear();

		if( targets[ index ].SameAs( current ) && capture.IsActive() )
			return;
		current  = targets[ index ];
		hasFrame = false;
		LogToFile( "Fuente elegida: " + current.label );
		capture.Start( current, showCursor );
	}

	CaptureEngine capture;
	CaptureTarget current;
	std::string sourceText = DEFAULT_SOURCE;
	std::string lastMissing;
	std::string lastError;
	bool searchPending = true;
	std::chrono::steady_clock::time_point lastSearch;

	bool keepAspect = true;
	bool showCursor = true;
	float crop[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };//left, right, top, bottom

	GLuint texture         = 0;
	int textureWidth       = 0;
	int textureHeight      = 0;
	bool hasFrame          = false;
	unsigned int viewportWidth  = 0;
	unsigned int viewportHeight = 0;
};
}// namespace

static CFFGLPluginInfo PluginInfo(
	ScreenCaptureArena6::CreateInstance,                    // Create method
	"MJS6",                                                 // Plugin unique ID
	"Captura Pantalla",                                     // Plugin name (max 16 characters)
	1,                                                      // API major version number
	000,                                                    // API minor version number
	PLUGIN_VERSION_MAJOR,                                   // Plugin major version number
	PLUGIN_VERSION_MINOR,                                   // Plugin minor version number
	FF_SOURCE,                                              // Plugin type
	"Captura en vivo una pantalla o una ventana del Mac",   // Plugin description
	"Screen Capture para Resolume Arena 6 (ScreenCaptureKit)"// About
);

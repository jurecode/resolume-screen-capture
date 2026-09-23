#include "ScreenCapture.h"
#include "FileLog.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
using namespace ffglex;

enum ParamType : FFUInt32
{
	PT_SOURCE,
	PT_REFRESH,
	PT_SEARCH,
	PT_FIT,
	PT_CURSOR,
	PT_RESTORE_MINIMIZED,
	PT_CROP_LEFT,
	PT_CROP_RIGHT,
	PT_CROP_TOP,
	PT_CROP_BOTTOM,
	PT_UPDATE_ACTION,
	PT_UPDATE_LATER,
	PT_UPDATE_SKIP,
};

enum FitMode
{
	FIT_STRETCH,
	FIT_CONTAIN,
	FIT_COVER,
};

//Default dropdown entry: element 0 is "(ninguna)", element 1 the first monitor.
static const int DEFAULT_SOURCE = 1;
//Each crop slider goes up to half of the image.
static const float MAX_CROP = 0.5f;
static const std::chrono::seconds SEARCH_RETRY_INTERVAL( 2 );

static CFFGLPluginInfo PluginInfo(
	PluginFactory< ScreenCapture >,                                   // Create method
	"MJSC",                                                           // Plugin unique ID
	"Captura Pantalla",                                               // Plugin name (max 16 characters)
	2,                                                                // API major version number
	1,                                                                // API minor version number
	PLUGIN_VERSION_MAJOR,                                             // Plugin major version number
	PLUGIN_VERSION_MINOR,                                             // Plugin minor version number
	FF_SOURCE,                                                        // Plugin type
	"Captura en vivo una pantalla o una ventana de Windows",          // Plugin description
	"Screen Capture para Resolume Arena (Windows Graphics Capture)"   // About
);

static const char vertexShaderCode[] = R"(#version 410 core
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

static const char fragmentShaderCode[] = R"(#version 410 core
uniform sampler2D Capture;
uniform int HasFrame;
uniform vec4 CropRect; //x0, y0, x1, y1 inside the captured image, top-left origin
uniform vec2 Scale;    //How much of the output the image covers, >1 means letterboxed

in vec2 uv;

out vec4 fragColor;

void main()
{
	if( HasFrame == 0 )
	{
		fragColor = vec4( 0.0 );
		return;
	}

	vec2 p = ( uv - 0.5 ) * Scale + 0.5;
	if( p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 )
	{
		fragColor = vec4( 0.0 );//Transparent bars so the layer below shows through.
		return;
	}

	//Captured rows are stored top to bottom, OpenGL output goes bottom to top.
	vec2 src = mix( CropRect.xy, CropRect.zw, vec2( p.x, 1.0 - p.y ) );
	fragColor = vec4( texture( Capture, src ).rgb, 1.0 );
}
)";

ScreenCapture::ScreenCapture()
{
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	targets       = EnumerateCaptureTargets();
	selectedIndex = std::min( DEFAULT_SOURCE, static_cast< int >( targets.size() ) - 1 );
	selected      = targets[ selectedIndex ];

	SetOptionParamInfo( PT_SOURCE, "Fuente", static_cast< unsigned int >( targets.size() ), static_cast< float >( selectedIndex ) );
	for( size_t index = 0; index < targets.size(); ++index )
		SetParamElementInfo( PT_SOURCE, static_cast< unsigned int >( index ), targets[ index ].label.c_str(), static_cast< float >( index ) );

	SetParamInfof( PT_REFRESH, "Actualizar lista", FF_TYPE_EVENT );
	SetParamInfo( PT_SEARCH, "Buscar ventana", FF_TYPE_TEXT, "" );

	SetOptionParamInfo( PT_FIT, "Ajuste", 3, static_cast< float >( fitMode ) );
	SetParamElementInfo( PT_FIT, FIT_STRETCH, "Estirar", static_cast< float >( FIT_STRETCH ) );
	SetParamElementInfo( PT_FIT, FIT_CONTAIN, "Encajar", static_cast< float >( FIT_CONTAIN ) );
	SetParamElementInfo( PT_FIT, FIT_COVER, "Rellenar", static_cast< float >( FIT_COVER ) );

	SetParamInfo( PT_CURSOR, "Mostrar cursor", FF_TYPE_BOOLEAN, showCursor );
	SetParamInfo( PT_RESTORE_MINIMIZED, "Restaurar si se minimiza", FF_TYPE_BOOLEAN, restoreMinimized );
#ifdef __APPLE__
	SetParamVisibility( PT_RESTORE_MINIMIZED, false, false );//macOS keeps the last image instead.
#endif

	SetParamInfo( PT_CROP_LEFT, "Izquierda", FF_TYPE_STANDARD, 0.0f );
	SetParamInfo( PT_CROP_RIGHT, "Derecha", FF_TYPE_STANDARD, 0.0f );
	SetParamInfo( PT_CROP_TOP, "Arriba", FF_TYPE_STANDARD, 0.0f );
	SetParamInfo( PT_CROP_BOTTOM, "Abajo", FF_TYPE_STANDARD, 0.0f );
	for( unsigned int param : { PT_CROP_LEFT, PT_CROP_RIGHT, PT_CROP_TOP, PT_CROP_BOTTOM } )
		SetParamGroup( param, "Recorte" );

	Updater::Get().Start();
	SetParamInfof( PT_UPDATE_ACTION, "Actualizar plugin", FF_TYPE_EVENT );
	SetParamInfof( PT_UPDATE_LATER, "Mas tarde", FF_TYPE_EVENT );
	SetParamInfof( PT_UPDATE_SKIP, "Omitir version", FF_TYPE_EVENT );
	for( unsigned int param : { PT_UPDATE_ACTION, PT_UPDATE_LATER, PT_UPDATE_SKIP } )
		SetParamGroup( param, "Actualizacion" );
	SyncUpdateUi( false );
}

FFResult ScreenCapture::InitGL( const FFGLViewportStruct* vp )
{
	if( !CaptureEngine::IsSupported() )
	{
#ifdef __APPLE__
		Log( "La captura necesita macOS 12.3 o superior." );
#else
		Log( "Windows Graphics Capture no esta disponible. Se necesita Windows 10 (1903) o superior." );
#endif
		return FF_FAIL;
	}
	if( !shader.Compile( vertexShaderCode, fragmentShaderCode ) || !quad.Initialise() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	static std::once_flag logVersionOnce;
	std::call_once( logVersionOnce, [] { LogToFile( "Captura Pantalla v" PLUGIN_VERSION_STRING " cargado" ); } );
	CaptureEngine::HasPermission();//On macOS this asks for Screen Recording the first time.

	glGenTextures( 1, &texture );
	{
		Scoped2DTextureBinding textureBinding( texture );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	}
	textureWidth    = 0;
	textureHeight   = 0;
	hasFrame        = false;
	restartRequired = true;

	//Use base-class init as success result so that it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

FFResult ScreenCapture::DeInitGL()
{
	capture.Stop();
	shader.FreeGLResources();
	quad.Release();
	if( texture != 0 )
		glDeleteTextures( 1, &texture );
	texture  = 0;
	hasFrame = false;
	return FF_SUCCESS;
}

FFResult ScreenCapture::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	SyncUpdateUi( true );

	if( restartRequired )
	{
		restartRequired = false;
		StartSelectedCapture();
	}

	if( capture.WasClosed() )
	{
		Log( "La fuente se cerro: " + selected.label );
		capture.Stop();
		hasFrame = false;
	}

	std::string error = capture.TakeError();
	if( !error.empty() && error != lastCaptureError )
		Log( error );//The search retry can hit the same error every 2 seconds, log it once.
	if( !error.empty() )
		lastCaptureError = error;

	//"Buscar ventana" keeps looking, so the capture comes back by itself when the
	//presentation / browser window is opened again.
	if( !capture.IsActive() && !searchText.empty() )
	{
		auto now = std::chrono::steady_clock::now();
		if( now - lastSearchRetry >= SEARCH_RETRY_INTERVAL )
		{
			lastSearchRetry = now;
			RefreshTargets();
			if( ApplySearch( true ) )
				StartSelectedCapture();
		}
	}

	capture.TakeFrame( [ this ]( const unsigned char* bgra, int width, int height ) {
		UploadFrame( bgra, width, height );
	} );

	float x0 = crop[ 0 ] * MAX_CROP;
	float x1 = 1.0f - crop[ 1 ] * MAX_CROP;
	float y0 = crop[ 2 ] * MAX_CROP;
	float y1 = 1.0f - crop[ 3 ] * MAX_CROP;

	float scaleX = 1.0f;
	float scaleY = 1.0f;
	if( hasFrame && fitMode != FIT_STRETCH && currentViewport.height > 0 )
	{
		float sourceAspect = ( textureWidth * ( x1 - x0 ) ) / ( textureHeight * ( y1 - y0 ) );
		float outputAspect = static_cast< float >( currentViewport.width ) / static_cast< float >( currentViewport.height );
		bool sourceIsWider = sourceAspect > outputAspect;
		//Encajar: scale > 1 on the short axis adds bars. Rellenar: scale < 1 on the long axis crops.
		if( ( fitMode == FIT_CONTAIN ) == sourceIsWider )
			scaleY = sourceAspect / outputAspect;
		else
			scaleX = outputAspect / sourceAspect;
	}

	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	ScopedSamplerActivation activateSampler( 0 );
	Scoped2DTextureBinding textureBinding( texture );
	shader.Set( "Capture", 0 );
	shader.Set( "HasFrame", hasFrame ? 1 : 0 );
	shader.Set( "CropRect", x0, y0, x1, y1 );
	shader.Set( "Scale", scaleX, scaleY );
	quad.Draw();

	return FF_SUCCESS;
}

void ScreenCapture::UploadFrame( const unsigned char* bgra, int width, int height )
{
	Scoped2DTextureBinding textureBinding( texture );
	if( width != textureWidth || height != textureHeight )
	{
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr );
		textureWidth  = width;
		textureHeight = height;
	}

	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, bgra );
	hasFrame = true;
}

void ScreenCapture::StartSelectedCapture()
{
	//Both calls return immediately; the capture thread does the actual work.
	hasFrame = false;
	if( selected.kind == CaptureTarget::Kind::None )
	{
		LogToFile( "Fuente: (ninguna)" );
		capture.Stop();
		return;
	}
	LogToFile( "Fuente elegida: " + selected.label );
	capture.Start( selected, showCursor );
}

void ScreenCapture::RefreshTargets()
{
	std::vector< CaptureTarget > fresh = EnumerateCaptureTargets();

	bool unchanged = fresh.size() == targets.size();
	for( size_t index = 0; unchanged && index < fresh.size(); ++index )
		unchanged = fresh[ index ].SameAs( targets[ index ] ) && fresh[ index ].label == targets[ index ].label;
	if( unchanged )
		return;

	targets = std::move( fresh );

	std::vector< std::string > names;
	std::vector< float > values;
	for( size_t index = 0; index < targets.size(); ++index )
	{
		names.push_back( targets[ index ].label );
		values.push_back( static_cast< float >( index ) );
	}
	SetParamElements( PT_SOURCE, names, values, true );

	//Keep pointing at the same monitor/window, which probably moved to another position.
	int newIndex = 0;
	for( size_t index = 0; index < targets.size(); ++index )
	{
		if( targets[ index ].SameAs( selected ) )
			newIndex = static_cast< int >( index );
	}
	if( newIndex == 0 && selected.kind != CaptureTarget::Kind::None )
		restartRequired = true;//It's gone: stop showing it.
	Select( newIndex, true );
}

void ScreenCapture::Select( int index, bool notifyHost )
{
	index = std::max( 0, std::min( index, static_cast< int >( targets.size() ) - 1 ) );
	if( !targets[ index ].SameAs( selected ) )
		restartRequired = true;

	selectedIndex = index;
	selected      = targets[ index ];
	if( notifyHost )
		RaiseParamEvent( PT_SOURCE, FF_EVENT_FLAG_VALUE );
}

bool ScreenCapture::ApplySearch( bool forceRestart )
{
	int index = FindTarget( targets, searchText );
	if( index < 0 )
		return false;
	Select( index, true );
	if( forceRestart )
		restartRequired = true;
	return true;
}

void ScreenCapture::SyncUpdateUi( bool raiseEvents )
{
	Updater::Status status = Updater::Get().GetStatus();
	if( status.revision == shownUpdateRevision )
		return;
	shownUpdateRevision = status.revision;

	//Every clip shares the same updater, only the first one to notice a change logs it.
	static std::atomic< uint32_t > loggedRevision{ 0 };
	uint32_t previous = loggedRevision.load();
	bool shouldLog    = raiseEvents && previous != status.revision && loggedRevision.compare_exchange_strong( previous, status.revision );

	std::string action;
	bool offerChoices = false;
	switch( status.state )
	{
	case Updater::State::Disabled:
		for( unsigned int param : { PT_UPDATE_ACTION, PT_UPDATE_LATER, PT_UPDATE_SKIP } )
			SetParamVisibility( param, false, raiseEvents );
		return;
	case Updater::State::Checking:
		action = "Buscando actualizaciones...";
		break;
	case Updater::State::UpToDate:
		action = "v" + status.currentVersion + " al dia (buscar ahora)";
		break;
	case Updater::State::Available:
		action       = "Instalar v" + status.latestVersion;
		offerChoices = true;
		if( shouldLog )
			Log( "Nueva version v" + status.latestVersion + " disponible. " + status.notes );
		break;
	case Updater::State::Postponed:
		action = "Instalar v" + status.latestVersion + " (pospuesta)";
		break;
	case Updater::State::Downloading:
		action = "Descargando v" + status.latestVersion + "...";
		break;
	case Updater::State::Installed:
		action = "v" + status.latestVersion + " lista: reinicia Arena";
		if( shouldLog )
			Log( "v" + status.latestVersion + " instalada. Reinicia Resolume Arena para usarla." );
		break;
	case Updater::State::Failed:
		action = "Error al actualizar (reintentar)";
		if( shouldLog )
			Log( "Actualizacion: " + status.error );
		break;
	}

	SetParamVisibility( PT_UPDATE_ACTION, true, raiseEvents );
	SetParamDisplayName( PT_UPDATE_ACTION, action, raiseEvents );
	SetParamVisibility( PT_UPDATE_LATER, offerChoices, raiseEvents );
	SetParamVisibility( PT_UPDATE_SKIP, offerChoices, raiseEvents );
}

void ScreenCapture::Log( const std::string& message )
{
	FFGLLog::LogToHost( ( "[Captura Pantalla] " + message ).c_str() );
	LogToFile( message );
}

FFResult ScreenCapture::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case PT_SOURCE:
		Select( static_cast< int >( std::lround( value ) ), false );
		break;
	case PT_REFRESH:
		if( value != 0.0f )
		{
			RefreshTargets();
			ApplySearch( false );
		}
		break;
	case PT_UPDATE_ACTION:
		if( value != 0.0f )
			Updater::Get().PressAction();
		break;
	case PT_UPDATE_LATER:
		if( value != 0.0f )
			Updater::Get().RemindLater();
		break;
	case PT_UPDATE_SKIP:
		if( value != 0.0f )
			Updater::Get().SkipVersion();
		break;
	case PT_FIT:
		fitMode = std::max( 0, std::min( static_cast< int >( std::lround( value ) ), 2 ) );
		break;
	case PT_CURSOR:
		showCursor = value > 0.5f;
		capture.SetCursorVisible( showCursor );
		break;
	case PT_RESTORE_MINIMIZED:
		restoreMinimized = value > 0.5f;
		capture.SetRestoreMinimized( restoreMinimized );
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

FFResult ScreenCapture::SetTextParameter( unsigned int index, const char* value )
{
	if( index != PT_SEARCH )
		return FF_FAIL;

	searchText = value != nullptr ? value : "";
	if( !searchText.empty() )
	{
		RefreshTargets();
		ApplySearch( false );
	}
	return FF_SUCCESS;
}

float ScreenCapture::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PT_SOURCE:
		return static_cast< float >( selectedIndex );
	case PT_FIT:
		return static_cast< float >( fitMode );
	case PT_CURSOR:
		return showCursor ? 1.0f : 0.0f;
	case PT_RESTORE_MINIMIZED:
		return restoreMinimized ? 1.0f : 0.0f;
	case PT_CROP_LEFT:
	case PT_CROP_RIGHT:
	case PT_CROP_TOP:
	case PT_CROP_BOTTOM:
		return crop[ index - PT_CROP_LEFT ];
	default:
		return 0.0f;
	}
}

char* ScreenCapture::GetTextParameter( unsigned int index )
{
	static char empty[] = "";
	if( index == PT_SEARCH )
		return const_cast< char* >( searchText.c_str() );
	return empty;
}

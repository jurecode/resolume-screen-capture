#include "Zocalo.h"
#include "../FileLog.h"
#include "../Updater.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
using namespace ffglex;

enum ParamType : FFUInt32
{
	PT_PHOTO,
	PT_NAME,
	PT_SUBTITLE,
	PT_POSITION,
	PT_ENTER,
	PT_EXIT,
	PT_AUTO_ENTER,
	PT_AUTO_EXIT,
	PT_SIZE,
	PT_SPEED,
	PT_ROUND_PHOTO,
	PT_BAR_HUE,
	PT_BAR_SAT,
	PT_BAR_BRI,
	PT_BAR_ALPHA,
	PT_ACCENT_HUE,
	PT_ACCENT_SAT,
	PT_ACCENT_BRI,
	PT_ACCENT_ALPHA,
	PT_UPDATE_ACTION,
	PT_UPDATE_LATER,
	PT_UPDATE_SKIP,
};

enum Position
{
	POSITION_BOTTOM_LEFT,
	POSITION_BOTTOM_RIGHT,
	POSITION_BOTTOM_CENTER,
	POSITION_TOP_LEFT,
	POSITION_TOP_RIGHT,
	POSITION_COUNT,
};

namespace
{
// Design, in "design pixels" of a 1080p output at Tamano 50%.
const float RADIUS      = 92;
const float RING        = 6;
const float BAR_HEIGHT  = 128;
const float BAR_RADIUS  = 16;
const float STRIPE      = 7;
const float PAD_INNER   = 30; //Photo edge to text.
const float PAD_OUTER   = 46; //Text to the bar's far end.
const float MARGIN_X    = 80;
const float MARGIN_Y    = 70;
const float NAME_FONT   = 54;
const float SUB_FONT    = 34;
const float TEXT_SLIDE  = 28; //How far texts travel while fading in.

// Texts and photos are drawn big once and scaled down on the GPU.
const int NAME_RENDER     = 120;
const int SUB_RENDER      = 80;
const int INITIALS_RENDER = 140;
const int PHOTO_MAX_SIDE  = 1024;

// Timeline, in seconds at Velocidad 50%.
const float ENTER_END   = 1.25f;
const float EXIT_END    = 0.85f;
const float MOVE_RETURN = 1.4f; //Moving: the bar starts unfolding again at the new corner.
const float MOVE_END    = 2.35f;
const float REACTIVATE_GAP = 0.4f;//Not drawn for this long: the clip was triggered again.

int Margin( int fontPixels )
{
	return fontPixels / 8 + 2;//Same margin drawing::RenderText leaves around the glyphs.
}

float Seg( float t, float start, float end )
{
	return std::max( 0.0f, std::min( 1.0f, ( t - start ) / ( end - start ) ) );
}
float EaseOutCubic( float x )
{
	return 1.0f - std::pow( 1.0f - x, 3.0f );
}
float EaseInCubic( float x )
{
	return x * x * x;
}
float EaseInOutCubic( float x )
{
	return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow( -2.0f * x + 2.0f, 3.0f ) / 2.0f;
}
float EaseOutBack( float x )
{
	//Overshoots a little before settling: the photo "pops" in.
	const float c1 = 1.70158f, c3 = c1 + 1.0f;
	return 1.0f + c3 * std::pow( x - 1.0f, 3.0f ) + c1 * std::pow( x - 1.0f, 2.0f );
}

float Luminance( float r, float g, float b )
{
	return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// First letter of the first and last word, e.g. "Matías Jure" -> "MJ". UTF-8 aware.
std::string Initials( const std::string& name )
{
	std::vector< std::string > letters;
	bool atWordStart = true;
	for( size_t index = 0; index < name.size(); )
	{
		unsigned char byte = static_cast< unsigned char >( name[ index ] );
		size_t length      = byte < 0x80 ? 1 : byte < 0xE0 ? 2 : byte < 0xF0 ? 3 : 4;
		if( byte == ' ' )
			atWordStart = true;
		else if( atWordStart )
		{
			std::string letter = name.substr( index, length );
			if( length == 1 )
				letter[ 0 ] = static_cast< char >( toupper( byte ) );
			letters.push_back( letter );
			atWordStart = false;
		}
		index += length;
	}
	if( letters.empty() )
		return "";
	return letters.size() == 1 ? letters[ 0 ] : letters.front() + letters.back();
}
}// namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Zocalo >,                                        // Create method
	"MJZC",                                                         // Plugin unique ID
	"Zocalo",                                                       // Plugin name (max 16 characters)
	2,                                                              // API major version number
	1,                                                              // API minor version number
	PLUGIN_VERSION_MAJOR,                                           // Plugin major version number
	PLUGIN_VERSION_MINOR,                                           // Plugin minor version number
	FF_SOURCE,                                                      // Plugin type
	"Presentacion animada con foto, nombre y subtitulo (lower third)",// Plugin description
	"Zocalo para Resolume Arena"                                    // About
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
uniform vec2 Resolution;

uniform vec4 Bar;          //x0, y0, x1, y1 in pixels from the top-left
uniform float BarRadius;
uniform float Stripe;
uniform vec4 BarColor;     //Straight alpha
uniform vec3 Accent;
uniform vec3 TextColor;

uniform sampler2D NameTexture;
uniform vec4 NameRect;
uniform float NameAlpha;
uniform sampler2D SubtitleTexture;
uniform vec4 SubtitleRect;
uniform float SubtitleAlpha;

uniform vec2 PhotoCenter;
uniform float PhotoRadius;
uniform float PhotoAlpha;
uniform float Ring;
uniform int RoundPhoto;
uniform int HasPhoto;
uniform sampler2D PhotoTexture;
uniform vec2 PhotoSize;
uniform sampler2D InitialsTexture;
uniform vec2 InitialsSize;
uniform vec3 InitialsColor;

in vec2 uv;

out vec4 fragColor;

float RoundedBox( vec2 p, vec2 center, vec2 halfSize, float radius )
{
	vec2 q = abs( p - center ) - halfSize + radius;
	return length( max( q, 0.0 ) ) + min( max( q.x, q.y ), 0.0 ) - radius;
}

//Signed distance -> how much of this pixel is inside, for smooth one-pixel edges.
float Coverage( float distance )
{
	return clamp( 0.5 - distance, 0.0, 1.0 );
}

//Layers are premultiplied while compositing.
vec4 Over( vec4 below, vec4 above )
{
	return above + below * ( 1.0 - above.a );
}

float TextAt( sampler2D glyphs, vec4 rect, vec2 p )
{
	vec2 t = ( p - rect.xy ) / ( rect.zw - rect.xy );
	if( t.x < 0.0 || t.y < 0.0 || t.x > 1.0 || t.y > 1.0 )
		return 0.0;
	//Pick the mipmap level from the drawn size: automatic selection doesn't work inside the
	//branch above and would sample the wrong level (faded text, lines along the edges).
	float level = max( 0.0, log2( float( textureSize( glyphs, 0 ).x ) / max( rect.z - rect.x, 1.0 ) ) );
	return textureLod( glyphs, t, level ).a;
}

void main()
{
	vec2 p = vec2( uv.x, 1.0 - uv.y ) * Resolution;
	vec4 color = vec4( 0.0 );

	if( Bar.z > Bar.x + 0.5 )
	{
		float inside = Coverage( RoundedBox( p, ( Bar.xy + Bar.zw ) * 0.5, ( Bar.zw - Bar.xy ) * 0.5, BarRadius ) );
		bool stripe  = p.y > Bar.w - Stripe;
		float alpha  = stripe ? 1.0 : BarColor.a;
		vec3 fill    = stripe ? Accent : BarColor.rgb;
		color = Over( color, vec4( fill * alpha, alpha ) * inside );

		//Texts only show inside the bar, so they appear as it unfolds.
		float name = TextAt( NameTexture, NameRect, p ) * NameAlpha * inside;
		color = Over( color, vec4( TextColor * name, name ) );
		float subtitle = TextAt( SubtitleTexture, SubtitleRect, p ) * SubtitleAlpha * inside * 0.8;
		color = Over( color, vec4( TextColor * subtitle, subtitle ) );
	}

	if( PhotoRadius > 0.5 && PhotoAlpha > 0.0 )
	{
		float edge  = RoundPhoto == 1 ? length( p - PhotoCenter ) - PhotoRadius
		                              : RoundedBox( p, PhotoCenter, vec2( PhotoRadius ), PhotoRadius * 0.16 );
		float outer = Coverage( edge );
		float inner = Coverage( edge + Ring );
		vec2 local  = ( p - PhotoCenter ) / ( 2.0 * max( PhotoRadius - Ring, 1.0 ) ) + 0.5;
		vec3 picture;
		if( HasPhoto == 1 )
		{
			//Fill the shape, cropping the photo's longer side.
			float aspect = PhotoSize.x / PhotoSize.y;
			vec2 t = local;
			if( aspect > 1.0 )
				t.x = 0.5 + ( t.x - 0.5 ) / aspect;
			else
				t.y = 0.5 + ( t.y - 0.5 ) * aspect;
			picture = texture( PhotoTexture, t ).rgb;
		}
		else
		{
			vec4 rect = vec4( PhotoCenter - InitialsSize * 0.5, PhotoCenter + InitialsSize * 0.5 );
			picture = mix( Accent, InitialsColor, TextAt( InitialsTexture, rect, p ) );
		}
		vec4 photo = vec4( picture, 1.0 ) * inner + vec4( Accent, 1.0 ) * ( outer - inner );
		color = Over( color, photo * PhotoAlpha );
	}

	fragColor = color.a > 0.0 ? vec4( color.rgb / color.a, color.a ) : vec4( 0.0 );
}
)";

Zocalo::Zocalo()
{
	SetMinInputs( 0 );
	SetMaxInputs( 0 );

	SetFileParamInfo( PT_PHOTO, "Foto", { "jpg", "jpeg", "png", "bmp", "gif", "tif", "tiff", "heic", "webp" }, "" );
	SetParamInfo( PT_NAME, "Nombre", FF_TYPE_TEXT, name.c_str() );
	SetParamInfo( PT_SUBTITLE, "Subtitulo", FF_TYPE_TEXT, subtitle.c_str() );

	SetOptionParamInfo( PT_POSITION, "Posicion", POSITION_COUNT, static_cast< float >( position ) );
	SetParamElementInfo( PT_POSITION, POSITION_BOTTOM_LEFT, "Abajo izquierda", POSITION_BOTTOM_LEFT );
	SetParamElementInfo( PT_POSITION, POSITION_BOTTOM_RIGHT, "Abajo derecha", POSITION_BOTTOM_RIGHT );
	SetParamElementInfo( PT_POSITION, POSITION_BOTTOM_CENTER, "Abajo centro", POSITION_BOTTOM_CENTER );
	SetParamElementInfo( PT_POSITION, POSITION_TOP_LEFT, "Arriba izquierda", POSITION_TOP_LEFT );
	SetParamElementInfo( PT_POSITION, POSITION_TOP_RIGHT, "Arriba derecha", POSITION_TOP_RIGHT );

	SetParamInfof( PT_ENTER, "Entrar", FF_TYPE_EVENT );
	SetParamInfof( PT_EXIT, "Salir", FF_TYPE_EVENT );
	SetParamInfo( PT_AUTO_ENTER, "Entrar al activar", FF_TYPE_BOOLEAN, autoEnter );
	SetParamInfo( PT_AUTO_EXIT, "Salir despues", FF_TYPE_STANDARD, autoExitSeconds );
	SetParamRange( PT_AUTO_EXIT, 0.0f, 60.0f );
	SetParamInfo( PT_SIZE, "Tamano", FF_TYPE_STANDARD, size );
	SetParamInfo( PT_SPEED, "Velocidad", FF_TYPE_STANDARD, speed );
	SetParamInfo( PT_ROUND_PHOTO, "Foto redonda", FF_TYPE_BOOLEAN, roundPhoto );
	for( unsigned int param : { PT_AUTO_ENTER, PT_AUTO_EXIT, PT_SIZE, PT_SPEED, PT_ROUND_PHOTO } )
		SetParamGroup( param, "Estilo" );

	SetParamInfo( PT_BAR_HUE, "Color barra", FF_TYPE_HUE, barHsba[ 0 ] );
	SetParamInfo( PT_BAR_SAT, "Color barra_Sat", FF_TYPE_SATURATION, barHsba[ 1 ] );
	SetParamInfo( PT_BAR_BRI, "Color barra_Bri", FF_TYPE_BRIGHTNESS, barHsba[ 2 ] );
	SetParamInfo( PT_BAR_ALPHA, "Color barra_Alpha", FF_TYPE_ALPHA, barHsba[ 3 ] );
	SetParamInfo( PT_ACCENT_HUE, "Color acento", FF_TYPE_HUE, accentHsba[ 0 ] );
	SetParamInfo( PT_ACCENT_SAT, "Color acento_Sat", FF_TYPE_SATURATION, accentHsba[ 1 ] );
	SetParamInfo( PT_ACCENT_BRI, "Color acento_Bri", FF_TYPE_BRIGHTNESS, accentHsba[ 2 ] );
	SetParamInfo( PT_ACCENT_ALPHA, "Color acento_Alpha", FF_TYPE_ALPHA, accentHsba[ 3 ] );

	Updater::Get().Start();
	SetParamInfof( PT_UPDATE_ACTION, "Actualizar plugin", FF_TYPE_EVENT );
	SetParamInfof( PT_UPDATE_LATER, "Mas tarde", FF_TYPE_EVENT );
	SetParamInfof( PT_UPDATE_SKIP, "Omitir version", FF_TYPE_EVENT );
	for( unsigned int param : { PT_UPDATE_ACTION, PT_UPDATE_LATER, PT_UPDATE_SKIP } )
		SetParamGroup( param, "Actualizacion" );
	SyncUpdateUi( false );
}

FFResult Zocalo::InitGL( const FFGLViewportStruct* vp )
{
	static std::once_flag logVersionOnce;
	std::call_once( logVersionOnce, [] { LogToFile( "Zocalo v" PLUGIN_VERSION_STRING " cargado" ); } );

	if( !shader.Compile( vertexShaderCode, fragmentShaderCode ) || !quad.Initialise() )
	{
		DeInitGL();
		return FF_FAIL;
	}
	textsDirty  = true;
	photoDirty  = true;
	hasRendered = false;
	return CFFGLPlugin::InitGL( vp );
}

FFResult Zocalo::DeInitGL()
{
	shader.FreeGLResources();
	quad.Release();
	for( GLuint* texture : { &nameTexture, &subtitleTexture, &initialsTexture, &photoTexture } )
	{
		if( *texture != 0 )
			glDeleteTextures( 1, texture );
		*texture = 0;
	}
	return FF_SUCCESS;
}

float Zocalo::Unit() const
{
	return currentViewport.height / 1080.0f * ( 0.5f + size );
}

float Zocalo::BarLength() const
{
	float u         = Unit();
	float nameWidth = nameBitmap.Empty() ? 0.0f : ( nameBitmap.width - 2 * Margin( NAME_RENDER ) ) * NAME_FONT * u / NAME_RENDER;
	float subWidth  = subtitleBitmap.Empty() ? 0.0f : ( subtitleBitmap.width - 2 * Margin( SUB_RENDER ) ) * SUB_FONT * u / SUB_RENDER;
	return ( RADIUS + PAD_INNER + PAD_OUTER ) * u + std::max( nameWidth, subWidth );
}

Zocalo::Layout Zocalo::LayoutFor( int corner ) const
{
	float u = Unit();
	float width = static_cast< float >( currentViewport.width ), height = static_cast< float >( currentViewport.height );
	float radius = RADIUS * u, marginX = MARGIN_X * u, marginY = MARGIN_Y * u;
	switch( corner )
	{
	case POSITION_BOTTOM_RIGHT:
		return { width - marginX - radius, height - marginY - radius, -1.0f };
	case POSITION_BOTTOM_CENTER:
		//Centre the whole group: from the photo's left edge to the bar's end.
		return { width * 0.5f - ( barLengthShown - radius ) * 0.5f, height - marginY - radius, 1.0f };
	case POSITION_TOP_LEFT:
		return { marginX + radius, marginY + radius, 1.0f };
	case POSITION_TOP_RIGHT:
		return { width - marginX - radius, marginY + radius, -1.0f };
	case POSITION_BOTTOM_LEFT:
	default:
		return { marginX + radius, height - marginY - radius, 1.0f };
	}
}

void Zocalo::StartPhase( Phase next )
{
	phase        = next;
	phaseTime    = 0;
	shownSeconds = 0;
}

void Zocalo::Advance()
{
	Clock::time_point now = Clock::now();
	float elapsed = hasRendered ? std::chrono::duration< float >( now - lastFrame ).count() : 0.0f;
	bool reactivated = !hasRendered || elapsed > REACTIVATE_GAP;
	lastFrame   = now;
	hasRendered = true;
	if( reactivated )
	{
		elapsed = 0;
		if( autoEnter )
			StartPhase( Phase::Entering );
	}
	elapsed = std::min( elapsed, 0.1f );
	phaseTime += elapsed * ( 0.4f + speed * 1.2f );

	switch( phase )
	{
	case Phase::Entering:
		if( phaseTime >= ENTER_END )
			StartPhase( Phase::Shown );
		break;
	case Phase::Moving:
		if( phaseTime >= MOVE_END )
			StartPhase( Phase::Shown );
		break;
	case Phase::Exiting:
		if( phaseTime >= EXIT_END )
			StartPhase( Phase::Hidden );
		break;
	case Phase::Shown:
		shownSeconds += elapsed;
		if( autoExitSeconds > 0.0f && shownSeconds >= autoExitSeconds )
			StartPhase( Phase::Exiting );
		break;
	case Phase::Hidden:
		break;
	}

	//When the name changes while visible, the bar stretches to fit instead of jumping.
	float target   = BarLength();
	barLengthShown = barLengthShown <= 0.0f ? target : barLengthShown + ( target - barLengthShown ) * std::min( 1.0f, elapsed * 10.0f );
}

Zocalo::Pose Zocalo::CurrentPose() const
{
	Pose pose;
	float t = phaseTime;
	switch( phase )
	{
	case Phase::Hidden:
		break;
	case Phase::Shown:
		pose.photo = pose.bar = pose.name = pose.subtitle = 1;
		break;
	case Phase::Entering:
		pose.photo    = EaseOutBack( Seg( t, 0.0f, 0.5f ) );
		pose.bar      = EaseOutCubic( Seg( t, 0.3f, 0.9f ) );
		pose.name     = EaseOutCubic( Seg( t, 0.6f, 1.1f ) );
		pose.subtitle = EaseOutCubic( Seg( t, 0.75f, 1.25f ) );
		break;
	case Phase::Exiting:
		pose.subtitle = 1 - EaseInCubic( Seg( t, 0.0f, 0.25f ) );
		pose.name     = 1 - EaseInCubic( Seg( t, 0.05f, 0.3f ) );
		pose.bar      = 1 - EaseInCubic( Seg( t, 0.2f, 0.6f ) );
		pose.photo    = 1 - EaseInCubic( Seg( t, 0.5f, EXIT_END ) );
		break;
	case Phase::Moving:
		//Fold the bar into the photo, travel, unfold at the new corner.
		pose.photo  = 1;
		pose.travel = EaseInOutCubic( Seg( t, 0.6f, MOVE_RETURN ) );
		if( t < MOVE_RETURN )
		{
			pose.newCorner = false;
			pose.subtitle  = 1 - EaseInCubic( Seg( t, 0.0f, 0.25f ) );
			pose.name      = 1 - EaseInCubic( Seg( t, 0.05f, 0.3f ) );
			pose.bar       = 1 - EaseInCubic( Seg( t, 0.2f, 0.6f ) );
		}
		else
		{
			pose.bar      = EaseOutCubic( Seg( t, MOVE_RETURN, MOVE_RETURN + 0.6f ) );
			pose.name     = EaseOutCubic( Seg( t, MOVE_RETURN + 0.3f, MOVE_RETURN + 0.8f ) );
			pose.subtitle = EaseOutCubic( Seg( t, MOVE_RETURN + 0.45f, MOVE_END ) );
		}
		break;
	}
	return pose;
}

void Zocalo::UploadBitmap( GLuint& texture, const Bitmap& bitmap )
{
	static const unsigned char transparent[ 4 ] = { 0, 0, 0, 0 };
	if( texture == 0 )
		glGenTextures( 1, &texture );
	Scoped2DTextureBinding textureBinding( texture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	if( bitmap.Empty() )
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, transparent );
	else
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, bitmap.width, bitmap.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, bitmap.rgba.data() );
	glGenerateMipmap( GL_TEXTURE_2D );
}

void Zocalo::RefreshTextures()
{
	if( textsDirty )
	{
		textsDirty     = false;
		nameBitmap     = drawing::RenderText( name, NAME_RENDER, true );
		subtitleBitmap = drawing::RenderText( subtitle, SUB_RENDER, false );
		initialsBitmap = drawing::RenderText( Initials( name ), INITIALS_RENDER, true );
		UploadBitmap( nameTexture, nameBitmap );
		UploadBitmap( subtitleTexture, subtitleBitmap );
		UploadBitmap( initialsTexture, initialsBitmap );
	}

	if( photoLoad.valid() && photoLoad.wait_for( std::chrono::seconds( 0 ) ) == std::future_status::ready )
	{
		PhotoResult result = photoLoad.get();
		if( !result.error.empty() )
			Log( result.error );
		photoBitmap = std::move( result.bitmap );
		photoDirty  = true;
	}
	if( photoDirty )
	{
		photoDirty = false;
		hasPhoto   = !photoBitmap.Empty();
		UploadBitmap( photoTexture, photoBitmap );
	}
}

FFResult Zocalo::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	SyncUpdateUi( true );
	RefreshTextures();
	Advance();

	Pose pose    = CurrentPose();
	float u      = Unit();
	Layout here  = LayoutFor( position );
	float centerX = here.centerX, centerY = here.centerY, direction = here.direction;
	if( phase == Phase::Moving )
	{
		centerX = movingFromX + ( here.centerX - movingFromX ) * pose.travel;
		centerY = movingFromY + ( here.centerY - movingFromY ) * pose.travel;
		if( !pose.newCorner )
			direction = LayoutFor( movingFrom ).direction;
	}

	//Bar: from the photo's centre (hidden behind it) outwards.
	float barLength = barLengthShown * pose.bar;
	float barTop    = centerY - BAR_HEIGHT * u * 0.5f;
	float barBottom = barTop + BAR_HEIGHT * u;
	float barLeft   = direction > 0 ? centerX : centerX - barLength;
	float barRight  = direction > 0 ? centerX + barLength : centerX;

	//Texts: name above subtitle, vertically centred above the accent stripe.
	float nameScale    = NAME_FONT * u / NAME_RENDER;
	float subScale     = SUB_FONT * u / SUB_RENDER;
	float nameMargin   = Margin( NAME_RENDER ) * nameScale;
	float subMargin    = Margin( SUB_RENDER ) * subScale;
	float nameHeight   = nameBitmap.Empty() ? 0.0f : nameBitmap.height * nameScale - 2 * nameMargin;
	float subHeight    = subtitleBitmap.Empty() ? 0.0f : subtitleBitmap.height * subScale - 2 * subMargin;
	float blockTop     = barTop + ( BAR_HEIGHT * u - STRIPE * u - nameHeight - subHeight ) * 0.5f;
	float textStart    = centerX + direction * ( RADIUS + PAD_INNER ) * u;
	auto textRect = [ & ]( const Bitmap& bitmap, float scale, float margin, float top, float progress, float rect[ 4 ] ) {
		float width = bitmap.width * scale, height = bitmap.height * scale;
		float slide = -direction * ( 1.0f - progress ) * TEXT_SLIDE * u;
		float left  = direction > 0 ? textStart - margin + slide : textStart + margin - width + slide;
		rect[ 0 ] = left;
		rect[ 1 ] = top - margin;
		rect[ 2 ] = left + width;
		rect[ 3 ] = top - margin + height;
	};
	float nameRect[ 4 ], subRect[ 4 ];
	textRect( nameBitmap, nameScale, nameMargin, blockTop, pose.name, nameRect );
	textRect( subtitleBitmap, subScale, subMargin, blockTop + nameHeight, pose.subtitle, subRect );

	//Colours.
	float bar[ 4 ], accent[ 3 ];
	HSVtoRGB( barHsba[ 0 ] >= 1.0f ? 0.0f : barHsba[ 0 ], barHsba[ 1 ], barHsba[ 2 ], bar[ 0 ], bar[ 1 ], bar[ 2 ] );
	bar[ 3 ] = barHsba[ 3 ];
	HSVtoRGB( accentHsba[ 0 ] >= 1.0f ? 0.0f : accentHsba[ 0 ], accentHsba[ 1 ], accentHsba[ 2 ], accent[ 0 ], accent[ 1 ], accent[ 2 ] );
	float text     = Luminance( bar[ 0 ], bar[ 1 ], bar[ 2 ] ) > 0.55f ? 0.07f : 1.0f;
	bool darkOnAccent = Luminance( accent[ 0 ], accent[ 1 ], accent[ 2 ] ) > 0.55f;

	float photoRadius   = RADIUS * u * std::max( 0.0f, pose.photo );
	float initialsScale = RADIUS * u * 1.0f / INITIALS_RENDER;

	ScopedShaderBinding shaderBinding( shader.GetGLID() );
	shader.Set( "Resolution", static_cast< float >( currentViewport.width ), static_cast< float >( currentViewport.height ) );
	shader.Set( "Bar", barLeft, barTop, barRight, barBottom );
	shader.Set( "BarRadius", BAR_RADIUS * u );
	shader.Set( "Stripe", STRIPE * u );
	shader.Set( "BarColor", bar[ 0 ], bar[ 1 ], bar[ 2 ], bar[ 3 ] );
	shader.Set( "Accent", accent[ 0 ], accent[ 1 ], accent[ 2 ] );
	shader.Set( "TextColor", text, text, text );
	shader.Set( "NameRect", nameRect[ 0 ], nameRect[ 1 ], nameRect[ 2 ], nameRect[ 3 ] );
	shader.Set( "NameAlpha", pose.name );
	shader.Set( "SubtitleRect", subRect[ 0 ], subRect[ 1 ], subRect[ 2 ], subRect[ 3 ] );
	shader.Set( "SubtitleAlpha", pose.subtitle );
	shader.Set( "PhotoCenter", centerX, centerY );
	shader.Set( "PhotoRadius", photoRadius );
	shader.Set( "PhotoAlpha", std::min( 1.0f, std::max( 0.0f, pose.photo * 1.5f ) ) );
	shader.Set( "Ring", RING * u * std::min( 1.0f, std::max( 0.0f, pose.photo ) ) );
	shader.Set( "RoundPhoto", roundPhoto ? 1 : 0 );
	shader.Set( "HasPhoto", hasPhoto ? 1 : 0 );
	shader.Set( "PhotoSize", static_cast< float >( std::max( 1, photoBitmap.width ) ), static_cast< float >( std::max( 1, photoBitmap.height ) ) );
	shader.Set( "InitialsSize", initialsBitmap.width * initialsScale * std::max( 0.0f, pose.photo ), initialsBitmap.height * initialsScale * std::max( 0.0f, pose.photo ) );
	if( darkOnAccent )
		shader.Set( "InitialsColor", bar[ 0 ], bar[ 1 ], bar[ 2 ] );
	else
		shader.Set( "InitialsColor", 1.0f, 1.0f, 1.0f );

	//One texture unit per image.
	struct Binding
	{
		const char* name;
		GLuint texture;
	} bindings[] = { { "NameTexture", nameTexture }, { "SubtitleTexture", subtitleTexture }, { "PhotoTexture", photoTexture }, { "InitialsTexture", initialsTexture } };
	for( int unit = 0; unit < 4; ++unit )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, bindings[ unit ].texture );
		shader.Set( bindings[ unit ].name, unit );
	}
	quad.Draw();
	for( int unit = 3; unit >= 0; --unit )
	{
		glActiveTexture( GL_TEXTURE0 + unit );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}
	return FF_SUCCESS;
}

FFResult Zocalo::SetFloatParameter( unsigned int index, float value )
{
	switch( index )
	{
	case PT_POSITION:
	{
		int next = std::max( 0, std::min( static_cast< int >( std::lround( value ) ), POSITION_COUNT - 1 ) );
		if( next == position )
			break;
		if( phase == Phase::Shown || phase == Phase::Moving )
		{
			//Travel from wherever the photo is right now.
			Layout from = LayoutFor( position );
			float fromX = from.centerX, fromY = from.centerY;
			if( phase == Phase::Moving )
			{
				Pose pose = CurrentPose();
				fromX     = movingFromX + ( from.centerX - movingFromX ) * pose.travel;
				fromY     = movingFromY + ( from.centerY - movingFromY ) * pose.travel;
			}
			movingFrom  = position;
			movingFromX = fromX;
			movingFromY = fromY;
			StartPhase( Phase::Moving );
		}
		position = next;
		break;
	}
	case PT_ENTER:
		if( value != 0.0f )
			StartPhase( Phase::Entering );
		break;
	case PT_EXIT:
		if( value != 0.0f && phase != Phase::Hidden && phase != Phase::Exiting )
			StartPhase( Phase::Exiting );
		break;
	case PT_AUTO_ENTER:
		autoEnter = value > 0.5f;
		break;
	case PT_AUTO_EXIT:
		autoExitSeconds = std::max( 0.0f, value );
		break;
	case PT_SIZE:
		size = value;
		break;
	case PT_SPEED:
		speed = value;
		break;
	case PT_ROUND_PHOTO:
		roundPhoto = value > 0.5f;
		break;
	case PT_BAR_HUE:
	case PT_BAR_SAT:
	case PT_BAR_BRI:
	case PT_BAR_ALPHA:
		barHsba[ index - PT_BAR_HUE ] = value;
		break;
	case PT_ACCENT_HUE:
	case PT_ACCENT_SAT:
	case PT_ACCENT_BRI:
	case PT_ACCENT_ALPHA:
		accentHsba[ index - PT_ACCENT_HUE ] = value;
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
	default:
		return FF_FAIL;
	}
	return FF_SUCCESS;
}

FFResult Zocalo::SetTextParameter( unsigned int index, const char* value )
{
	std::string text = value != nullptr ? value : "";
	switch( index )
	{
	case PT_PHOTO:
		if( text == photoPath )
			break;
		photoPath = text;
		if( text.empty() )
		{
			photoBitmap = Bitmap();
			photoDirty  = true;
		}
		else
		{
			//Big photos take a moment to decode: do it off Resolume's thread.
			photoLoad = std::async( std::launch::async, [ text ] {
				PhotoResult result;
				drawing::LoadPhoto( text, PHOTO_MAX_SIDE, result.bitmap, result.error );
				return result;
			} );
		}
		break;
	case PT_NAME:
		if( text != name )
		{
			name       = text;
			textsDirty = true;
		}
		break;
	case PT_SUBTITLE:
		if( text != subtitle )
		{
			subtitle   = text;
			textsDirty = true;
		}
		break;
	default:
		return FF_FAIL;
	}
	return FF_SUCCESS;
}

float Zocalo::GetFloatParameter( unsigned int index )
{
	switch( index )
	{
	case PT_POSITION:
		return static_cast< float >( position );
	case PT_AUTO_ENTER:
		return autoEnter ? 1.0f : 0.0f;
	case PT_AUTO_EXIT:
		return autoExitSeconds;
	case PT_SIZE:
		return size;
	case PT_SPEED:
		return speed;
	case PT_ROUND_PHOTO:
		return roundPhoto ? 1.0f : 0.0f;
	case PT_BAR_HUE:
	case PT_BAR_SAT:
	case PT_BAR_BRI:
	case PT_BAR_ALPHA:
		return barHsba[ index - PT_BAR_HUE ];
	case PT_ACCENT_HUE:
	case PT_ACCENT_SAT:
	case PT_ACCENT_BRI:
	case PT_ACCENT_ALPHA:
		return accentHsba[ index - PT_ACCENT_HUE ];
	default:
		return 0.0f;
	}
}

char* Zocalo::GetTextParameter( unsigned int index )
{
	static char empty[] = "";
	switch( index )
	{
	case PT_PHOTO:
		return const_cast< char* >( photoPath.c_str() );
	case PT_NAME:
		return const_cast< char* >( name.c_str() );
	case PT_SUBTITLE:
		return const_cast< char* >( subtitle.c_str() );
	default:
		return empty;
	}
}

char* Zocalo::GetParameterDisplay( unsigned int index )
{
	switch( index )
	{
	case PT_AUTO_EXIT:
		if( autoExitSeconds < 0.5f )
			snprintf( displayBuffer, sizeof( displayBuffer ), "Nunca" );
		else
			snprintf( displayBuffer, sizeof( displayBuffer ), "%.0f s", autoExitSeconds );
		return displayBuffer;
	case PT_SIZE:
	case PT_SPEED:
		snprintf( displayBuffer, sizeof( displayBuffer ), "%.0f%%", GetFloatParameter( index ) * 100.0f );
		return displayBuffer;
	default:
		return CFFGLPlugin::GetParameterDisplay( index );
	}
}

void Zocalo::SyncUpdateUi( bool raiseEvents )
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

void Zocalo::Log( const std::string& message )
{
	FFGLLog::LogToHost( ( "[Zocalo] " + message ).c_str() );
	LogToFile( message );
}

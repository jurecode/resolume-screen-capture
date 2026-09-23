#pragma once
#include <FFGLSDK.h>
#include "../Drawing.h"
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

// FFGL source for Resolume Arena: an animated lower third ("zocalo") with a photo, a name and a
// subtitle that enters, leaves, and travels between corners of the screen.
//
// When another Zocalo clip replaces this one in the same layer, Resolume stops drawing this one
// instantly. So each visible Zocalo publishes its Look, and the clip that takes over first plays
// the previous one's exit animation before its own entrance.
class Zocalo : public CFFGLPlugin
{
public:
	// Everything needed to draw one lower third. Bitmaps are shared and never modified.
	struct Look
	{
		std::shared_ptr< const Bitmap > name, subtitle, initials, photo;
		float barHsba[ 4 ]    = {};
		float accentHsba[ 4 ] = {};
		bool roundPhoto       = true;
		float size            = 0.5f;
		int position          = 0;
		float barLength       = 0;//Pixels, from the photo centre to the bar's far end.
	};

	Zocalo();
	~Zocalo() override;

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;
	char* GetParameterDisplay( unsigned int index ) override;

private:
	using Clock = std::chrono::steady_clock;

	enum class Phase
	{
		Hidden,
		Entering,
		Shown,
		Exiting,
		Moving,
	};

	// Where each part is in its animation right now (0 = gone, 1 = fully in).
	struct Pose
	{
		float photo = 0, bar = 0, name = 0, subtitle = 0;
		float travel   = 1;   //Moving: 0 at the old corner, 1 at the new one.
		bool newCorner = true;//Which corner's layout the bar and texts use.
	};

	struct Layout
	{
		float centerX, centerY;//Photo centre, pixels from the top-left.
		float direction;       //+1: bar grows to the right of the photo, -1: to the left.
	};

	// GPU copies of a Look's bitmaps, re-uploaded only when a bitmap changes.
	struct Textures
	{
		GLuint ids[ 4 ]             = {};//name, subtitle, initials, photo
		std::shared_ptr< const Bitmap > uploaded[ 4 ];//Held, so a freed bitmap's address is never mistaken for it.
	};

	// A previous Zocalo that Resolume cut off: we play its exit for it.
	struct Outgoing
	{
		bool active = false;
		Look look;
		Textures textures;
		float time = 0;
	};

	// A Zocalo that was visible when we started: if it stops drawing, it was cut off.
	struct Candidate
	{
		const Zocalo* owner;
		uint64_t frames;//Its published frame count when we started.
		Clock::time_point lastDrawn;
		Look look;
	};

	struct PhotoResult
	{
		Bitmap bitmap;
		std::string error;
	};

	void Advance();                  //Moves the animation clock and handles phase changes.
	void StartPhase( Phase next );
	void HandleClipTriggers();       //"Salir con otro clip": react to clips triggered in Resolume.
	void CheckCutOffCandidates( Clock::time_point now );
	void Publish( Clock::time_point now );
	Pose CurrentPose() const;
	static Pose ExitPose( float t );
	Look CurrentLook() const;
	float Unit( float lookSize ) const;//One "design pixel" at the current output size and Tamano.
	float BarLength( const Look& look ) const;
	Layout LayoutFor( int corner, float unit, float barLength ) const;
	void RefreshBitmaps();            //Re-draws texts after they change, picks up a loaded photo.
	void Upload( Textures& target, const Look& look );
	void Release( Textures& target );
	void Draw( const Look& look, Textures& target, const Pose& pose, const Layout& layout, float barLength );
	void SyncUpdateUi( bool raiseEvents );
	void Log( const std::string& message );

	// Parameters
	std::string photoPath;
	std::string name      = "Nombre Apellido";
	std::string subtitle  = "Cargo o descripcion";
	int position          = 0;
	bool autoEnter        = true;
	float autoExitSeconds = 0;
	float size            = 0.5f;
	float speed           = 0.5f;
	bool roundPhoto       = true;
	bool exitOnOtherClip  = true;
	float oscPort         = 7001;//Resolume's default OSC output port.
	float barHsba[ 4 ]    = { 0.625f, 0.75f, 0.20f, 0.92f };
	float accentHsba[ 4 ] = { 0.112f, 0.85f, 1.00f, 1.00f };

	// Animation
	Phase phase        = Phase::Hidden;
	float phaseTime    = 0;//Seconds into the current phase, already multiplied by the speed.
	float shownSeconds = 0;//Real seconds fully visible, for "Salir despues".
	Clock::time_point lastFrame;
	bool hasRendered = false;
	Clock::time_point activatedAt;  //When the clip last started being drawn.
	uint64_t lastTriggerSeen = 0;   //Newest OSC clip trigger already handled.
	bool ownClipKnown        = false;//Which clip in Resolume is this one (learned from OSC).
	int ownLayer = 0, ownClip = 0;
	int movingFrom    = 0;           //Corner we're travelling away from.
	float movingFromX = 0, movingFromY = 0;
	float barLengthShown = 0;        //Eases towards BarLength() when the texts change.

	// Taking over from a Zocalo that was cut off
	std::vector< Candidate > candidates;
	Clock::time_point candidatesCheckAt;
	Outgoing outgoing;
	uint64_t publishedFrames = 0;

	// Drawing
	bool textsDirty = true;
	std::shared_ptr< const Bitmap > nameBitmap, subtitleBitmap, initialsBitmap, photoBitmap;
	Textures textures;
	std::future< PhotoResult > photoLoad;
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	uint32_t shownUpdateRevision = UINT32_MAX;
	char displayBuffer[ 32 ]     = {};
};

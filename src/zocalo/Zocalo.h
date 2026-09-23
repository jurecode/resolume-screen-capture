#pragma once
#include <FFGLSDK.h>
#include "../Drawing.h"
#include <chrono>
#include <cstdint>
#include <future>
#include <string>

// FFGL source for Resolume Arena: an animated lower third ("zocalo") with a photo, a name and a
// subtitle that enters, leaves, and travels between corners of the screen.
class Zocalo : public CFFGLPlugin
{
public:
	Zocalo();

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
		float travel     = 1;   //Moving: 0 at the old corner, 1 at the new one.
		bool newCorner   = true;//Which corner's layout the bar and texts use.
	};

	struct Layout
	{
		float centerX, centerY;//Photo centre, pixels from the top-left.
		float direction;       //+1: bar grows to the right of the photo, -1: to the left.
	};

	void Advance();                  //Moves the animation clock and handles phase changes.
	Pose CurrentPose() const;
	Layout LayoutFor( int position ) const;
	float BarLength() const;         //From the photo centre to the bar's far end, in pixels.
	float Unit() const;              //One "design pixel" at the current output size and Tamano.
	void StartPhase( Phase next );
	void RefreshTextures();          //Re-draws texts after they change, uploads a newly loaded photo.
	void UploadBitmap( GLuint& texture, const Bitmap& bitmap );
	void SyncUpdateUi( bool raiseEvents );
	void Log( const std::string& message );

	// Parameters
	std::string photoPath;
	std::string name     = "Nombre Apellido";
	std::string subtitle = "Cargo o descripcion";
	int position         = 0;
	bool autoEnter       = true;
	float autoExitSeconds = 0;
	float size           = 0.5f;
	float speed          = 0.5f;
	bool roundPhoto      = true;
	float barHsba[ 4 ]    = { 0.625f, 0.75f, 0.20f, 0.92f };
	float accentHsba[ 4 ] = { 0.112f, 0.85f, 1.00f, 1.00f };

	// Animation
	using Clock = std::chrono::steady_clock;
	Phase phase = Phase::Hidden;
	float phaseTime = 0;        //Seconds into the current phase, already multiplied by the speed.
	float shownSeconds = 0;     //Real seconds fully visible, for "Salir despues".
	Clock::time_point lastFrame;
	bool hasRendered = false;
	int movingFrom   = 0;       //Corner we're travelling away from.
	float movingFromX = 0, movingFromY = 0;
	float barLengthShown = 0;   //Eases towards BarLength() when the texts change.

	// Drawing
	struct PhotoResult
	{
		Bitmap bitmap;
		std::string error;
	};
	bool textsDirty = true;
	bool photoDirty = true;
	Bitmap nameBitmap, subtitleBitmap, initialsBitmap, photoBitmap;
	GLuint nameTexture = 0, subtitleTexture = 0, initialsTexture = 0, photoTexture = 0;
	bool hasPhoto = false;
	std::future< PhotoResult > photoLoad;
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	uint32_t shownUpdateRevision = UINT32_MAX;
	char displayBuffer[ 32 ]     = {};
};

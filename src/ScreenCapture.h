#pragma once
#include <FFGLSDK.h>
#include "CaptureTargets.h"
#include "WgcCapture.h"
#include "Updater.h"
#include <chrono>

// FFGL source for Resolume Arena: shows a live capture of a monitor or a window.
class ScreenCapture : public CFFGLPlugin
{
public:
	ScreenCapture();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;
	char* GetTextParameter( unsigned int index ) override;

private:
	void RefreshTargets();                 //Re-list monitors/windows and push the new dropdown to Resolume.
	void Select( int index, bool notifyHost );
	bool ApplySearch( bool forceRestart ); //Selects the first target matching searchText.
	void StartSelectedCapture();
	void UploadFrame( const unsigned char* bgra, int width, int height );
	void SyncUpdateUi( bool raiseEvents );//Mirrors the shared Updater state into the "Actualizacion" buttons.
	void Log( const std::string& message );

	std::vector< CaptureTarget > targets;
	CaptureTarget selected;
	int selectedIndex = 0;

	std::string searchText;
	int fitMode          = 1;
	bool showCursor      = true;
	bool restoreMinimized = true;
	float crop[ 4 ]      = { 0.0f, 0.0f, 0.0f, 0.0f };//left, right, top, bottom
	bool restartRequired = true;

	WgcCapture capture;
	std::chrono::steady_clock::time_point lastSearchRetry;
	uint32_t shownUpdateRevision = UINT32_MAX;
	std::string lastCaptureError;

	GLuint texture = 0;
	int textureWidth  = 0;
	int textureHeight = 0;
	bool hasFrame     = false;

	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;
};

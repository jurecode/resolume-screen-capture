#pragma once
#include "CaptureTargets.h"
#include <functional>
#include <memory>
#include <string>
#include <thread>

// Captures a monitor or a window. Windows: Windows.Graphics.Capture (win/CaptureEngineWin.cpp).
// macOS: ScreenCaptureKit (mac/CaptureEngineMac.mm).
//
// All the capture work runs off Resolume's thread (a private thread on Windows, ScreenCaptureKit's
// own queues on macOS). Resolume's render thread never waits on the system: Start/Stop return
// immediately and TakeFrame only picks up the newest frame that is already finished.
class CaptureEngine
{
public:
	// bgra points at `height` tightly packed rows of `width` BGRA pixels, valid during the callback.
	using FrameCallback = std::function< void( const unsigned char* bgra, int width, int height ) >;

	CaptureEngine();
	~CaptureEngine();
	CaptureEngine( const CaptureEngine& ) = delete;
	CaptureEngine& operator=( const CaptureEngine& ) = delete;

	static bool IsSupported();
	// macOS asks the user for the Screen Recording permission the first time. Windows needs none.
	static bool HasPermission();

	void Start( const CaptureTarget& target, bool showCursor );
	void Stop();
	void SetCursorVisible( bool visible );
	void SetRestoreMinimized( bool restore );//Show a minimized captured window again, behind all others.

	bool IsActive() const; //Starting or capturing.
	bool WasClosed() const;//The captured window was closed or the monitor was disconnected.

	// Calls onFrame with the newest frame of the current target, if one arrived since the last call.
	bool TakeFrame( const FrameCallback& onFrame );

	// Returns the last error (and clears it), or an empty string.
	std::string TakeError();

private:
	struct Impl;
	std::shared_ptr< Impl > impl;
	std::thread worker;//Windows only.
};

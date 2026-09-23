#pragma once
#include "CaptureTargets.h"
#include <functional>
#include <memory>
#include <string>
#include <thread>

// Captures a monitor or a window with Windows.Graphics.Capture (Windows 10 1903 or newer).
//
// All the capture work (WinRT, Direct3D, copying frames from the GPU) runs on a private thread.
// Resolume's render thread never waits on Windows: Start/Stop return immediately and
// TakeFrame only picks up the newest frame that is already finished.
class WgcCapture
{
public:
	// bgra points at `height` tightly packed rows of `width` BGRA pixels, valid during the callback.
	using FrameCallback = std::function< void( const unsigned char* bgra, int width, int height ) >;

	WgcCapture();
	~WgcCapture();
	WgcCapture( const WgcCapture& ) = delete;
	WgcCapture& operator=( const WgcCapture& ) = delete;

	static bool IsSupported();

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
	std::thread worker;
};

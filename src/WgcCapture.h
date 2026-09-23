#pragma once
#include "CaptureTargets.h"
#include <functional>
#include <memory>
#include <string>

// Captures a monitor or a window with Windows.Graphics.Capture (Windows 10 1903 or newer).
// Frames are read back to the CPU so they can be uploaded into Resolume's OpenGL context.
// Every method must be called from the same thread (Resolume's render thread).
class WgcCapture
{
public:
	// bgra points at `height` rows of `width` BGRA pixels, each row `rowPitch` bytes apart.
	// The pointer is only valid during the callback.
	using FrameCallback = std::function< void( const unsigned char* bgra, int width, int height, int rowPitch ) >;

	WgcCapture();
	~WgcCapture();
	WgcCapture( const WgcCapture& ) = delete;
	WgcCapture& operator=( const WgcCapture& ) = delete;

	static bool IsSupported();

	bool Start( const CaptureTarget& target, bool showCursor );
	void Stop();
	bool IsActive() const;
	bool WasClosed() const;//The captured window was closed or the monitor was disconnected.
	void SetCursorVisible( bool visible );

	// Delivers at most one frame through onFrame. Returns true when it did.
	// Frames arrive one render tick after Windows produces them so we never stall the GPU.
	bool Poll( const FrameCallback& onFrame );

	const std::string& GetLastError() const;

private:
	struct Impl;
	std::unique_ptr< Impl > impl;
};

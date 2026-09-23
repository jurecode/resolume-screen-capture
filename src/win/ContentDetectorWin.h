#pragma once
#include <windows.h>
#include <memory>
#include <thread>

// "Solo contenido": finds where the photo or video sits inside a window (Photos, Media Player,
// Movies & TV...) through UI Automation, so the capture can be cropped to just that area.
//
// Runs on its own thread because asking another program for its UI tree can take a while.
// Every method is safe to call from any thread.
class ContentDetector
{
public:
	ContentDetector();
	~ContentDetector();
	ContentDetector( const ContentDetector& ) = delete;
	ContentDetector& operator=( const ContentDetector& ) = delete;

	void SetWindow( HWND window );//nullptr: nothing to follow (monitor capture, stopped...).
	void SetEnabled( bool enabled );

	// Content area as x0, y0, x1, y1 fractions of the captured window, top-left origin.
	// False when disabled or nothing was found: use the whole window.
	bool GetRect( float rect[ 4 ] ) const;

private:
	struct Impl;
	std::shared_ptr< Impl > impl;
	std::thread worker;
};

#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Something that can be captured: a whole monitor or a single top-level window.
struct CaptureTarget
{
	enum class Kind
	{
		None,
		Monitor,
		Window
	};

	Kind kind        = Kind::None;
	HMONITOR monitor = nullptr;
	HWND window      = nullptr;
	std::string label;     // UTF-8 text shown in Resolume's dropdown.
	std::wstring searchKey;// Lowercase label + full title + exe name, used by "Buscar ventana".

	bool SameAs( const CaptureTarget& other ) const
	{
		return kind == other.kind && monitor == other.monitor && window == other.window;
	}
};

// Returns "(ninguna)" first, then every monitor, then every capturable window.
std::vector< CaptureTarget > EnumerateCaptureTargets();

// False when the window was closed or the monitor was unplugged.
bool IsTargetAlive( const CaptureTarget& target );

// Index of the first target whose searchKey contains the text (case-insensitive), or -1.
int FindTarget( const std::vector< CaptureTarget >& targets, const std::string& utf8Text );

std::string ToUtf8( const std::wstring& text );
std::wstring FromUtf8( const std::string& text );

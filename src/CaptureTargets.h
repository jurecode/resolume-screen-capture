#pragma once
#include <cstdint>
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

	Kind kind   = Kind::None;
	uint64_t id = 0;       // Windows: HMONITOR / HWND. macOS: CGDirectDisplayID / CGWindowID.
	std::string label;     // UTF-8 text shown in Resolume's dropdown.
	std::string searchKey; // Lowercase label + full title + program name, used by "Buscar ventana".

	bool SameAs( const CaptureTarget& other ) const
	{
		return kind == other.kind && id == other.id;
	}
};

// Returns "(ninguna)" first, then every monitor, then every capturable window. (Per platform.)
std::vector< CaptureTarget > EnumerateCaptureTargets();

// False when the window was closed or the monitor was unplugged. (Per platform.)
bool IsTargetAlive( const CaptureTarget& target );

// Index of the first target whose searchKey contains the text (case-insensitive), or -1.
int FindTarget( const std::vector< CaptureTarget >& targets, const std::string& utf8Text );

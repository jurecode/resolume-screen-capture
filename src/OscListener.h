#pragma once
#include <chrono>
#include <cstdint>
#include <vector>

// Listens to Resolume's OSC output (Arena > Preferences > OSC > Output) on this computer, to learn
// when clips are triggered. One listener per process, shared by every plugin instance.
namespace osc
{
struct ClipTrigger
{
	uint64_t id;//Increasing; use it to ask only for new triggers.
	std::chrono::steady_clock::time_point time;
	int layer;//-1 when a whole column was triggered.
	int clip; //Clip number, or the column number.
};

// Starts listening on 127.0.0.1:port, or moves the listener to a new port. Cheap to call often.
void Listen( int port );

// Triggers received after `afterId`, oldest first.
std::vector< ClipTrigger > TriggersSince( uint64_t afterId );

// Id of the newest trigger so far (0 = none), to skip everything that already happened.
uint64_t LatestTriggerId();
}// namespace osc

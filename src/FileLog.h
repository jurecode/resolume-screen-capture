#pragma once
#include <string>

// Appends a timestamped line to Documents\CapturaPantalla-log.txt so users can send us what
// happened. Safe to call from any thread.
void LogToFile( const std::string& message );

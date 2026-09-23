#pragma once
#include <string>

// Appends a timestamped line to Documents\<LOG_FILE_NAME> so users can send us what
// happened. Safe to call from any thread.
void LogToFile( const std::string& message );

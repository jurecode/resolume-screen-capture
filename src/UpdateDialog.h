#pragma once
#include <string>

// Small window offering "Actualizar ahora / Mas tarde / Omitir esta version". Runs on its own
// thread and returns immediately; does nothing if the window is already open.
void ShowUpdateDialog( const std::string& currentVersion, const std::string& newVersion, const std::string& notes );

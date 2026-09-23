#pragma once
// What makes each plugin of this project distinct for the shared code (updater, log, update
// window). CMake sets these per plugin; the defaults are the screen capture plugin's.

#ifndef PLUGIN_DISPLAY_NAME
#	define PLUGIN_DISPLAY_NAME "Captura Pantalla"
#endif
// Prefix for the saved "later / skip" choices, so each plugin remembers its own.
#ifndef PLUGIN_SETTINGS_PREFIX
#	define PLUGIN_SETTINGS_PREFIX ""
#endif
#ifndef LOG_FILE_NAME
#	define LOG_FILE_NAME "CapturaPantalla-log.txt"
#endif
// latest.json fields holding this plugin's download for this platform.
#ifndef UPDATE_URL_FIELD
#	define UPDATE_URL_FIELD "url"
#endif
#ifndef UPDATE_SHA256_FIELD
#	define UPDATE_SHA256_FIELD "sha256"
#endif

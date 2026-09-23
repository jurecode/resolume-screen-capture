#pragma once
#include <string>

// What the shared code needs from the operating system.
// Implemented in win/PlatformWin.cpp and mac/PlatformMac.mm.
namespace platform
{
// ---- Updates ----
bool HttpsGet( const std::string& url, std::string& body, std::string& error );
std::string Sha256Hex( const std::string& data );//Lowercase hex.
std::string ReadSetting( const char* name );    //Survives Resolume restarts.
void WriteSetting( const char* name, const std::string& value );
void ShowNotification( const std::string& title, const std::string& text );//Unobtrusive, never steals focus.
void KeepPluginLoaded();     //Background threads must never outlive our code.
void CleanupPreviousUpdate();//Removes what the last update left behind.
// Replaces the installed plugin with the downloaded package (the dll on Windows, a zipped
// bundle on macOS). The new version loads the next time Resolume starts. Returns an error or "".
std::string InstallUpdate( const std::string& package );

// ---- Misc ----
std::string LogFilePath();//UTF-8 path inside the user's Documents folder.
std::string LowercaseUtf8( const std::string& text );
}// namespace platform

#ifdef _WIN32
std::string ToUtf8( const std::wstring& text );
std::wstring FromUtf8( const std::string& text );
#endif

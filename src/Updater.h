#pragma once
#include <cstdint>
#include <string>

// Remote updates for the plugin.
//
// Periodically downloads a small JSON manifest over HTTPS:
//   { "version": "1.2.0", "url": "https://.../ScreenCapture.dll", "sha256": "...", "notes": "..." }
// When the version is newer it shows a Windows notification and offers the choice in Resolume
// (install / later / skip). Installing downloads the dll, checks its SHA-256 and swaps it in place;
// the new version is loaded the next time Resolume starts (a loaded dll can't be replaced live).
//
// A single instance is shared by every clip of the plugin and does its work on its own thread,
// so all methods are safe to call from Resolume's render thread.
class Updater
{
public:
	enum class State
	{
		Disabled,   //Built without a manifest URL.
		Checking,
		UpToDate,
		Available,  //Newer version found, user hasn't decided yet.
		Postponed,  //Newer version found, user chose "later" or "skip".
		Downloading,
		Installed,  //New dll in place, waiting for Resolume to restart.
		Failed,
	};

	struct Status
	{
		State state = State::Disabled;
		std::string currentVersion;
		std::string latestVersion;
		std::string notes;
		std::string error;
		uint32_t revision = 0;//Increments on every change so the UI knows when to refresh.
	};

	static Updater& Get();

	void Start();//Starts the background thread once; later calls do nothing.
	Status GetStatus();

	void PressAction(); //The main button: install when an update exists, otherwise check again.
	void RemindLater(); //Silences the alert for a while.
	void SkipVersion(); //Never alert about the current latest version again.

private:
	Updater() = default;
	struct Impl;
	Impl* impl = nullptr;
};

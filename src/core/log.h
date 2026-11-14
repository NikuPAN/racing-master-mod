#pragma once

// Plain-text log at %LOCALAPPDATA%\RacingMasterMod\mod.log.
// The previous run is kept as mod.prev.log. Safe to call from any thread.

void LogOpen();
void Log(const char* format, ...);

// Full path of the log file (empty string if it could not be opened).
const wchar_t* LogPath();

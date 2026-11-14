#pragma once

// Plain-text UTF-8 log at %LOCALAPPDATA%\RacingMasterMod\mod.log. Safe to call from any thread.
//
// Earlier runs are kept next to it as mod.<date>-<time>.log (the newest five). When another
// copy of the game still holds mod.log, this process writes mod.<process id>.log instead.
//
// Lines go straight to the file without a disk flush, so the thread that presents frames never
// waits on the disk. Rule: the frame thread logs one-off events only (first frame, a failure,
// a fault); anything periodic is written by the worker thread.

void LogOpen();
void Log(const char* format, ...);

// Converts text for a Log() call. Log lines are UTF-8, so wide strings (file paths, names) go
// through this and are printed with %s. The wide-string format specifier must not appear in Log
// calls: the C runtime's default locale cannot convert Chinese characters and would drop the
// whole line. The build script refuses that specifier anywhere under src\.
const char* LogUtf8(const wchar_t* text, char* buffer, int bufferSize);

// Full path of the log file (empty string if it could not be opened).
const wchar_t* LogPath();

// Folder that holds the log and the settings file (empty string if it could not be determined).
const wchar_t* LogDirectory();

#pragma once

#include <windows.h>

// Starts the mod's own thread. Called from DllMain, which may create a thread but must not wait,
// log, read files or load libraries. The thread does everything slow: opens the log, reads the
// settings, waits for the game window, installs the frame hook, then writes the periodic
// status line and watches that frames keep arriving.
//
// testHost: the process is the developer test host (hidden window allowed, test-only switches
// honoured).
void StartModThread(HMODULE self, bool testHost);

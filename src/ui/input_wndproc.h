#pragma once

#include <windows.h>

// Subclasses the game window's procedure so the panel can take the mouse messages that land on
// it and the hotkeys, and forward everything else to the game untouched. The rules per message
// are at the top of input_wndproc.cpp. Installing from a thread other than the window's is
// fine; the procedure itself always runs on the window's thread.
//
// The subclass is never removed (another overlay may have chained on top of it). "Off" is
// Shared().enabled == false, which makes the procedure forward every message.
bool InstallWindowSubclass(HWND window);

// True once a fault inside the mod's part of the window procedure switched input handling off.
bool WindowProcedureFaulted();

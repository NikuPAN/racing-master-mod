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

// Test host only (RACING_MASTER_MOD_TEST_PANEL_OFF): what the first F12 key press does, so the
// ways the panel switches itself off can be tested. `WindowProcedureFault`: the mod's part of the
// procedure writes through a null pointer. `ImGuiAssert`: the frame thread raises a Dear ImGui
// assertion on its next panel frame (InputTakeTestAssertRequest).
enum class TestPanelOff { None, WindowProcedureFault, ImGuiAssert };
void InputScheduleTestPanelOff(TestPanelOff what);
bool InputTakeTestAssertRequest();   // frame thread; true once after F12 asked for the assertion

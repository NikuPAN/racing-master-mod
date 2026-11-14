#pragma once

// The panel's widgets and what its buttons and hotkeys do. Frame thread only, except
// PanelConfigure (worker, before the hook). The only file that calls ImGui widget functions.

#include "hotkeys.h"
#include "strings.h"

#include "../core/settings.h"

#include <windows.h>

struct PanelFrameInfo {
    HWND window;          // the game window: owner of the file pickers
    float displayWidth;   // back buffer, pixels
    float displayHeight;
    bool displayOnly;     // [panel] mode=display: drawn open, cannot be used
};

// Worker thread, once.
void PanelConfigure(const UiSettings& settings, const HotkeyTable& keys, bool testHost, bool standaloneBuild);

// True when something should be drawn this frame: the panel is open, or a short notice is showing.
bool PanelWantsFrame(bool panelOpen, ULONGLONG now);

// Between ImGui::NewFrame and ImGui::Render.
void BuildPanel(const PanelFrameInfo& info, bool panelOpen);

// A hotkey (or the keyboard's media key) for the music: acts on the chosen source.
void RunMusicCommand(Command command);

// Called inside a frame when a click went to the game while the panel was open: open drop-down
// lists close and a text field lets go of the keyboard.
void PanelClickedOutside();

// Called once on the first frame: hands the saved "pause when not in focus" choice to the player.
void PanelFirstFrame();

unsigned PanelTextSize();   // 0 small, 1 medium, 2 large

// Test host only: the widget centres and state, for tools\run-host-test.ps1.
struct PanelTestItem {
    char name[24];
    float x, y;   // centre, back-buffer pixels
};
int PanelTestItems(const PanelTestItem** items);
int PanelAppendTestState(char* buffer, int size);

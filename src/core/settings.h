#pragma once

#include <windows.h>

// Keys under [keys], in the order the panel's Command enum uses (src/ui/hotkeys.h).
enum UiKey { UiKeyPanel = 0, UiKeyStop, UiKeyPrevious, UiKeyPlayPause, UiKeyNext, UiKeyLoop, UiKeyCount };

// How much of the panel runs ([panel] mode). Lets the owner introduce the two new behaviours one
// at a time on a throwaway account: drawing first, then the window procedure (clicks and keys).
enum UiMode : unsigned {
    UiModeOff = 0,       // no panel: the loader alone, as in 0.2.0
    UiModeDisplay = 1,   // the panel is drawn, always open, but cannot be clicked; no keys; the game window is not touched
    UiModeOn = 2,        // the full panel (default)
};

// Where the panel sits ([panel] corner). The numbers are what the panel stores; bottom centre was
// added in 0.3.1 as 4 and top centre in 0.3.2 as 5, so the older places keep their numbers. Every
// place is flush with the screen edges it touches (0.3.2, the owner's decision).
enum UiCorner : unsigned {
    UiCornerTopLeft = 0,
    UiCornerTopRight = 1,
    UiCornerBottomLeft = 2,
    UiCornerBottomRight = 3,
    UiCornerBottomCentre = 4,   // the default since 0.3.1 (the owner's choice: top-right covered the game's diamond counter)
    UiCornerTopCentre = 5,      // 0.3.2
    UiCornerCount = 6,
};

// The in-game panel's settings: the [panel] and [keys] sections.
struct UiSettings {
    unsigned mode = UiModeOn;
    unsigned language = 0;            // 0 = follow Windows (auto), 1 = English, 2 = Traditional Chinese
    unsigned textSize = 1;            // 0 small, 1 medium, 2 large; never pixels
    bool pauseWhenUnfocused = false;  // pause the music while the game is not the active app
    unsigned source = 0;              // 0 = the player's own files, 1 = another app's media session
    unsigned corner = UiCornerBottomCentre;   // a UiCorner
    bool compact = false;             // the one-row panel instead of the full one (0.3.2)
    bool mediaKeys = true;            // react to the keyboard's own media keys (local files only)
    // The key names as typed; the panel parses them and logs a refused one with the accepted names.
    char keys[UiKeyCount][32] = {"Insert", "F5", "F6", "F7", "F8", "F9"};
};

struct Settings {
    // 0 = do nothing, 1 = load and write the log only, 2 = also hook frames and draw the marker.
    unsigned stage = 2;
    // How long the "mod is alive" square stays on screen, 0 to 600 seconds. Off by default since
    // 0.3.2: the owner, after three clean game runs, "please remove the black and white square
    // rendering as the plugin is proven to work". Kept as a setting for diagnosis.
    unsigned markerSeconds = 0;
    // [debug] present_probe=1: the present probe (present_probe.h), a diagnostic; off by default.
    bool presentProbe = false;
    UiSettings ui;
};

// Reads %LOCALAPPDATA%\RacingMasterMod\settings.ini first, then racing-master-mod.ini next to
// the DLL (values in that file win). Both understand UTF-8 with or without a byte order mark and
// UTF-16 in either byte order as Notepad writes it, with any line ending. Keys before the first
// [heading] count as [mod]. A missing file means defaults. A file that is there but cannot be
// read, or a stage value that is not 0, 1 or 2, switches the mod OFF and says why in the log.
// A file whose lines were all ignored gets a log line saying so. Call after LogOpen().
Settings ReadSettings(HMODULE self);

// Full path of the per-user settings file (empty when the folder is unknown). Also the file the
// panel writes its changed [panel] values back to.
const wchar_t* UserSettingsPath();

enum class IniFileState { Missing, Unreadable, Read };

// Loads a settings file as UTF-8 text (16 KB cap), decoding the byte order marks described
// above. `text` is only valid when the result is Read. *hadByteOrderMark tells a writer whether
// to put one back. Uses a static buffer: one caller at a time (the worker at start-up, later
// only the panel's settings writer thread).
IniFileState LoadIniFileText(const wchar_t* path, char* text, int textSize, DWORD* error, bool* hadByteOrderMark);

#pragma once

#include <windows.h>

struct Settings {
    // 0 = do nothing, 1 = load and write the log only, 2 = also hook frames and draw the marker.
    unsigned stage = 2;
    // How long the "mod is alive" square stays on screen, 0 to 600 seconds.
    unsigned markerSeconds = 20;
};

// Reads %LOCALAPPDATA%\RacingMasterMod\settings.ini first, then racing-master-mod.ini next to
// the DLL (values in that file win). Both understand UTF-8 with or without a byte order mark and
// UTF-16 in either byte order as Notepad writes it, with any line ending. Keys before the first
// [heading] count as [mod]. A missing file means defaults. A file that is there but cannot be
// read, or a stage value that is not 0, 1 or 2, switches the mod OFF and says why in the log.
// A file whose lines were all ignored gets a log line saying so. Call after LogOpen().
Settings ReadSettings(HMODULE self);

#pragma once

// %LOCALAPPDATA%\RacingMasterMod\settings.ini has two writers inside the game: the panel's
// settings writer (its [panel] values, src/ui/ui_settings.cpp) and the music engine's I/O thread
// (its [music] section, src/music/player.cpp). Each reads the whole file, changes its own lines
// and replaces the file. Without a lock, two saves close together could each start from the same
// old copy, and the later one would silently undo the earlier one's change.
//
// Both hold this lock from reading the file until the new copy is in place. It is never held
// across a Log() call or anything that waits on another thread.

#include <windows.h>

inline SRWLOCK g_settingsFileLock = SRWLOCK_INIT;   // constant initialisation: no start-up code

class SettingsFileLock {
public:
    SettingsFileLock() { AcquireSRWLockExclusive(&g_settingsFileLock); }
    ~SettingsFileLock() { ReleaseSRWLockExclusive(&g_settingsFileLock); }
    SettingsFileLock(const SettingsFileLock&) = delete;
    SettingsFileLock& operator=(const SettingsFileLock&) = delete;
};

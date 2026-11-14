#pragma once

#include "../core/settings.h"

// Writes the values the player changes in the panel (language, text size, "pause music when the
// game is not in focus", and the chosen source) back into the per-user settings.ini, so they
// hold for the next session. Only those four keys under [panel] are touched; every other line of
// the file is kept as it was, comments included. The file is written by a small thread of its
// own, never by the thread that presents frames, and the hand-off takes no lock.
//
// Note: racing-master-mod.ini beside the DLL overrides settings.ini when it sets the same key;
// the writer only ever writes settings.ini.

// Worker thread, once. Starts the writer thread.
void UiSettingsStartWriter();

// Any thread. Hands the values to the writer thread and returns at once. Several requests in
// quick succession are written once, with the latest values.
void UiSettingsRequestSave(const UiSettings& settings);

// How many saves have been written so far (the test layout file reports it).
long UiSettingsSavesWritten();

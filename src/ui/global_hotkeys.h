#pragma once

#include "hotkeys.h"

// Minimal mode ([panel] mode=keys): the panel is drawn but not clickable, and the six configured
// hotkeys are registered as GLOBAL hotkeys (RegisterHotKey on the mod's own thread) instead of the
// mod replacing the game window's procedure. This is the smallest touch on the game while still
// showing a panel, for cautious players weighing ban risk: the mod never sits in the game's input
// path and never subclasses its window.
//
// Because the hotkeys are global, they fire whatever window has focus, so the keys also work when
// the game is not in front. The keyboard's own media keys are left to Windows in this mode (there
// is no window procedure to take them). Registration can fail if another program already owns a
// key; that key then does nothing and the log says so.

namespace global_hotkeys {

// Starts the hotkey thread, which registers `table`'s keys and runs a message loop until the
// process ends. `table` must outlive the process (PanelStart publishes it and never frees it).
void Start(const HotkeyTable* table);

}  // namespace global_hotkeys

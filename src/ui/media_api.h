#pragma once

// The panel's one include for what it plays and controls:
//
//   music::Player and the file pickers: the music engine (src/music), "My files".
//   remote::Remote: the media remote (src/remote), "Other app", reached through other_app.h,
//       which stands in an example app in the test host only.

#include "../music/player.h"
#include "../music/picker.h"

#include "../remote/remote.h"

// Threads: the panel calls these from the thread that presents frames (buttons, hotkeys) and
// calls music::Player::OnGameFocusChanged from the game's window thread. Each call must return
// quickly; slow work (files, devices, Windows media sessions) belongs on the module's own thread.

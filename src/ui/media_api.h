#pragma once

// The one place that decides where music::Player comes from (the file picker with it).
//
//   RMM_UI_STANDALONE defined (while the music module is not in the tree): stand-in
//       declarations and the stand-in implementation in src/ui/standalone/stubs.cpp, with an
//       example playlist and no sound.
//   not defined (integrated build): the real music module's headers.
//
// build\build.ps1 defines RMM_UI_STANDALONE while src\music\player.h does not exist. To switch,
// add the music module's sources to the build; nothing in the panel changes.
//
// The media remote (remote::Remote, src/remote/remote.h) is always the real one; the panel
// reaches it through other_app.h, which stands in an example app in the test host only.

#if defined(RMM_UI_STANDALONE)
#include "standalone/interfaces.h"
#else
#include "../music/player.h"
#include "../music/picker.h"
#endif

#include "../remote/remote.h"

// Threads: the panel calls these from the thread that presents frames (buttons, hotkeys) and
// calls music::Player::OnGameFocusChanged from the game's window thread. Each call must return
// quickly; slow work (files, devices, Windows media sessions) belongs on the module's own thread.

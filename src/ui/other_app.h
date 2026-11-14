#pragma once

// "Other app" as the panel sees it: the real media remote (src/remote), or, in the developer
// test host, an example app inside the mod.
//
// Why the example: the real remote reads every media session on the PC, so a panel test would
// show (and save in its screenshots) whatever the developer happens to be playing, and could
// not be repeated. In the test host the example is used unless
// RACING_MASTER_MOD_TEST_REMOTE=real; RACING_MASTER_MOD_TEST_REMOTE=limited (default), full or
// none picks what the example allows. The real remote's own tests (runs 15-22) do not go
// through here. In the game this file only forwards to remote::Remote.
//
// Threads: Start from the worker or on the player's action; the rest from the frame thread.

#include "../remote/remote.h"

namespace other_app {

// Starts the real remote the first time (cheap afterwards). Does nothing for the example.
void Start();

remote::Snapshot GetSnapshot();
void Play();
void Pause();
void Stop();
void Next();
void Previous();
void SetRepeat(remote::Repeat repeat);

// Worker, when the game window has closed: lets go of the other app if Start ever ran (in the
// game the media thread only lets go and sleeps; see remote.h).
void Shutdown();

}  // namespace other_app

#pragma once

// A silent media session for the developer test host. It publishes itself to Windows as a music
// player on a window that is never shown, plays no sound, and records every command it receives,
// so the mod's remote control can be tested without sending anything to a real player.
//
// While it exists it is Windows' current media session (a new session takes over at once), so the
// keyboard's media keys and the Windows media flyout point at it for the few seconds of a run.
//
// Main thread only, and the thread must pump window messages: commands arrive on Windows' threads
// and are applied on this one.

#include <windows.h>

namespace fake_player {

// Every fake player's app id starts with this. The id itself comes from
// RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID, which tools\run-host-test.ps1 makes new for every run;
// without it the process id is appended. Windows matches sessions by app id only, so two test runs
// sharing one id (another run on the same PC at the same time) would command each other's player.
// The mod's test guard only lets commands through to RACING_MASTER_MOD_TEST_MEDIA_APP, which the
// script sets to this same id.
constexpr wchar_t kAppIdPrefix[] = L"RacingMasterMod.TestHost.FakePlayer";

// The app id this process publishes under, as wide text and as UTF-8 (the mod writes it to its log
// in UTF-8). Valid after PrepareProcess().
const wchar_t* AppId();
const char* AppIdUtf8();

enum class Behaviour {
    ApplyAll,        // does what it is asked; listens for loop changes
    IgnorePause,     // accepts "pause" but keeps playing
    NoRepeatNoNext,  // does not listen for loop changes and offers no "next"
    VanishOnPause,   // switches its media session off when asked to pause
};

// Gives the process its app id (printed as "fake player: app id ...") and initialises COM on this
// thread (single-threaded apartment). Must run on the main thread before any window is created and
// before Start().
bool PrepareProcess();

// Publishes the session (paused, title "Test track 1"). Returns false with a printed reason.
bool Start(Behaviour behaviour);

// Removes the session and its window.
void Stop();

// The commands received so far, in order, as "play, pause, next, loop all, ...".
const char* Received();

// The playback state the fake player reports right now ("paused", "playing", ...).
const char* StatusWord();

// Leaves COM on this thread. After Stop().
void FinishProcess();

}  // namespace fake_player

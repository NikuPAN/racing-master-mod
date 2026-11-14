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

// The app id Windows shows for the session. The mod's test guard only lets commands through to
// this id (RACING_MASTER_MOD_TEST_MEDIA_APP).
constexpr wchar_t kAppId[] = L"RacingMasterMod.TestHost.FakePlayer";

enum class Behaviour {
    ApplyAll,        // does what it is asked; listens for loop changes
    IgnorePause,     // accepts "pause" but keeps playing
    NoRepeatNoNext,  // does not listen for loop changes and offers no "next"
    VanishOnPause,   // switches its media session off when asked to pause
};

// Gives the process its app id and initialises COM on this thread (single-threaded apartment).
// Must run on the main thread before any window is created and before Start().
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

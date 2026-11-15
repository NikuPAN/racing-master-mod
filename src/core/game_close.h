#pragma once

// The player closing the game, as the game window's procedure sees it, handed to the worker.
//
// The worker's window watch (worker.cpp) only notices a closed game once the window is gone and
// no frame has arrived for about a second. In the game the process often ends before that: twice
// on 2026-11-15 the owner closed it with the window's X and mod.log simply stopped, with no final
// save. So the window procedure subclass (mode=on) passes these messages on to the worker the
// moment they arrive, and the game handles every one of them exactly as before:
//
//   WM_CLOSE, WM_SYSCOMMAND SC_CLOSE,   a close request. The worker saves the song and its place
//   WM_QUERYENDSESSION                  at once and the music plays on: the game may still say no
//                                       (Unreal turns SC_CLOSE into its own deferred close, and a
//                                       game can ask "are you sure?").
//   WM_DESTROY (of the window the       the window is going. The worker stops the music (fade) and
//   panel draws into), WM_ENDSESSION    makes the final save; the window procedure waits for that
//   with wParam TRUE                    at most kGameCloseWaitMs after passing the message on, so
//                                       the save is done before the process can end.
//
// Window-thread functions use only atomics, SetEvent and one bounded wait: no file, no log, no
// lock, nothing delay-loaded. The worker creates the events before the hook is installed.

#include <windows.h>

constexpr DWORD kGameCloseWaitMs = 500;

// Worker, before the frame hook (and so the window subclass) exists.
void GameCloseInit();

// Window thread. A close request: wakes the worker; never waits.
void GameCloseRequested();

// Window thread, after the game's own procedure handled WM_DESTROY or WM_ENDSESSION: wakes the
// worker and waits until it has stopped and saved the music, at most kGameCloseWaitMs, and only
// while the worker is watching (it is not once the music has stopped, or when it never started).
// The first call wakes the worker; call GameWindowEndingSignal first, before the game's procedure,
// so the worker starts while the game handles the message.
void GameWindowEndingSignal();
void GameWindowEndingWait();

// Worker: the event to wait on instead of sleeping (null if it could not be created).
HANDLE GameCloseWakeEvent();

struct GameCloseState {
    unsigned requests;   // close requests so far (WM_CLOSE, SC_CLOSE, WM_QUERYENDSESSION)
    bool ending;         // the game window is being destroyed, or Windows is ending the session
};
GameCloseState GameCloseRead();

// Worker: whether a window procedure that sees the window go should wait for it.
void GameCloseSetWatching(bool watching);

// Worker: the music has stopped and its final save is done; releases a waiting window procedure.
void GameCloseMusicStopped();

#include "game_close.h"

namespace {

// Created once by the worker and never closed: a window procedure may still hold them.
HANDLE g_wake = nullptr;      // auto-reset: the worker's sleep between window checks
HANDLE g_stopped = nullptr;   // manual-reset: the music has stopped and is saved
volatile LONG g_requests = 0;
volatile LONG g_ending = 0;
volatile LONG g_watching = 0;

}  // namespace

void GameCloseInit() {
    if (!g_wake) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!g_stopped) {
        g_stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
}

void GameCloseRequested() {
    InterlockedIncrement(&g_requests);
    if (g_wake) {
        SetEvent(g_wake);
    }
}

void GameWindowEndingSignal() {
    InterlockedExchange(&g_ending, 1);
    if (g_wake) {
        SetEvent(g_wake);
    }
}

void GameWindowEndingWait() {
    if (g_stopped && InterlockedCompareExchange(&g_watching, 0, 0) != 0) {
        WaitForSingleObject(g_stopped, kGameCloseWaitMs);
    }
}

HANDLE GameCloseWakeEvent() {
    return g_wake;
}

GameCloseState GameCloseRead() {
    GameCloseState state;
    state.requests = static_cast<unsigned>(InterlockedCompareExchange(&g_requests, 0, 0));
    state.ending = InterlockedCompareExchange(&g_ending, 0, 0) != 0;
    return state;
}

void GameCloseSetWatching(bool watching) {
    InterlockedExchange(&g_watching, watching ? 1 : 0);
}

void GameCloseMusicStopped() {
    InterlockedExchange(&g_watching, 0);
    if (g_stopped) {
        SetEvent(g_stopped);
    }
}

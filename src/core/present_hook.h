#pragma once

// Hooks IDXGISwapChain::Present / Present1 so the mod gets a callback once per frame.
//
// The hook replaces two entries in DXGI's swap chain function table (the same place overlays
// such as Steam, Discord and OBS attach to). No game code or game file is modified.
//
// That table is shared by every swap chain in the process, so the mod binds to one: the first
// that presents to a visible window of this process with a client area of at least 320x200.
// If that one stops presenting for two seconds (the game rebuilt its swap chain), the next
// qualifying one takes over.
//
// A fault inside the per-frame code releases everything held, writes one log line and switches
// the per-frame code off until the game restarts.

struct PresentHookOptions {
    // How long to draw the small "mod is alive" square after the first frame.
    unsigned markerSeconds = 20;
    // Test host only: its window is never shown, so a hidden window may qualify.
    bool acceptHiddenWindow = false;
    // Test host only: fault deliberately at this frame (0 = never) to check the recovery path.
    unsigned testFaultAtFrame = 0;
};

// Returns false if the hook could not be installed; the reason is written to the log.
// Worker thread only. The options are read-only once the hook is in place.
bool InstallPresentHook(const PresentHookOptions& options);

struct PresentHookCounters {
    unsigned long long hookCalls;    // every Present/Present1 that reached the hook (any swap chain)
    unsigned long long gameFrames;   // frames of the swap chain the mod is bound to
};
PresentHookCounters PresentHookGetCounters();

// True after a fault in the per-frame code; the mod then stays off for this session.
bool PresentHookSwitchedOff();

// The bound swap chain as the frame thread last saw it (sampled about once a second), for the
// worker's "game window" log line. known is false until the first game frame.
struct PresentHookWindowState {
    bool known;
    bool exclusiveFullscreen;
    void* window;      // the swap chain's output window (an HWND)
};
PresentHookWindowState PresentHookGetWindowState();

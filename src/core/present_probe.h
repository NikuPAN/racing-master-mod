#pragma once

// The present probe: a diagnostic for the owner's NVIDIA Smooth Motion question, off by default
// and switched on only by [debug] present_probe=1 in settings.ini. What each finding would mean:
// docs/agent-notes/present-probe.md.
//
// Off: the hook does one relaxed load of g_probeOn per present and nothing else.
// On: the threads that present record into fixed-size tables with interlocked operations only
// (no allocation, no lock, no logging, no file): per entry point (Present / Present1), the module
// that called it (by return address, cached by address range), the thread (up to 8), the swap
// chain and its function table (up to 4), flags and sync intervals, how many threads were inside
// the hook at once, what the frame work did for each caller, and Windows' own present count of
// the game's swap chain. The worker turns that into one compact block in mod.log every 30 s.

#include <windows.h>

struct IDXGISwapChain;

// Set once by the worker before the hook is installed; read by every present.
extern volatile LONG g_probeOn;

// What the per-frame work did with one present, per caller.
enum class ProbeOutcome : int {
    Drawn = 0,         // the game's frame, and the panel was drawn on it
    GameFrame = 1,     // the game's frame, nothing of the panel drawn (closed, or not ready)
    OtherChain = 2,    // a swap chain that is not the game's
    Skipped = 3,       // another thread was inside the frame work: passed straight on
    Off = 4,           // the frame work is off for the session (after a fault), or a test present
    Count = 5,
};

// Frame threads, only while g_probeOn. ProbeEnter returns the caller's slot (or -1), which
// ProbeRecordOutcome takes; ProbeLeave ends the "in flight" count.
int ProbeEnter(int entry, void* returnAddress, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
void ProbeRecordOutcome(int callerSlot, ProbeOutcome outcome);
void ProbeLeave();
// Frame thread, inside the frame work, for the game's swap chain: IDXGISwapChain::GetLastPresentCount.
void ProbeRecordPresentCount(UINT count);

// Worker. `originalPresent`/`originalPresent1`: what the hook calls on; `table`: the DXGI swap
// chain function table the hook patched (entry 8 is Present). Writes the block to mod.log.
void ProbeReport(void* originalPresent, void* originalPresent1, void** table, unsigned long long gameFrames,
                 unsigned seconds);

// Test host: every present the probe recorded (0 while it is off).
unsigned long long ProbeRecordedCalls();

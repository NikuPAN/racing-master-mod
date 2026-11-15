#pragma once

// Test-only hooks for the music engine. Nothing here is called by the mod in the game; the test
// program (tools\testhost\musictest.cpp) uses it to run the engine without a sound device.
//
// Call Configure before Player::Get().Start(). In Manual mode there is no device thread: the
// test pulls audio with Render, which runs the very same callback the device would.

#include "audio_output.h"

#include <cstdint>

namespace music::testing {

struct Config {
    OutputMode output = OutputMode::Wasapi;
    bool silentDevice = false;         // real device, zero output (developer switch, never in automated runs)
    const wchar_t* dataDir = nullptr;  // overrides %LOCALAPPDATA%\RacingMasterMod
    unsigned positionSaveMs = 0;       // how often the playing position is saved; 0 = the engine's 10 s
};
void Configure(const Config& config);

// Manual output only: renders `frames` stereo float frames into `out`. Returns false when the
// output is not in Manual mode (the buffer is then zeroed).
bool Render(float* out, uint32_t frames);

// Blocks until every command posted so far has been handled AND the player thread has finished
// one full pass of its loop after it (ring filled, track boundaries checked, snapshot published).
// At most `timeoutMs`; returns false on timeout.
bool Sync(unsigned timeoutMs);

// Blocks until the I/O thread has no scan, tag read or save pending and the player has applied
// the results (at most `timeoutMs`).
bool WaitForIdleIo(unsigned timeoutMs);

// The device keeps running but delivers nothing, until the engine's watchdog restarts it.
void SimulateDeviceLoss();

struct Counters {
    uint64_t consumed;
    uint64_t produced;
    uint64_t callbacks;
    uint32_t underruns;
    uint32_t sampleRate;
    bool deviceStarted;
    uint32_t opens;          // source opens since Start (successful or not)
    uint32_t deviceRestarts; // watchdog restarts since Start
    uint32_t settingsSaves;  // [music] saves handed to the I/O thread or done at shutdown, since Start
};
Counters GetCounters();

}  // namespace music::testing

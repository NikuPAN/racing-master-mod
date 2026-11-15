#pragma once

// The output side of the engine: a 2-second lock-free ring of 32-bit float stereo at the
// pipeline rate, the audio callback that drains it, and the device it runs on.
//
// Output modes:
//   Wasapi  one miniaudio playback device on WASAPI shared mode, default endpoint (the game).
//   Null    miniaudio's null backend: a real device thread on a timer, no audio hardware.
//   Manual  no device at all; the test calls Render(), which runs the very same callback.
// In test-host mode the engine refuses Wasapi.
//
// Everything the callback shares with the player thread is an atomic; no lock is ever taken on
// the device thread. The gate protocol (close -> fade -> acknowledge -> reset ring -> refill ->
// open) is what makes a manual change click-free and keeps stale audio out of the output.
//
// Lifetime: the shared block (atomics, ring) is allocated once and never freed while the process
// runs, because the frame thread may store userHold and volume into it at any moment, including
// while the player thread re-creates the device. Init/Uninit only create and destroy the device
// and the ring inside it.

#include <cstdint>

namespace music {

enum class OutputMode { Wasapi, Null, Manual };

struct OutputShared;

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    // Creates the device (not for Manual) and the ring. Returns false if the device could not be
    // opened; then nothing else may be called except Uninit. `silent` keeps the real device
    // stream but writes zeros (a developer switch to watch the Windows mixer without sound).
    bool Init(OutputMode mode, bool silent);
    void Uninit();
    bool IsInitialised() const { return initialised_; }
    OutputMode Mode() const { return mode_; }

    bool StartDevice();   // idempotent; also clears a simulated device loss
    void StopDevice();    // idempotent
    bool DeviceStarted() const { return deviceStarted_; }
    // miniaudio reported that the device stopped on its own (unplugged, endpoint gone).
    bool DeviceStoppedByItself() const;

    uint32_t SampleRate() const { return sampleRate_; }
    uint32_t RingCapacityFrames() const;

    // ---- player-thread side of the ring ----
    // Space the writer may fill now, and the ring pointer for it (may be fewer than asked at the
    // wrap; call again). Commit what was written.
    bool AcquireWrite(uint32_t* frames, float** out);
    void CommitWrite(uint32_t frames);
    // Copies `frames` frames into the ring, looping over the wrap. Returns frames written.
    uint32_t Write(const float* in, uint32_t frames);
    uint32_t FramesQueued() const;  // produced - consumed, in frames
    uint32_t FreeFrames() const;

    // ---- gate ----
    // Closes the gate and waits (at most `timeoutMs`) until the callback has faded out. Returns
    // true if acknowledged; then the ring is reset and `produced` equals `consumed`. On a timeout
    // the ring is left alone and the callback drops everything written so far instead.
    bool CloseGateAndFlush(unsigned timeoutMs);
    void OpenGate();
    bool GateOpen() const;

    // Instant pause/stop from any thread: treated as gate closed by the callback.
    void SetUserHold(bool hold);
    bool UserHold() const;

    void SetVolume(float linearGain);  // 0..1, applied with a ramp in the callback; any thread
    float Volume() const;

    // How long the gate takes to fade the sound in or out (pause, stop, song change, seek). 10 ms
    // by default (just enough to avoid a click); any thread. 0 = instant.
    void SetFadeMs(unsigned ms);

    // The callback never consumes past this frame count (UINT64_MAX = no limit).
    void SetPlayLimit(uint64_t frame);
    uint64_t PlayLimit() const;

    uint64_t Consumed() const;   // frames taken from the ring since Init (monotonic)
    uint64_t Produced() const;   // frames written into the ring since Init
    uint32_t Underruns() const;
    uint64_t Callbacks() const;

    // Manual mode only: runs the callback into `out` (interleaved stereo float). Any thread.
    void Render(float* out, uint32_t frames);

    // Test support: the device keeps running but delivers nothing until StartDevice is called
    // again (what a stuck or vanished endpoint looks like to the engine).
    void SimulateDeviceLoss();

private:
    OutputShared* shared_ = nullptr;
    OutputMode mode_ = OutputMode::Manual;
    uint32_t sampleRate_ = 48000;
    bool initialised_ = false;
    bool deviceStarted_ = false;
    bool contextReady_ = false;
    bool deviceReady_ = false;
};

}  // namespace music

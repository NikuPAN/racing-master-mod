#include "audio_output.h"

#include "miniaudio_config.h"
#include "miniaudio.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <new>

namespace music {

struct OutputShared {
    ma_context context = {};
    ma_device device = {};
    ma_pcm_rb ring = {};
    std::atomic<bool> ringReady{false};
    bool silent = false;
    uint32_t rate = 48000;

    std::atomic<uint64_t> consumed{0};
    std::atomic<uint64_t> produced{0};
    std::atomic<uint64_t> playLimit{UINT64_MAX};
    std::atomic<uint64_t> discardBefore{0};
    std::atomic<bool> gateOpen{false};
    std::atomic<bool> userHold{false};
    std::atomic<uint32_t> gateEpoch{0};
    std::atomic<uint32_t> gateAck{0};
    std::atomic<float> volume{1.0f};
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint64_t> callbacks{0};
    std::atomic<bool> deviceStopped{false};  // set by the notification callback only
    std::atomic<bool> lossSimulated{false};

    // Manual mode only: Render (test thread) and the player thread's ring reset never overlap.
    // The device path does not use it; there the gate handshake provides the same guarantee.
    SRWLOCK renderLock = SRWLOCK_INIT;

    // Callback-private state (one reader thread at a time).
    float gateGain = 0.0f;
    float lastVolume = 1.0f;
};

namespace {

void DeviceNotification(const ma_device_notification* notification) {
    // Runs on miniaudio's own threads, possibly in the middle of a re-route: store a flag only.
    // Never start, stop or re-create the device from here.
    auto* shared = static_cast<OutputShared*>(notification->pDevice->pUserData);
    if (!shared) return;
    if (notification->type == ma_device_notification_type_stopped) shared->deviceStopped = true;
    if (notification->type == ma_device_notification_type_started) shared->deviceStopped = false;
}

void Acknowledge(OutputShared* shared) {
    shared->gateAck.store(shared->gateEpoch.load(std::memory_order_acquire), std::memory_order_release);
}

// The one audio callback. `out` arrives zeroed (noPreSilencedOutputBuffer stays false); Render
// zeroes it too. No locks, no allocation, no system calls.
void Callback(OutputShared* shared, float* out, ma_uint32 frames) {
    shared->callbacks.fetch_add(1, std::memory_order_relaxed);
    if (shared->lossSimulated.load(std::memory_order_relaxed)) return;
    if (!shared->ringReady.load(std::memory_order_acquire)) return;

    // A flush that timed out: drop stale audio without copying, keeping the one-reader rule.
    const uint64_t discardBefore = shared->discardBefore.load(std::memory_order_acquire);
    uint64_t consumed = shared->consumed.load(std::memory_order_relaxed);
    while (consumed < discardBefore) {
        const uint64_t left = discardBefore - consumed;
        ma_uint32 n = static_cast<ma_uint32>(left > 0xFFFFFFFFull ? 0xFFFFFFFFull : left);
        void* p = nullptr;
        if (ma_pcm_rb_acquire_read(&shared->ring, &n, &p) != MA_SUCCESS || n == 0) break;
        ma_pcm_rb_commit_read(&shared->ring, n);
        consumed += n;
        shared->consumed.store(consumed, std::memory_order_release);
    }

    const bool open = shared->gateOpen.load(std::memory_order_acquire) && !shared->userHold.load(std::memory_order_acquire);
    const float step = 100.0f / static_cast<float>(shared->rate);  // gate fade: 10 ms
    const float targetVolume = shared->volume.load(std::memory_order_relaxed);
    const float volumeFrom = shared->lastVolume;
    shared->lastVolume = targetVolume;

    if (!open && shared->gateGain <= 0.0f) {
        Acknowledge(shared);
        return;  // silence, nothing consumed
    }

    const uint64_t limit = shared->playLimit.load(std::memory_order_acquire);
    uint64_t wanted = frames;
    if (limit <= consumed) wanted = 0;
    else if (limit - consumed < wanted) wanted = limit - consumed;

    ma_uint32 done = 0;
    bool starved = false;
    while (done < wanted) {
        ma_uint32 n = static_cast<ma_uint32>(wanted - done);
        void* p = nullptr;
        if (ma_pcm_rb_acquire_read(&shared->ring, &n, &p) != MA_SUCCESS || n == 0) {
            starved = true;
            break;
        }
        const float* in = static_cast<const float*>(p);
        ma_uint32 used = 0;
        for (; used < n; ++used) {
            // Gate gain moves linearly towards 1 or 0 over 10 ms; volume ramps across the buffer,
            // so neither a pause nor a volume jump makes a step in the waveform.
            if (open) {
                shared->gateGain = shared->gateGain + step > 1.0f ? 1.0f : shared->gateGain + step;
            } else {
                shared->gateGain = shared->gateGain - step < 0.0f ? 0.0f : shared->gateGain - step;
                if (shared->gateGain <= 0.0f) break;  // faded out: stop consuming here
            }
            const float t = static_cast<float>(done + used) / static_cast<float>(frames);
            const float gain = shared->gateGain * (volumeFrom + (targetVolume - volumeFrom) * t);
            out[(done + used) * 2] = in[used * 2] * gain;
            out[(done + used) * 2 + 1] = in[used * 2 + 1] * gain;
        }
        ma_pcm_rb_commit_read(&shared->ring, used);
        done += used;
        if (used < n) break;
    }
    if (starved && open && limit > consumed + done) shared->underruns.fetch_add(1, std::memory_order_relaxed);
    shared->consumed.store(consumed + done, std::memory_order_release);
    if (!open && (shared->gateGain <= 0.0f || done < frames)) {
        // Faded out, or nothing left to fade (ring empty or play limit reached): the output is
        // silent from here, so the player thread may now touch the ring.
        shared->gateGain = 0.0f;
        Acknowledge(shared);
    }
    if (shared->silent) memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
}

void DataCallback(ma_device* device, void* out, const void*, ma_uint32 frames) {
    Callback(static_cast<OutputShared*>(device->pUserData), static_cast<float*>(out), frames);
}

}  // namespace

AudioOutput::AudioOutput() : shared_(new (std::nothrow) OutputShared) {}

AudioOutput::~AudioOutput() {
    // The engine that owns this object is never destroyed (no work may run at process exit), so
    // in practice this only runs in a test program that builds its own AudioOutput.
    Uninit();
    delete shared_;
}

bool AudioOutput::Init(OutputMode mode, bool silent) {
    if (initialised_) return true;
    if (!shared_) return false;
    mode_ = mode;
    shared_->silent = silent;
    shared_->consumed = 0;
    shared_->produced = 0;
    shared_->discardBefore = 0;
    shared_->playLimit = UINT64_MAX;
    shared_->gateOpen = false;
    shared_->gateGain = 0.0f;
    shared_->deviceStopped = false;
    shared_->lossSimulated = false;
    shared_->lastVolume = shared_->volume.load();

    if (mode != OutputMode::Manual) {
        ma_context_config contextConfig = ma_context_config_init();
        ma_backend backends[1] = {mode == OutputMode::Wasapi ? ma_backend_wasapi : ma_backend_null};
        if (ma_context_init(backends, 1, &contextConfig, &shared_->context) != MA_SUCCESS) {
            Uninit();
            return false;
        }
        contextReady_ = true;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID = nullptr;  // default endpoint, so miniaudio follows default-device changes
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = 0;  // the device's own rate
        config.periodSizeInMilliseconds = 20;
        config.dataCallback = DataCallback;
        config.notificationCallback = DeviceNotification;
        config.pUserData = shared_;
        config.wasapi.noAutoStreamRouting = MA_FALSE;
        config.wasapi.usage = ma_wasapi_usage_default;  // no MMCSS: do not compete with the game's audio thread
        config.noPreSilencedOutputBuffer = MA_FALSE;
        config.noClip = MA_FALSE;
        if (ma_device_init(&shared_->context, &config, &shared_->device) != MA_SUCCESS) {
            Uninit();
            return false;
        }
        deviceReady_ = true;
        sampleRate_ = shared_->device.sampleRate;
    } else {
        sampleRate_ = 48000;
    }
    shared_->rate = sampleRate_;
    if (ma_pcm_rb_init(ma_format_f32, 2, sampleRate_ * 2, nullptr, nullptr, &shared_->ring) != MA_SUCCESS) {
        Uninit();
        return false;
    }
    shared_->ringReady = true;
    initialised_ = true;
    return true;
}

void AudioOutput::Uninit() {
    if (!shared_) return;
    if (deviceReady_) {
        // ma_device_uninit stops the device thread first; after it no callback runs.
        ma_device_uninit(&shared_->device);
    }
    if (shared_->ringReady) {
        AcquireSRWLockExclusive(&shared_->renderLock);  // Manual mode: no Render in progress
        shared_->ringReady = false;
        ReleaseSRWLockExclusive(&shared_->renderLock);
        ma_pcm_rb_uninit(&shared_->ring);
    }
    if (contextReady_) ma_context_uninit(&shared_->context);
    initialised_ = deviceStarted_ = contextReady_ = deviceReady_ = false;
}

bool AudioOutput::StartDevice() {
    if (!initialised_) return false;
    shared_->lossSimulated = false;
    if (mode_ == OutputMode::Manual) {
        deviceStarted_ = true;
        return true;
    }
    if (deviceStarted_ && !shared_->deviceStopped && ma_device_is_started(&shared_->device)) return true;
    if (ma_device_start(&shared_->device) != MA_SUCCESS) return false;
    shared_->deviceStopped = false;
    deviceStarted_ = true;
    return true;
}

void AudioOutput::StopDevice() {
    if (!initialised_ || !deviceStarted_) return;
    if (mode_ != OutputMode::Manual) ma_device_stop(&shared_->device);
    deviceStarted_ = false;
}

bool AudioOutput::DeviceStoppedByItself() const {
    return initialised_ && deviceStarted_ && mode_ != OutputMode::Manual && shared_->deviceStopped;
}

uint32_t AudioOutput::RingCapacityFrames() const {
    return initialised_ ? sampleRate_ * 2 : 0;
}

bool AudioOutput::AcquireWrite(uint32_t* frames, float** out) {
    *out = nullptr;
    if (!initialised_) {
        *frames = 0;
        return false;
    }
    ma_uint32 n = *frames;
    void* p = nullptr;
    if (ma_pcm_rb_acquire_write(&shared_->ring, &n, &p) != MA_SUCCESS) {
        *frames = 0;
        return false;
    }
    *frames = n;
    *out = static_cast<float*>(p);
    return n > 0;
}

void AudioOutput::CommitWrite(uint32_t frames) {
    if (!initialised_) return;
    ma_pcm_rb_commit_write(&shared_->ring, frames);
    shared_->produced.fetch_add(frames, std::memory_order_release);
}

uint32_t AudioOutput::Write(const float* in, uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        float* p = nullptr;
        if (!AcquireWrite(&n, &p) || n == 0) {
            CommitWrite(0);
            break;
        }
        memcpy(p, in + static_cast<size_t>(done) * 2, static_cast<size_t>(n) * 2 * sizeof(float));
        CommitWrite(n);
        done += n;
    }
    return done;
}

uint32_t AudioOutput::FramesQueued() const {
    if (!initialised_) return 0;
    const uint64_t produced = shared_->produced.load(std::memory_order_acquire);
    const uint64_t consumed = shared_->consumed.load(std::memory_order_acquire);
    return produced > consumed ? static_cast<uint32_t>(produced - consumed) : 0;
}

uint32_t AudioOutput::FreeFrames() const {
    if (!initialised_) return 0;
    const uint32_t queued = FramesQueued();
    const uint32_t capacity = RingCapacityFrames();
    return queued < capacity ? capacity - queued : 0;
}

bool AudioOutput::CloseGateAndFlush(unsigned timeoutMs) {
    if (!initialised_) return false;
    const uint32_t epoch = shared_->gateEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    shared_->gateOpen.store(false, std::memory_order_release);
    bool acknowledged = false;
    bool manualLocked = false;
    if (mode_ == OutputMode::Manual) {
        // The reader is whichever thread calls Render. Holding the lock proves it is idle.
        AcquireSRWLockExclusive(&shared_->renderLock);
        manualLocked = true;
        acknowledged = true;
        shared_->gateGain = 0.0f;
    } else if (!deviceStarted_) {
        acknowledged = true;  // a stopped device runs no callback
        shared_->gateGain = 0.0f;
    } else {
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        for (;;) {
            if (shared_->gateAck.load(std::memory_order_acquire) == epoch) {
                acknowledged = true;
                break;
            }
            if (shared_->deviceStopped.load(std::memory_order_acquire) && !ma_device_is_started(&shared_->device)) {
                acknowledged = true;  // a stopped device runs no callback
                shared_->gateGain = 0.0f;
                break;
            }
            if (GetTickCount64() >= until) break;
            Sleep(1);
        }
    }
    shared_->playLimit.store(UINT64_MAX, std::memory_order_release);
    if (acknowledged) {
        ma_pcm_rb_reset(&shared_->ring);
        const uint64_t consumed = shared_->consumed.load(std::memory_order_acquire);
        shared_->produced.store(consumed, std::memory_order_release);
        shared_->discardBefore.store(consumed, std::memory_order_release);
        if (manualLocked) ReleaseSRWLockExclusive(&shared_->renderLock);
        return true;
    }
    // The reader may still be inside the callback: never reset the ring. Let it drop what is there.
    shared_->discardBefore.store(shared_->produced.load(std::memory_order_acquire), std::memory_order_release);
    return false;
}

void AudioOutput::OpenGate() {
    if (!initialised_) return;
    shared_->gateOpen.store(true, std::memory_order_release);
}

bool AudioOutput::GateOpen() const {
    return initialised_ && shared_->gateOpen.load(std::memory_order_acquire);
}

void AudioOutput::SetUserHold(bool hold) {
    if (shared_) shared_->userHold.store(hold, std::memory_order_release);
}

bool AudioOutput::UserHold() const {
    return shared_ && shared_->userHold.load(std::memory_order_acquire);
}

void AudioOutput::SetVolume(float linearGain) {
    if (shared_) shared_->volume.store(linearGain, std::memory_order_relaxed);
}

float AudioOutput::Volume() const {
    return shared_ ? shared_->volume.load(std::memory_order_relaxed) : 1.0f;
}

void AudioOutput::SetPlayLimit(uint64_t frame) {
    if (shared_) shared_->playLimit.store(frame, std::memory_order_release);
}

uint64_t AudioOutput::PlayLimit() const {
    return shared_ ? shared_->playLimit.load(std::memory_order_acquire) : UINT64_MAX;
}

uint64_t AudioOutput::Consumed() const {
    return shared_ ? shared_->consumed.load(std::memory_order_acquire) : 0;
}

uint64_t AudioOutput::Produced() const {
    return shared_ ? shared_->produced.load(std::memory_order_acquire) : 0;
}

uint32_t AudioOutput::Underruns() const {
    return shared_ ? shared_->underruns.load(std::memory_order_relaxed) : 0;
}

uint64_t AudioOutput::Callbacks() const {
    return shared_ ? shared_->callbacks.load(std::memory_order_relaxed) : 0;
}

void AudioOutput::Render(float* out, uint32_t frames) {
    memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
    if (!shared_ || mode_ != OutputMode::Manual) return;
    AcquireSRWLockExclusive(&shared_->renderLock);
    if (shared_->ringReady.load(std::memory_order_acquire)) Callback(shared_, out, frames);
    ReleaseSRWLockExclusive(&shared_->renderLock);
}

void AudioOutput::SimulateDeviceLoss() {
    if (shared_) shared_->lossSimulated.store(true, std::memory_order_relaxed);
}

}  // namespace music

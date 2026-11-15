#include "loudness.h"

#include "source.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace music {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAbsoluteGateLufs = -70.0;
constexpr double kRelativeGateLu = -10.0;
constexpr uint32_t kMeasureChunk = 4096;

double Loudness(double weightedMeanSquare) {
    return -0.691 + 10.0 * std::log10(weightedMeanSquare);
}

}  // namespace

bool LoudnessMeter::Init(uint32_t rate, uint32_t channels) {
    if (rate < 8000 || channels == 0 || channels > 8) return false;
    channels_ = channels;
    // K-weighting, BS.1770-4: a high shelf (head acoustics) then a high-pass (the RLB curve),
    // designed for any sample rate from the analogue prototype. The prototype's constants are the
    // ones libebur128 (MIT) uses; at 48 kHz they give the coefficients printed in the standard.
    double f0 = 1681.974450955533;
    const double gainDb = 3.999843853973347;
    double q = 0.7071752369554196;
    double k = std::tan(kPi * f0 / rate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    double a0 = 1.0 + k / q + k * k;
    shelf_ = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
              2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = std::tan(kPi * f0 / rate);
    a0 = 1.0 + k / q + k * k;
    highPass_ = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};

    for (uint32_t c = 0; c < 8; ++c) {
        // L, R, C count once, LFE not at all, the surround pair 1.41 times (BS.1770-4 table 3).
        weight_[c] = c == 3 ? 0.0 : (c == 4 || c == 5) ? 1.41 : 1.0;
        for (double& s : state_[c]) s = 0;
    }
    if (channels == 1) weight_[0] = 2.0;   // mono plays on both speakers
    subBlockFrames_ = rate / 10;
    subBlockFill_ = 0;
    subBlockSum_ = 0;
    recentCount_ = 0;
    blocks_.clear();
    peak_ = 0;
    return true;
}

void LoudnessMeter::Add(const float* in, uint32_t frames) {
    if (channels_ == 0) return;
    for (uint32_t f = 0; f < frames; ++f) {
        double sum = 0;
        for (uint32_t c = 0; c < channels_; ++c) {
            const float sample = in[static_cast<size_t>(f) * channels_ + c];
            peak_ = std::max(peak_, std::fabs(sample));
            double* z = state_[c];
            // Two biquads, transposed direct form II.
            const double x = sample;
            const double y1 = shelf_.b0 * x + z[0];
            z[0] = shelf_.b1 * x - shelf_.a1 * y1 + z[1];
            z[1] = shelf_.b2 * x - shelf_.a2 * y1;
            const double y2 = highPass_.b0 * y1 + z[2];
            z[2] = highPass_.b1 * y1 - highPass_.a1 * y2 + z[3];
            z[3] = highPass_.b2 * y1 - highPass_.a2 * y2;
            sum += weight_[c] * y2 * y2;
        }
        subBlockSum_ += sum;
        if (++subBlockFill_ < subBlockFrames_) continue;
        // A 100 ms step is complete: the 400 ms block ending here is the last four steps.
        recent_[recentCount_ % 4] = subBlockSum_ / subBlockFrames_;
        ++recentCount_;
        subBlockSum_ = 0;
        subBlockFill_ = 0;
        if (recentCount_ >= 4) blocks_.push_back((recent_[0] + recent_[1] + recent_[2] + recent_[3]) / 4.0);
    }
}

bool LoudnessMeter::Integrated(double* lufs) const {
    const double absoluteGate = std::pow(10.0, (kAbsoluteGateLufs + 0.691) / 10.0);
    double sum = 0;
    size_t count = 0;
    for (const double z : blocks_) {
        if (z > absoluteGate) {
            sum += z;
            ++count;
        }
    }
    if (count == 0) return false;
    const double relativeGate = sum / count * std::pow(10.0, kRelativeGateLu / 10.0);
    const double gate = std::max(absoluteGate, relativeGate);
    sum = 0;
    count = 0;
    for (const double z : blocks_) {
        if (z > gate) {
            sum += z;
            ++count;
        }
    }
    if (count == 0) return false;
    *lufs = Loudness(sum / count);
    return true;
}

float LevelGain(double lufs, float peak) {
    double gain = std::pow(10.0, std::min(kTargetLufs - lufs, kMaxBoostDb) / 20.0);
    // Turning a song down is always safe; raising it stops where its loudest sample reaches 0.95.
    if (gain > 1.0 && peak > 0) gain = std::max(1.0, std::min(gain, 0.95 / peak));
    return static_cast<float>(gain);
}

LoudnessResult MeasureFile(const std::wstring& path, const std::atomic<bool>& stop, bool* stopped) {
    LoudnessResult result;
    *stopped = false;
    OpenError error = OpenError::None;
    std::unique_ptr<Source> source = OpenSource(path, &error);
    if (!source) return result;
    const SourceInfo info = source->Info();
    LoudnessMeter meter;
    if (!meter.Init(info.sampleRate, info.channels)) return result;
    std::vector<float> buffer(static_cast<size_t>(kMeasureChunk) * info.channels);
    for (;;) {
        if (stop.load(std::memory_order_relaxed)) {
            *stopped = true;
            return result;
        }
        uint32_t got = 0;
        bool failed = false;
        if (!GuardedRead(source.get(), buffer.data(), kMeasureChunk, &got, &failed)) {
            AbandonSource(source);  // the decoder faulted; its state cannot be trusted
            return result;
        }
        meter.Add(buffer.data(), got);
        if (got < kMeasureChunk || failed) break;
    }
    result.measured = meter.Integrated(&result.lufs);
    result.peak = meter.Peak();
    return result;
}

}  // namespace music

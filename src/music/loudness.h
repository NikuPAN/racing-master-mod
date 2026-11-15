#pragma once

// Even volume (0.3.2): measures how loud a song is, so the whole list can play at one level.
//
// The measure is the integrated loudness of EBU R128 / ITU-R BS.1770-4 (the one streaming
// services and most music players use): K-weighting, mean square over 400 ms blocks that overlap
// by 75 %, an absolute gate at -70 LUFS and a relative gate 10 LU below the first average.
// The sample peak is kept too, so a quiet song is never raised into clipping.
//
// The owner (2026-11-15): "maintain the sound track at about the same level for the whole list,
// like a DAP does", by measuring every track once (not guessing while it plays).

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace music {

// Where every song is brought to. -14 LUFS is the level Spotify and YouTube play at, so songs
// sound as loud as listeners expect from those apps.
constexpr double kTargetLufs = -14.0;
// A song is never raised more than this, and never so far that its loudest sample passes 0.95.
constexpr double kMaxBoostDb = 12.0;

class LoudnessMeter {
public:
    // `rate` in Hz, `channels` 1..8 in the usual file order (L R C LFE Ls Rs ...). One channel is
    // counted twice, because the player sends it to both speakers.
    bool Init(uint32_t rate, uint32_t channels);
    void Add(const float* interleaved, uint32_t frames);
    // Integrated loudness in LUFS; false when no block passed the gates (silence, or shorter than
    // 400 ms).
    bool Integrated(double* lufs) const;
    float Peak() const { return peak_; }

private:
    struct Biquad {
        double b0, b1, b2, a1, a2;
    };
    uint32_t channels_ = 0;
    Biquad shelf_{}, highPass_{};
    double state_[8][4] = {};      // per channel: shelf z1 z2, high-pass z1 z2
    double weight_[8] = {};
    uint32_t subBlockFrames_ = 0;  // 100 ms
    uint32_t subBlockFill_ = 0;
    double subBlockSum_ = 0;       // weighted sum of squares in the sub-block being filled
    double recent_[4] = {};        // the last four sub-blocks (one 400 ms block)
    uint32_t recentCount_ = 0;
    std::vector<double> blocks_;   // weighted mean square of every complete 400 ms block
    float peak_ = 0;
};

// The gain (linear) that brings a song measured at `lufs` with sample peak `peak` to kTargetLufs,
// capped as above.
float LevelGain(double lufs, float peak);

struct LoudnessResult {
    bool measured = false;  // false: the file could not be decoded, or it is silent
    double lufs = 0;
    float peak = 0;
};

// Decodes the whole file and measures it. Runs on the measuring thread only. `stop` is checked
// between chunks; a stopped measurement returns measured = false and *stopped = true.
LoudnessResult MeasureFile(const std::wstring& path, const std::atomic<bool>& stop, bool* stopped);

}  // namespace music

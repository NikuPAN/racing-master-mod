// WAV, FLAC and MP3 through miniaudio's decoders (dr_wav, dr_flac, dr_mp3).
#include "source.h"

#include "miniaudio_config.h"
#include "miniaudio.h"
#include "text.h"

namespace music {

namespace {

class MiniaudioSource : public Source {
public:
    MiniaudioSource(const std::wstring& pathX, ma_encoding_format format) : pathX_(pathX), format_(format) {}

    ~MiniaudioSource() override {
        if (open_) ma_decoder_uninit(&decoder_);
    }

    bool Open(ma_uint32 seekPoints) {
        if (open_) {
            ma_decoder_uninit(&decoder_);
            open_ = false;
        }
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);  // native channels and rate
        config.encodingFormat = format_;                                        // exactly one format, never a guess
        config.seekPointCount = seekPoints;
        if (ma_decoder_init_file_w(pathX_.c_str(), &config, &decoder_) != MA_SUCCESS) return false;
        open_ = true;
        info_.sampleRate = decoder_.outputSampleRate;
        info_.channels = decoder_.outputChannels;
        ma_uint64 length = 0;
        info_.lengthFrames = ma_decoder_get_length_in_pcm_frames(&decoder_, &length) == MA_SUCCESS && length > 0
                                 ? static_cast<int64_t>(length)
                                 : -1;
        return info_.sampleRate != 0 && info_.channels != 0;
    }

    SourceInfo Info() const override { return info_; }

    uint32_t Read(float* out, uint32_t frames, bool* failed) override {
        if (!open_) {  // a re-open for the seek table failed: the file changed or vanished
            *failed = true;
            return 0;
        }
        ma_uint64 got = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder_, out, frames, &got);
        if (result != MA_SUCCESS && result != MA_AT_END) {
            *failed = got == 0;  // a read error before any frame: the track breaks here
        }
        return static_cast<uint32_t>(got);
    }

    bool Seek(int64_t frame) override {
        if (frame < 0) return false;
        if (format_ == ma_encoding_format_mp3 && !seekTable_) {
            // The first seek in an MP3 re-opens the decoder with a seek table: the open stays fast
            // for gapless play and every seek from now on costs under a millisecond.
            seekTable_ = true;
            if (!Open(128)) return false;
        }
        return open_ && ma_decoder_seek_to_pcm_frame(&decoder_, static_cast<ma_uint64>(frame)) == MA_SUCCESS;
    }

private:
    std::wstring pathX_;
    ma_encoding_format format_;
    ma_decoder decoder_ = {};
    bool open_ = false;
    bool seekTable_ = false;
    SourceInfo info_;
};

}  // namespace

std::unique_ptr<Source> OpenMiniaudioSource(const std::wstring& path, Kind kind, OpenError* error) {
    ma_encoding_format format = ma_encoding_format_unknown;
    switch (kind) {
        case Kind::Wav: format = ma_encoding_format_wav; break;
        case Kind::Flac: format = ma_encoding_format_flac; break;
        case Kind::Mp3: format = ma_encoding_format_mp3; break;
        default: *error = OpenError::Unsupported; return nullptr;
    }
    std::unique_ptr<MiniaudioSource> source(new MiniaudioSource(ToExtendedPath(path), format));
    if (!source->Open(0)) {
        // With a format named, miniaudio reports every failure the same way; Missing and
        // AccessDenied were decided by the sniff before this call, so what is left is Damaged.
        *error = OpenError::Damaged;
        return nullptr;
    }
    if (source->Info().lengthFrames == 0) {
        *error = OpenError::Empty;
        return nullptr;
    }
    *error = OpenError::None;
    return source;
}

}  // namespace music

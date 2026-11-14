// OGG Vorbis through stb_vorbis's pull API over a FILE* from _wfopen (the narrow-path opener
// cannot take a Chinese path).
//
// Ownership rule: stb_vorbis_open_file(f, 1, ...) owns the FILE* from that call on, ALSO when
// the open fails (it closes the file on its failure path). Never fclose it here.
#include "source.h"

#include "tag_reader.h"
#include "text.h"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include <cstdio>
#include <cstring>

namespace music {

namespace {

stb_vorbis* OpenVorbis(const std::wstring& path, OpenError* error) {
    const std::wstring pathX = ToExtendedPath(path);
    FILE* file = _wfopen(pathX.c_str(), L"rb");
    if (!file) {
        *error = OpenError::AccessDenied;
        return nullptr;
    }
    int code = 0;
    stb_vorbis* vorbis = stb_vorbis_open_file(file, 1, &code, nullptr);  // stb_vorbis now owns `file`
    if (!vorbis) {
        *error = OpenError::Damaged;
        return nullptr;
    }
    return vorbis;
}

class VorbisSource : public Source {
public:
    explicit VorbisSource(stb_vorbis* vorbis) : vorbis_(vorbis) {
        const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
        info_.sampleRate = info.sample_rate;
        info_.channels = static_cast<uint32_t>(info.channels);
        const unsigned length = stb_vorbis_stream_length_in_samples(vorbis_);
        info_.lengthFrames = length > 0 ? static_cast<int64_t>(length) : -1;
    }
    ~VorbisSource() override { stb_vorbis_close(vorbis_); }  // closes the file too

    SourceInfo Info() const override { return info_; }

    uint32_t Read(float* out, uint32_t frames, bool* failed) override {
        const int got = stb_vorbis_get_samples_float_interleaved(vorbis_, static_cast<int>(info_.channels), out,
                                                                  static_cast<int>(frames * info_.channels));
        if (got < 0) {
            *failed = true;
            return 0;
        }
        if (got == 0 && stb_vorbis_get_error(vorbis_) != VORBIS__no_error) {
            // Distinguish a clean end from a broken stream; either way the track ends here.
            *failed = false;
        }
        return static_cast<uint32_t>(got);
    }

    bool Seek(int64_t frame) override {
        if (frame < 0 || frame > 0xFFFFFFFFll) return false;
        return stb_vorbis_seek(vorbis_, static_cast<unsigned>(frame)) != 0;
    }

private:
    stb_vorbis* vorbis_;
    SourceInfo info_;
};

}  // namespace

std::unique_ptr<Source> OpenVorbisSource(const std::wstring& path, OpenError* error) {
    stb_vorbis* vorbis = OpenVorbis(path, error);
    if (!vorbis) return nullptr;
    std::unique_ptr<VorbisSource> source(new VorbisSource(vorbis));
    if (source->Info().sampleRate == 0 || source->Info().channels == 0) {
        *error = OpenError::Damaged;
        return nullptr;
    }
    *error = OpenError::None;
    return source;
}

bool ReadVorbisTags(const std::wstring& path, Tags* tags) {
    OpenError error = OpenError::None;
    stb_vorbis* vorbis = OpenVorbis(path, &error);
    if (!vorbis) return false;
    const stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    const unsigned length = stb_vorbis_stream_length_in_samples(vorbis);
    if (info.sample_rate > 0 && length > 0) tags->durationSeconds = static_cast<double>(length) / info.sample_rate;
    const stb_vorbis_comment comment = stb_vorbis_get_comment(vorbis);
    for (int i = 0; i < comment.comment_list_length; ++i) {
        const char* entry = comment.comment_list[i];
        if (!entry) continue;
        if (_strnicmp(entry, "TITLE=", 6) == 0 && tags->title.empty()) tags->title = FromUtf8(entry + 6);
        else if (_strnicmp(entry, "ARTIST=", 7) == 0 && tags->artist.empty()) tags->artist = FromUtf8(entry + 7);
    }
    stb_vorbis_close(vorbis);
    return true;
}

}  // namespace music

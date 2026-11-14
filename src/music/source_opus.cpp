// Opus in an Ogg container (.opus, and .ogg/.oga files that hold Opus) through libopusfile,
// which handles seeking, chained streams and malformed input on top of libopus and libogg.
//
// The file is read through the engine's own Windows handle (opened from the \\?\ path), so no C
// runtime FILE* is involved. libopusfile does not take ownership of the stream when the open
// fails, so this file closes the handle on that path itself; after a successful open, op_free
// calls the close callback.
//
// Output is always 48 kHz stereo float: Opus is defined at 48 kHz, and op_read_float_stereo
// downmixes or upmixes any channel layout.
#include "source.h"

#include "tag_reader.h"
#include "text.h"

#include <windows.h>

#include <opusfile.h>

#include <cstdio>
#include <cstring>

namespace music {

namespace {

int ReadCallback(void* stream, unsigned char* out, int bytes) {
    DWORD read = 0;
    if (bytes <= 0) return 0;
    if (!ReadFile(static_cast<HANDLE>(stream), out, static_cast<DWORD>(bytes), &read, nullptr)) return -1;
    return static_cast<int>(read);
}

int SeekCallback(void* stream, opus_int64 offset, int whence) {
    LARGE_INTEGER move;
    move.QuadPart = offset;
    const DWORD method = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
    return SetFilePointerEx(static_cast<HANDLE>(stream), move, nullptr, method) ? 0 : -1;
}

opus_int64 TellCallback(void* stream) {
    LARGE_INTEGER zero = {};
    LARGE_INTEGER position = {};
    if (!SetFilePointerEx(static_cast<HANDLE>(stream), zero, &position, FILE_CURRENT)) return -1;
    return position.QuadPart;
}

int CloseCallback(void* stream) {
    return CloseHandle(static_cast<HANDLE>(stream)) ? 0 : -1;
}

const OpusFileCallbacks kCallbacks = {ReadCallback, SeekCallback, TellCallback, CloseCallback};

OggOpusFile* OpenOpus(const std::wstring& path, OpenError* error) {
    const std::wstring pathX = ToExtendedPath(path);
    const HANDLE file = CreateFileW(pathX.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = OpenError::AccessDenied;
        return nullptr;
    }
    int code = 0;
    OggOpusFile* opus = op_open_callbacks(file, &kCallbacks, nullptr, 0, &code);
    if (!opus) {
        CloseHandle(file);  // the library does not own the stream after a failed open
        *error = OpenError::Damaged;
        return nullptr;
    }
    return opus;
}

class OpusSource : public Source {
public:
    explicit OpusSource(OggOpusFile* opus) : opus_(opus) {
        info_.sampleRate = 48000;
        info_.channels = 2;
        const ogg_int64_t total = op_pcm_total(opus_, -1);
        info_.lengthFrames = total > 0 ? static_cast<int64_t>(total) : -1;
    }
    ~OpusSource() override { op_free(opus_); }

    SourceInfo Info() const override { return info_; }

    uint32_t Read(float* out, uint32_t frames, bool* failed) override {
        uint32_t done = 0;
        int holes = 0;
        while (done < frames) {
            const int got = op_read_float_stereo(opus_, out + static_cast<size_t>(done) * 2, static_cast<int>((frames - done) * 2));
            if (got == 0) break;  // end of file
            if (got < 0) {
                // A damaged page: the library skipped it and reading can go on. A file that is
                // nothing but holes ends here instead of keeping the player thread busy.
                if (got == OP_HOLE && ++holes < 64) continue;
                *failed = done == 0;
                break;
            }
            holes = 0;
            done += static_cast<uint32_t>(got);
        }
        return done;
    }

    bool Seek(int64_t frame) override {
        if (frame < 0) return false;
        return op_pcm_seek(opus_, frame) == 0;
    }

private:
    OggOpusFile* opus_;
    SourceInfo info_;
};

}  // namespace

std::unique_ptr<Source> OpenOpusSource(const std::wstring& path, OpenError* error) {
    OggOpusFile* opus = OpenOpus(path, error);
    if (!opus) return nullptr;
    *error = OpenError::None;
    return std::unique_ptr<Source>(new OpusSource(opus));
}

bool ReadOpusTags(const std::wstring& path, Tags* tags) {
    OpenError error = OpenError::None;
    OggOpusFile* opus = OpenOpus(path, &error);
    if (!opus) return false;
    const ogg_int64_t total = op_pcm_total(opus, -1);
    if (total > 0) tags->durationSeconds = static_cast<double>(total) / 48000.0;
    const OpusTags* comments = op_tags(opus, -1);
    if (comments) {
        const char* title = opus_tags_query(comments, "TITLE", 0);
        const char* artist = opus_tags_query(comments, "ARTIST", 0);
        if (title) tags->title = FromUtf8(title);
        if (artist) tags->artist = FromUtf8(artist);
    }
    op_free(opus);
    return true;
}

}  // namespace music

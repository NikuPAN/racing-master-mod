#pragma once

// One open music file, producing interleaved 32-bit float at its native rate and channel count.
// Four implementations: miniaudio (WAV, FLAC, MP3), stb_vorbis (OGG), opusfile (Opus) and the
// Media Foundation Source Reader (M4A/AAC/ALAC, raw ADTS .aac).
//
// OpenSource decides the decoder by file content first and extension second, never by trial
// and error: a damaged FLAC handed to the MP3 decoder would play as noise.
//
// Every call into a decoder that the player thread makes goes through the Guarded* functions,
// which contain an access violation inside third-party code (the track is reported as damaged
// and the decoder object is abandoned without running its clean-up; its state cannot be trusted).

#include <cstdint>
#include <memory>
#include <string>

namespace music {

struct SourceInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    int64_t lengthFrames = -1;  // -1 = unknown
};

class Source {
public:
    virtual ~Source() = default;
    virtual SourceInfo Info() const = 0;
    // Fewer frames than asked = end of file. *failed is set when the decoder broke mid-track.
    virtual uint32_t Read(float* out, uint32_t frames, bool* failed) = 0;
    virtual bool Seek(int64_t frame) = 0;
};

enum class OpenError {
    None,
    Missing,       // the file does not exist (any more)
    AccessDenied,  // locked by another program or no permission
    OnlineOnly,    // a cloud placeholder that would have to be downloaded first
    Unsupported,   // not a format the engine plays
    Damaged,       // the decoder refused the file
    NoAacDecoder,  // Media Foundation is not on this Windows edition
    Empty,         // the file holds no audio at all
};

enum class Kind { Unknown, Wav, Flac, Mp3, Vorbis, Opus, MediaFoundation };

// Steps 1-3 of the sniffing rule. Returns Kind::Unknown with *error set when the file cannot be
// used; a recognised file returns its kind with *error = None.
Kind SniffFile(const std::wstring& path, OpenError* error);

// Opens the file with the right decoder. Returns null with *error set on failure.
std::unique_ptr<Source> OpenSource(const std::wstring& path, OpenError* error);

// Plain-English wording for the list and the status line.
const wchar_t* DescribeProblem(OpenError error);

// The exception filter of every guard: faults inside a decoder that are turned into "this file
// is damaged" (access violation, in-page error, misalignment, integer divide, illegal
// instruction). Anything else (stack overflow, a breakpoint) is not swallowed. A /GS cookie
// failure or the C runtime's invalid-parameter handler ends the process without an exception and
// cannot be caught at all; that is why every FILE* the engine hands to the C runtime is checked.
int ContainableFault(unsigned long code);

// Guarded calls (SEH): return false if the decoder faulted; the caller then abandons `source`
// with AbandonSource (releases nothing, the object is leaked on purpose).
bool GuardedRead(Source* source, float* out, uint32_t frames, uint32_t* got, bool* failed);
bool GuardedSeek(Source* source, int64_t frame, bool* ok);
void AbandonSource(std::unique_ptr<Source>& source);

// Per-format openers (source_*.cpp). `kind` is what SniffFile found.
std::unique_ptr<Source> OpenMiniaudioSource(const std::wstring& path, Kind kind, OpenError* error);
std::unique_ptr<Source> OpenVorbisSource(const std::wstring& path, OpenError* error);
std::unique_ptr<Source> OpenOpusSource(const std::wstring& path, OpenError* error);
std::unique_ptr<Source> OpenMediaFoundationSource(const std::wstring& path, OpenError* error);

// True once mfplat.dll and mfreadwrite.dll were loaded from System32 and MFStartup succeeded.
// False on Windows editions without Media Foundation; then no Media Foundation function is
// ever called. Call from any thread; the first call does the work.
bool MediaFoundationAvailable();

}  // namespace music

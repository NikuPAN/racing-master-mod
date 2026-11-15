#pragma once

// Shared pieces of musictest.exe, the silent test program for the music engine.
//
// Each musictest_*.cpp file registers its cases with MT_TEST; musictest.cpp runs them in order
// of their number and prints one line per check: "PASS <name>", "FAIL <name> -- <detail>" or
// "SKIPPED <name> -- <reason>". tools\run-host-test.ps1 records every line. Nothing makes a
// sound (no audio device except miniaudio's null backend) and no window or dialog is shown.

#include <cstdint>
#include <string>
#include <vector>

namespace mt {

using TestFn = void (*)();
struct Registrar {
    Registrar(int order, const char* title, TestFn fn);
};
#define MT_TEST(order, id, title)                       \
    static void id();                                   \
    static const mt::Registrar id##_registrar(order, title, id); \
    static void id()

// ---- results ----
void Pass(const char* name);
void Fail(const char* name, const char* format, ...);
void Skip(const char* name, const char* reason);
// Pass or Fail; returns `ok`. The detail is only printed on failure.
bool Check(bool ok, const char* name, const char* format = "", ...);

// ---- folders ----
const std::wstring& WorkDir();   // argv[1]: a temporary folder for this run
const std::wstring& RepoDir();   // argv[2]: the repository (for committed fixtures)
// The folder every generated fixture goes into; its name holds Traditional and Simplified
// Chinese, a space, '#' and '%'.
const std::wstring& FixtureDir();
// A new empty folder under the work folder (for an engine's data folder).
std::wstring NewDir(const std::wstring& name);
bool WriteBytes(const std::wstring& path, const std::string& bytes);
std::string ReadBytes(const std::wstring& path);
bool Exists(const std::wstring& path);

// ---- fixtures (musictest_fixtures.cpp) ----
constexpr double kAmplitude = 0.25;          // every generated sine
constexpr double kExpectedRms = 0.17677669;  // kAmplitude / sqrt(2)
// 16-bit PCM WAV of a sine, the same in every channel. `startSample` continues an earlier file's
// sine without a jump (the gapless fixtures are one sine cut in two). Accepts \\?\ paths.
bool WriteWav(const std::wstring& path, uint32_t rate, uint32_t channels, uint32_t frames, double frequency,
              uint64_t startSample = 0, double amplitude = kAmplitude);
// Encodes a sine with a Windows Media Foundation encoder (FLAC, MP3, AAC in .m4a). Returns the
// HRESULT; the caller reports SKIPPED when this PC has no such encoder.
long EncodeWithMediaFoundation(const std::wstring& path, const wchar_t* format, uint32_t rate, double seconds,
                               double frequency);
// Ogg Opus written with libopus and libogg (musictest_opus.cpp).
bool WriteOpus(const std::wstring& path, double frequency, double seconds, const char* title, const char* artist);
// Ogg Vorbis written with libvorbis, which this test program links and the mod does not
// (musictest_vorbis.cpp).
bool WriteVorbis(const std::wstring& path, uint32_t rate, double frequency, double seconds, const char* title, const char* artist);

// ---- signal checks on interleaved float audio ----
struct Signal {
    std::vector<float> samples;  // interleaved
    uint32_t channels = 2;
    uint32_t rate = 48000;
    size_t Frames() const { return channels ? samples.size() / channels : 0; }
};
double Rms(const Signal& s, size_t fromFrame, size_t toFrame);           // channel 0
double Frequency(const Signal& s, size_t fromFrame, size_t toFrame);     // from upward zero crossings, channel 0
double MaxStep(const Signal& s, size_t fromFrame, size_t toFrame);       // largest sample-to-sample change
size_t LongestZeroRun(const Signal& s, size_t fromFrame, size_t toFrame);
// RMS within 5 % of kExpectedRms and frequency within 1 % of `frequency`, measured over the
// middle 70 % of the signal (encoder delay and fades at the ends are left out). Two checks.
void CheckSine(const std::string& name, const Signal& s, double frequency);
// Decodes a whole file through the engine's own OpenSource (native rate and channels). Returns
// false and the engine's OpenError as a number when it cannot be opened (musictest_miniaudio.cpp).
bool DecodeFile(const std::wstring& path, Signal* out, int* openError, int64_t* lengthFrames);
// Labels 50 ms windows with the letter of the nearest known frequency ('.' = silence, '?' =
// none) and collapses repeats: "AB." means A, then B, then silence.
std::string Sequence(const Signal& s, const std::vector<std::pair<char, double>>& known);
// Index of the first 50 ms window labelled `letter`, as a time in seconds; -1 if none.
double FirstWindow(const Signal& s, const std::vector<std::pair<char, double>>& known, char letter);

}  // namespace mt

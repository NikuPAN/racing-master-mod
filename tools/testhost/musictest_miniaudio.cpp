// WAV, FLAC and MP3 through miniaudio (src\music\source_miniaudio.cpp), the content sniffing
// (src\music\source.cpp) and the files that must be refused cleanly. Fixtures live in a folder
// whose name holds Chinese, a space, '#' and '%'; one more sits under a path longer than 260
// characters.
#include "musictest.h"

#include "music/source.h"

#include <windows.h>

#include <cstdio>
#include <memory>

using namespace music;

namespace mt {

bool DecodeFile(const std::wstring& path, Signal* out, int* openError, int64_t* lengthFrames) {
    OpenError error = OpenError::None;
    std::unique_ptr<Source> source = OpenSource(path, &error);
    *openError = static_cast<int>(error);
    *lengthFrames = -1;
    out->samples.clear();
    if (!source) return false;
    const SourceInfo info = source->Info();
    out->rate = info.sampleRate;
    out->channels = info.channels;
    *lengthFrames = info.lengthFrames;
    std::vector<float> buffer(static_cast<size_t>(4096) * info.channels);
    for (;;) {
        uint32_t got = 0;
        bool failed = false;
        if (!GuardedRead(source.get(), buffer.data(), 4096, &got, &failed)) {
            AbandonSource(source);
            return false;
        }
        out->samples.insert(out->samples.end(), buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(got) * info.channels);
        if (got < 4096 || failed) break;
    }
    return true;
}

}  // namespace mt

namespace {

Kind Sniff(const std::wstring& path) {
    OpenError error = OpenError::None;
    return SniffFile(path, &error);
}

void DecodeAndCheck(const char* format, const std::wstring& path, Kind kind, double frequency, uint32_t rate, int64_t exactLength) {
    char name[160];
    snprintf(name, sizeof(name), "%s: recognised by its content", format);
    mt::Check(Sniff(path) == kind, name, "sniffed as kind %d", static_cast<int>(Sniff(path)));
    mt::Signal signal;
    int error = 0;
    int64_t length = 0;
    snprintf(name, sizeof(name), "%s: opens and decodes from a Chinese folder name", format);
    if (!mt::Check(mt::DecodeFile(path, &signal, &error, &length), name, "open error %d", error)) return;
    snprintf(name, sizeof(name), "%s: native rate and two channels", format);
    mt::Check(signal.rate == rate && signal.channels == 2, name, "rate %u, channels %u", signal.rate, signal.channels);
    if (exactLength > 0) {
        snprintf(name, sizeof(name), "%s: exact length known at open and decoded in full", format);
        mt::Check(length == exactLength && static_cast<int64_t>(signal.Frames()) == exactLength, name,
                  "length %lld, decoded %zu, expected %lld", static_cast<long long>(length), signal.Frames(),
                  static_cast<long long>(exactLength));
    }
    mt::CheckSine(format, signal, frequency);
}

}  // namespace

MT_TEST(300, DecodeWav, "decoding: WAV (miniaudio)") {
    const std::wstring path = mt::FixtureDir() + L"\\測試 wav #1 100%.wav";
    mt::Check(mt::WriteWav(path, 44100, 2, 44100, 440.0), "WAV fixture written");
    DecodeAndCheck("WAV 44.1 kHz", path, Kind::Wav, 440.0, 44100, 44100);

    // Nested folders until the path is longer than 260 characters.
    std::wstring deep = mt::FixtureDir();
    while (deep.size() < 280) {
        deep += L"\\長路徑 简体 folder";
        CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr);
    }
    const std::wstring longPath = deep + L"\\long 測試.wav";
    mt::Check(mt::WriteWav(L"\\\\?\\" + longPath, 48000, 2, 24000, 660.0), "WAV fixture under a path longer than 260 characters");
    mt::Signal signal;
    int error = 0;
    int64_t length = 0;
    char name[128];
    snprintf(name, sizeof(name), "WAV: a %zu-character path opens and decodes", longPath.size());
    if (mt::Check(mt::DecodeFile(longPath, &signal, &error, &length), name, "open error %d", error)) {
        mt::CheckSine("WAV under a long path", signal, 660.0);
    }
}

MT_TEST(310, DecodeFlac, "decoding: FLAC (miniaudio)") {
    const std::wstring path = mt::FixtureDir() + L"\\測試 flac #1 100%.flac";
    const long hr = mt::EncodeWithMediaFoundation(path, L"flac", 44100, 1.0, 660.0);
    if (hr < 0) {
        char reason[96];
        snprintf(reason, sizeof(reason), "this PC's Media Foundation has no FLAC encoder (0x%08lx)", static_cast<unsigned long>(hr));
        mt::Skip("FLAC decoding", reason);
        return;
    }
    DecodeAndCheck("FLAC", path, Kind::Flac, 660.0, 44100, 44100);
}

MT_TEST(320, DecodeMp3, "decoding: MP3 (miniaudio), seeking with the seek table") {
    const std::wstring path = mt::FixtureDir() + L"\\測試 mp3 #1 100%.mp3";
    const long hr = mt::EncodeWithMediaFoundation(path, L"mp3", 44100, 1.0, 880.0);
    if (hr < 0) {
        char reason[96];
        snprintf(reason, sizeof(reason), "this PC's Media Foundation has no MP3 encoder (0x%08lx)", static_cast<unsigned long>(hr));
        mt::Skip("MP3 decoding", reason);
        return;
    }
    DecodeAndCheck("MP3", path, Kind::Mp3, 880.0, 44100, 0);

    // Two seeks: the first re-opens the decoder with a seek table, the second uses it.
    OpenError error = OpenError::None;
    std::unique_ptr<Source> source = OpenSource(path, &error);
    if (!mt::Check(source != nullptr, "MP3: opens for seeking")) return;
    const int64_t length = source->Info().lengthFrames;
    auto remainingAfterSeek = [&](int64_t frame) -> int64_t {
        bool ok = false;
        if (!GuardedSeek(source.get(), frame, &ok) || !ok) return -1;
        std::vector<float> buffer(4096 * 2);
        int64_t total = 0;
        for (;;) {
            uint32_t got = 0;
            bool failed = false;
            if (!GuardedRead(source.get(), buffer.data(), 4096, &got, &failed)) return -1;
            total += got;
            if (got < 4096 || failed) break;
        }
        return total;
    };
    const int64_t afterFirst = remainingAfterSeek(26460);   // 0.6 s
    const int64_t afterSecond = remainingAfterSeek(8820);   // 0.2 s
    mt::Check(length > 0 && afterFirst == length - 26460, "MP3: first seek lands on the exact frame", "length %lld, %lld frames left",
              static_cast<long long>(length), static_cast<long long>(afterFirst));
    mt::Check(length > 0 && afterSecond == length - 8820, "MP3: a second seek backwards lands on the exact frame",
              "length %lld, %lld frames left", static_cast<long long>(length), static_cast<long long>(afterSecond));
}

MT_TEST(330, RefusedFiles, "decoding: missing, damaged and wrong files are refused cleanly") {
    const std::wstring dir = mt::FixtureDir();
    OpenError error = OpenError::None;
    std::unique_ptr<Source> source = OpenSource(dir + L"\\不存在 missing.mp3", &error);
    mt::Check(!source && error == OpenError::Missing, "a missing file is reported as missing (not damaged)", "error %d",
              static_cast<int>(error));

    // Random letters named .mp3: no MPEG sync anywhere, so the MP3 decoder must refuse it.
    std::string letters(20000, 'a');
    uint32_t seed = 12345;
    for (char& c : letters) {
        seed = seed * 1103515245u + 12345u;
        c = static_cast<char>('a' + (seed >> 16) % 26);
    }
    mt::WriteBytes(dir + L"\\garbage 測試.mp3", letters);
    source = OpenSource(dir + L"\\garbage 測試.mp3", &error);
    mt::Check(!source && error == OpenError::Damaged, "random bytes named .mp3 are refused as damaged", "error %d",
              static_cast<int>(error));

    mt::WriteBytes(dir + L"\\empty.flac", "");
    source = OpenSource(dir + L"\\empty.flac", &error);
    mt::Check(!source && error == OpenError::Empty, "an empty file is reported as empty", "error %d", static_cast<int>(error));

    mt::WriteBytes(dir + L"\\notes.txt", "just some text");
    source = OpenSource(dir + L"\\notes.txt", &error);
    mt::Check(!source && error == OpenError::Unsupported, "a text file is not music", "error %d", static_cast<int>(error));

    source = OpenSource(dir, &error);
    mt::Check(!source && error == OpenError::Unsupported, "a folder is not a music file", "error %d", static_cast<int>(error));

    // A WAV whose name says .mp3 is played as WAV: content decides, not the name.
    mt::WriteWav(dir + L"\\really a wav.mp3", 48000, 2, 4800, 440.0);
    mt::Check(Sniff(dir + L"\\really a wav.mp3") == Kind::Wav, "content wins over the extension (a WAV named .mp3)");

    // A WAV cut in half: it opens (the header is intact) but holds fewer frames than it claims.
    const std::wstring full = dir + L"\\cut source.wav";
    mt::WriteWav(full, 48000, 2, 48000, 440.0);
    const std::string bytes = mt::ReadBytes(full);
    mt::WriteBytes(dir + L"\\cut in half.wav", bytes.substr(0, bytes.size() / 2));
    mt::Signal signal;
    int code = 0;
    int64_t length = 0;
    const bool opened = mt::DecodeFile(dir + L"\\cut in half.wav", &signal, &code, &length);
    mt::Check(opened && signal.Frames() < 30000, "a file cut in half decodes only what is there, without a fault",
              "opened %d, %zu frames, stated length %lld", opened ? 1 : 0, signal.Frames(), static_cast<long long>(length));
}

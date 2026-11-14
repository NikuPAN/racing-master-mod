// M4A / AAC through the Media Foundation Source Reader (src\music\source_mf.cpp) and the tag
// reader built on Media Foundation's property store (title, artist, duration for MP3, FLAC, WAV,
// M4A).
#include "musictest.h"

#include "music/source.h"
#include "music/tag_reader.h"

#include <cmath>
#include <cstdio>
#include <memory>

using namespace music;

MT_TEST(350, DecodeAac, "decoding: M4A / AAC (Media Foundation)") {
    mt::Check(MediaFoundationAvailable(), "Media Foundation loads from System32 on this PC");
    const std::wstring path = mt::FixtureDir() + L"\\測試 aac #1 100%.m4a";
    const long hr = mt::EncodeWithMediaFoundation(path, L"aac", 44100, 1.0, 550.0);
    if (hr < 0) {
        char reason[96];
        snprintf(reason, sizeof(reason), "this PC's Media Foundation has no AAC encoder (0x%08lx)", static_cast<unsigned long>(hr));
        mt::Skip("M4A / AAC decoding", reason);
    } else {
        OpenError error = OpenError::None;
        mt::Check(SniffFile(path, &error) == Kind::MediaFoundation, "M4A: recognised by its content");
        mt::Signal signal;
        int code = 0;
        int64_t length = 0;
        if (mt::Check(mt::DecodeFile(path, &signal, &code, &length), "M4A: opens and decodes from a Chinese folder name", "error %d", code)) {
            mt::Check(signal.rate == 44100 && signal.channels == 2, "M4A: native rate and two channels", "rate %u, channels %u", signal.rate,
                      signal.channels);
            // AAC keeps the encoder's priming and padding (Media Foundation does not trim them).
            mt::Check(signal.Frames() >= 44100 && signal.Frames() < 44100 + 4096, "M4A: about one second decoded (plus encoder padding)",
                      "%zu frames", signal.Frames());
            mt::CheckSine("M4A / AAC", signal, 550.0);
        }

        std::unique_ptr<Source> source = OpenSource(path, &error);
        if (mt::Check(source != nullptr, "M4A: opens for seeking")) {
            bool ok = false;
            GuardedSeek(source.get(), 22050, &ok);
            std::vector<float> buffer(4096 * 2);
            size_t left = 0;
            for (;;) {
                uint32_t got = 0;
                bool failed = false;
                if (!GuardedRead(source.get(), buffer.data(), 4096, &got, &failed)) break;
                left += got;
                if (got < 4096 || failed) break;
            }
            mt::Check(ok && left + 22050 >= signal.Frames() - 2048 && left + 22050 <= signal.Frames() + 2048,
                      "M4A: seek to 0.5 s leaves about half the file", "%zu frames left of %zu", left, signal.Frames());
        }
    }

    // A text file named .m4a: no MP4 header, so Media Foundation is asked and refuses it.
    const std::wstring text = mt::FixtureDir() + L"\\text 測試.m4a";
    mt::WriteBytes(text, "This is not an audio file, it only has the name of one.\r\n");
    OpenError error = OpenError::None;
    std::unique_ptr<Source> source = OpenSource(text, &error);
    mt::Check(!source && error == OpenError::Damaged, "a text file named .m4a is refused as damaged", "error %d", static_cast<int>(error));
}

MT_TEST(360, TagsMf, "tags: duration and title through Media Foundation") {
    struct Item {
        const wchar_t* name;
        Kind kind;
        double seconds;
    };
    const Item items[] = {
        {L"\\測試 wav #1 100%.wav", Kind::Wav, 1.0},
        {L"\\測試 flac #1 100%.flac", Kind::Flac, 1.0},
        {L"\\測試 mp3 #1 100%.mp3", Kind::Mp3, 1.0},
        {L"\\測試 aac #1 100%.m4a", Kind::MediaFoundation, 1.0},
    };
    for (const Item& item : items) {
        const std::wstring path = mt::FixtureDir() + item.name;
        char name[128];
        snprintf(name, sizeof(name), "tags: %s duration read without decoding",
                 item.kind == Kind::Wav ? "WAV" : item.kind == Kind::Flac ? "FLAC" : item.kind == Kind::Mp3 ? "MP3" : "M4A");
        if (!mt::Exists(path)) {
            mt::Skip(name, "the fixture could not be made on this PC (see the decoding case)");
            continue;
        }
        Tags tags;
        bool faulted = false;
        const bool ok = ReadTags(path, item.kind, &tags, &faulted);
        // Media Foundation's MP3 and AAC durations are estimates (the design brief measured
        // 299.59 s for 300.03 s), hence the tolerance.
        mt::Check(ok && !faulted && std::fabs(tags.durationSeconds - item.seconds) < 0.1, name, "read %d, %.3f s", ok ? 1 : 0,
                  tags.durationSeconds);
    }
}

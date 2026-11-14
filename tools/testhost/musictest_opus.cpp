// Opus in Ogg through libopusfile (src\music\source_opus.cpp). The fixture is made at test time
// with the libopus encoder (built from the same pinned opus release) and a small Ogg writer on
// libogg: OpusHead, OpusTags with a Chinese title, then 20 ms packets, with the final granule
// position set so the decoder trims the encoder's padding (RFC 7845).
#include "musictest.h"

#include "music/source.h"
#include "music/tag_reader.h"
#include "music/text.h"

#include <ogg/ogg.h>
#include <opus.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

using namespace music;

namespace mt {

namespace {

void Le32(std::string* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

bool WritePages(HANDLE file, ogg_stream_state* stream, bool flush) {
    ogg_page page;
    while (flush ? ogg_stream_flush(stream, &page) : ogg_stream_pageout(stream, &page)) {
        DWORD written = 0;
        if (!WriteFile(file, page.header, static_cast<DWORD>(page.header_len), &written, nullptr)) return false;
        if (!WriteFile(file, page.body, static_cast<DWORD>(page.body_len), &written, nullptr)) return false;
    }
    return true;
}

}  // namespace

bool WriteOpus(const std::wstring& path, double frequency, double seconds, const char* title, const char* artist) {
    int error = 0;
    OpusEncoder* encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    if (!encoder || error != OPUS_OK) return false;
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(128000));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        opus_encoder_destroy(encoder);
        return false;
    }
    ogg_stream_state stream;
    ogg_stream_init(&stream, 0x5EED);
    bool ok = true;
    ogg_int64_t packetNumber = 0;

    std::string head = "OpusHead";
    head.push_back(1);  // version
    head.push_back(2);  // channels
    head.push_back(static_cast<char>(lookahead & 0xFF));
    head.push_back(static_cast<char>((lookahead >> 8) & 0xFF));  // pre-skip
    Le32(&head, 48000);                                         // original rate
    head.push_back(0);
    head.push_back(0);  // output gain
    head.push_back(0);  // mapping family 0 (mono or stereo)
    ogg_packet packet = {};
    packet.packet = reinterpret_cast<unsigned char*>(head.data());
    packet.bytes = static_cast<long>(head.size());
    packet.b_o_s = 1;
    packet.packetno = packetNumber++;
    ogg_stream_packetin(&stream, &packet);
    ok = ok && WritePages(file, &stream, true);

    std::string tags = "OpusTags";
    const std::string vendor = "racing-master-mod test";
    Le32(&tags, static_cast<uint32_t>(vendor.size()));
    tags += vendor;
    const std::string titleTag = std::string("TITLE=") + title;
    const std::string artistTag = std::string("ARTIST=") + artist;
    Le32(&tags, 2);
    Le32(&tags, static_cast<uint32_t>(titleTag.size()));
    tags += titleTag;
    Le32(&tags, static_cast<uint32_t>(artistTag.size()));
    tags += artistTag;
    packet = {};
    packet.packet = reinterpret_cast<unsigned char*>(tags.data());
    packet.bytes = static_cast<long>(tags.size());
    packet.packetno = packetNumber++;
    ogg_stream_packetin(&stream, &packet);
    ok = ok && WritePages(file, &stream, true);

    const int64_t total = static_cast<int64_t>(seconds * 48000);
    const int64_t frames = (total + lookahead + 959) / 960;
    std::vector<float> pcm(960 * 2);
    unsigned char out[4000];
    for (int64_t k = 0; ok && k < frames; ++k) {
        for (int i = 0; i < 960; ++i) {
            const int64_t t = k * 960 + i;
            const float v = t < total ? static_cast<float>(kAmplitude * std::sin(6.283185307179586 * frequency * t / 48000.0)) : 0.0f;
            pcm[static_cast<size_t>(i) * 2] = pcm[static_cast<size_t>(i) * 2 + 1] = v;
        }
        const opus_int32 bytes = opus_encode_float(encoder, pcm.data(), 960, out, sizeof(out));
        if (bytes < 0) {
            ok = false;
            break;
        }
        const bool last = k == frames - 1;
        packet = {};
        packet.packet = out;
        packet.bytes = bytes;
        packet.e_o_s = last ? 1 : 0;
        packet.granulepos = last ? lookahead + total : (k + 1) * 960;
        packet.packetno = packetNumber++;
        ogg_stream_packetin(&stream, &packet);
        ok = WritePages(file, &stream, last);
    }
    ogg_stream_clear(&stream);
    opus_encoder_destroy(encoder);
    CloseHandle(file);
    return ok;
}

}  // namespace mt

MT_TEST(370, DecodeOpus, "decoding: Opus (libopusfile)") {
    const std::wstring path = mt::FixtureDir() + L"\\測試 opus #1 100%.opus";
    const char* title = "\xE6\xB8\xAC\xE8\xA9\xA6\xE6\xA8\x99\xE9\xA1\x8C";  // 測試標題 in UTF-8
    if (!mt::Check(mt::WriteOpus(path, 770.0, 1.0, title, "Test artist"), "Opus fixture written with libopus")) return;
    OpenError error = OpenError::None;
    mt::Check(SniffFile(path, &error) == Kind::Opus, "Opus: recognised by its content (OpusHead)");
    mt::Signal signal;
    int code = 0;
    int64_t length = 0;
    if (mt::Check(mt::DecodeFile(path, &signal, &code, &length), "Opus: opens and decodes from a Chinese folder name", "error %d", code)) {
        mt::Check(signal.rate == 48000 && signal.channels == 2, "Opus: 48 kHz stereo", "rate %u, channels %u", signal.rate, signal.channels);
        mt::Check(length == 48000 && signal.Frames() == 48000, "Opus: encoder delay and padding trimmed (exactly 1 s)",
                  "length %lld, decoded %zu", static_cast<long long>(length), signal.Frames());
        mt::CheckSine("Opus", signal, 770.0);
    }

    Tags tags;
    bool faulted = false;
    mt::Check(ReadTags(path, Kind::Opus, &tags, &faulted) && tags.title == FromUtf8(title) && tags.artist == L"Test artist" &&
                  std::fabs(tags.durationSeconds - 1.0) < 0.001,
              "Opus: Chinese title, artist and duration read for the list", "duration %.3f s", tags.durationSeconds);

    std::unique_ptr<Source> source = OpenSource(path, &error);
    if (mt::Check(source != nullptr, "Opus: opens for seeking")) {
        bool ok = false;
        GuardedSeek(source.get(), 36000, &ok);
        std::vector<float> buffer(4096 * 2);
        size_t left = 0;
        for (;;) {
            uint32_t got = 0;
            bool failed = false;
            if (!GuardedRead(source.get(), buffer.data(), 4096, &got, &failed)) break;
            left += got;
            if (got < 4096 || failed) break;
        }
        mt::Check(ok && left == 12000, "Opus: seek to 0.75 s lands on the exact sample", "%zu frames left", left);
    }

    // The header of a real file followed by junk: refused (or ended) cleanly, never a fault.
    std::string bytes = mt::ReadBytes(path);
    bytes.resize(std::min<size_t>(bytes.size(), 200));
    bytes.append(3000, '\x5A');
    const std::wstring broken = mt::FixtureDir() + L"\\broken opus 測試.opus";
    mt::WriteBytes(broken, bytes);
    mt::Signal junk;
    const bool opened = mt::DecodeFile(broken, &junk, &code, &length);
    mt::Check(!opened ? code == static_cast<int>(OpenError::Damaged) : junk.Frames() < 48000,
              "Opus: a damaged file is refused or ends early, without a fault", "opened %d, error %d", opened ? 1 : 0, code);
    mt::Check(DeleteFileW(broken.c_str()) != FALSE, "Opus: the file is closed after a refused open (it can be deleted)");
}

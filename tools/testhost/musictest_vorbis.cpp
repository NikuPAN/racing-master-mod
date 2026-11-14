// OGG Vorbis through stb_vorbis's pull API (src\music\source_vorbis.cpp).
//
// Windows has no Vorbis encoder, so the fixture is made at test time with Xiph's reference
// encoder (libvorbis 1.3.7, fetched and hash-checked by build\fetch-deps.ps1, linked into this
// test program only). The mod itself decodes with stb_vorbis and never contains libvorbis.
#include "musictest.h"

#include "music/source.h"
#include "music/tag_reader.h"
#include "music/text.h"

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include <windows.h>

#include <cmath>
#include <memory>

using namespace music;

namespace mt {

namespace {

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

bool WriteVorbis(const std::wstring& path, uint32_t rate, double frequency, double seconds, const char* title, const char* artist) {
    vorbis_info info;
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 2, static_cast<long>(rate), 0.6f) != 0) {
        vorbis_info_clear(&info);
        return false;
    }
    vorbis_comment comment;
    vorbis_comment_init(&comment);
    vorbis_comment_add_tag(&comment, "TITLE", title);
    vorbis_comment_add_tag(&comment, "ARTIST", artist);
    vorbis_dsp_state dsp;
    vorbis_block block;
    vorbis_analysis_init(&dsp, &info);
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream;
    ogg_stream_init(&stream, 0x0BB5);

    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        ogg_packet header;
        ogg_packet headerComment;
        ogg_packet headerCode;
        vorbis_analysis_headerout(&dsp, &comment, &header, &headerComment, &headerCode);
        ogg_stream_packetin(&stream, &header);
        ogg_stream_packetin(&stream, &headerComment);
        ogg_stream_packetin(&stream, &headerCode);
        ok = WritePages(file, &stream, true);
    }
    const long total = static_cast<long>(seconds * rate);
    long done = 0;
    bool ended = false;
    while (ok && !ended) {
        const long frames = total - done < 1024 ? total - done : 1024;
        if (frames > 0) {
            float** buffer = vorbis_analysis_buffer(&dsp, static_cast<int>(frames));
            for (long i = 0; i < frames; ++i) {
                const float v = static_cast<float>(kAmplitude * std::sin(6.283185307179586 * frequency * (done + i) / rate));
                buffer[0][i] = v;
                buffer[1][i] = v;
            }
            vorbis_analysis_wrote(&dsp, static_cast<int>(frames));
            done += frames;
        } else {
            vorbis_analysis_wrote(&dsp, 0);  // end of stream
        }
        while (vorbis_analysis_blockout(&dsp, &block) == 1) {
            vorbis_analysis(&block, nullptr);
            vorbis_bitrate_addblock(&block);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&dsp, &packet)) {
                ogg_stream_packetin(&stream, &packet);
                ok = ok && WritePages(file, &stream, false);
                if (packet.e_o_s) ended = true;
            }
        }
        if (frames <= 0 && !ended) ended = true;
    }
    if (ok) ok = WritePages(file, &stream, true);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return ok;
}

}  // namespace mt

MT_TEST(340, DecodeVorbis, "decoding: OGG Vorbis (stb_vorbis)") {
    const std::wstring path = mt::FixtureDir() + L"\\測試 vorbis #1 100%.ogg";
    const char* title = "\xE6\xB8\xAC\xE8\xA9\xA6\xE6\xA8\x99\xE9\xA1\x8C";  // 測試標題 in UTF-8
    if (mt::Check(mt::WriteVorbis(path, 44100, 440.0, 1.0, title, "Test artist"), "OGG Vorbis fixture written with libvorbis")) {
        OpenError error = OpenError::None;
        mt::Check(SniffFile(path, &error) == Kind::Vorbis, "OGG Vorbis: recognised by its content");
        mt::Signal signal;
        int code = 0;
        int64_t length = 0;
        if (mt::Check(mt::DecodeFile(path, &signal, &code, &length), "OGG Vorbis: opens from a Chinese folder name (wide-path opener)",
                      "error %d", code)) {
            mt::Check(signal.rate == 44100 && signal.channels == 2, "OGG Vorbis: native rate and two channels", "rate %u, channels %u",
                      signal.rate, signal.channels);
            mt::Check(length == 44100 && signal.Frames() == 44100, "OGG Vorbis: exact length known at open and decoded in full",
                      "length %lld, decoded %zu", static_cast<long long>(length), signal.Frames());
            mt::CheckSine("OGG Vorbis", signal, 440.0);
        }
        Tags tags;
        bool faulted = false;
        mt::Check(ReadTags(path, Kind::Vorbis, &tags, &faulted) && tags.title == FromUtf8(title) && tags.artist == L"Test artist" &&
                      std::fabs(tags.durationSeconds - 1.0) < 0.001,
                  "OGG Vorbis: Chinese title, artist and duration read for the list", "duration %.3f s", tags.durationSeconds);
        std::unique_ptr<Source> source = OpenSource(path, &error);
        if (mt::Check(source != nullptr, "OGG Vorbis: opens for seeking")) {
            bool ok = false;
            GuardedSeek(source.get(), 33075, &ok);
            std::vector<float> buffer(4096 * 2);
            size_t left = 0;
            for (;;) {
                uint32_t got = 0;
                bool failed = false;
                if (!GuardedRead(source.get(), buffer.data(), 4096, &got, &failed)) break;
                left += got;
                if (got < 4096 || failed) break;
            }
            mt::Check(ok && left == 11025, "OGG Vorbis: seek to 0.75 s lands on the exact sample", "%zu frames left", left);
        }
    }

    // An Ogg page that claims to be Vorbis, followed by junk: stb_vorbis must refuse it, and must
    // have closed the file itself (the engine never closes it a second time), so it can be deleted.
    std::string fake = "OggS";
    fake.append(24, '\0');
    fake += "\x01vorbis";
    fake.append(4000, 'x');
    const std::wstring broken = mt::FixtureDir() + L"\\fake vorbis 測試.ogg";
    mt::WriteBytes(broken, fake);
    OpenError error = OpenError::None;
    std::unique_ptr<Source> source = OpenSource(broken, &error);
    mt::Check(!source && error == OpenError::Damaged, "OGG Vorbis: a damaged file is refused as damaged", "error %d",
              static_cast<int>(error));
    mt::Check(DeleteFileW(broken.c_str()) != FALSE, "OGG Vorbis: after a refused open the file is closed (it can be deleted)",
              "DeleteFile error %lu", GetLastError());
}

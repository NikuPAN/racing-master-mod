// playlist.tsv and the [music] section of settings.ini (src\music\playlist_store.*): format,
// round trip, and the rule that a file cut short never loads as a shorter list.
#include "musictest.h"

#include "music/playlist_store.h"

#include <cstdio>

using namespace music;

MT_TEST(400, PlaylistFormat, "playlist.tsv: round trip and cut files") {
    std::vector<StoredTrack> tracks(3);
    tracks[0].path = L"C:\\Music\\\u6E2C\u8A66\u97F3\u6A02\\01 \u6B4C #1 100%.flac";
    tracks[0].title = L"\u6E2C\u8A66\u6A19\u984C\twith a tab";
    tracks[0].artist = L"\u7B80\u4F53\u827A\u672F\u5BB6";
    tracks[0].durationMs = 215000;
    tracks[0].fileSize = 31234567;
    tracks[0].lastWrite = 133700000000000000ull;
    std::wstring longPath = L"D:\\";
    while (longPath.size() < 300) longPath += L"\u9577\u8DEF\u5F91 folder\\";
    tracks[1].path = longPath + L"track.mp3";
    tracks[2].path = L"\\\\server\\share\\music\\track.opus";
    tracks[2].durationMs = -1;

    const std::string text = FormatPlaylist(tracks);
    std::vector<StoredTrack> back;
    const bool parsed = ParsePlaylist(text, &back);
    bool same = parsed && back.size() == 3;
    for (size_t i = 0; same && i < 3; ++i) {
        same = back[i].path == tracks[i].path && back[i].artist == tracks[i].artist && back[i].durationMs == tracks[i].durationMs &&
               back[i].fileSize == tracks[i].fileSize && back[i].lastWrite == tracks[i].lastWrite;
    }
    mt::Check(same, "round trip keeps Chinese names, a 300-character path and a network path");
    mt::Check(parsed && back[0].title == L"\u6E2C\u8A66\u6A19\u984C with a tab", "a tab inside a title is saved as a space");
    mt::Check(text.compare(0, 3, "\xEF\xBB\xBF") != 0 && text.compare(0, 14, "RMM-PLAYLIST\t1") == 0, "UTF-8 without a byte order mark");

    // Cut at every byte: either the complete list loads, or nothing does (then the .bak is used).
    size_t shorter = 0;
    size_t complete = 0;
    for (size_t cut = 0; cut < text.size(); ++cut) {
        std::vector<StoredTrack> partial;
        if (ParsePlaylist(text.substr(0, cut), &partial)) {
            if (partial.size() == 3) ++complete;
            else ++shorter;
        }
    }
    char name[128];
    snprintf(name, sizeof(name), "a file cut at any of its %zu bytes never loads as a shorter list", text.size());
    mt::Check(shorter == 0, name, "%zu cuts loaded a shorter list", shorter);

    std::vector<StoredTrack> ignored;
    mt::Check(!ParsePlaylist("RMM-PLAYLIST\t1\r\nT\t1\t2\t3\tt\ta\tC:\\x.mp3\r\nEND\t2\r\n", &ignored), "an END count that does not match is refused");
    mt::Check(ParsePlaylist("RMM-PLAYLIST\t1\nX\tfuture line\nEND\t0\n", &ignored) && ignored.empty(),
              "unknown line types are ignored (a later version may add some); LF line endings work");
    mt::Check(!ParsePlaylist("something else\r\nEND\t0\r\n", &ignored), "a file without the header line is refused");
}

MT_TEST(410, SettingsSection, "settings.ini: the [music] section is merged, the rest is kept") {
    // Saved by Notepad as UTF-16 with the loader's [mod] section and a comment.
    const std::wstring original = L"; my settings\r\n[mod]\r\nstage=1\r\n[music]\r\nvolume=10\r\nloop=one\r\n[other]\r\nkey=\u503C\r\n";
    std::string bytes = "\xFF\xFE";
    for (wchar_t c : original) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>(c >> 8));
    }
    MusicSettings read;
    ReadMusicSection(bytes, &read);
    mt::Check(read.volumePercent == 10 && read.loop == 1, "the [music] section is read from a UTF-16 file");

    MusicSettings settings;
    settings.volumePercent = 55;
    settings.loop = 0;
    settings.pauseWhenGameUnfocused = true;
    settings.currentPath = L"C:\\Music\\\u6E2C\u8A66.mp3";
    settings.positionMs = 83500;
    const std::string merged = WriteMusicSection(bytes, settings);
    mt::Check(merged.find("; my settings\r\n[mod]\r\nstage=1\r\n") == 0, "the comment and the [mod] lines are kept, in UTF-8");
    mt::Check(merged.find("[other]\r\nkey=\xE5\x80\xBC") != std::string::npos, "other sections are kept");
    mt::Check(merged.find("volume=10") == std::string::npos && merged.find("volume=55") != std::string::npos,
              "the old [music] values are replaced, not repeated");
    MusicSettings again;
    ReadMusicSection(merged, &again);
    mt::Check(again.volumePercent == 55 && again.loop == 0 && again.pauseWhenGameUnfocused && again.currentPath == settings.currentPath &&
                  again.positionMs == 83500,
              "the merged file reads back the same values");
    const std::string fresh = WriteMusicSection("", settings);
    mt::Check(fresh.compare(0, 9, "[music]\r\n") == 0, "a new file starts with [music]");
    MusicSettings bad;
    ReadMusicSection("[music]\r\nvolume=250\r\nloop=sometimes\r\nposition_ms=-5\r\n", &bad);
    mt::Check(bad.volumePercent == 60 && bad.loop == 2 && bad.positionMs == 0, "values out of range keep their defaults");
}

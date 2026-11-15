#pragma once

// playlist.tsv and the [music] section of settings.ini: parsing and formatting only, no I/O.
//
// playlist.tsv (UTF-8, no byte order mark, one record per line, tab-separated, path last):
//   RMM-PLAYLIST<tab>1
//   T<tab><size bytes><tab><last write FILETIME><tab><duration ms or -1><tab><title><tab><artist><tab><full path>
//   L<tab><loudness in 1/100 LUFS><tab><sample peak in millionths>   (0.3.2, optional, after its T)
//   L<tab>none                       the song was measured and gave nothing (silent or undecodable)
//   END<tab><number of T lines>
// A file is accepted only when the first line matches and the END count equals the number of T
// lines, so a file cut short by a crash never loads as a shorter list. Unknown line types are
// ignored so a later version can add lines (0.3.1 reads a 0.3.2 list and drops the L lines).
// An L line belongs to the T line above it and is only written while that row's size and time are
// known, so a changed file is measured again exactly when its tags are read again.

#include <cstdint>
#include <string>
#include <vector>

namespace music {

struct StoredTrack {
    std::wstring path;
    std::wstring title;   // empty = not read yet
    std::wstring artist;
    int64_t durationMs = -1;
    uint64_t fileSize = 0;      // for the tag cache: unchanged size and time = tags still valid
    uint64_t lastWrite = 0;     // FILETIME as a number; 0 = unknown
    // Even volume (loudness.h): 0 = not measured, 1 = measured (lufs, peak), 2 = measured, no result.
    int loudnessState = 0;
    double lufs = 0;
    float peak = 0;
};

std::string FormatPlaylist(const std::vector<StoredTrack>& tracks);
// Returns false when the text is not a complete playlist file.
bool ParsePlaylist(const std::string& text, std::vector<StoredTrack>* tracks);

struct MusicSettings {
    int volumePercent = 60;
    int loop = 2;                    // 0 off, 1 one, 2 all (the LoopMode order)
    bool pauseWhenGameUnfocused = false;
    std::wstring currentPath;        // the cued track
    int64_t positionMs = 0;
    bool evenVolume = true;          // even_volume: play every song at about the same loudness (0.3.2)
};

// Rewrites the [music] section inside an existing settings.ini (other sections are kept line for
// line) or appends it. `existingIni` is the file's bytes in any encoding the loader accepts (UTF-8
// with or without a byte order mark, UTF-16 in either byte order); the result is UTF-8 without a
// byte order mark, which the loader also reads.
std::string WriteMusicSection(const std::string& existingIni, const MusicSettings& settings);
// Reads the [music] section (same encodings as above); keys that are missing keep their defaults.
void ReadMusicSection(const std::string& ini, MusicSettings* settings);

}  // namespace music

#include "playlist_store.h"

#include "text.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace music {

namespace {

constexpr char kHeader[] = "RMM-PLAYLIST\t1";

std::vector<std::string_view> SplitLines(const std::string& text) {
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of("\r\n", start);
        if (end == std::string::npos) end = text.size();
        lines.push_back(std::string_view(text).substr(start, end - start));
        if (end >= text.size()) break;
        start = end + 1;
        if (text[end] == '\r' && start < text.size() && text[start] == '\n') ++start;
    }
    return lines;
}

std::vector<std::string_view> SplitTabs(std::string_view line, size_t maxFields) {
    std::vector<std::string_view> fields;
    size_t start = 0;
    while (fields.size() + 1 < maxFields) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos) break;
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    fields.push_back(line.substr(start));
    return fields;
}

bool ParseInt(std::string_view text, long long* out) {
    if (text.empty() || text.size() > 20) return false;
    char buffer[24];
    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    char* end = nullptr;
    *out = _strtoi64(buffer, &end, 10);
    return end && *end == '\0';
}

bool ParseUnsigned(std::string_view text, unsigned long long* out) {
    if (text.empty() || text.size() > 20 || text[0] == '-') return false;
    char buffer[24];
    memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    char* end = nullptr;
    *out = _strtoui64(buffer, &end, 10);
    return end && *end == '\0';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

bool IsHeading(std::string_view line, const char* name) {
    line = Trim(line);
    if (line.size() < 2 || line.front() != '[') return false;
    const size_t close = line.find(']');
    if (close == std::string_view::npos) return false;
    const std::string_view inner = Trim(line.substr(1, close - 1));
    return inner.size() == strlen(name) && _strnicmp(inner.data(), name, inner.size()) == 0;
}

bool IsAnyHeading(std::string_view line) {
    line = Trim(line);
    return !line.empty() && line.front() == '[';
}

}  // namespace

std::string FormatPlaylist(const std::vector<StoredTrack>& tracks) {
    std::string out = kHeader;
    out += "\r\n";
    size_t written = 0;
    for (const StoredTrack& track : tracks) {
        bool ok = true;
        const std::string path = ToUtf8(track.path, &ok);
        if (!ok || path.empty()) continue;  // a name with an unpaired surrogate cannot be saved loss-free
        out += "T\t";
        out += std::to_string(track.fileSize);
        out += '\t';
        out += std::to_string(track.lastWrite);
        out += '\t';
        out += std::to_string(track.durationMs);
        out += '\t';
        out += ToUtf8(SingleLine(track.title));
        out += '\t';
        out += ToUtf8(SingleLine(track.artist));
        out += '\t';
        out += path;
        out += "\r\n";
        if (track.loudnessState == 1 && track.lastWrite != 0) {
            out += "L\t";
            out += std::to_string(static_cast<long long>(std::lround(track.lufs * 100.0)));
            out += '\t';
            out += std::to_string(static_cast<long long>(std::lround(static_cast<double>(track.peak) * 1e6)));
            out += "\r\n";
        } else if (track.loudnessState == 2 && track.lastWrite != 0) {
            out += "L\tnone\r\n";
        }
        if (track.lastPlayed > 0) {
            out += "P\t";
            out += std::to_string(track.lastPlayed);
            out += "\r\n";
        }
        ++written;
    }
    out += "END\t";
    out += std::to_string(written);
    out += "\r\n";
    return out;
}

bool ParsePlaylist(const std::string& text, std::vector<StoredTrack>* tracks) {
    tracks->clear();
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    const std::vector<std::string_view> lines = SplitLines(body);
    if (lines.empty() || lines[0] != kHeader) return false;
    std::vector<StoredTrack> parsed;
    bool ended = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (line.empty()) continue;
        if (ended) continue;  // anything after END is ignored
        if (line.size() >= 2 && line[0] == 'T' && line[1] == '\t') {
            const std::vector<std::string_view> fields = SplitTabs(line, 7);
            if (fields.size() != 7) return false;
            StoredTrack track;
            unsigned long long size = 0;
            unsigned long long time = 0;
            long long duration = -1;
            if (!ParseUnsigned(fields[1], &size) || !ParseUnsigned(fields[2], &time) || !ParseInt(fields[3], &duration)) return false;
            track.fileSize = size;
            track.lastWrite = time;
            track.durationMs = duration;
            track.title = FromUtf8(fields[4]);
            track.artist = FromUtf8(fields[5]);
            track.path = FromUtf8(fields[6]);
            if (track.path.empty()) return false;
            parsed.push_back(std::move(track));
        } else if (line.size() >= 2 && line[0] == 'L' && line[1] == '\t') {
            // Loudness of the row above. A line that does not parse is ignored (the song is
            // simply measured again), never a reason to refuse the list.
            if (parsed.empty() || parsed.back().lastWrite == 0) continue;
            const std::vector<std::string_view> fields = SplitTabs(line, 3);
            long long lufs = 0;
            long long peak = 0;
            if (fields.size() == 2 && fields[1] == "none") {
                parsed.back().loudnessState = 2;
            } else if (fields.size() == 3 && ParseInt(fields[1], &lufs) && ParseInt(fields[2], &peak) && lufs > -20000 &&
                       lufs < 2000 && peak >= 0 && peak <= 100000000) {
                parsed.back().loudnessState = 1;
                parsed.back().lufs = static_cast<double>(lufs) / 100.0;
                parsed.back().peak = static_cast<float>(static_cast<double>(peak) / 1e6);
            }
        } else if (line.size() >= 2 && line[0] == 'P' && line[1] == '\t') {
            // When the row above last counted as heard. Unreadable: ignored (the song counts as unheard).
            long long seconds = 0;
            if (!parsed.empty() && ParseInt(line.substr(2), &seconds) && seconds > 0) parsed.back().lastPlayed = seconds;
        } else if (line.size() >= 4 && line.compare(0, 4, "END\t") == 0) {
            long long count = -1;
            if (!ParseInt(line.substr(4), &count) || count != static_cast<long long>(parsed.size())) return false;
            ended = true;
        }
        // other line types: ignored
    }
    if (!ended) return false;
    *tracks = std::move(parsed);
    return true;
}

std::string WriteMusicSection(const std::string& existingIni, const MusicSettings& settings) {
    std::string section = "[music]\r\n";
    section += "volume=" + std::to_string(settings.volumePercent) + "\r\n";
    section += std::string("loop=") + (settings.loop == 0 ? "off" : settings.loop == 1 ? "one" : "all") + "\r\n";
    section += std::string("pause_when_game_unfocused=") + (settings.pauseWhenGameUnfocused ? "1" : "0") + "\r\n";
    section += "current=" + ToUtf8(SingleLine(settings.currentPath)) + "\r\n";
    section += "position_ms=" + std::to_string(settings.positionMs) + "\r\n";
    section += std::string("even_volume=") + (settings.evenVolume ? "1" : "0") + "\r\n";

    // The file may have been saved by Notepad as UTF-16 or with a byte order mark; the loader's own
    // reader accepts all of those. Read it as text, never as raw bytes, or the [mod] lines would be
    // destroyed on the first save.
    const std::string body = TextFileToUtf8(existingIni);
    std::vector<std::string_view> kept;
    bool inMusic = false;
    bool replaced = false;
    size_t sectionAt = 0;
    for (const std::string_view line : SplitLines(body)) {
        if (IsHeading(line, "music")) {
            inMusic = true;
            if (!replaced) {
                sectionAt = kept.size();
                replaced = true;
            }
            continue;
        }
        if (inMusic && IsAnyHeading(line)) inMusic = false;
        if (inMusic) continue;  // the old [music] lines are dropped
        kept.push_back(line);
    }
    // Blank lines at the very end (SplitLines yields one for a final line break) are not kept.
    while (!kept.empty() && Trim(kept.back()).empty()) kept.pop_back();
    if (!replaced || sectionAt > kept.size()) sectionAt = kept.size();
    // Exactly one blank line between [music] and the text on either side, for whoever reads the
    // file: the blank lines that stood next to the old section are dropped and one is put back.
    // (0.3.0 lost it when the old section was last: its blank line became a trailing one above.)
    size_t beforeEnd = sectionAt;
    while (beforeEnd > 0 && Trim(kept[beforeEnd - 1]).empty()) --beforeEnd;
    size_t afterStart = sectionAt;
    while (afterStart < kept.size() && Trim(kept[afterStart]).empty()) ++afterStart;
    std::string out;
    for (size_t i = 0; i < beforeEnd; ++i) {
        out += kept[i];
        out += "\r\n";
    }
    if (!out.empty()) out += "\r\n";
    out += section;
    if (afterStart < kept.size()) out += "\r\n";
    for (size_t i = afterStart; i < kept.size(); ++i) {
        out += kept[i];
        out += "\r\n";
    }
    return out;
}

void ReadMusicSection(const std::string& ini, MusicSettings* settings) {
    const std::string body = TextFileToUtf8(ini);
    bool inMusic = false;
    for (std::string_view line : SplitLines(body)) {
        line = Trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            inMusic = IsHeading(line, "music");
            continue;
        }
        if (!inMusic) continue;
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        long long number = 0;
        if (key == "volume") {
            if (ParseInt(value, &number) && number >= 0 && number <= 100) settings->volumePercent = static_cast<int>(number);
        } else if (key == "loop") {
            if (value == "off") settings->loop = 0;
            else if (value == "one") settings->loop = 1;
            else if (value == "all") settings->loop = 2;
        } else if (key == "pause_when_game_unfocused") {
            if (value == "1" || value == "true" || value == "yes") settings->pauseWhenGameUnfocused = true;
            else if (value == "0" || value == "false" || value == "no") settings->pauseWhenGameUnfocused = false;
        } else if (key == "current") {
            settings->currentPath = FromUtf8(value);
        } else if (key == "position_ms") {
            if (ParseInt(value, &number) && number >= 0) settings->positionMs = number;
        } else if (key == "even_volume") {
            if (value == "1" || value == "true" || value == "yes") settings->evenVolume = true;
            else if (value == "0" || value == "false" || value == "no") settings->evenVolume = false;
        }
    }
}

}  // namespace music

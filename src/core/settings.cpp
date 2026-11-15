#include "settings.h"

#include "log.h"

#include <cstring>
#include <cwchar>

namespace {

constexpr wchar_t kLocalIniName[] = L"settings.ini";
constexpr wchar_t kDllIniName[] = L"racing-master-mod.ini";

wchar_t g_userSettingsPath[MAX_PATH] = L"";

// "12" -> 12. Anything that is not a plain non-negative number is refused.
bool ParseNumber(const char* text, unsigned* out) {
    if (*text == '\0') {
        return false;
    }
    unsigned long long value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(*p - '0');
        if (value > 0xFFFFFFFFull) {
            return false;
        }
    }
    *out = static_cast<unsigned>(value);
    return true;
}

// "1", "true", "yes", "on" -> true; "0", "false", "no", "off" -> false.
bool ParseSwitch(const char* text, bool* out) {
    if (_stricmp(text, "1") == 0 || _stricmp(text, "true") == 0 || _stricmp(text, "yes") == 0 ||
        _stricmp(text, "on") == 0) {
        *out = true;
        return true;
    }
    if (_stricmp(text, "0") == 0 || _stricmp(text, "false") == 0 || _stricmp(text, "no") == 0 ||
        _stricmp(text, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

// Matches the value against a list of words; returns the index or -1.
int MatchWord(const char* value, const char* const* words, int count) {
    for (int i = 0; i < count; ++i) {
        if (_stricmp(value, words[i]) == 0) {
            return i;
        }
    }
    return -1;
}

char* Trim(char* text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    char* end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        --end;
    }
    *end = '\0';
    return text;
}

enum class Section { Mod, Panel, Keys, Music, Debug, Other };

// [debug]: diagnostics for a developer or for the owner's test runs; nothing a player needs.
void ApplyDebugKey(const char* key, const char* value, const char* path8, Settings* settings) {
    if (_stricmp(key, "present_probe") == 0) {
        if (!ParseSwitch(value, &settings->presentProbe)) {
            Log("settings: \"present_probe=%s\" under [debug] is not 0 or 1 (in %s); the present probe stays off",
                value, path8);
            settings->presentProbe = false;
        }
    } else {
        Log("settings: unknown key \"%s\" under [debug] is ignored (in %s); the only one is present_probe", key, path8);
    }
}

void ApplyPanelKey(const char* key, const char* value, const char* path8, UiSettings* ui) {
    static const char* const kLanguages[] = {"auto", "en", "zh-TW"};
    static const char* const kSizes[] = {"small", "medium", "large"};
    // In UiCorner order. "bottom-center" and "top-center" are accepted too.
    static const char* const kCorners[UiCornerCount] = {"top-left",     "top-right",     "bottom-left",
                                                        "bottom-right", "bottom-centre", "top-centre"};
    static const char* const kModes[] = {"off", "display", "on"};
    static const char* const kSources[] = {"my_files", "other_app"};
    if (_stricmp(key, "mode") == 0) {
        const int index = MatchWord(value, kModes, 3);
        if (index >= 0) {
            ui->mode = static_cast<unsigned>(index);
        } else {
            Log("settings: \"mode=%s\" is not off, display or on (in %s); the panel stays on", value, path8);
        }
    } else if (_stricmp(key, "source") == 0) {
        const int index = MatchWord(value, kSources, 2);
        if (index >= 0) {
            ui->source = static_cast<unsigned>(index);
        } else {
            Log("settings: \"source=%s\" is not my_files or other_app (in %s); using my_files", value, path8);
        }
    } else if (_stricmp(key, "language") == 0) {
        int index = MatchWord(value, kLanguages, 3);
        if (index < 0 && (_stricmp(value, "zh") == 0 || _stricmp(value, "zh-HK") == 0 || _stricmp(value, "zh-Hant") == 0 ||
                          _stricmp(value, "zh-MO") == 0)) {
            index = 2;
        }
        if (index >= 0) {
            ui->language = static_cast<unsigned>(index);
        } else {
            Log("settings: \"language=%s\" is not auto, en or zh-TW (in %s); the panel keeps following Windows", value,
                path8);
        }
    } else if (_stricmp(key, "text_size") == 0) {
        const int index = MatchWord(value, kSizes, 3);
        if (index >= 0) {
            ui->textSize = static_cast<unsigned>(index);
        } else {
            Log("settings: \"text_size=%s\" is not small, medium or large (in %s); using medium", value, path8);
        }
    } else if (_stricmp(key, "pause_when_unfocused") == 0) {
        if (!ParseSwitch(value, &ui->pauseWhenUnfocused)) {
            Log("settings: \"pause_when_unfocused=%s\" is not 0 or 1 (in %s); the music keeps playing when the "
                "game is not in focus",
                value, path8);
        }
    } else if (_stricmp(key, "corner") == 0) {
        int index = MatchWord(value, kCorners, UiCornerCount);
        if (index < 0 && _stricmp(value, "bottom-center") == 0) {
            index = UiCornerBottomCentre;
        }
        if (index < 0 && _stricmp(value, "top-center") == 0) {
            index = UiCornerTopCentre;
        }
        if (index >= 0) {
            ui->corner = static_cast<unsigned>(index);
        } else {
            Log("settings: \"corner=%s\" is not top-left, top-centre, top-right, bottom-left, bottom-centre or "
                "bottom-right (in %s); using bottom-centre",
                value, path8);
        }
    } else if (_stricmp(key, "compact") == 0) {
        if (!ParseSwitch(value, &ui->compact)) {
            Log("settings: \"compact=%s\" is not 0 or 1 (in %s); the panel shows in full", value, path8);
        }
    } else if (_stricmp(key, "mute_game_music") == 0) {
        if (!ParseSwitch(value, &ui->muteGameMusic)) {
            Log("settings: \"mute_game_music=%s\" is not 0 or 1 (in %s); the game's music is turned off at start",
                value, path8);
        }
    } else {
        Log("settings: unknown key \"%s\" under [panel] is ignored (in %s)", key, path8);
    }
}

void ApplyKeysKey(const char* key, const char* value, const char* path8, UiSettings* ui) {
    static const char* const kActions[UiKeyCount] = {"panel", "stop", "previous", "play_pause", "next", "loop"};
    const int action = MatchWord(key, kActions, UiKeyCount);
    if (action >= 0) {
        if (strlen(value) < sizeof(ui->keys[action])) {
            // Validated by the panel, which knows the key names and logs a refused one.
            strcpy_s(ui->keys[action], sizeof(ui->keys[action]), value);
        } else {
            Log("settings: \"%s=%s\" is too long to be a key name (in %s); the default key stays", key, value, path8);
        }
    } else if (_stricmp(key, "media_keys") == 0) {
        if (!ParseSwitch(value, &ui->mediaKeys)) {
            Log("settings: \"media_keys=%s\" is not 0 or 1 (in %s); the keyboard's media keys stay on", value, path8);
        }
    } else {
        Log("settings: unknown key \"%s\" under [keys] is ignored (in %s); the actions are panel, stop, previous, "
            "play_pause, next, loop and media_keys",
            key, path8);
    }
}

// Applies one file to `settings`. Returns true when the file existed.
bool ApplyIniFile(const wchar_t* path, Settings* settings) {
    char path8[MAX_PATH * 3];
    LogUtf8(path, path8, sizeof(path8));
    static char text[16384];
    DWORD error = 0;
    bool bom = false;
    const IniFileState state = LoadIniFileText(path, text, sizeof(text), &error, &bom);
    if (state == IniFileState::Missing) {
        return false;
    }
    if (state == IniFileState::Unreadable) {
        settings->stage = 0;
        Log("settings: %s is there but could not be read (error %lu); the mod stays off until the file can be "
            "read or is removed",
            path8, error);
        return true;
    }
    Log("settings: read %s", path8);

    // Lines before the first [heading] count as [mod], so a file that is just "stage=0" works.
    // The [music] section, which the music engine writes into the same file, is skipped without
    // counting as "ignored" and without a log line: a file with only that section must not
    // produce the "changed nothing" line below on every start.
    Section section = Section::Mod;
    int contentLines = 0;   // lines that are not blank and not a comment
    int keysSeen = 0;       // key=value lines under a known heading, whether or not the value was accepted
    for (char* line = text; line && *line;) {
        // Lines may end in CR LF (Windows), LF alone, or CR alone (some editors on other systems).
        char* next = strpbrk(line, "\r\n");
        if (next) {
            *next++ = '\0';
        }
        char* entry = Trim(line);
        line = next;
        if (*entry == '\0' || *entry == ';' || *entry == '#') {
            continue;
        }
        if (*entry == '[') {
            char* close = strchr(entry, ']');
            if (close) {
                *close = '\0';
            }
            char* name = Trim(entry + 1);
            if (_stricmp(name, "mod") == 0) {
                section = Section::Mod;
            } else if (_stricmp(name, "panel") == 0) {
                section = Section::Panel;
            } else if (_stricmp(name, "keys") == 0) {
                section = Section::Keys;
            } else if (_stricmp(name, "music") == 0) {
                section = Section::Music;   // the music engine reads and writes it itself
            } else if (_stricmp(name, "debug") == 0) {
                section = Section::Debug;
            } else {
                section = Section::Other;
                Log("settings: the heading [%s] is not one the mod knows ([mod], [panel], [keys], [music] or [debug]); "
                    "the lines under it are ignored (in %s)",
                    name, path8);
            }
            continue;
        }
        if (section == Section::Music) {
            continue;
        }
        ++contentLines;
        char* equals = strchr(entry, '=');
        if (section == Section::Other || !equals) {
            continue;
        }
        ++keysSeen;
        *equals = '\0';
        char* key = Trim(entry);
        char* value = Trim(equals + 1);
        for (char* p = value; *p; ++p) {   // a comment after the value
            if ((*p == ';' || *p == '#') && p > value && (p[-1] == ' ' || p[-1] == '\t')) {
                *p = '\0';
                value = Trim(value);
                break;
            }
        }
        if (section == Section::Panel) {
            ApplyPanelKey(key, value, path8, &settings->ui);
            continue;
        }
        if (section == Section::Keys) {
            ApplyKeysKey(key, value, path8, &settings->ui);
            continue;
        }
        if (section == Section::Debug) {
            ApplyDebugKey(key, value, path8, settings);
            continue;
        }
        unsigned number = 0;
        const bool isNumber = ParseNumber(value, &number);
        if (_stricmp(key, "stage") == 0) {
            if (isNumber && number <= 2) {
                settings->stage = number;
            } else {
                // Keep reading so that every bad value in the file is reported in one go.
                settings->stage = 0;
                Log("settings: \"stage=%s\" is not 0, 1 or 2 (in %s); the mod stays off. Under the [mod] line write "
                    "stage=2 for the full mod or stage=0 to switch it off.",
                    value, path8);
            }
        } else if (_stricmp(key, "marker_seconds") == 0) {
            if (isNumber && number <= 600) {
                settings->markerSeconds = number;
            } else {
                Log("settings: \"marker_seconds=%s\" is not a number of seconds from 0 to 600 (in %s); using %u. "
                    "Under the [mod] line write, for example, marker_seconds=20.",
                    value, path8, settings->markerSeconds);
            }
        } else {
            Log("settings: unknown key \"%s\" under [mod] is ignored (in %s)", key, path8);
        }
    }
    if (contentLines > 0 && keysSeen == 0) {
        // Say so rather than silently applying nothing: the player did write something.
        Log("settings: %s changed nothing, because none of its lines is a key=value line under a [mod] heading; "
            "the first line of the file should be [mod]",
            path8);
    }
    return true;
}

// A player who copies settings.ini next to version.dll (into the game folder) would otherwise see
// it ignored without a word. `dllIniPath` is the full path of racing-master-mod.ini; `name` points
// at its file name inside that buffer, which is reused for the check. Any letter case matches
// (Windows file names are not case-sensitive).
void ReportMisplacedSettingsFile(wchar_t* dllIniPath, wchar_t* name) {
    char dllIni8[MAX_PATH * 3];
    LogUtf8(dllIniPath, dllIni8, sizeof(dllIni8));
    lstrcpyW(name, kLocalIniName);
    const DWORD attributes = GetFileAttributesW(dllIniPath);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        lstrcmpiW(dllIniPath, g_userSettingsPath) == 0) {
        return;
    }
    char found8[MAX_PATH * 3], user8[MAX_PATH * 3];
    LogUtf8(dllIniPath, found8, sizeof(found8));
    LogUtf8(g_userSettingsPath, user8, sizeof(user8));
    Log("settings: found %s next to version.dll and ignored it; the mod reads %s or %s, so rename the file to "
        "racing-master-mod.ini or move it into that second folder",
        found8, dllIni8, user8[0] ? user8 : "%LOCALAPPDATA%\\RacingMasterMod\\settings.ini");
}

}  // namespace

IniFileState LoadIniFileText(const wchar_t* path, char* text, int textSize, DWORD* error, bool* hadByteOrderMark) {
    *hadByteOrderMark = false;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return (*error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND) ? IniFileState::Missing
                                                                                 : IniFileState::Unreadable;
    }
    static char raw[16384];
    DWORD size = 0;
    const BOOL readOk = ReadFile(file, raw, sizeof(raw) - 2, &size, nullptr);
    *error = readOk ? 0 : GetLastError();
    CloseHandle(file);
    if (!readOk) {
        return IniFileState::Unreadable;
    }
    raw[size] = raw[size + 1] = '\0';
    unsigned char* bytes = reinterpret_cast<unsigned char*>(raw);
    const bool utf16LittleEndian = size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
    const bool utf16BigEndian = size >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF;
    if (utf16LittleEndian || utf16BigEndian) {
        // UTF-16 with a byte order mark: Notepad's "UTF-16 LE" and "UTF-16 BE" choices. Windows
        // works in little endian, so a big endian file has each pair of bytes swapped first.
        *hadByteOrderMark = true;
        if (utf16BigEndian) {
            for (DWORD i = 2; i + 1 < size; i += 2) {
                const unsigned char high = bytes[i];
                bytes[i] = bytes[i + 1];
                bytes[i + 1] = high;
            }
        }
        const int characters = static_cast<int>((size - 2) / 2);
        const int converted = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(raw + 2), characters,
                                                  text, textSize - 1, nullptr, nullptr);
        text[converted > 0 ? converted : 0] = '\0';
    } else {
        const int skip = (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ? 3 : 0;
        *hadByteOrderMark = skip != 0;
        const int length = static_cast<int>(size) - skip;
        memcpy(text, raw + skip, static_cast<size_t>(length < textSize - 1 ? length : textSize - 1));
        text[length < textSize - 1 ? length : textSize - 1] = '\0';
    }
    return IniFileState::Read;
}

const wchar_t* UserSettingsPath() {
    return g_userSettingsPath;
}

Settings ReadSettings(HMODULE self) {
    Settings settings;

    // 1. The per-user file, which needs no administrator rights and survives game updates.
    const wchar_t* dir = LogDirectory();
    if (dir[0] != L'\0' && lstrlenW(dir) + 1 + lstrlenW(kLocalIniName) < MAX_PATH) {
        lstrcpyW(g_userSettingsPath, dir);
        lstrcatW(g_userSettingsPath, L"\\");
        lstrcatW(g_userSettingsPath, kLocalIniName);
        ApplyIniFile(g_userSettingsPath, &settings);
    }

    // 2. The file next to the DLL, if present, overrides.
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && (slash - path) + 1 + lstrlenW(kDllIniName) < MAX_PATH) {
            lstrcpyW(slash + 1, kDllIniName);
            ApplyIniFile(path, &settings);
            ReportMisplacedSettingsFile(path, slash + 1);
        }
    }
    return settings;
}

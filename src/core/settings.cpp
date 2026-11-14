#include "settings.h"

#include "log.h"

#include <cstring>
#include <cwchar>

namespace {

constexpr wchar_t kLocalIniName[] = L"settings.ini";
constexpr wchar_t kDllIniName[] = L"racing-master-mod.ini";

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

enum class FileState { Missing, Unreadable, Read };

// Loads the file as UTF-8 text into `text` (16 KB cap). The text buffer is only valid when Read.
FileState LoadIniText(const wchar_t* path, char* text, int textSize, DWORD* error) {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return (*error == ERROR_FILE_NOT_FOUND || *error == ERROR_PATH_NOT_FOUND) ? FileState::Missing
                                                                                 : FileState::Unreadable;
    }
    static char raw[16384];
    DWORD size = 0;
    const BOOL readOk = ReadFile(file, raw, sizeof(raw) - 2, &size, nullptr);
    *error = readOk ? 0 : GetLastError();
    CloseHandle(file);
    if (!readOk) {
        return FileState::Unreadable;
    }
    raw[size] = raw[size + 1] = '\0';
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(raw);
    if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        // UTF-16 little endian with byte order mark (Notepad's "Unicode").
        const int converted = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(raw + 2), -1, text,
                                                  textSize, nullptr, nullptr);
        if (converted <= 0) {
            text[0] = '\0';
        }
    } else {
        const int skip = (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) ? 3 : 0;
        const int length = static_cast<int>(size) - skip;
        memcpy(text, raw + skip, static_cast<size_t>(length < textSize - 1 ? length : textSize - 1));
        text[length < textSize - 1 ? length : textSize - 1] = '\0';
    }
    return FileState::Read;
}

// Applies one file to `settings`. Returns true when the file existed.
bool ApplyIniFile(const wchar_t* path, Settings* settings) {
    char path8[MAX_PATH * 3];
    LogUtf8(path, path8, sizeof(path8));
    static char text[16384];
    DWORD error = 0;
    const FileState state = LoadIniText(path, text, sizeof(text), &error);
    if (state == FileState::Missing) {
        return false;
    }
    if (state == FileState::Unreadable) {
        settings->stage = 0;
        Log("settings: %s is there but could not be read (error %lu); the mod stays off until the file can be "
            "read or is removed",
            path8, error);
        return true;
    }
    Log("settings: read %s", path8);

    bool inMod = false;
    for (char* line = text; line && *line;) {
        char* next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char* entry = Trim(line);
        line = next;
        if (*entry == '\0' || *entry == ';' || *entry == '#') {
            continue;
        }
        if (*entry == '[') {
            inMod = _strnicmp(entry, "[mod]", 5) == 0;
            continue;
        }
        char* equals = strchr(entry, '=');
        if (!inMod || !equals) {
            continue;
        }
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
        unsigned number = 0;
        const bool isNumber = ParseNumber(value, &number);
        if (_stricmp(key, "stage") == 0) {
            if (isNumber && number <= 2) {
                settings->stage = number;
            } else {
                // Keep reading so that every bad value in the file is reported in one go.
                settings->stage = 0;
                Log("settings: \"stage=%s\" is not 0, 1 or 2 (in %s); the mod stays off. Write stage=2 for the full "
                    "mod, stage=0 to switch it off.",
                    value, path8);
            }
        } else if (_stricmp(key, "marker_seconds") == 0) {
            if (isNumber && number <= 600) {
                settings->markerSeconds = number;
            } else {
                Log("settings: \"marker_seconds=%s\" is not a number of seconds from 0 to 600 (in %s); using %u",
                    value, path8, settings->markerSeconds);
            }
        } else {
            Log("settings: unknown key \"%s\" under [mod] is ignored (in %s)", key, path8);
        }
    }
    return true;
}

}  // namespace

Settings ReadSettings(HMODULE self) {
    Settings settings;

    // 1. The per-user file, which needs no administrator rights and survives game updates.
    const wchar_t* dir = LogDirectory();
    if (dir[0] != L'\0' && lstrlenW(dir) + 1 + lstrlenW(kLocalIniName) < MAX_PATH) {
        wchar_t path[MAX_PATH];
        lstrcpyW(path, dir);
        lstrcatW(path, L"\\");
        lstrcatW(path, kLocalIniName);
        ApplyIniFile(path, &settings);
    }

    // 2. The file next to the DLL, if present, overrides.
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && (slash - path) + 1 + lstrlenW(kDllIniName) < MAX_PATH) {
            lstrcpyW(slash + 1, kDllIniName);
            ApplyIniFile(path, &settings);
        }
    }
    return settings;
}

#include "ui_settings.h"

#include "../core/log.h"
#include "../core/settings_file_lock.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

// The latest values to save, packed into one word so the frame thread hands them over with a
// single atomic store. Bit 31 = "something to save".
//   bits 0-1 language, 2-3 text size, 4 pause when unfocused, 5 source, 6-8 corner
std::atomic<uint32_t> g_pending{0};
constexpr uint32_t kPendingFlag = 0x80000000u;
HANDLE g_wake = nullptr;
volatile LONG g_written = 0;

constexpr int kKeyCount = 5;
const char* const kKeys[kKeyCount] = {"language", "text_size", "pause_when_unfocused", "source", "corner"};

uint32_t Pack(const UiSettings& s) {
    return kPendingFlag | (s.language & 3u) | ((s.textSize & 3u) << 2) | (s.pauseWhenUnfocused ? 0x10u : 0u) |
           ((s.source & 1u) << 5) | ((s.corner & 7u) << 6);
}

void ValueWords(uint32_t packed, char values[kKeyCount][16]) {
    static const char* const kLanguages[] = {"auto", "en", "zh-TW", "auto"};
    static const char* const kSizes[] = {"small", "medium", "large", "medium"};
    // UiCorner order; the reader's words (settings.cpp).
    static const char* const kCorners[8] = {"top-left",      "top-right",     "bottom-left",   "bottom-right",
                                            "bottom-centre", "bottom-centre", "bottom-centre", "bottom-centre"};
    strcpy_s(values[0], 16, kLanguages[packed & 3u]);
    strcpy_s(values[1], 16, kSizes[(packed >> 2) & 3u]);
    strcpy_s(values[2], 16, (packed & 0x10u) ? "1" : "0");
    strcpy_s(values[3], 16, (packed & 0x20u) ? "other_app" : "my_files");
    strcpy_s(values[4], 16, kCorners[(packed >> 6) & 7u]);
}

struct Line {
    const char* text;   // not terminated
    int length;
};

int SkipBlanks(const Line& line, int i) {
    while (i < line.length && (line.text[i] == ' ' || line.text[i] == '\t')) ++i;
    return i;
}

bool StartsWithKey(const Line& line, const char* key) {
    int i = SkipBlanks(line, 0);
    const int keyLength = static_cast<int>(strlen(key));
    if (i + keyLength > line.length || _strnicmp(line.text + i, key, static_cast<size_t>(keyLength)) != 0) {
        return false;
    }
    i = SkipBlanks(line, i + keyLength);
    return i < line.length && line.text[i] == '=';
}

// True for any "[...]" line; *isPanel tells whether it is [panel].
bool IsHeading(const Line& line, bool* isPanel) {
    int i = SkipBlanks(line, 0);
    if (i >= line.length || line.text[i] != '[') {
        return false;
    }
    i = SkipBlanks(line, i + 1);
    *isPanel = false;
    if (i + 5 <= line.length && _strnicmp(line.text + i, "panel", 5) == 0) {
        const int j = SkipBlanks(line, i + 5);
        *isPanel = j < line.length && line.text[j] == ']';
    }
    return true;
}

void Append(char* out, int outSize, int* used, const char* text, int length) {
    if (*used + length >= outSize) {
        length = outSize - 1 - *used;
    }
    if (length > 0) {
        memcpy(out + *used, text, static_cast<size_t>(length));
        *used += length;
    }
    out[*used] = '\0';
}

void AppendKey(char* out, int outSize, int* used, int key, const char values[kKeyCount][16], bool newLine) {
    char text[64];
    const int n = snprintf(text, sizeof(text), "%s=%s%s", kKeys[key], values[key], newLine ? "\r\n" : "");
    Append(out, outSize, used, text, n);
}

bool IsBlank(const Line& line) {
    return SkipBlanks(line, 0) >= line.length;
}

// `out` ends with its last line (with its line ending) followed by exactly one blank line, or is
// left empty when there is nothing in it yet. Blank lines at its end are dropped first.
void EndWithOneBlankLine(char* out, int outSize, int* used) {
    int content = *used;
    while (content > 0 && (out[content - 1] == '\r' || out[content - 1] == '\n')) --content;
    if (content == 0) {
        *used = 0;
        out[0] = '\0';
        return;
    }
    // Keep the last line's own ending (CR LF, LF or CR), then add one blank line.
    int keep = content;
    if (keep < *used) {
        keep += (out[keep] == '\r' && keep + 1 < *used && out[keep + 1] == '\n') ? 2 : 1;
    }
    *used = keep;
    out[*used] = '\0';
    if (keep == content) {
        Append(out, outSize, used, "\r\n", 2);
    }
    Append(out, outSize, used, "\r\n", 2);
}

// Rewrites `text` (the file as UTF-8) into `out` with the five [panel] keys set. Every other line
// is copied as it was, with its own line ending; added lines use CR LF. Keys missing from [panel]
// go right after its last line, and [panel] is separated from the sections around it by exactly
// one blank line (0.3.0 put missing keys after the blank line that ended the section, and
// appended [panel] with no blank line before it).
void MergePanelKeys(const char* text, uint32_t packed, char* out, int outSize) {
    char values[kKeyCount][16];
    ValueWords(packed, values);
    bool written[kKeyCount] = {};
    int used = 0;
    out[0] = '\0';
    bool inPanel = false;
    bool sawPanel = false;
    const char* heldBlanks = nullptr;   // blank lines inside [panel] not copied yet
    int heldLength = 0;
    auto addMissingKeys = [&]() {
        for (int k = 0; k < kKeyCount; ++k) {
            if (!written[k]) {
                if (used > 0 && out[used - 1] != '\n' && out[used - 1] != '\r') {
                    Append(out, outSize, &used, "\r\n", 2);
                }
                AppendKey(out, outSize, &used, k, values, true);
                written[k] = true;
            }
        }
    };
    const char* p = text;
    while (*p) {
        const char* end = strpbrk(p, "\r\n");
        const Line line = {p, end ? static_cast<int>(end - p) : static_cast<int>(strlen(p))};
        int endingLength = 0;
        if (end) {
            endingLength = (end[0] == '\r' && end[1] == '\n') ? 2 : 1;
        }
        bool isPanel = false;
        if (IsHeading(line, &isPanel)) {
            if (inPanel) {
                addMissingKeys();   // leaving [panel]: the keys it did not have, after its last line
            }
            if (inPanel || isPanel) {
                EndWithOneBlankLine(out, outSize, &used);   // the join next to [panel]
            }
            heldBlanks = nullptr;
            heldLength = 0;
            inPanel = isPanel;
            sawPanel = sawPanel || isPanel;
            Append(out, outSize, &used, line.text, line.length);
        } else if (inPanel && IsBlank(line)) {
            if (!heldBlanks) heldBlanks = line.text;
            heldLength = static_cast<int>(line.text + line.length + endingLength - heldBlanks);
            p = line.text + line.length + endingLength;
            continue;
        } else {
            if (heldBlanks) {
                Append(out, outSize, &used, heldBlanks, heldLength);   // blank lines inside [panel] stay
                heldBlanks = nullptr;
                heldLength = 0;
            }
            bool replaced = false;
            for (int k = 0; inPanel && k < kKeyCount && !replaced; ++k) {
                if (StartsWithKey(line, kKeys[k])) {
                    AppendKey(out, outSize, &used, k, values, false);
                    written[k] = true;
                    replaced = true;
                }
            }
            if (!replaced) {
                Append(out, outSize, &used, line.text, line.length);
            }
        }
        if (end) {
            Append(out, outSize, &used, end, endingLength);
        }
        p = line.text + line.length + endingLength;
    }
    if (used > 0 && out[used - 1] != '\n' && out[used - 1] != '\r') {
        Append(out, outSize, &used, "\r\n", 2);
    }
    if (!sawPanel) {
        EndWithOneBlankLine(out, outSize, &used);
        Append(out, outSize, &used, "[panel]\r\n", 9);
    }
    addMissingKeys();   // [panel] last in the file (its trailing blank lines are dropped), or just added
}

enum class SaveResult { Saved, Unreadable, TooLarge, NotCreated, NotWritten };

// LoadIniFileText reads at most this many bytes of a file. A larger file could only be merged by
// cutting off its end, so it is left as it is.
constexpr DWORD kMaxSettingsBytes = 16382;

// Reads, merges and replaces the file. Runs under the settings file lock (the music engine
// rewrites its [music] section in the same file), so it does not log; the caller does.
SaveResult ReplaceSettingsFile(const wchar_t* path, uint32_t packed, DWORD* error) {
    // A UTF-16 file grows by up to half when it becomes UTF-8, so the text buffers hold three
    // times what the reader takes; a smaller buffer would make the conversion fail and the merge
    // start from nothing.
    static char text[3 * 16384];
    static char merged[3 * 16384 + 512];
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &data) &&
        (data.nFileSizeHigh != 0 || data.nFileSizeLow > kMaxSettingsBytes)) {
        *error = ERROR_FILE_TOO_LARGE;
        return SaveResult::TooLarge;
    }
    bool bom = false;
    const IniFileState state = LoadIniFileText(path, text, sizeof(text), error, &bom);
    if (state == IniFileState::Unreadable) {
        return SaveResult::Unreadable;
    }
    if (state == IniFileState::Missing) {
        text[0] = '\0';
        bom = false;
    }
    MergePanelKeys(text, packed, merged, sizeof(merged));

    // Written to a new file first and then swapped in, so a crash mid-write cannot leave half a
    // file. UTF-8; a byte order mark is kept when the original had one (a UTF-16 file becomes
    // UTF-8 with a mark, which the reader accepts).
    wchar_t temporary[MAX_PATH + 8];
    lstrcpynW(temporary, path, MAX_PATH);
    lstrcatW(temporary, L".new");
    const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return SaveResult::NotCreated;
    }
    DWORD writtenBytes = 0;
    bool ok = true;
    if (bom) {
        const unsigned char mark[3] = {0xEF, 0xBB, 0xBF};
        ok = WriteFile(file, mark, 3, &writtenBytes, nullptr) != FALSE;
    }
    const DWORD length = static_cast<DWORD>(strlen(merged));
    ok = ok && WriteFile(file, merged, length, &writtenBytes, nullptr) && writtenBytes == length;
    CloseHandle(file);
    if (ok) {
        ok = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING) != FALSE;
    }
    if (!ok) {
        *error = GetLastError();
        DeleteFileW(temporary);
        return SaveResult::NotWritten;
    }
    return SaveResult::Saved;
}

void WriteSettings(uint32_t packed) {
    const wchar_t* path = UserSettingsPath();
    if (path[0] == L'\0') {
        Log("panel: the panel's settings cannot be saved because the settings folder is unknown");
        return;
    }
    char path8[MAX_PATH * 3];
    LogUtf8(path, path8, sizeof(path8));
    DWORD error = 0;
    SaveResult result;
    {
        const SettingsFileLock lock;
        result = ReplaceSettingsFile(path, packed, &error);
    }
    switch (result) {
        case SaveResult::Saved:
            Log("panel: settings saved to %s", path8);
            break;
        case SaveResult::Unreadable:
            Log("panel: the panel's settings were not saved because %s could not be read (error %lu)", path8, error);
            break;
        case SaveResult::TooLarge:
            Log("panel: the panel's settings were not saved because %s is larger than 16 KB, more than the panel "
                "reads; the file was left as it is so nothing in it is cut off",
                path8);
            break;
        case SaveResult::NotCreated:
            Log("panel: the panel's settings were not saved because a new copy of %s could not be created (error %lu)",
                path8, error);
            break;
        case SaveResult::NotWritten:
            Log("panel: the panel's settings were not saved to %s (error %lu)", path8, error);
            break;
    }
}

DWORD WINAPI WriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        const uint32_t packed = g_pending.exchange(0, std::memory_order_acquire);
        if (packed & kPendingFlag) {
            WriteSettings(packed);
            InterlockedIncrement(&g_written);
        }
    }
}

}  // namespace

void UiSettingsStartWriter() {
    if (g_wake) {
        return;
    }
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) {
        Log("panel: changes made in the panel will not be saved (the writer could not start, error %lu)",
            GetLastError());
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, WriterThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        Log("panel: changes made in the panel will not be saved (the writer thread could not start, error %lu)",
            GetLastError());
        CloseHandle(g_wake);
        g_wake = nullptr;
    }
}

void UiSettingsRequestSave(const UiSettings& settings) {
    if (!g_wake) {
        return;
    }
    g_pending.store(Pack(settings), std::memory_order_release);
    SetEvent(g_wake);
}

long UiSettingsSavesWritten() {
    return g_written;
}

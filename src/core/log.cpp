#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

CRITICAL_SECTION g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
wchar_t g_path[MAX_PATH] = L"";
wchar_t g_dir[MAX_PATH] = L"";
volatile LONG g_ready = 0;

constexpr int kKeptLogs = 5;
constexpr ULONGLONG kWeekIn100ns = 7ull * 24 * 60 * 60 * 10000000ull;

HANDLE OpenLog(const wchar_t* path) {
    // Readers are allowed. A second process cannot rename or reopen the file and takes the
    // fallback name instead (do not add FILE_SHARE_DELETE: it would let a second process
    // rename the first one's live log).
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

void AppendNumber(wchar_t* text, unsigned long value, int digits) {
    wchar_t buffer[16];
    int n = 0;
    do {
        buffer[n++] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    } while (value && n < 15);
    while (n < digits && n < 15) {
        buffer[n++] = L'0';
    }
    wchar_t* end = text + lstrlenW(text);
    while (n > 0) {
        *end++ = buffer[--n];
    }
    *end = L'\0';
}

bool AllDigits(const wchar_t* text, int length) {
    for (int i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9') {
            return false;
        }
    }
    return length > 0;
}

ULONGLONG ToUInt64(FILETIME time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

// Moves mod.log aside as mod.<date>-<time>.log (named after its last write), keeps the newest
// five of those, and deletes mod.<process id>.log files older than a week.
void ArchiveEarlierLogs() {
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(g_path, GetFileExInfoStandard, &attributes)) {
        FILETIME local = {};
        SYSTEMTIME t = {};
        FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
        wchar_t archived[MAX_PATH];
        lstrcpyW(archived, g_dir);
        lstrcatW(archived, L"\\mod.");
        AppendNumber(archived, t.wYear, 4);
        AppendNumber(archived, t.wMonth, 2);
        AppendNumber(archived, t.wDay, 2);
        lstrcatW(archived, L"-");
        AppendNumber(archived, t.wHour, 2);
        AppendNumber(archived, t.wMinute, 2);
        AppendNumber(archived, t.wSecond, 2);
        lstrcatW(archived, L".log");
        // Fails while another process still holds mod.log; that process keeps its log and this
        // one falls back to mod.<process id>.log below.
        MoveFileExW(g_path, archived, MOVEFILE_REPLACE_EXISTING);
    }

    wchar_t pattern[MAX_PATH];
    lstrcpyW(pattern, g_dir);
    lstrcatW(pattern, L"\\mod.*.log");
    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    FILETIME nowTime = {};
    GetSystemTimeAsFileTime(&nowTime);
    const ULONGLONG now = ToUInt64(nowTime);

    constexpr int kMaxTracked = 64;
    wchar_t archives[kMaxTracked][32];
    int archiveCount = 0;
    do {
        const wchar_t* name = found.cFileName;
        const int length = lstrlenW(name);
        if (lstrcmpiW(name, L"mod.log") == 0 || length < 9 || length >= 32) {
            continue;
        }
        const wchar_t* middle = name + 4;   // between "mod." and ".log"
        const int middleLength = length - 8;
        const bool dated = middleLength == 15 && middle[8] == L'-' && AllDigits(middle, 8) && AllDigits(middle + 9, 6);
        if (dated) {
            if (archiveCount < kMaxTracked) {
                lstrcpyW(archives[archiveCount++], name);
            }
        } else if (AllDigits(middle, middleLength) && now - ToUInt64(found.ftLastWriteTime) > kWeekIn100ns) {
            wchar_t full[MAX_PATH];
            lstrcpyW(full, g_dir);
            lstrcatW(full, L"\\");
            lstrcatW(full, name);
            DeleteFileW(full);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);

    // The date in the name sorts as text, newest last. Delete everything but the newest five.
    for (int i = 1; i < archiveCount; ++i) {
        wchar_t current[32];
        lstrcpyW(current, archives[i]);
        int j = i - 1;
        while (j >= 0 && lstrcmpW(archives[j], current) > 0) {
            lstrcpyW(archives[j + 1], archives[j]);
            --j;
        }
        lstrcpyW(archives[j + 1], current);
    }
    for (int i = 0; i < archiveCount - kKeptLogs; ++i) {
        wchar_t full[MAX_PATH];
        lstrcpyW(full, g_dir);
        lstrcatW(full, L"\\");
        lstrcatW(full, archives[i]);
        DeleteFileW(full);
    }
}

}  // namespace

void LogOpen() {
    if (g_ready) {
        return;
    }
    InitializeCriticalSection(&g_lock);

    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", g_dir, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 48) {
        g_dir[0] = L'\0';
        InterlockedExchange(&g_ready, 1);
        return;
    }
    lstrcatW(g_dir, L"\\RacingMasterMod");
    CreateDirectoryW(g_dir, nullptr);

    lstrcpyW(g_path, g_dir);
    lstrcatW(g_path, L"\\mod.log");
    ArchiveEarlierLogs();

    g_file = OpenLog(g_path);
    if (g_file == INVALID_HANDLE_VALUE) {
        // Another copy of the game still holds mod.log: write a log of our own instead of none.
        lstrcpyW(g_path, g_dir);
        lstrcatW(g_path, L"\\mod.");
        AppendNumber(g_path, GetCurrentProcessId(), 1);
        lstrcatW(g_path, L".log");
        g_file = OpenLog(g_path);
    }
    if (g_file == INVALID_HANDLE_VALUE) {
        g_path[0] = L'\0';
    }
    InterlockedExchange(&g_ready, 1);
}

void Log(const char* format, ...) {
    if (!g_ready || g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", t.wYear, t.wMonth, t.wDay, t.wHour,
                     t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, format);
    const int m = vsnprintf(line + n, sizeof(line) - n - 2, format, args);
    va_end(args);
    if (m > 0) {
        n += (m < static_cast<int>(sizeof(line)) - n - 2) ? m : static_cast<int>(sizeof(line)) - n - 3;
    }
    line[n++] = '\r';
    line[n++] = '\n';

    // No FlushFileBuffers: WriteFile hands the line to Windows, which keeps it even if this
    // process dies. A disk flush here cost about a millisecond on the thread that presents frames.
    EnterCriticalSection(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

const char* LogUtf8(const wchar_t* text, char* buffer, int bufferSize) {
    if (bufferSize <= 0) {
        return "";
    }
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, bufferSize, nullptr, nullptr);
    if (written <= 0) {
        buffer[0] = '\0';
    }
    return buffer;
}

const wchar_t* LogPath() {
    return g_path;
}

const wchar_t* LogDirectory() {
    return g_dir;
}

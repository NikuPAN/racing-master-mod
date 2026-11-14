#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

CRITICAL_SECTION g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
wchar_t g_path[MAX_PATH] = L"";
bool g_ready = false;

}  // namespace

void LogOpen() {
    if (g_ready) {
        return;
    }
    InitializeCriticalSection(&g_lock);
    g_ready = true;

    wchar_t dir[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 40) {
        return;
    }
    lstrcatW(dir, L"\\RacingMasterMod");
    CreateDirectoryW(dir, nullptr);

    wchar_t previous[MAX_PATH];
    lstrcpyW(g_path, dir);
    lstrcatW(g_path, L"\\mod.log");
    lstrcpyW(previous, dir);
    lstrcatW(previous, L"\\mod.prev.log");
    MoveFileExW(g_path, previous, MOVEFILE_REPLACE_EXISTING);

    g_file = CreateFileW(g_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        g_path[0] = L'\0';
    }
}

void Log(const char* format, ...) {
    if (!g_ready || g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", t.wYear, t.wMonth,
                     t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, format);
    const int m = vsnprintf(line + n, sizeof(line) - n - 2, format, args);
    va_end(args);
    if (m > 0) {
        n += (m < static_cast<int>(sizeof(line)) - n - 2) ? m : static_cast<int>(sizeof(line)) - n - 3;
    }
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_lock);
    DWORD written = 0;
    WriteFile(g_file, line, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(g_file);
    LeaveCriticalSection(&g_lock);
}

const wchar_t* LogPath() {
    return g_path;
}

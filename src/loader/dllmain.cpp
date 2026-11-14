// Entry point of the stand-in version.dll.
//
// In every process it only forwards the 17 version.dll functions to Windows. The mod itself
// starts in exactly one case: the process is the game (g112-Win64-Shipping.exe), or the
// developer test host has set RACING_MASTER_MOD_TEST_HOST=1.

#include "proxy.h"

#include "../core/log.h"
#include "../core/present_hook.h"

#include <windows.h>

#include <cwchar>

namespace {

constexpr wchar_t kGameExe[] = L"g112-Win64-Shipping.exe";
constexpr wchar_t kIniName[] = L"racing-master-mod.ini";
constexpr char kVersion[] = "0.1.0 (loader test)";

HMODULE g_self = nullptr;
bool g_testHost = false;

struct Settings {
    // 0 = do nothing, 1 = load and write the log only, 2 = also hook frames and draw the marker.
    unsigned stage = 2;
    unsigned markerSeconds = 20;
};

Settings ReadSettings() {
    Settings settings;
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return settings;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || (slash - path) + 1 + lstrlenW(kIniName) >= MAX_PATH) {
        return settings;
    }
    lstrcpyW(slash + 1, kIniName);
    settings.stage = GetPrivateProfileIntW(L"mod", L"stage", static_cast<int>(settings.stage), path);
    settings.markerSeconds =
        GetPrivateProfileIntW(L"mod", L"marker_seconds", static_cast<int>(settings.markerSeconds), path);
    if (settings.stage > 2) settings.stage = 2;
    if (settings.markerSeconds > 600) settings.markerSeconds = 600;
    return settings;
}

struct WindowSearch {
    DWORD pid;
    bool requireVisible;
    HWND found;
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid) {
        return TRUE;
    }
    if (search->requireVisible) {
        RECT rect = {};
        if (!IsWindowVisible(window) || !GetClientRect(window, &rect) || rect.right < 320 || rect.bottom < 200) {
            return TRUE;
        }
    }
    search->found = window;
    return FALSE;
}

DWORD WINAPI ModThread(LPVOID) {
    LogOpen();
    const Settings settings = ReadSettings();

    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("Racing Master Mod %s loaded into %ls (process %lu)", kVersion, exe, GetCurrentProcessId());
    Log("settings: stage %u, marker for %u s%s", settings.stage, settings.markerSeconds,
        g_testHost ? " (developer test host)" : "");

    if (settings.stage == 0) {
        Log("stage 0: the mod is switched off in %ls; nothing else will happen", kIniName);
        return 0;
    }
    if (settings.stage == 1) {
        Log("stage 1: loaded only; frames are not hooked");
        for (unsigned minutes = 1;; ++minutes) {
            Sleep(60000);
            Log("still running after %u min", minutes);
        }
    }

    // Wait until the game has its window before touching Direct3D.
    WindowSearch search = {GetCurrentProcessId(), !g_testHost, nullptr};
    for (unsigned waited = 0; !search.found; waited += 250) {
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
        if (!search.found) {
            if (waited == 120000) {
                Log("still waiting for the game window after 2 min");
            }
            Sleep(250);
        }
    }
    Log("game window found");
    if (!g_testHost) {
        Sleep(3000);
    }

    if (InstallPresentHook(settings.markerSeconds)) {
        Log("ready: a white square appears in the top-left corner for %u s", settings.markerSeconds);
    } else {
        Log("the frame hook was not installed; the game runs unmodified");
    }
    return 0;
}

bool IsGameProcess() {
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return lstrcmpiW(name, kGameExe) == 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    g_self = instance;
    DisableThreadLibraryCalls(instance);
    if (!LoadRealVersionDll()) {
        return FALSE;
    }

    wchar_t flag[8];
    g_testHost = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_HOST", flag, 8) == 1 && flag[0] == L'1';
    if (g_testHost || IsGameProcess()) {
        // The thread starts running only after Windows has finished loading this DLL.
        const HANDLE thread = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}

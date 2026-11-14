#include "worker.h"

#include "log.h"
#include "present_hook.h"
#include "settings.h"

#include "../music/player.h"
#include "../remote/remote.h"
#include "../ui/overlay.h"

namespace {

constexpr char kVersion[] = "0.3.0-dev (loader and music panel)";

HMODULE g_self = nullptr;
bool g_testHost = false;

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
    RECT rect = {};
    if (!GetClientRect(window, &rect) || rect.right < 320 || rect.bottom < 200) {
        return TRUE;
    }
    if (search->requireVisible && !IsWindowVisible(window)) {
        return TRUE;
    }
    search->found = window;
    return FALSE;
}

// "12" -> 12; anything else -> 0.
unsigned ParseDigits(const wchar_t* text) {
    unsigned value = 0;
    if (*text == L'\0') {
        return 0;
    }
    for (const wchar_t* p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9' || value > 100000000) {
            return 0;
        }
        value = value * 10 + static_cast<unsigned>(*p - L'0');
    }
    return value;
}

// Runs after the hook is in place: the periodic status line and the watchdog. Nothing periodic
// is written from the thread that presents frames.
void WatchFrames() {
    const ULONGLONG attachedAt = GetTickCount64();
    ULONGLONG lastReport = attachedAt;
    unsigned long long framesAtLastReport = 0;
    bool warned = false;
    bool resumedLogged = false;
    for (;;) {
        Sleep(1000);
        if (PresentHookSwitchedOff()) {
            return;   // the fault handler already wrote the reason
        }
        const PresentHookCounters counters = PresentHookGetCounters();
        const ULONGLONG now = GetTickCount64();
        if (!warned && now - attachedAt >= 10000) {
            warned = true;
            if (counters.gameFrames == 0) {
                if (counters.hookCalls == 0) {
                    Log("no frame has reached the mod 10 s after it attached (hook calls 0, game frames 0): the game "
                        "is showing its frames through something the mod cannot see, such as an overlay (Discord, "
                        "Steam, the NVIDIA app) or another mod that loads first. The mod is idle.");
                } else {
                    Log("frames reach the mod (%llu so far) but none came from a visible window of at least 320x200 "
                        "(game frames 0): the game may be minimised or still starting. The mod stays idle until "
                        "such a window shows frames.",
                        counters.hookCalls);
                }
            } else {
                resumedLogged = true;
            }
        } else if (warned && !resumedLogged && counters.gameFrames != 0) {
            resumedLogged = true;
            Log("frames from the game window have started arriving (hook calls %llu, game frames %llu)",
                counters.hookCalls, counters.gameFrames);
        }
        if (now - lastReport >= 30000) {
            const double seconds = static_cast<double>(now - lastReport) / 1000.0;
            Log("running: %llu hook calls, %llu game frames so far, %.0f frames per second over the last %.0f s",
                counters.hookCalls, counters.gameFrames,
                static_cast<double>(counters.gameFrames - framesAtLastReport) / seconds, seconds);
            lastReport = now;
            framesAtLastReport = counters.gameFrames;
        }
    }
}

DWORD WINAPI ModThread(LPVOID) {
    LogOpen();

    // First line of every log: which program loaded the mod. The settings lines follow.
    wchar_t exe[MAX_PATH] = L"";
    char exe8[MAX_PATH * 3];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("Racing Master Mod %s loaded into %s (process %lu)", kVersion, LogUtf8(exe, exe8, sizeof(exe8)),
        GetCurrentProcessId());

    const Settings settings = ReadSettings(g_self);
    Log("settings: stage %u, marker for %u s%s", settings.stage, settings.markerSeconds,
        g_testHost ? " (developer test host)" : "");

    if (settings.stage == 0) {
        Log("stage 0: the mod is switched off; nothing else will happen");
        return 0;
    }
    // Developer test host only: the media-session test script, when one is set. In the game the
    // music panel starts the remote side itself; nothing here touches it.
    if (g_testHost) {
        remote::StartTestScriptIfAsked();
    }

    if (settings.stage == 1) {
        Log("stage 1: loaded only; frames are not hooked");
        for (unsigned minutes = 1;; ++minutes) {
            Sleep(60000);
            Log("still running after %u min", minutes);
        }
    }

    // Wait until the game has its window before touching Direct3D. Poll four times a second,
    // once a second after two minutes.
    WindowSearch search = {GetCurrentProcessId(), !g_testHost, nullptr};
    for (ULONGLONG waited = 0; !search.found;) {
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
        if (!search.found) {
            if (waited == 120000) {
                Log("still waiting for the game window after 2 min");
            }
            const DWORD interval = waited < 120000 ? 250 : 1000;
            Sleep(interval);
            waited += interval;
        }
    }
    Log("game window found");
    {
        // user32 is delay-loaded. The frame thread uses these three functions to recognise the
        // game's swap chain, so each is called once here and never resolved on the frame thread.
        DWORD pid = 0;
        RECT rect = {};
        GetWindowThreadProcessId(search.found, &pid);
        GetClientRect(search.found, &rect);
        IsWindowVisible(search.found);
    }
    if (!g_testHost) {
        Sleep(3000);
    }
    // The music panel's start-up work that must not happen on the game's threads (settings,
    // hotkeys, loading the shader compiler). Before the hook, so the first frame finds it done.
    PanelStart(settings.ui, g_testHost);
    // The panel's "My files" is the music engine: without a panel there is no way to control it,
    // so it starts only with one. It reads the saved list and starts its two threads; no sound
    // device and no Media Foundation until the first song plays.
    if (settings.ui.mode != UiModeOff) {
        music::Player::Get().Start();
    }

    PresentHookOptions options;
    options.markerSeconds = settings.markerSeconds;
    options.acceptHiddenWindow = g_testHost;
    if (g_testHost) {
        wchar_t fault[16] = L"";
        const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_FAULT", fault, 16);
        if (n > 0 && n < 16) {
            options.testFaultAtFrame = ParseDigits(fault);
            if (options.testFaultAtFrame) {
                Log("test host: a deliberate fault is scheduled for frame %u", options.testFaultAtFrame);
            }
        }
    }
    if (!InstallPresentHook(options)) {
        Log("the frame hook was not installed; the game runs unmodified");
        return 0;
    }
    Log("ready: a white square appears in the top-left corner for %u s", settings.markerSeconds);
    WatchFrames();
    return 0;
}

}  // namespace

bool ModIsTestHost() {
    return g_testHost;
}

void StartModThread(HMODULE self, bool testHost) {
    g_self = self;
    g_testHost = testHost;
    // The thread starts running only after Windows has finished loading this DLL.
    const HANDLE thread = CreateThread(nullptr, 0, ModThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

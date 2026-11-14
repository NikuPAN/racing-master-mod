#include "worker.h"

#include "log.h"
#include "present_hook.h"
#include "settings.h"

#include "../music/player.h"
#include "../remote/remote.h"
#include "../ui/other_app.h"
#include "../ui/overlay.h"

#include <string>

namespace {

constexpr char kVersion[] = "0.3.0 (music panel, music player, control of other music apps)";

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

// Watches the window the panel draws into. The game is closing when that window is gone and no
// frame has reached the hook for a second; a window that is gone while frames still arrive (the
// game moved to a new window) is not a close, and is logged once.
class GameWindowWatch {
public:
    // Called every 250 ms. True once the game window has closed.
    bool Closed() {
        const HWND window = PanelGameWindow();
        if (!window || IsWindow(window)) {
            goneSeen_ = false;
            quietTicks_ = 0;
            return false;
        }
        const unsigned long long calls = PresentHookGetCounters().hookCalls;
        if (!goneSeen_) {
            // The first tick without the window only takes the count: frames drawn in the last
            // 250 ms before the window went are not "frames still arrive". It counts as the first
            // quiet tick, so the music still stops about a second after the window went.
            goneSeen_ = true;
            lastCalls_ = calls;
            quietTicks_ = 1;
            return false;
        }
        if (calls != lastCalls_) {
            lastCalls_ = calls;
            quietTicks_ = 0;
            if (!replacedLogged_) {
                replacedLogged_ = true;
                Log("the game window closed but frames still arrive; the music keeps playing");
            }
            return false;
        }
        return ++quietTicks_ >= 4;
    }

private:
    unsigned long long lastCalls_ = 0;
    unsigned quietTicks_ = 0;
    bool goneSeen_ = false;
    bool replacedLogged_ = false;
};

// When the game closes: the music fades out instead of being cut off mid-sound, the list and
// settings get their final save, and the other-app control lets go.
void StopMusicParts() {
    Log("music: the game window has closed; stopping the music and saving the list");
    music::Player::Get().Shutdown();
    other_app::Shutdown();
    Log("music: stopped and saved");
}

// Runs after the hook is in place: the periodic status line, the watchdog, and (when the music
// started) the game window, every 250 ms. Nothing periodic is written from the thread that
// presents frames.
void WatchFrames(bool musicStarted) {
    const ULONGLONG attachedAt = GetTickCount64();
    ULONGLONG lastReport = attachedAt;
    unsigned long long framesAtLastReport = 0;
    bool warned = false;
    bool resumedLogged = false;
    bool faultHandled = false;
    GameWindowWatch window;
    for (unsigned tick = 1;; ++tick) {
        Sleep(250);
        if (musicStarted && window.Closed()) {
            StopMusicParts();
            return;
        }
        const bool hookOff = PresentHookSwitchedOff();
        if (!faultHandled && (hookOff || PanelSwitchedOff())) {
            // The panel is the only way to pause or stop the music in the game. Once it is off for
            // the session (a fault in the frame code, which also stops the marker; or a panel
            // error on its own: an ImGui assertion or a fault in the window procedure), a song
            // that is playing would play on with nothing to stop it.
            faultHandled = true;
            if (musicStarted) {
                music::Player::Get().Pause();
                if (hookOff) {
                    Log("music: paused, because the panel that controls it has switched off with the rest of the "
                        "mod after the error above; it plays again after the game is restarted and a song is chosen");
                } else {
                    Log("music: paused, because the music panel has switched itself off after an error (see the "
                        "panel line in this log); it plays again after the game is restarted and a song is chosen");
                }
            }
        }
        if (hookOff) {
            if (!musicStarted) {
                return;
            }
            continue;   // only the game window is still watched, for the final save
        }
        if (tick % 4 != 0) {
            continue;
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

// After PanelStart and before the frame hook, so the panel's first frame and the first hotkey
// find both parts running. The music player ("My files") is controlled only through the panel,
// so it starts only with one; it reads the saved list and starts its two threads, and opens no
// sound device and no Media Foundation until the first song plays. The control of other music
// apps starts here only when "Other app" was the player's last choice; otherwise the panel starts
// it when the player picks "Other app".
bool StartMusicParts(const UiSettings& ui) {
    if (ui.mode == UiModeOff) {
        Log("music: neither your own songs nor other music apps are started, because the panel is off "
            "([panel] mode=off)");
        return false;
    }
    music::Player& player = music::Player::Get();
    player.Start();
    char folder8[MAX_PATH * 3] = "";
    const std::wstring folder = player.MusicFolder();
    LogUtf8(folder.c_str(), folder8, sizeof(folder8));
    Log("music: the player for your own songs has started; songs copied into %s are added at every start",
        folder8[0] ? folder8 : "(no folder: LOCALAPPDATA is not set)");
    if (ui.source == 1) {
        other_app::Start();   // "Other app" was the player's last choice
    }
    return true;
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
    const bool musicStarted = StartMusicParts(settings.ui);

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
    WatchFrames(musicStarted);
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

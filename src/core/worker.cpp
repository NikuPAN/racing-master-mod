#include "worker.h"

#include "game_close.h"
#include "log.h"
#include "present_hook.h"
#include "present_probe.h"
#include "settings.h"

#include "../music/player.h"
#include "../remote/remote.h"
#include "../ui/other_app.h"
#include "../ui/overlay.h"

#include <cstdio>
#include <string>

namespace {

constexpr char kVersion[] = "0.3.1 (music panel, music player, control of other music apps)";

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

// Watches the window the panel draws into: the backstop for a close the window procedure did not
// see (display mode has no window procedure; a game may also end its window without the
// messages). The game is closing when that window is gone and no frame has reached the hook for a
// second; a window that is gone while frames still arrive (the game moved to a new window) is not
// a close, and is logged once. After a close request the second is not waited for.
class GameWindowWatch {
public:
    // Called every 250 ms. True once the game window has closed.
    bool Closed(bool closeRequested) {
        const HWND window = PanelGameWindow();
        if (!window || IsWindow(window)) {
            goneSeen_ = false;
            quietTicks_ = 0;
            return false;
        }
        if (closeRequested) {
            return true;
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

// One log line whenever the game window changes and then holds for about 0.75 s: its size,
// windowed / borderless / exclusive full screen, minimised, and whether it is the window in
// front. So a test run's log shows that a resize, an alt-tab or a mode switch happened and that
// the mod carried on through it. The frame thread only samples the swap chain's full-screen state
// (about once a second, so a switch to or from exclusive full screen shows up to a second late);
// everything else is read fresh here, on the worker. The window procedure writes nothing.
class WindowStateLog {
public:
    // Called every 250 ms.
    void Tick() {
        const PresentHookWindowState chain = PresentHookGetWindowState();
        const HWND window = static_cast<HWND>(chain.window);
        if (!chain.known || !window || !IsWindow(window)) {
            return;
        }
        State now;
        now.minimised = IsIconic(window) != FALSE;
        now.inFront = !now.minimised && GetForegroundWindow() == window;
        RECT client = {};
        if (GetClientRect(window, &client)) {
            now.width = static_cast<unsigned>(client.right);
            now.height = static_cast<unsigned>(client.bottom);
        }
        now.kind = chain.exclusiveFullscreen ? Kind::Exclusive
                                             : (CoversItsMonitor(window) ? Kind::Borderless : Kind::Windowed);
        if (!havePrevious_ || !Same(now, previous_)) {
            havePrevious_ = true;
            previous_ = now;   // changed: wait for it to settle (a drag-resize logs once, at the end)
            steady_ = 0;
            return;
        }
        if (++steady_ < kSteadyTicks) {
            return;
        }
        if (logged_ && Same(now, last_)) {
            return;
        }
        logged_ = true;
        last_ = now;
        if (now.minimised) {
            Log("game window: minimised");
            return;
        }
        static const char* const kKinds[] = {"windowed", "borderless full screen", "exclusive full screen"};
        Log("game window: %ux%u, %s, %s", now.width, now.height, kKinds[static_cast<int>(now.kind)],
            now.inFront ? "in front" : "behind another window");
    }

private:
    enum class Kind { Windowed = 0, Borderless = 1, Exclusive = 2 };
    struct State {
        bool minimised = false;
        bool inFront = false;
        unsigned width = 0;
        unsigned height = 0;
        Kind kind = Kind::Windowed;
    };
    static bool Same(const State& a, const State& b) {
        if (a.minimised || b.minimised) {
            return a.minimised == b.minimised;   // size and mode are stale while minimised
        }
        return a.inFront == b.inFront && a.width == b.width && a.height == b.height && a.kind == b.kind;
    }
    // A window without a title bar that covers its whole monitor.
    static bool CoversItsMonitor(HWND window) {
        if ((GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {
            return false;
        }
        RECT rect = {};
        MONITORINFO monitor = {};
        monitor.cbSize = sizeof(monitor);
        const HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (!handle || !GetWindowRect(window, &rect) || !GetMonitorInfoW(handle, &monitor)) {
            return false;
        }
        return rect.left <= monitor.rcMonitor.left && rect.top <= monitor.rcMonitor.top &&
               rect.right >= monitor.rcMonitor.right && rect.bottom >= monitor.rcMonitor.bottom;
    }

    static constexpr unsigned kSteadyTicks = 3;   // three more samples alike: 0.75 s
    State previous_;
    State last_;
    unsigned steady_ = 0;
    bool havePrevious_ = false;
    bool logged_ = false;
};

// How often the present probe writes its block: 30 s; the test host may ask for less with
// RACING_MASTER_MOD_TEST_PROBE_SECONDS. Worker only.
unsigned ProbeIntervalSeconds() {
    static unsigned seconds = 0;
    if (seconds == 0) {
        seconds = 30;
        wchar_t value[16] = L"";
        const DWORD n = g_testHost ? GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_PROBE_SECONDS", value, 16) : 0;
        const unsigned asked = n > 0 && n < 16 ? ParseDigits(value) : 0;
        if (asked >= 1 && asked <= 600) {
            seconds = asked;
        }
    }
    return seconds;
}

// NVIDIA Smooth Motion (the driver's frame generation) works through NvPresent64.dll, which the
// driver loads into the game. With it on, the owner's game run saw 120 presents a second and the
// panel blinking as a whole. One plain line in the log, nothing on the panel. Checked once a
// second from the worker (GetModuleHandleW only: this process, no other) until it is seen. In the
// test host RACING_MASTER_MOD_TEST_SMOOTH_MOTION_DLL names another DLL to look for.
class SmoothMotionCheck {
public:
    void Tick() {
        if (logged_) {
            return;
        }
        if (!name_[0]) {
            lstrcpyW(name_, L"NvPresent64.dll");
            wchar_t test[64] = L"";
            const DWORD n = g_testHost ? GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_SMOOTH_MOTION_DLL", test, 64) : 0;
            if (n > 0 && n < 64) {
                lstrcpyW(name_, test);
            }
        }
        if (GetModuleHandleW(name_)) {
            logged_ = true;
            Log("NVIDIA Smooth Motion is on for this game (NvPresent64.dll is loaded); the music panel may blink. Turn "
                "Smooth Motion off for Racing Master in the NVIDIA app to keep the panel steady.");
        }
    }

private:
    wchar_t name_[64] = L"";
    bool logged_ = false;
};

// When the game closes: the music fades out instead of being cut off mid-sound, the list and
// settings get their final save, a window procedure waiting for that is released, and the
// other-app control lets go (in the game it only lets go and sleeps, so it comes last).
void StopMusicParts(bool closing) {
    if (closing) {
        Log("music: the game window is closing; stopping the music and saving the list");
    } else {
        Log("music: the game window has closed; stopping the music and saving the list");
    }
    music::Player::Get().Shutdown();
    Log("music: stopped and saved");
    GameCloseMusicStopped();
    other_app::Shutdown();
}

// The player asking the game to close, as the window procedure saw it (game_close.h). A request
// saves the song and its place at once and lets the music play on: the game may still stay open
// (a question first, or a close that is cancelled). The window going stops the music.
class CloseWatch {
public:
    // Called whenever the worker wakes. True when the music must stop now.
    bool Check(ULONGLONG now) {
        const GameCloseState state = GameCloseRead();
        if (state.ending) {
            return true;
        }
        if (state.requests != seenRequests_) {
            seenRequests_ = state.requests;
            requestedAt_ = now;
            stillOpenLogged_ = false;
            music::Player::Get().SaveNow();
            Log("music: the game was asked to close; the song and its place are saved now, and the music plays on "
                "until the game window closes");
        }
        if (requestedAt_ && !stillOpenLogged_ && now - requestedAt_ >= 3000) {
            stillOpenLogged_ = true;
            const HWND window = PanelGameWindow();
            if (window && IsWindow(window)) {
                Log("music: the game is still open 3 s after it was asked to close (it may be asking first, or the "
                    "close was cancelled); the music carries on");
            }
        }
        return false;
    }
    bool Requested() const { return seenRequests_ != 0; }

private:
    unsigned seenRequests_ = 0;
    ULONGLONG requestedAt_ = 0;
    bool stillOpenLogged_ = false;
};

// Sleeps until `until` (GetTickCount64), waking early whenever the window procedure signals a
// close. True when it woke for that.
bool WaitForTickOrClose(ULONGLONG until) {
    const HANDLE wake = GameCloseWakeEvent();
    const ULONGLONG now = GetTickCount64();
    const DWORD wait = until > now ? static_cast<DWORD>(until - now) : 0;
    if (!wake) {
        Sleep(wait);
        return false;
    }
    return WaitForSingleObject(wake, wait) == WAIT_OBJECT_0;
}

// Runs after the hook is in place: the periodic status line, the watchdog, and (when the music
// started) the game window, every 250 ms, and at once when the window procedure reports a close.
// Nothing periodic is written from the thread that presents frames.
void WatchFrames(bool musicStarted) {
    const ULONGLONG attachedAt = GetTickCount64();
    ULONGLONG lastReport = attachedAt;
    unsigned long long framesAtLastReport = 0;
    bool warned = false;
    bool resumedLogged = false;
    bool faultHandled = false;
    bool concurrentLogged = false;
    bool otherThreadLogged = false;
    bool threadMoveLogged = false;
    ULONGLONG lastProbeReport = attachedAt;
    SmoothMotionCheck smoothMotion;
    smoothMotion.Tick();
    GameWindowWatch window;
    WindowStateLog windowLog;
    CloseWatch close;
    GameCloseSetWatching(musicStarted);
    ULONGLONG nextTick = attachedAt + 250;
    for (unsigned tick = 1;; ++tick) {
        // Between ticks only a close is acted on; the rest keeps its 250 ms rhythm.
        while (GetTickCount64() < nextTick) {
            if (WaitForTickOrClose(nextTick) && musicStarted && close.Check(GetTickCount64())) {
                StopMusicParts(true);
                return;
            }
        }
        nextTick += 250;
        if (GetTickCount64() > nextTick + 1000) {
            nextTick = GetTickCount64() + 250;   // fell behind (a slow step): no burst of catch-up ticks
        }
        if (musicStarted) {
            const bool ending = close.Check(GetTickCount64());
            if (ending || window.Closed(close.Requested())) {
                StopMusicParts(ending || close.Requested());
                return;
            }
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
        windowLog.Tick();
        if (tick % 4 != 0) {
            continue;
        }
        const PresentHookCounters counters = PresentHookGetCounters();
        const ULONGLONG now = GetTickCount64();
        smoothMotion.Tick();
        if (g_probeOn && now - lastProbeReport >= ProbeIntervalSeconds() * 1000ull) {
            PresentHookProbeReport(static_cast<unsigned>((now - lastProbeReport + 500) / 1000));
            lastProbeReport = now;
        }
        if (!otherThreadLogged && counters.otherThreadPresents != 0) {
            otherThreadLogged = true;
            Log("the game's frames also arrive from a second thread (%llu so far); the mod draws the panel and the marker "
                "only on the thread that showed the game's frames first and passes the others on untouched, because "
                "drawing from two threads could disturb the game's own drawing. NVIDIA Smooth Motion and similar "
                "driver features do this.",
                counters.otherThreadPresents);
        }
        if (!threadMoveLogged && counters.frameThreadMoves != 0) {
            threadMoveLogged = true;
            Log("the game's frames now come from another thread (the first showed none for a second); the mod draws "
                "there from now on");
        }
        if (!concurrentLogged && counters.concurrentSkips != 0) {
            concurrentLogged = true;
            Log("frames were shown from two threads at the same moment (%llu so far); each one that arrived while "
                "another was being drawn was passed on without the panel and the marker, so the mod's drawing never "
                "runs twice at once.",
                counters.concurrentSkips);
        }
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
            char skips[256] = "";
            int used = 0;
            if (counters.otherThreadPresents != 0) {
                used += snprintf(skips + used, sizeof(skips) - static_cast<size_t>(used),
                                 ", %llu from a second thread passed on", counters.otherThreadPresents);
            }
            if (counters.frameThreadMoves != 0) {
                used += snprintf(skips + used, sizeof(skips) - static_cast<size_t>(used),
                                 ", moved to another thread %llu times", counters.frameThreadMoves);
            }
            if (counters.concurrentSkips != 0) {
                snprintf(skips + used, sizeof(skips) - static_cast<size_t>(used),
                         ", %llu passed on while another thread was drawing", counters.concurrentSkips);
            }
            Log("running: %llu hook calls, %llu game frames so far, %.0f frames per second over the last %.0f s%s",
                counters.hookCalls, counters.gameFrames,
                static_cast<double>(counters.gameFrames - framesAtLastReport) / seconds, seconds, skips);
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
    // The close events first: the window procedure subclass (installed on the first frame) uses them.
    GameCloseInit();
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
    if (settings.presentProbe) {
        // Before the hook exists, so every present it sees is recorded.
        InterlockedExchange(&g_probeOn, 1);
        Log("present probe: on ([debug] present_probe=1); a block of \"present probe\" lines follows every %u s",
            ProbeIntervalSeconds());
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

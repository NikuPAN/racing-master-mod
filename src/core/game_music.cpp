#include "game_music.h"

#include "log.h"

#include <windows.h>

namespace game_music {
namespace {

// Wwise's own functions, by their exported (decorated) names in the game exe.
using IsInitializedFn = bool (*)();
using MuteBackgroundMusicFn = void (*)(bool);
using GetBackgroundMusicMuteFn = bool (*)();
constexpr const char kIsInitialized[] = "?IsInitialized@SoundEngine@AK@@YA_NXZ";
constexpr const char kMuteBackgroundMusic[] = "?MuteBackgroundMusic@SoundEngine@AK@@YAX_N@Z";
constexpr const char kGetBackgroundMusicMute[] = "?GetBackgroundMusicMute@SoundEngine@AK@@YA_NXZ";

IsInitializedFn g_isInitialized = nullptr;
MuteBackgroundMusicFn g_mute = nullptr;
GetBackgroundMusicMuteFn g_getMute = nullptr;

// Written by the worker, read by the frame thread.
volatile LONG g_availability = static_cast<LONG>(Availability::Looking);
// What the mod keeps: -1 = leave the game alone (the setting is off and the button never pressed),
// 0 = music on, 1 = music off. Written by the worker at start and by the panel's button.
volatile LONG g_wanted = -1;

// Worker only.
bool g_started = false;
unsigned g_ticks = 0;
bool g_waitLogged = false;
LONG g_lastApplied = -1;        // the state the mod last set
unsigned g_reapplied = 0;       // times the game (or anything else) changed it back

enum class CallResult { NotReady, AlreadySo, Changed, Faulted };

// The Wwise calls, under a fault guard: a fault switches the feature off for the session, the
// game carries on. No C++ objects here, so __try is allowed.
CallResult ApplyGuarded(bool mute) {
    __try {
        if (!g_isInitialized()) {
            return CallResult::NotReady;
        }
        if (g_getMute() == mute) {
            return CallResult::AlreadySo;
        }
        g_mute(mute);
        return CallResult::Changed;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return CallResult::Faulted;
    }
}

}  // namespace

void Start(bool muteAtStart) {
    if (g_started) {
        return;
    }
    g_started = true;
    const HMODULE exe = GetModuleHandleW(nullptr);
    g_isInitialized = reinterpret_cast<IsInitializedFn>(reinterpret_cast<void*>(GetProcAddress(exe, kIsInitialized)));
    g_mute = reinterpret_cast<MuteBackgroundMusicFn>(reinterpret_cast<void*>(GetProcAddress(exe, kMuteBackgroundMusic)));
    g_getMute =
        reinterpret_cast<GetBackgroundMusicMuteFn>(reinterpret_cast<void*>(GetProcAddress(exe, kGetBackgroundMusicMute)));
    if (!g_isInitialized || !g_mute || !g_getMute) {
        InterlockedExchange(&g_availability, static_cast<LONG>(Availability::Unavailable));
        Log("game music: this version of the game does not offer the sound engine's music switch, so the mod cannot "
            "turn the game's music off; use the music switch in the game's own sound settings");
        return;
    }
    InterlockedExchange(&g_wanted, muteAtStart ? 1 : -1);
    Log("game music: the game's sound engine offers its music switch; %s",
        muteAtStart ? "the game's music is turned off once the sound engine runs ([panel] mute_game_music=1)"
                    : "the game's music is left as it is ([panel] mute_game_music=0)");
}

void Tick() {
    if (!g_started) {
        return;
    }
    const Availability availability = static_cast<Availability>(g_availability);
    if (availability == Availability::Unavailable || availability == Availability::Failed) {
        return;
    }
    ++g_ticks;
    const LONG wanted = g_wanted;
    // About once a second, or at once after the button changed what is wanted.
    if (g_ticks % 4 != 0 && wanted == g_lastApplied) {
        return;
    }
    if (wanted < 0) {
        return;
    }
    const CallResult result = ApplyGuarded(wanted == 1);
    switch (result) {
        case CallResult::NotReady:
            if (!g_waitLogged) {
                g_waitLogged = true;
                Log("game music: waiting for the game's sound engine to start");
            }
            return;
        case CallResult::Faulted:
            InterlockedExchange(&g_availability, static_cast<LONG>(Availability::Failed));
            Log("game music: the call into the game's sound engine failed; the game music button is off for this "
                "session and the game carries on");
            return;
        case CallResult::AlreadySo:
        case CallResult::Changed:
            break;
    }
    InterlockedExchange(&g_availability, static_cast<LONG>(Availability::Ready));
    if (result == CallResult::Changed) {
        if (wanted == g_lastApplied) {
            // The mod had set this already: something changed it back.
            ++g_reapplied;
            if (g_reapplied <= 5 || g_reapplied % 50 == 0) {
                Log("game music: the game turned its music %s again; the mod set it %s again (%u time(s) so far)",
                    wanted == 1 ? "on" : "off", wanted == 1 ? "off" : "on", g_reapplied);
            }
        } else {
            Log("game music: %s", wanted == 1 ? "turned off" : "turned on again");
        }
    } else if (wanted != g_lastApplied) {
        Log("game music: already %s", wanted == 1 ? "off" : "on");
    }
    g_lastApplied = wanted;
}

void Request(bool mute) {
    InterlockedExchange(&g_wanted, mute ? 1 : 0);
}

State GetState() {
    State state;
    state.availability = static_cast<Availability>(g_availability);
    state.muted = g_wanted == 1;
    return state;
}

}  // namespace game_music

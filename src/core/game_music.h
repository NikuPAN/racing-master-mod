#pragma once

// Turning the game's own music off and on (0.3.2, the owner's request: "a one-click mute in-game
// music button", and "once the plugin loaded, mute in-game music, default yes").
//
// How: the game plays its sound through Audiokinetic Wwise, and its exe exports Wwise's own
// AK::SoundEngine::MuteBackgroundMusic(bool), with IsInitialized() and GetBackgroundMusicMute()
// (checked with dumpbin on the 2026-11-01 exe). The mod looks them up by name in the exe's export
// table and calls them from the worker thread: no game memory is written, no game file is changed
// (the game's own switch lives in Saved\SaveGames\race_global.sav in the game folder, which the mod
// never writes), and nothing outlives the session. Whether this silences the game's music only, and
// all of it, depends on how the game's sound project marks its music; only an in-game test shows
// that (docs/agent-notes/game-music.md). If a game update renames the functions, the button says
// so and the setting does nothing.
//
// Threads: GameMusicRequest and GameMusicGetState are cheap atomics for the frame thread; every
// Wwise call and every log line happens on the worker (GameMusicStart, GameMusicTick).

namespace game_music {

enum class Availability {
    Looking,       // not started yet, or the game's sound engine has not started yet
    Ready,         // the button works
    Unavailable,   // this game version does not export the three functions
    Failed,        // a call faulted; off for the session (logged)
};

struct State {
    Availability availability;
    bool muted;    // what the mod is keeping (true = the game's music is off)
};

// Worker, once, with mode=on only: looks the functions up and records the start choice.
void Start(bool muteAtStart);

// Worker, every 250 ms. About once a second: once the sound engine runs, applies the wanted state
// and applies it again when it was changed by someone else (logged; the owner's decision).
void Tick();

// Frame thread (the panel's button): this session only; the start setting is separate.
void Request(bool mute);

State GetState();

}  // namespace game_music

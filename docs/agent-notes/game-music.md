---
name: game-music
description: How the mod turns the game's own music off and on (0.3.2) through the Wwise functions the game exe exports, why this way and not another, and what only the game can confirm
type: decision
---

The owner asked (2026-11-15) for a one-click button that mutes the game's own music, and a
setting that mutes it whenever the mod loads, on by default.

**How:** the game plays its sound through Audiokinetic Wwise, and `g112-Win64-Shipping.exe`
exports Wwise's own functions by name. `src/core/game_music.cpp` looks up three of them with
`GetProcAddress(GetModuleHandleW(nullptr), ...)`:
`?IsInitialized@SoundEngine@AK@@YA_NXZ`, `?MuteBackgroundMusic@SoundEngine@AK@@YAX_N@Z` and
`?GetBackgroundMusicMute@SoundEngine@AK@@YA_NXZ` (checked with dumpbin on the 2026-11-01 exe).
The worker waits for `IsInitialized`, applies what is wanted, and about once a second applies it
again if something changed it back (logged; the first five times, then every 50th). Calls run
under a fault guard: a fault switches the feature off for the session with one log line.

- `[panel] mute_game_music` (default 1): turn the game's music off at start. Written by the panel
  writer like the other `[panel]` keys. Only with `mode=on`; display mode calls nothing in the game.
- The bar's **Game music: on/off** button changes it for this session only (the owner's choice);
  the start setting is separate, in the settings row.
- If the exports are missing (a game update), the button and the setting are greyed with a plain
  reason pointing at the game's own music switch, and the log says so once.

**Why this way:** the game's own switch is `MusicOpen` in `Saved\SaveGames\race_global.sav`, in the
game folder (found by a file-time experiment with the owner on 2026-11-15: the game wrote that file
and a scrambled copy, `Saved\LocalData\e1618aae22372e345abd519484b8c0d1.dat`, the moment the
setting changed). The mod never writes the game folder, the scrambled copy looks like a tamper
check, and an edit would only apply at the next start. Muting the game's Windows audio session would
silence engine sounds, effects and voices too (Wwise uses one session). Unreal's own SoundMix is
not available: the game starts with `-nosound` and leaves sound to Wwise. Research notes:
workflow `wf_3235d82a-b11`, scripts in `%TEMP%\rmm-mute-research\`.

**Verified:** in the test host, which exports stand-ins under the same names (run 40b: the wait,
the mute at start, the button both ways, the re-apply after "the game" turned it back on). In the
game on 2026-11-15 18:24 the mod found the three functions and logged "game music: turned off"
0.3 s after the panel started.

**Not verified:** that `MuteBackgroundMusic` silences this game's music, and only its music. That
depends on how the game's sound project marks its music busses, which cannot be read from disk;
only the owner's ears can tell. If it has no effect, the owner's decision is to drop the feature
and keep using the game's own switch (no unpacking of game data).

**How to apply:** never write `race_global.sav` or anything under the game folder; keep every
Wwise call on the worker; if the owner reports no effect, remove the button and the setting rather
than looking for another way in.

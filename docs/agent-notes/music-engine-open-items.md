---
name: music-engine-open-items
description: What the music engine still needs at integration time and what is unverified until it runs in the game; a checklist for whoever wires the panel to it
type: project
---

Status 2026-11-14: the engine is built, linked into `version.dll` (kept there by
`src/music/link_anchor.cpp`, never called) and tested silently (`music-tests.md`). Nothing calls
it from the loader yet.

**Integration (with the panel):**
1. `music::Player::Get().Start()` from `ModThread` after the Present hook is installed, gated by
   a stage the owner agrees on (the brief proposes stage 3 = engine and drop-in folder, stage 4 =
   file dialog; `dllmain.cpp` clamps `stage` to 2 today, and the other briefs must agree on the
   numbers). Then delete `link_anchor.cpp`, its line in `build.ps1` and `/INCLUDE:MusicEngineLinkAnchor`.
2. Do not call `Shutdown()` from `DllMain`. The game can end at any moment; saves are already
   crash-safe and frequent.
3. Game exit: the brief asks the player thread to watch `IsWindow(gameWindow)` and close the gate
   when the window is gone (no cut mid-waveform). Not built: the engine does not know the window
   yet. Add it with the window handle at integration.
4. Focus: the panel calls `OnGameFocusChanged(bool)`; `SetPauseWhenGameUnfocused` is the owner's
   setting (design decision 11).
5. Dialog owner: `OpenAddFilesDialog(GetAncestor(gameWindow, GA_ROOT), ...)`; the panel must say
   the game takes no input while it is open and must not offer it in exclusive fullscreen.
6. Drop-in folder watching (`FindFirstChangeNotificationW`) is not built; the folder is scanned at
   `Start` and on `RescanMusicFolder()`. The panel can offer a "rescan" button.
7. `Player` has no "move track" command (the contract has none); the logic supports it.

**Unverified until an in-game run (throwaway account, stage by stage):** that the game tolerates
the extra threads, a WASAPI stream, Media Foundation and COM in its process; that the game's audio
is in shared mode and in the default session (one mixer entry for game and music); real device
re-routing (headset unplugged) and the length of its gap; the file dialog's z-order, focus and
blocked input; Windows "N" editions without Media Foundation (M4A then reports a clear message;
the rest must keep working); cloud placeholder files; old Big5/GBK ID3 tags.

**Why:** these are the parts a silent test on this PC cannot reach, and the integration steps the
engine module was told not to take.

**How to apply:** tick items off here (or move them into `CLAUDE.md`) as the panel work lands.

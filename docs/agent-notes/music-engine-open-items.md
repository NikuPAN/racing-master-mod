---
name: music-engine-open-items
description: What the music engine still needs after its integration with the panel, and what is unverified until it runs in the game
type: project
---

Status 2026-11-15: the engine is wired into `version.dll` (branch `integrate`, version 0.3.0)
and driven by the panel in the hidden test host (runs 25-38 and the end-to-end run 40). How it is
wired: `integration.md`.

**Done at integration (were open items 1-5):**
1. Started from `ModThread` (before the hook, after `PanelStart`), gated by `[panel] mode`
   instead of new stage numbers (`mode=off` starts nothing). `link_anchor.cpp` and its
   `/INCLUDE` are gone.
2. `Shutdown()` is never called from `DllMain`.
3. Game exit: the **worker** (not the player thread) watches the panel's window every 250 ms and
   calls `Shutdown()` (fade, final save) once it is gone and no frame arrived for a second.
4. Focus: the panel calls `OnGameFocusChanged` on `WM_ACTIVATEAPP`; `SetPauseWhenGameUnfocused`
   on its first frame and from the settings row (tested end to end, run 40).
5. Dialog owner: `GetAncestor(gameWindow, GA_ROOT)`; the hints say the game takes no input while
   it is open; both buttons are greyed in exclusive full screen and while a dialog is open.

**Still open:**
6. Drop-in folder watching (`FindFirstChangeNotificationW`) is not built; the folder is scanned
   at `Start` and on `RescanMusicFolder()`. The panel has no "rescan" button yet.
7. `Player` has no "move track" command (the contract has none); the logic supports it.

**Unverified until an in-game run (throwaway account):** that the game
tolerates the extra threads, a WASAPI stream, Media Foundation and COM in its process; that the
game's audio is in shared mode and in the default session (one mixer entry for game and music);
real device re-routing (headset unplugged) and the length of its gap; the file dialog's z-order,
focus and blocked input; Windows "N" editions without Media Foundation (M4A then reports a clear
message; the rest must keep working); cloud placeholder files; old Big5/GBK ID3 tags.

**Why:** these are the parts a silent test on this PC cannot reach.

**How to apply:** tick items off here (or move them into `CLAUDE.md`) as they land.

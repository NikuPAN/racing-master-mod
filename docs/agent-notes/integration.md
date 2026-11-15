---
name: integration
description: How 0.3.0 wires the music panel, the music engine and the media remote into version.dll (and how 0.3.1 sees the game close), the choices made while wiring and why, and what only the game can confirm
type: decision
---

Built on branch `integrate` (2026-11-15): a `--no-ff` merge of `music/panel` onto main's music
engine, then one commit per wiring step. Tested in the hidden test host only.

**Start-up order (`ModThread`, worker thread):** identity line, `ReadSettings`, stage checks, wait
for the game window, `PanelStart`, `StartMusicParts`, Present hook, `WatchFrames`.

- The music player starts **before the hook**, so the panel's first frame and the first hotkey
  find the restored list. `Start()` only reads two small files in `%LOCALAPPDATA%` and starts two
  threads; no sound device and no Media Foundation until the first Play. The engine's own open
  items had proposed "after the hook"; commands posted before `Start` are kept and applied after
  it, so either order works, and before is the one where the panel never shows an empty list.
- `[panel] mode` replaces the engine brief's stages 3 and 4: `off` starts nothing of the music
  (the loader alone, as in 0.2.0), `display` starts the music player too (the panel shows the real
  list, but nothing can press play), `on` is everything.
- The media remote starts only when "Other app" was the last choice (worker) or when the player
  picks it (panel). Starting it always would put WinRT traffic into every game session for no
  use; whether it works from the elevated game is still open (`media-remote-elevated-game.md`).
- One plain log line per part: the music player with the music folder's full path, the
  other-app control (or, in the test host, that "Other app" is the example), or the mode=off line.

**While running:** the marker and then the panel draw inside `GuardedOnFrame`'s fault guard;
focus reaches `Player::OnGameFocusChanged` through the panel's `WM_ACTIVATEAPP` rule (none in
display mode); Add files / Add folder use the game's top-level window as owner and are greyed in
exclusive full screen and while a file window is open; the panel copies the song list only when
it changed (`CurrentSnapshot`).

**Window log line (`WindowStateLog` in `worker.cpp`):** "game window: 2558x1360, windowed, in
front" (or borderless full screen / exclusive full screen, behind another window, minimised),
written when that changes and then holds for three more 250 ms samples, so the owner's test log
proves the resize, alt-tab and mode-switch steps happened and the mod carried on. The frame thread
only samples the swap chain's exclusive full-screen state and window about once a second
(`PresentHookGetWindowState`); size (client area), minimised, in front and borderless are read
from user32 on the worker. A state must hold for about 1 s to be logged (up to 2 s for a switch
to or from exclusive full screen). Run 40 checks the hidden host's line; minimised, in front and
the full-screen kinds can only be seen in the game.

**Shutdown (0.3.1):** 0.3.0 only had the window watch below, and in the owner's game runs of
2026-11-15 the process ended before it fired: twice `mod.log` simply stopped after a close with the
window's X, with no final save. Now, in `mode=on`, the window procedure subclass passes the close
to the worker the moment it happens (`src/core/game_close.h`), and the game gets every message
unchanged:

- **Close request** (`WM_CLOSE`, `WM_SYSCOMMAND` `SC_CLOSE`, `WM_QUERYENDSESSION`): the worker,
  woken through an auto-reset event it now waits on instead of `Sleep(250)`, calls
  `Player::SaveNow()` (list, song and place at once, crash-safe, under `SettingsFileLock`) and logs
  "music: the game was asked to close ...". **The music plays on** (decision made here, see
  below). If the window is still there 3 s later the worker logs "music: the game is still open 3 s
  after it was asked to close ...; the music carries on". Unreal 4.26 handles `SC_CLOSE` itself
  and turns it into a deferred close of its own, so for the X button the game window sees
  `WM_SYSCOMMAND` and probably never a `WM_CLOSE` (from the engine source as remembered, not
  checked against this game: that is why both are watched).
- **Window going** (`WM_DESTROY` of the window the panel draws into, `WM_ENDSESSION` with TRUE):
  the window procedure wakes the worker, calls the game's procedure, then waits at most 500 ms
  (`kGameCloseWaitMs`) on a manual-reset event the worker sets right after `Player::Shutdown()`
  (fade, final save) and the "music: stopped and saved" line, so the save is on disk before the
  game can end its process. Only while the worker is watching; never on `WM_CLOSE`.
- **Backstop:** the worker still checks the window every 250 ms. Gone, and no frame for a second
  (or gone at all after a close request): the same stop, logged as "the game window has closed".
  This is the only path in `mode=display`, which has no window procedure. There, a gone window
  while frames still arrive is logged once and ignored. In `mode=on` it is not: `WM_DESTROY` of the
  panel's window stops the music whatever happens next, so a game that destroyed its window and
  carried on in a new one would lose its music for the session (Unreal does not normally do this;
  not tested). The review's other note on this path, that a close request stays "requested" for the
  whole session and skips the one-second wait, changes nothing observable for that reason: in
  `mode=on` the `WM_DESTROY` stop comes first, and `mode=display` sees no requests at all. Left as is.
  `WM_CLOSE`/`SC_CLOSE` count from any subclassed window, but only windows the panel has drawn into
  are subclassed; a close of an older one would cost one extra save and a misleading "asked to
  close" line, nothing else. Also left as is.
- **A request and the window going at once:** if the game destroys its window inside the close,
  both can reach the worker before it wakes, and then only "is closing" is logged, not "asked to
  close" (review finding; in the test host the worker woke in between in every run, also before
  any change, so it could not be shown and was not changed). The owner's steps treat "is closing"
  and "stopped and saved" as the lines that matter.
- **Periodic save:** in every mode, while a song plays its place is saved every 10 s when it
  moved (`music-engine.md`), so a forced exit (the game ending from its own menu, a crash) loses
  at most 10 s.
- `other_app::Shutdown()` runs after the event is set (in the game the media thread only lets go
  and sleeps).

Why the music plays on after a request (instead of pausing, or stopping at once): a close can be
cancelled (a question first, or Unreal's own handling), and a stopped engine would then need a
restart; the request already saved the place, and the fade comes when the window really goes.
The owner may prefer a pause on request; that is a small change in `CloseWatch` (`worker.cpp`).

Test host: run 40 clicks the X (`SC_CLOSE`) while "the game" refuses to close (C1-C3: saved at
once, music plays on, the still-open line, the panel still works), then `main()` closes with
`WM_CLOSE` and ends the process with `TerminateProcess` the moment `SendMessage` returns
(`RACING_MASTER_MOD_TEST_EXIT_AFTER_CLOSE=1`); the script checks the log and `settings.ini`. Every
`mode=on` panel run checks "stopped and saved before the window was gone"; the display-mode run
checks the backstop.
Whenever the panel is off for the session the worker pauses the music once, because nothing could
stop it any more: after a fault in the per-frame code (which also stops the marker), an ImGui
assertion, or a fault in the window procedure (which now also closes the panel, since Insert goes
to the game from then on). `PanelSwitchedOff()` in `overlay.h`; runs 36, 43 and 44. Nothing runs
from `DllMain`.

**settings.ini:** the panel's writer and the engine's I/O thread both rewrite it; both hold
`SettingsFileLock` (`src/core/settings_file_lock.h`). A `settings.ini` next to the DLL is
reported once and not read.

**Merge decisions (in the merge commit):** the panel and Dear ImGui now use the core's compiler
flags (`/EHs-c- /D_HAS_EXCEPTIONS=0`), so standard library types shared with the engine are
compiled one way; gate 3 also covers the panel's own objects (it caught a global `ImGuiStyle` in
`scale.cpp`); the delay-load list is the union (USER32, ole32, SHELL32, MFPlat, MFReadWrite,
d3dcompiler_47). The engine had to start in the merge commit itself: without the stand-in the
panel's tests need a running engine. The stand-in (`src/ui/standalone`) was removed right after;
no test needed it.

**Test-only hooks added:** `RACING_MASTER_MOD_TEST_PICKER_FILES` (a suppressed dialog reports
those paths), `RACING_MASTER_MOD_TEST_FAKE_PLAYER=1` (panel mode publishes the fake player, under
the id in `RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID`, new for every run), and
with `RACING_MASTER_MOD_TEST_REMOTE=real` the remote follows only `RACING_MASTER_MOD_TEST_MEDIA_APP`.
`musictest.exe --fixtures <folder>` writes the end-to-end songs. All need the test-host flag.

**Only the game can confirm:** that NetEase's protection tolerates ImGui drawing, the window
subclass, the extra threads, WASAPI, Media Foundation and COM (try `mode=display` first); that
Insert and F5-F9 are free in the game's UI; the music's level against the game's; the file
window over the game; which close messages the game window really gets (`SC_CLOSE` for the X,
then `WM_DESTROY`?) and whether the process lives the 0.5 s the final save needs (the log answers
it: "asked to close", "is closing", "stopped and saved"); a game that ends without any of those
messages loses at most 10 s of the place in the song (saves are crash-safe); "Other app" from the
elevated game.

**Why:** the owner tests 0.3.0 in the game next; each choice above trades a little start-up
work for a panel that never shows half a state, and every guard says what it did in the log.

**How to apply:** a new part that starts with the mod goes into `StartMusicParts` with one log
line and respects `mode=off`; a new writer of `settings.ini` takes `SettingsFileLock`; a new
end-to-end behaviour gets a step in `ScriptIntegrated` or `ScriptOtherReal` in `testhost.cpp`.

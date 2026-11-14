---
name: integration
description: How 0.3.0 wires the music panel, the music engine and the media remote into version.dll, the choices made while wiring and why, and what only the game can confirm
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

**Shutdown:** the worker watches the panel's window every 250 ms. Gone, and no frame for a second:
`Player::Shutdown()` (fade, final save) and `other_app::Shutdown()` (in the game the media thread
only lets go and sleeps). A gone window while frames still arrive is logged once and ignored.
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
window over the game; whether the 250 ms window watch sees the game close (Unreal destroys its
window before exit; a game killed outright gets no final save, which is safe because saves are
crash-safe); "Other app" from the elevated game.

**Why:** the owner tests 0.3.0 in the game next; each choice above trades a little start-up
work for a panel that never shows half a state, and every guard says what it did in the log.

**How to apply:** a new part that starts with the mod goes into `StartMusicParts` with one log
line and respects `mode=off`; a new writer of `settings.ini` takes `SettingsFileLock`; a new
end-to-end behaviour gets a step in `ScriptIntegrated` or `ScriptOtherReal` in `testhost.cpp`.

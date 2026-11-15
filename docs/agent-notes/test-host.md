---
name: test-host
description: How tools/run-host-test.ps1 and testhost.exe check the loader and the music panel without the game, the conditions that make a flip-model frame readable, and why every media run has its own fake player id
type: reference
---

`tools\run-host-test.ps1` runs 46 hidden programs (45-46 since 0.3.1: two threads presenting at
once, the "game" one with its pipeline state bound and checked around every present while the
mod must draw only on it, and the present probe; runs 1-14 the loader, runs 15-22 the media
remote (see `media-remote-tests.md`), run 23 the music engine's `musictest.exe` with one check
per line it prints (see `music-tests.md`), runs 25-38 the music panel driving the real engine,
39-44 the integration: `[panel] mode=off`, the two end-to-end runs, a misplaced settings.ini, and
the panel switching itself off after a window-procedure fault or an ImGui assertion)
with `LOCALAPPDATA`
pointed at its own temp folder, one folder per run, so the real `mod.log` from a game run is
never archived or overwritten. Runs 7-9, 12-14, 24, 25 and 26 are settings-file cases (UTF-8 BOM,
UTF-16 in both byte orders, bad values, override file, no `[mod]` heading with CR-only line
endings, a heading other than `[mod]`, only a `[music]` section, `[panel]` and `[keys]` values). `testhost.exe` renders on a worker thread to a window that is
never shown; `bystander.exe` is a kernel32-only console program that stands in for the crash
reporter and other programs that import `version.dll` without being the game.

Conditions found by the review's checker, each of which otherwise produces a misleading FAIL:

1. **The host must be named `testhost.exe`** and run with `RACING_MASTER_MOD_TEST_HOST=1`. Under
   another name the mod stays dormant by design and every marker check prints "not found".
2. **Reading a frame back on a flip-model chain** (the game's `FLIP_DISCARD`, 2 buffers): the
   hook draws into buffer 0 before the real `Present` runs, and a normal `Present` then rotates
   the buffers. So: at least one ordinary `Present(0, 0)` first (as the very first present,
   `DXGI_PRESENT_DO_NOT_SEQUENCE` returns `DXGI_ERROR_INVALID_CALL`), then clear buffer 0, then
   `Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE)`: the hook draws, DXGI does not rotate, buffer 0 is
   copied to a staging texture. Without the clear, a marker from two frames earlier passes for
   the current one.
3. **The fault run** needs `RACING_MASTER_MOD_TEST_FAULT=<frame>` as well as the host flag. The
   mod then writes through a null pointer at that frame while it holds its references; the host
   checks that the marker is gone, that `ResizeBuffers` still returns `S_OK`, and the script
   checks the one-line fault report in the log.

Pixels are decoded per format: `R10G10B10A2_UNORM` is 10 bits per channel in one 32-bit word
(red in the low bits), `B8G8R8A8_UNORM` is bytes in B, G, R order.

**Why:** the first version of the host used a layout the game does not use and passed on exit
code 1 for both "no marker" and "forwarding failed"; the review showed what it did not prove.

## Music panel runs (25-38)

`testhost.exe panel <folder> <seconds> <script> [name]`, scripts `shots`, `shot1`, `resize`,
`fault`, `display`, `input`. What they rely on:

4. **The host binds 14 pieces of "game" pipeline state before every Present** (render target on
   an offscreen texture, rasteriser, viewport, scissor, blend with factor and mask, depth-stencil
   with reference, topology, a compute shader, samplers in slots 0 and 2, constant buffers) and
   checks all of them afterwards. This is the fingerprint the panel needs, since the stock DX11
   backend clears the compute shader and never restores it.
5. **The mod reports to the host through `test-layout.txt`** (test host only): panel state, the
   ImGui windows in back-buffer pixels (for "nothing drawn outside the panel"), the hit-test
   rectangles and widget centres in client coordinates (for the posted clicks). The hidden window's
   client area (2542x1321) is smaller than the back buffer (2558x1360), so the client-to-buffer
   scaling path is exercised in every run.
6. **Every message is posted to the host's own window** and reaches the mod's subclass on the
   host's main thread; the host's class procedure counts what gets past it ("the game"). A
   `WM_APP+7` marker message tells the render thread that everything posted before it was handled.
7. **The screenshot is the flip-model read-back above**: an ordinary Present, then a cleared
   buffer 0 presented with `DO_NOT_SEQUENCE`. The panel draws on that present too (by design).
   Pixel checks: over 90 % of the panel's pixels drawn, text pixels present, **zero** pixels
   changed outside the panel windows (2 px tolerance for antialiased edges). The clear colour is
   read from the bottom-left corner, which the panel never covers in the screenshot runs (bottom
   centre by default; the corner is chosen only in the input run, which takes no screenshot).
   The layout file also gives `display W H`, the panel's own window as `bar x y w h` (also counted
   as a window) and `corner`; B1-B3 (shots), R3 (resize) and I23 (input) check the place.
8. **The list scrolls under the wheel test**: the input script scrolls it back before using
   row and remove-button positions (only the visible rows have widget centres).
   **At the bottom of the screen (the default since 0.3.1) the panel grows upward**, and ImGui
   sizes an auto-sized window one frame late, so for one frame after the list or the settings
   row changes size every widget sits lower than it will be drawn from then on. `Item()` therefore
   presents frames until a widget's centre is the same on two frames in a row (at most 8). Without
   it run 40's E7 missed "Settings" in about half the runs (2026-11-15): Add files had just made the
   list one row longer, and the button was read from that one frame.
9. **The panel drives the real music engine.** The script generates five quiet 20 s tone WAVs
   (neutral names in Chinese, Japanese, Korean, a character outside the basic plane, one long
   title) and copies them into each panel run's `RacingMasterMod\music` folder, which the engine
   scans at start on its own thread. Scripts that need the list wait for `tracks >= 4` and
   `scanning 0` in the layout file. The screenshot and resize runs also get
   `06 Cannot be played.wav` (64 bytes that are not audio): the screenshot script clicks it, the
   engine skips it with its reason and marks it (`unplayable 1` in the layout file), the list shot
   draws it greyed, and a second click must play nothing. The engine plays through miniaudio's null device (it refuses
   WASAPI whenever the test flag is set), so nothing is heard; the picker prepares the dialog and
   reports a cancel without showing it.

10. **Every panel run ends with the window closed and the music shut down.** After the render
    thread finishes, `main` closes the window with `SendMessage(WM_CLOSE)` (the host's procedure
    destroys it) and prints whether "music: stopped and saved" was already in the log when that
    returned: with the mod's window procedure (`mode=on`) it must be ("before the window was gone:
    yes"; the procedure waits up to 0.5 s after `WM_DESTROY`). Then it waits up to 10 s, pumping
    messages, for the line (display mode: the worker's window watch). With
    `RACING_MASTER_MOD_TEST_EXIT_AFTER_CLOSE=1` (run 40) it ends the process with
    `TerminateProcess` straight after the close instead, as the game did on 2026-11-15. While
    `g_refuseClose` is set the host's procedure swallows `WM_CLOSE` and `SC_CLOSE`, standing in for
    a game that asks first (run 40, C1-C3).
11. **End to end (runs 40-41).** `integrated` uses the songs from `musictest.exe --fixtures`, with
    `marker_seconds=600` so the screenshot also checks the loader's marker (`Shoot` with
    `kMarkerArea` leaves the marker out of the stray count and checks its white frame and dark
    centre). `other-real` needs `RACING_MASTER_MOD_TEST_FAKE_PLAYER=1` (the host publishes the fake
    player on its main thread, after `PrepareProcess`, before any window),
    `RACING_MASTER_MOD_TEST_REMOTE=real` and `RACING_MASTER_MOD_TEST_MEDIA_APP`. Their screenshots
    are copied to `out\preview\integrated`. The layout file carries `position` in whole seconds
    (written about once a second while a song plays) and, in "Other app", `other_session`,
    `other_playing`, `other_repeat`.

**How to apply:** a new panel feature gets a step in `ScriptInput` (behaviour) or a shot (look);
keep every message in-process and every window hidden. Nothing here may use `SendInput`.

## Media runs (15-22, 41): one fake player id per run

**Cause of the intermittent media failures (found 2026-11-15).** Until then every run published
its fake player as `RacingMasterMod.TestHost.FakePlayer`. Windows tells media sessions apart by
app id only, and the mod picks (and its test guard allows) the first listed session with the
wanted id. When another test run on this PC (another agent, another worktree) had its fake player
up at the same time, Windows listed two sessions with that id; the older one came first, so the
mod sent this run's commands to the other process. Reproduced on purpose: one extra fake player
with the fixed id kept alive during the full suite made 14 checks fail (runs 15-21 and 41) and
received every command the suite sent; a run with the same code alone passed. Not the cause:
leftovers after a run ends. Windows dropped a fake player's session within about 100 ms of its
process exiting, also when the process was killed. No module change was needed.

**The rule now:**

12. **Every media run gets its own id**: `RacingMasterMod.TestHost.FakePlayer.<12 hex>`, made by
    `New-FakeId` in the script and handed to `testhost.exe` as
    `RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID` and to the mod's guard as
    `RACING_MASTER_MOD_TEST_MEDIA_APP` (run 20 points the guard elsewhere and picks the run's id).
    The host prints `fake player: app id ...`; without the variable it appends its process id, so
    even a manual run never shares an id. Checks compare against the run's own id (`$run.Id`),
    never a fixed one. The panel's end-to-end check (O3, O4) builds its log text from
    `fake_player::AppIdUtf8()`.
13. **Before each media run the script waits for other runs' fake players to leave**
    (`Wait-NoOtherFakePlayers`): it reads Windows' session list from PowerShell (app ids only,
    nothing sent, no titles), waits up to 10 s while any id starting with the prefix is listed, and
    otherwise writes one plain NOTE (shown in the summary) and goes ahead. A player that outlasted
    the wait is not waited for again in the same suite. Sessions of other apps are ignored.
14. **The tests never follow Windows' current session.** The media script picks the run's id, and
    `other_app` pins the remote to it in run 41. A browser or player that is playing on the PC is
    listed in the log (`N apps found (ids)`) and never commanded; the guard refuses anything else.

Checked after the fix: the full suite with a fixed-id fake player and a player under an unrelated
id (standing in for a browser) alive throughout passed 343 of 343, and neither received anything.
Still true: only one suite should run on this PC at a time (both runs would make their own fake
player Windows' current session, and the media keys would point at whichever came last).

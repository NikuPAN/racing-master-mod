---
name: test-host
description: How tools/run-host-test.ps1 and testhost.exe check the loader and the music panel without the game, and the conditions that make a flip-model frame readable
type: reference
---

`tools\run-host-test.ps1` runs 42 hidden programs (runs 1-14 the loader, runs 15-22 the media
remote (see `media-remote-tests.md`), run 23 the music engine's `musictest.exe` with one check
per line it prints (see `music-tests.md`), runs 25-38 the music panel driving the real engine,
39-42 the integration: `[panel] mode=off`, the two end-to-end runs, a misplaced settings.ini)
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
   read from the bottom-left corner, which the top-right panel never covers.
8. **The list scrolls under the wheel test**: the input script scrolls it back before using
   row and remove-button positions (only the visible rows have widget centres).
9. **The panel drives the real music engine.** The script generates five quiet 20 s tone WAVs
   (neutral names in Chinese, Japanese, Korean, a character outside the basic plane, one long
   title) and copies them into each panel run's `RacingMasterMod\music` folder, which the engine
   scans at start on its own thread. Scripts that need the list wait for `tracks >= 4` and
   `scanning 0` in the layout file. The engine plays through miniaudio's null device (it refuses
   WASAPI whenever the test flag is set), so nothing is heard; the picker prepares the dialog and
   reports a cancel without showing it.

10. **Every panel run ends with the window closed and the music shut down.** After the render
    thread finishes, `main` destroys the window and (panel mode only) waits up to 10 s, pumping
    messages, for "music: stopped and saved" from the worker's window watch; the script checks it.
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

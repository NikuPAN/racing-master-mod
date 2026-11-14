---
name: test-host
description: How tools/run-host-test.ps1 and testhost.exe check the loader without the game, and the three conditions that make a flip-model frame readable
type: reference
---

`tools\run-host-test.ps1` runs 24 hidden programs (runs 15-22 are the media remote, see
`media-remote-tests.md`; run 23 is the music engine's `musictest.exe`, one check per line it
prints, see `music-tests.md`) with `LOCALAPPDATA`
pointed at its own temp folder, one folder per run, so the real `mod.log` from a game run is
never archived or overwritten. Runs 7-9, 12-14 and 24 are settings-file cases (UTF-8 BOM, UTF-16
in both byte orders, bad values, override file, no `[mod]` heading with CR-only line endings, a
heading other than `[mod]`, only a `[music]` section). `testhost.exe` renders on a worker thread to a window that is
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

**How to apply:** when the panel draws every frame, add a pipeline-state fingerprint (render
targets, viewport, blend, raster, depth state, shaders, input layout) taken before and after
`Present`, which must be identical. `ClearView` needs no such check; Dear ImGui does.

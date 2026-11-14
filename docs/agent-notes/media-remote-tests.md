---
name: media-remote-tests
description: How the media remote is tested without sending anything to a real player, and the side effect each test run has on the PC
type: reference
---

The rule: **no command may ever reach a real media session on the developer's PC.** Commands
are tested only against a silent fake player inside `testhost.exe`.

- `tools/testhost/fake_player.cpp` publishes a media session (app id
  `RacingMasterMod.TestHost.FakePlayer`) on a window that is never shown, plays nothing, records
  every button and loop request, and applies them on the host's main thread. Behaviours: apply
  everything, ignore pause, offer neither loop nor next, close its session on pause.
- The host sets its app id and calls `RoInitialize` (single-threaded) on its main thread before
  any window. Without that the player's factory call races the mod's apartment (brief's checker).
- **Hard guard**, in `Engine::Send` (the only function that sends): in the test host a command
  goes only to the app id in `RACING_MASTER_MOD_TEST_MEDIA_APP`. Run 20 proves it by pointing the
  guard at another id: both commands are refused and the fake player receives nothing.
- The mod runs the steps in `RACING_MASTER_MOD_TEST_MEDIA_SCRIPT` (test host only), picking
  `RACING_MASTER_MOD_TEST_MEDIA_PICK` or else the guard's id. Steps: `play pause toggle stop
  next previous repeat-off repeat-one repeat-all read quit`, plus test-only `sleep wake hang
  rebuild`. Each step waits until the previous one has settled. `quit` stops the media thread
  (the DLL exports nothing for it) and the script logs `music apps test: finished`, which the
  host waits for.
- `tools/run-host-test.ps1` runs 15-22: basic, ignore-pause, no-repeat, vanish, late, guard,
  lifecycle, and media mode with the flag unset. Every command line in the log must name the
  fake player, and no fake title may appear in the log.
- Run 41 (end to end) drives the same fake player from the panel: the host publishes it in panel
  mode with `RACING_MASTER_MOD_TEST_FAKE_PLAYER=1`, the worker starts the remote because
  `source=other_app`, and `other_app` pins the remote to `RACING_MASTER_MOD_TEST_MEDIA_APP` so a
  song playing elsewhere on the PC cannot take its place. It must receive exactly "play, next,
  loop one, pause"; the same safety checks apply.

**Side effect on the PC:** while a run lasts (1-12 s; run 41 for its whole length, a few seconds more) the fake player is Windows' current media
session, so the keyboard's media keys and the Windows media flyout point at it. Nothing is shown
or heard. Other sessions (a browser seen during development) are listed and read, never sent
anything.

**Why:** the owner forbade sending play, pause, stop, skip or repeat to any real session; the
guard makes that a property of the code, not of test discipline.

**How to apply:** a new command path gets a scenario here and goes through `Engine::Send`. Never
run `testhost.exe` with the test flag against the real `LOCALAPPDATA` (see `test-host.md`).

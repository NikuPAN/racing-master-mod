---
name: music-panel
description: How the in-game music panel (src/ui) was built on 2026-11-14, where it departs from the design brief and why, what is verified in the test host and what only the game can show
type: decision
---

Built on branch `music/panel`, on top of the media remote (`src/remote`), against a stand-in
music player while the engine was built in parallel. Since the merge on branch `integrate`
(2026-11-15, `integration.md`) "My files" is the real music engine and the stand-in is gone;
"Other app" uses the real remote through `other_app.cpp`, with an example app in the test host
(the real remote reads every media session on the PC) except in the end-to-end run 41. Design brief:
`local/design/imgui-overlay.md` (machine-local); its checker notes are all applied (error
callback, `CreateDeviceObjects` at start-up, `CompactCache` in the atlas valve, `MouseCursorScale`
last in `ApplyScale`, `static_assert` on `ImWchar`, `YuGothM.ttc`, `ClearInputMouse` after a gap,
one consumer for the input queue).

## Departures from the brief, and why

| Brief | Built | Why |
|---|---|---|
| Panel owns the whole mouse while open | Clicks outside the panel go to the game; hit test on the window thread against the panel's window rectangles from the last drawn frame | Owner's decision 10 (`design-decisions.md`). A drag stays with whoever got the button-down; a click that goes to the game closes an open drop-down |
| Rollout stages 3 and 4 via `stage=` | `[panel] mode=off|display|on` (default on), `stage` keeps 0/1/2 | Keeps the existing settings checks and log wording unchanged; `display` is the brief's stage 3, `on` its stage 4 |
| Hotkeys need the exact modifier state | Modifiers a hotkey names must be held; others are ignored | Shift is turbo and Ctrl/Alt are game controls: F7 must work while the player holds Shift |
| Focus from `WM_KILLFOCUS` / `WM_ACTIVATEAPP` | `WM_ACTIVATEAPP` only | The file picker is a window of the game process; choosing files must not count as "game not in focus" |
| Remove the queued `WM_CHAR` after every hotkey | Only for keys that make a character | F-keys and Insert queue none; an unconditional `PeekMessage(PM_REMOVE)` could take someone else's |
| Draw only on ordinary presents | Start-up waits for an ordinary present; drawing also happens on `DO_NOT_SEQUENCE` | The test host's flip-model read-back rule needs it; Unreal does not use the flag |
| `[hotkeys]` section | `[keys]` | Owner's wording in the task |
| Text fields out of scope | No text field in the panel; the keyboard path for one exists and is tested with a test-only field | The owner's rule names "the keys ImGui needs while a text field is focused" |

## Choices made here that the owner may want to change

- **Settings row behind a "Settings" button** at the end of row 1 (and "Hide" next to it), so the
  bar stays three rows. The owner listed a settings row without saying when it shows.
- **Place: bottom centre** by default since 0.3.1 (the owner's decision, 2026-11-15: top-right
  covered the game's diamond counter). The four corners stay selectable, in the settings row
  ("Position", added in 0.3.1, bottom centre listed first) and as `[panel] corner=`. At the bottom
  (centre or a bottom corner) the song list and the settings row open above the bar in mirror
  order, and so do the status lines that come and go (the engine's "Added ...", the other app's
  help and messages: between the list and the bar since 0.3.1's review, run 40 E6), so the bar stays
  put. One known flicker: ImGui sizes an auto-sized window from the previous frame's content, so on
  the one frame where the panel's height changes (list or settings row opened or closed, a status
  line appearing) the bar is drawn one row low and clipped. The fix would be two ImGui windows (the
  bar alone, anchored at the bottom, and the list and settings above it); not done, it is a layout
  change for the owner to agree. The list shows up to 8 rows and fewer when the whole panel would
  not fit on the screen (`ListRowsThatFit`, measured from the previous frame's content height);
  run 35 checks five sizes down to 1024x576 with large text, list and settings row open.
- **Add files / Add folder greyed** with the reason while a file window is already open and in
  exclusive full screen (the full-screen reason names the music folder). Added at integration.
- **Short notices** in the corner when the panel is closed: for 10 s after start "Music panel:
  press Insert to show it", after F9 what now repeats, and the reason when a key cannot act (no
  songs, the other app refuses). Nothing fails silently.
- **"Other app" mode**: no volume field and no list button (another app has neither here);
  controls the app does not allow are greyed with the reason; a timeline is shown as a progress
  line when the app reports one (the remote interface has no seek, so it cannot be dragged). The
  panel has no app picker yet, so `Remote::SelectApp` is unused and the remote follows Windows'
  current session.
- **Look** after the owner's preview review: opaque grey-blue fields, a 150-unit volume slider
  with the word in front, a divider between the two source buttons, row texts on the buttons'
  baseline, disabled controls at 40 % opacity.
- **Time row (0.3.2, the owner's decision 3):** both times get the width of the widest time the
  current item can show (`TimeWidth`: the shape of the length, h:mm:ss from an hour, measured with
  every digit, never less than "00:00"), so no time is cut off and the bar does not change size as
  the song plays. 0.3.0 reserved "00:00" and showed "1:1..." for a 1:12:30 video. The other app's
  timeline (`TimeLine`) takes a full button-row height and draws a thinner line centred in it, so
  it shares the times' centre line; 0.3.0 drew a half-height bar at the top of the row. The song
  list's titles also stop before lengths of an hour or more. Checks T1/T2 read `fit` and `at`
  lines from the layout file (every screenshot run; the example app plays 24:25 of 1:12:30).
- The chosen source is remembered in `settings.ini` (`source=`).
- The Traditional Chinese column of `src/ui/strings.h` is a **draft** for layout only.

## Changes to the previous builder's WIP (commit 718f9ff, never built)

- Window procedure rewritten for the owner's pass-through rule (moves over the panel kept, drags
  stay with their side, click-outside closes drop-downs, text-field keys), `windowsx.h` was
  missing, its own SEH guard added, publish order of the subclass slot fixed.
- Settings writer: the frame thread took an SRW lock; now one atomic word and an event.
- `external_interfaces.h` moved to `standalone/interfaces.h` behind `media_api.h`; once the real
  remote landed on main, its stand-in was replaced by `other_app.cpp`.
- Bug found by the input test and fixed: list rows used `ImGuiSelectableFlags_AllowOverlap`,
  which made them ignore clicks; the remove button sits beside the row, so the flag is not needed.
- Kept as written: hotkey parser, fonts (mapped chain, valve with `CompactCache`), scaling,
  `imconfig_mod.h`, the `[panel]`/`[keys]` reader.

## Verified (test host, 2026-11-14 on the panel branch; runs 25-38 since the merge)

Rendering in the game's swap-chain layout, English and draft Chinese, three text sizes, four
resolutions (PNGs in `out\screens`); 14 pieces of pipeline state unchanged after every present;
`ResizeBuffers` at four sizes while drawing; a fault between the two state swaps restores the
game's state, leaks nothing (`ResizeBuffers` S_OK) and gives every key and click back; display
mode takes nothing; clicks over the panel kept and outside forwarded; moves and wheel likewise;
Insert, F5-F9, auto-repeat, hotkeys with the panel closed; `ClearInputMouse` after a gap; the
drop-down closing on an outside click; text-field keys; `WM_ACTIVATEAPP` passed on; media keys
taken only for "My files"; the heartbeat; the settings writer keeping other lines; import gate
(KERNEL32 eager, USER32 and D3DCOMPILER_47 delay-loaded).

Since the merge (2026-11-15), on the real engine: every panel run lists generated songs from the
music folder; run 40 plays FLAC, MP3, Opus and OGG Vorbis through the panel (click, F7, F8, F9,
Add files through the picker, focus pausing) with the position advancing; run 41 drives the real
remote against the silent fake player; every panel run checks the music shutdown when its window
closes. After the integration review: a slider drag let go outside the game window (its release
went to another window) ends at the next mouse message, whose button flags show nothing held
(run 38, I19-I21; before the fix the volume followed the cursor and the next click was the panel's).
Two quick F9 presses step the repeat mode twice (I22): the panel remembers what it last asked for
(`Pending` in `panel.cpp`) until the engine shows it, at most 1.5 s. The same is done for the
repeat button and F7 with "Other app"; that path is not covered by a double-press test (the
example app answers at once, and run 41 sends single presses).

## Not verified (needs the real game, or not testable here)

- Anything in the game: whether NetEase's protection tolerates ImGui drawing and the window
  subclass (start with `mode=display`, then `on`, on the throwaway account), which thread
  presents, whether F5-F9 and Insert are free in the game's Lua UI, media keys reaching the window.
- The software cursor (`io.MouseDrawCursor` when the game hides the system cursor): the test host
  never draws it, and hiding the real cursor on this PC was not allowed.
- The atlas valve (`ClearFonts` + reload + `CompactCache` past 4096 px): needs thousands of
  never-seen glyphs; not exercised.
- The panel following a new Direct3D device (in `DrawFrame`), HDR (`R16G16B16A16_FLOAT` skip), `_SRGB` back
  buffers, the 120-failure back-off: written, not exercised.
- Exclusive full screen and a file window that stays open (both grey the Add buttons): the
  hidden host can reach neither.
- Runs 15-22 (media remote) failed twice on this PC while Windows listed a second session with
  the fake player's app id. Cause found and fixed on 2026-11-15: another test run's fake player
  under the same fixed id took the commands; every run now has its own id (`test-host.md`,
  items 12-14).

**Why:** a later agent needs to know which behaviours are the owner's decisions, which are this
build's suggestions, and which claims rest on the test host only.

**How to apply:** keep the owner's rules; ask before changing the suggestions above; treat every
"not verified" line as open until a game run or a test covers it.

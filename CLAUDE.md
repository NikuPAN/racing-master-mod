# Racing Master Mod

Unofficial mod for the PC version of Racing Master (巔峰極速, NetEase, internal name `g112`).
One DLL loaded into the game that will carry an in-game music panel and NVIDIA DLSS.
Free, MIT, README in English and Traditional Chinese. Repo is private until the owner makes it public.

@docs/agent-notes/README.md

## Current state (2026-11-15)

- **Version 0.3.0 integrated on branch `integrate` (not merged to main, not pushed), test host
  only.** The music panel (`src/ui`), the music engine (`src/music`) and the media remote
  (`src/remote`) are wired into one `version.dll`: 44 hidden runs, 335 checks in
  `tools\run-host-test.ps1`, including two end-to-end runs (40 "My files", 41 "Other app" against
  the silent fake player). How the parts are wired: "How the parts are wired" below and
  `docs/agent-notes/integration.md`. **Not in the real game yet**: the owner tries it on the
  throwaway account (steps were handed over with the integration); `mod.log` from that run is the
  next input.
- **Loader 0.2.0** (inside 0.3.0): forwards the 17 real `version.dll` functions, wakes up only
  inside `g112-Win64-Shipping.exe` (or `testhost.exe` with the test flag), hooks
  `IDXGISwapChain::Present`/`Present1`, binds to the game's one swap chain, draws a marker square
  for 20 s and writes `%LOCALAPPDATA%\RacingMasterMod\mod.log`.
- **0.2.0 has not been in the real game on its own.** It applies the loader review
  (`docs/agent-notes/loader-hardening.md`) and the follow-up review of that commit (settings
  reader fail-open cases, build gate, test script, log wording and order; same note). The review
  asked for one more in-person run on the throwaway account before the panel work starts,
  including drag-resize, window-mode switch, alt-tab and minimise/restore during the 20 s marker.
- **Loader 0.1.0 verified in the real game (2026-11-14, throwaway account):** the game loads the
  DLL, the launcher leaves the file in place, the marker shows on the login screen, about 60 fps.
  Swap chain: 2558x1360, DXGI format 24 (R10G10B10A2_UNORM), 2 buffers, swap effect 4
  (FLIP_DISCARD), windowed. The game's own log shows no reaction. This does not rule out a ban
  issued later.
- **Media remote built and verified in the test host only** (`src/remote`, branch
  `music/remote`, 2026-11-14): reads Windows media sessions and sends play, pause, stop,
  next, previous and loop to other apps, tested against a silent fake player (runs 15-22 of
  `tools\run-host-test.ps1`). Whether it works from the elevated game is open
  (`docs/agent-notes/media-remote-elevated-game.md`).
- **Music engine built and tested silently** (branch `music/engine`): `src/music`,
  MP3/FLAC/WAV/OGG/Opus/M4A, gapless, playlist and settings saved crash-safe, drop-in folder,
  file picker. `tools\run-host-test.ps1` run 23 (`out\musictest.exe`) checks it with no sound and
  no window.
- **Music panel built on branch `music/panel` (2026-11-14), merged with the engine on branch
  `integrate` (2026-11-15).** Dear ImGui 1.93.0 drawn from the Present hook, window-procedure
  subclass for clicks and hotkeys, the bar / list / settings row, English and a DRAFT Traditional
  Chinese table. Runs 25-38, PNGs in `out\screens`; end-to-end PNGs in `out\preview\integrated`.
  See `docs/agent-notes/music-panel.md` for what is verified and what is not.
- **In design:** DLSS (verified design briefs in `docs/agent-notes/local/design/` of the main
  repository).

## Order of work

1. Loader test in the real game (the owner, throwaway account). Read `mod.log` afterwards.
2. In-game music panel: Dear ImGui drawn from the Present hook; local files (MP3, FLAC, WAV,
   OGG) and remote control of other players through Windows media sessions; play, pause, stop,
   skip, loop one, loop all; English and Traditional Chinese.
3. DLSS 4.5 Super Resolution / DLAA through NVIDIA's official DLSS SDK on Direct3D 11,
   replacing the engine's TAA pass.
4. DLSS 5, only from NVIDIA's official release, only after step 3 works.

## Rules that must not be "fixed" away

- **Small, logical commits, one concern each.** Every commit builds with `build\build.ps1` and
  passes `tools\run-host-test.ps1`; tests land with the code they test. Feature work happens on
  a branch (worktrees under `../racing-master-mod-wt/`); work-in-progress checkpoints are
  replaced by logical commits before merging; pushed history is never rewritten. Subagents
  that commit are told this rule explicitly.

- **No anti-cheat bypass, ever.** No hiding the module, no unhooking the game's checks, no
  injector that opens the game process from outside. If the game blocks the DLL, stop and
  tell the owner. This was promised to the owner and is stated in the README.
- **No leaked or unofficial NVIDIA binaries.** Only the public DLSS SDK. The community
  "DLSS 5 one-click" tools use a leaked build; do not build on them.
- **Never test in the real game on the owner's main account**, and never copy the DLL into the game
  folder yourself. Hand the owner the exact steps instead.
- **Never touch the running game process** (no attaching, no memory reads). Analysis of the
  game is read-only and on disk.
- **The mod stays dormant outside the game.** `UniCrashReporter.exe` and other programs in the
  game folder also import `version.dll`; they must get plain forwarding and nothing else. That
  is why `version.dll` statically imports **KERNEL32 only**; everything else is delay-loaded.
  `build.ps1` fails if any other DLL appears in the import table, if the import dump did not
  list KERNEL32 at all (an empty dump must not pass), or if the delay-load list differs from
  `$delayLoads` in `build.ps1` (today USER32, ole32, SHELL32, MFPlat, MFReadWrite, d3dcompiler_47;
  each must be there, which also proves the music engine and the panel are still linked in). Every system DLL a later feature
  needs gets its own `/DELAYLOAD:` line and an entry in that list, or is loaded with
  `LoadLibraryW` from the worker thread. `tools\run-host-test.ps1` checks a bystander program.
  Media Foundation must stay delay-loaded: Windows "N" editions lack it, and a normal import
  would stop the game from starting there.
- **No start-up constructors in the mod's own code** (build gate 3: no `??__E` symbol from our
  objects in `version.map`). Anything like that runs inside every program that loads the DLL.
  Use function-local statics or constant initialisation; the music engine object is created on
  first use and never destroyed.
- **Third-party C is compiled with `/DNDEBUG`** (a live `assert()` shows a message box and ends
  the game) and never with opus's `ENABLE_HARDENING`/`ENABLE_ASSERTIONS` (they call `abort()`).
- **Nothing is held across frames** that would block the game's `ResizeBuffers` (no cached
  back buffer references). Everything the per-frame code acquires goes into `FrameRefs`
  (`src/core/frame.h`), never into a local below the guard.
- **The helper window stays an ordinary top-level window** (`dxgi_probe.cpp`). The review
  offered a message-only window (not enumerable); the owner has not chosen that, and the
  "no hiding" promise argues against it. The hook works identically either way.
- The README must keep the account-risk warning at the top, in both languages. Chinese is
  written by hand as Traditional Chinese for Taiwan/HK players, never machine-translated.

## Commands

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1          # out\version.dll, testhost.exe, bystander.exe, musictest.exe
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1  # 44 hidden runs, 335 checks, exit 0 = pass (about 2 min)
```

The test script copies the panel screenshots to `out\screens\*.png` (bar, list, settings row,
"Other app", each text size, English and draft Chinese, four resolutions, display mode, after a
fault). Look at them with the Read tool after any panel change; the owner reviews the Chinese there.
The two end-to-end screenshots (the engine's songs with the marker; "Other app" with the fake
player) go to `out\preview\integrated\`.

`build\fetch-deps.ps1` downloads the Windows SDK (Microsoft NuGet packages, SHA-256 pinned in
the script) into `.deps\`, the music libraries: miniaudio 0.11.26, stb_vorbis 1.24, libogg
1.3.6, opus 1.6.1, opusfile 0.12, and libvorbis 1.3.7 (test program only), each pinned to one
SHA-256, and Dear ImGui 1.93.0 (13 files from the tagged source tree, each SHA-256 pinned;
staged in `imgui-1.93.0.partial` and renamed only when every file matched). Licences and the
notices that must ship with the DLL: `THIRD_PARTY.md`. The Xiph libraries are built once into
`out\obj\xiph.lib` (and `vorbisenc.lib` for the tests); a stamp file rebuilds them when the
recipe changes. In a worktree `.deps` is a junction to the main repo's. With `-MsvcCrt` it also
downloads the Visual C++ runtime headers and libraries; `build.ps1` asks for that automatically
when Visual Studio has the compiler but no
headers. The build also writes `out\version.pdb` and `out\version.map` for turning a fault
offset from the log into a source line. **Never ship the PDB next to the DLL.**

Both scripts run from the repo root. To inspect the DLL's imports by hand use the MSVC linker as
dumpbin: `link.exe /DUMP /NOLOGO /IMPORTS out\version.dll` (the fetched SDK has no `dumpbin.exe`).

## Settings

Two files, both optional, same `[mod]` keys (`stage` 0/1/2, `marker_seconds` 0..600). The music
engine keeps its own `[music]` section in the first one (volume, loop, pause_when_game_unfocused,
current, position_ms) and rewrites only that section, in UTF-8, keeping the `[mod]` lines; the
loader skips `[music]` without logging "changed nothing".

1. `%LOCALAPPDATA%\RacingMasterMod\settings.ini` (no administrator rights, survives game updates)
2. `racing-master-mod.ini` next to `version.dll` (overrides 1 when present)

The reader (`src/core/settings.cpp`) accepts UTF-8 with or without a byte order mark and
UTF-16 with one in **either byte order** (all five encodings Windows Notepad offers), CR LF, LF
or CR-only line endings, `;`/`#` comments, digits-only numbers. Keys before the first `[heading]`
count as `[mod]`, so a file that is just `stage=0` works. A missing file means defaults. A file
that is present but unreadable, or a `stage` that is not 0, 1 or 2, switches the mod **off** with
a log line that quotes the value (fail-closed). A file whose lines were all ignored (only a
heading other than `[mod]`, no `key=value` lines) is applied as empty but gets a log line saying
so and naming `[mod]`; it does not switch the mod off. `GetPrivateProfile*` is not used: it
misses a section after a BOM and turns `-1` into 4294967295.

The music panel adds two sections (a bad value keeps the default and gets a log line quoting it;
it never switches the mod off):

```ini
[panel]
mode=on              ; off | display | on   (display: drawn and always open, cannot be clicked, no keys,
                     ;                       the game window is not touched - for the staged first game run)
language=auto        ; auto | en | zh-TW    (auto: Windows UI language zh-TW, zh-HK or zh-MO -> Chinese)
text_size=medium     ; small | medium | large  (never pixels)
pause_when_unfocused=0
source=my_files      ; my_files | other_app
corner=top-right     ; top-left | top-right | bottom-left | bottom-right
[keys]
panel=Insert         ; the owner's choices; [Ctrl+][Shift+][Alt+]Name, or none
stop=F5
previous=F6
play_pause=F7
next=F8
loop=F9
media_keys=1         ; the keyboard's media keys, only while "My files" is the source
```

The panel writes `language`, `text_size`, `pause_when_unfocused` and `source` back to the
per-user `settings.ini` when the player changes them (`src/ui/ui_settings.cpp`: own thread, every
other line kept, written to `settings.ini.new` and swapped in). It never writes the ini beside the DLL.
The engine's I/O thread rewrites `[music]` in the same file, so **both writers hold
`SettingsFileLock`** (`src/core/settings_file_lock.h`) from reading the file until the new copy is
in place, and log only after letting go. A new writer of `settings.ini` must take it too. Neither writer replaces a
`settings.ini` it could not read (held by another program, or larger than that writer reads: 16 KB
for the panel, 1 MB for the engine):
writing only its own section would drop the others, so it leaves the file as it is and logs why.

`settings.ini` (any letter case) **next to `version.dll` is not read**: the owner put one there
once and it was silently ignored. `ReadSettings` now logs one line naming it as ignored, with the
full paths of `racing-master-mod.ini` beside the DLL and the per-user `settings.ini` (run 42).

## Layout

- `src/loader/` — `version.dll` stand-in: `dllmain.cpp` (decide and start the worker, nothing
  else), `proxy.cpp` (table of the 17 real functions), assembly forwarding stubs, export list,
  version resource.
- `src/core/` — `log` (UTF-8 log, no flush, dated archives, `mod.<pid>.log` fallback),
  `settings`, `vtable_patch` (the only place that writes into a foreign function table),
  `dxgi_probe` (finds DXGI's swap chain table with a throwaway device), `present_hook` (hook,
  swap chain binding, fault guard, counters), `frame.h` (`FrameRefs`), `worker` (start-up
  order, watchdog, periodic status line).
- `src/remote/` — remote control of other music apps through Windows media sessions.
  `remote.h` is the panel's interface (plain types; `Snapshot` with a `Message` id for
  translation); `remote.cpp` is the only file that includes the WinRT ABI headers. See
  `docs/agent-notes/media-remote-design.md`.
- `src/music/` — the music engine (`docs/agent-notes/music-engine.md`): `player.h` is the
  public interface the panel codes against, `picker.h` the file dialogs, `player.cpp` the threads
  and playback, `audio_output` the ring/callback/device, `source*.cpp` one file per decoder
  family, `playlist_logic.h` the state machine, `playlist_store` the file formats, `testing.h`
  test-only hooks.
- `src/third_party/` — the one translation unit each for miniaudio and stb_vorbis.
- `src/ui/` — the music panel. `overlay` (start-up, per-frame sequence inside a swapped device
  context state, input feed, fault latch, test layout file), `panel` (the only file that calls
  ImGui widgets: bar, list, settings row, notices, what hotkeys do), `input_wndproc` + `input_queue.h`
  (window-procedure subclass, lock-free hand-off), `hotkeys`, `fonts` (mapped Windows fonts),
  `scale`, `strings.h` (every visible word; Chinese column is a DRAFT), `ui_settings` (writes
  `[panel]` back), `imgui_errors` (assert and error policy), `imconfig_mod.h`, `media_api.h` (the
  panel's one include for the music engine and the media remote), `other_app` (the panel's way to
  the media remote, with an example app in the test host). The stand-in music player the panel
  was first built against (`src/ui/standalone/`) was removed once the panel ran on the engine.
- `tools/testhost/` — `testhost.cpp`: Direct3D 11 program with a never-shown window that stands
  in for the game (game layout, worker render thread, checks T1-T8; `media` mode for the remote; `panel`
  mode with the scripts `shots`, `shot1`, `resize`, `fault`, `display`, `input`, `panel-off`, and the
  end-to-end `integrated` and `other-real`);
  `fake_player.cpp`: a silent media session the remote tests command; `bystander.cpp`: a
  kernel32-only console program that must see plain forwarding and nothing else;
  `musictest*.cpp`: the silent music engine tests (`docs/agent-notes/music-tests.md`).
- `build/` — build and dependency scripts. No CMake, no solution file.

## Threads and what each may do

| Thread | Runs | May do | Must not do |
|---|---|---|---|
| Loader (under the loader lock) | `DllMain` | read exe name and one environment variable, load the real `version.dll`, `CreateThread` | wait, log, read files |
| Worker (`ModThread`) | start-up, settings, `PanelStart`, starting the music parts, hook install, watchdog, periodic log, watching the game window (every 250 ms) and the final music shutdown | file I/O, `user32`, device creation, sleeping, `Player::Start`/`Shutdown` | touch the game's device context or swap chain |
| Game's present thread | `HookedPresent` -> `GuardedOnFrame` -> `PanelOnFrame` | Direct3D on the swap chain it was handed, ImGui, popping the input queue, calling the players (quick calls only), atomics, a one-off log line | file flushes, sleeping, periodic logging, waiting on a lock the worker holds during slow work |
| Game's window thread | `PanelWndProc` (subclass) | atomics, pushing the input queue, `PostCommand`, `Player::OnGameFocusChanged` (posts to the engine under its command lock, held for one `push_back`) | ImGui, Direct3D, files, other locks, per-message logging |
| Panel settings writer | `WriterThread` in `ui_settings.cpp` | read and rewrite `settings.ini` under `SettingsFileLock`, log after releasing it | anything else |
| Any game thread | forwarding stubs | jump | anything else |
| Media (`MediaThread`, created by `Remote::Start`) | every WinRT call, session reading, commands | COM in the multithreaded apartment, waits with deadlines, logging | leave the apartment in the game (`RoUninitialize` is test host only) |
| Windows' COM threads | media change notices | set an event and return | anything else |
| Frame / panel code | `GetSnapshot`, `Play`, `Pause` ... | copy the snapshot, queue a command | call WinRT, wait for the media thread |
| `rmm-player` (music, once started) | commands, decoding into the ring, the device, the snapshot | file reads of the music | tag reads, scans, saves |
| `rmm-io` (music) | folder scans, tags, crash-safe saves | file I/O | the ring or the device |
| miniaudio device thread | the audio callback | copy, gain, atomics | locks, allocation, system calls |

The frame thread may call every `music::Player` method: each only posts a command (Pause, Stop
and SetVolume also store one atomic), and `GetSnapshot` holds a lock only for a few assignments.
The panel reads the engine through `CurrentSnapshot()` in `panel.cpp` (one snapshot kept across
frames, refreshed with `GetSnapshotInto`, so the song list is copied only when it changed); never
call it again while still using a reference from an earlier call.

## How the parts are wired (0.3.0)

Details and the reasons: `docs/agent-notes/integration.md`.

- `ModThread`: identity line, settings, stage checks, wait for the game window, `PanelStart`
  (hotkeys, shader compiler, delay-load resolution, settings writer), then `StartMusicParts`:
  with `[panel] mode=off` nothing of the music starts (one log line says so); otherwise
  `music::Player::Get().Start()` (logs the music folder's full path) and, when `source=other_app`
  was the last choice, `other_app::Start()`. Then the Present hook, then `WatchFrames`.
- Per frame (`OnFrame`, inside `GuardedOnFrame`'s fault guard): the marker first, then
  `PanelOnFrame` (hotkeys every frame, ImGui only while something is shown).
- Focus: `WM_ACTIVATEAPP` in the panel's window subclass -> `Player::OnGameFocusChanged`; the
  engine pauses only when "pause when the game is not in focus" is ticked, and resumes on return.
  No subclass in `mode=display`, so no focus pausing there.
- Add files / Add folder: the picker gets `GetAncestor(window, GA_ROOT)`; both buttons are greyed
  with the reason while a file window is open and in exclusive full screen.
- Shutdown: `WatchFrames` checks every 250 ms whether the window the panel draws into is gone and
  no frame arrived for a second; then `Player::Shutdown()` (fade, final save) and
  `other_app::Shutdown()`. Nothing runs from `DllMain`. Whenever the panel is off for the session
  (a fault in the per-frame code, an ImGui assertion, or a fault in the window procedure, which
  also closes the panel) the worker pauses the music once (nothing could control it any more) and
  says why (`PanelSwitchedOff()`; runs 36, 43, 44).
- `src/music/link_anchor.cpp` is gone: the engine is really called now, and gate 1 still sees its
  DLLs.

## Gotchas

- The test host only activates the mod when `RACING_MASTER_MOD_TEST_HOST=1` is set **and** the
  executable is named `testhost.exe`. Under another name the mod stays dormant and every marker
  check reports "not found", which looks like a hook failure.
- `tools\run-host-test.ps1` points `LOCALAPPDATA` at a temp folder for every run, one folder per
  run. Never run `testhost.exe` with the test flag against the real `LOCALAPPDATA`: `LogOpen`
  archives the existing `mod.log`, which may be the evidence from a game run.
- Reading a flip-model frame back in a test: one ordinary `Present` first, then clear buffer 0 and
  `Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE)`, then copy buffer 0. See `docs/agent-notes/test-host.md`.
- `/W4 /WX` is on for the mod and the test programs: warnings fail the build.
- The mod is linked with `/MT` so players need no Visual C++ redistributable.
- `__try/__except` guards the per-frame code; keep C++ objects with destructors out of
  `GuardedOnFrame` (C2712). After a fault the mod releases `FrameRefs`, writes one log line
  (exception code, module, offset) and stays off until the game restarts. It does not retry.
- `PatchSlot` (`vtable_patch.cpp`) publishes the original pointer **before** switching the table
  entry. Every later hook goes through it; do not inline a table write elsewhere.
- Any delay-loaded function the frame thread or the window thread calls must be resolved on the
  worker first. `ModThread` warms up `GetWindowThreadProcessId`, `GetClientRect`,
  `IsWindowVisible`; `PanelStart` then resolves **all** of user32 and d3dcompiler_47 with
  `__HrLoadAllImportsForDll`, whose DLL-name match is **case-sensitive** (the names tried are
  `USER32.dll`/`user32.dll` and `d3dcompiler_47.dll`/`D3DCOMPILER_47.dll`; a miss is logged).
  `d3dcompiler_47.dll` is loaded by full path from System32 first, so a missing file is a log
  line, not a delay-load fault.

## Music panel (src/ui)

- **Owner's rules the code follows, do not "fix" them back to the design brief:** clicks outside
  the panel go to the game (the brief's "panel owns the mouse" was turned down); keyboard is only
  taken for hotkeys and while a panel text field has focus; Insert toggles; F5-F9 stop, previous,
  play/pause, next, repeat. Details and the other choices: `docs/agent-notes/music-panel.md`.
- **Click pass-through** = hit test on the window thread: the frame thread publishes the
  rectangles of the panel's clickable ImGui windows (client coordinates, popups included) after
  each frame; a mouse message inside one is queued and consumed, anything else is forwarded. A
  drag stays with the side that got its button-down. A button let go outside the game window
  (windowed mode) never sends its up here, so every mouse message's button flags are compared
  with who holds what, and a held button whose flag is clear is released there (run 38, I19-I21). A heartbeat (last drawn frame < 500 ms)
  hands the mouse back whenever the panel is not being drawn.
- **Drawing:** `SwapDeviceContextState(ours)`, stock DX11 backend, `ClearState()`,
  `SwapDeviceContextState(game's)`, then `refs.gameState` is released and **nulled**: the fault
  handler's `RestoreGameStateAfterFault` acts only while it is non-null (between the swaps). Never
  leave `gameState` set after the second swap, or a later fault would `ClearState` the game.
- The panel also draws on `DXGI_PRESENT_DO_NOT_SEQUENCE` presents (start-up waits for an ordinary
  one). The test host's read-back rule depends on it; Unreal does not use that flag.
- **ImGui error policy** (`imgui_errors.cpp`): `IM_ASSERT` logs once and switches the panel off;
  the `ErrorCallback` is mandatory with the three recovery outputs off (else ImGui asserts every
  frame, in the game only). The test host sets `ConfigErrorRecoveryEnableAssert` so mistakes fail
  tests; every panel run checks its log for `IMGUI ASSERT` and "reported a mistake".
- **"My files" = the real music engine** (`src/music`, `media_api.h` includes its headers).
  The panel's own test runs give it generated tone WAVs in the run's music folder
  (`docs/agent-notes/test-host.md`, item 9); in the test host it plays through the null device.
- **"Other app" = the real media remote** (`src/remote`), reached only through `other_app.cpp`.
  `other_app::Start()` (one log line) runs from the worker at start-up when "Other app" was the
  saved choice, and from the panel when the player picks it; nothing else starts it in the game.
  With `RACING_MASTER_MOD_TEST_REMOTE=real` in the test host it follows only the app in
  `RACING_MASTER_MOD_TEST_MEDIA_APP` (the fake player). In the **test host** `other_app`
  uses an example app instead (a web browser that allows play and pause; logged as `test app:`),
  because the real remote reads every media session on the PC and panel screenshots would show the
  developer's own music. `RACING_MASTER_MOD_TEST_REMOTE=real` switches the host to the real one;
  the remote's own tests (runs 15-22) do not go through `other_app`. The remote's status line is
  shown by its `Message` id from the table in `strings.h` (a `static_assert` in `panel.cpp` keeps
  the count in step with `remote::Message`).
- The panel calls `SetPauseWhenGameUnfocused` on its first frame (its `[panel]` value wins over
  the engine's `[music]` one). The picker's `done` callback may run on any thread.
- The panel and Dear ImGui are compiled with the core's flags (`/EHs-c- /D_HAS_EXCEPTIONS=0`,
  `/W4 /WX`) plus `/DIMGUI_USER_CONFIG=<imconfig_mod.h>` and the ImGui folders, so the standard
  library types the panel shares with the engine and the remote are built the same way on both
  sides. Before the merge the panel used `/EHsc`; do not switch it back.
- **Test-only switches** (read only with the test-host flag): `RACING_MASTER_MOD_PANEL=open`,
  `_LANG`, `_TEXT_SIZE`, `_SOURCE`, `_LIST=open`, `_SETTINGS_ROW=open`, `_TEST_PANEL_FAULT=<panel
  frame>`, `_TEST_PANEL_OFF=wndproc|assert` (the first F12 faults the window procedure, or raises
  an ImGui assertion on the next panel frame), `_TEST_TEXT_FIELD=1` (adds a text field to the settings row), `_TEST_REMOTE=limited|full|none|real`
  (the example "Other app", or the real remote), `_TEST_PICKER_FILES=<path>|<path>` (a suppressed
  file dialog reports these instead of a cancel), and for `testhost.exe` itself
  `_TEST_FAKE_PLAYER=1` (panel mode publishes the silent fake player). In the test host the mod writes `%LOCALAPPDATA%\RacingMasterMod\test-layout.txt`
  (state, window rectangles, widget centres) whenever it changes; the host reads it to aim its
  posted messages. Never in the game.
- The test host's own window procedure counts what "the game" receives; panel runs also check 14
  pieces of the host's pipeline state after every Present.
- No `%ls` under `src\`: `build.ps1` refuses it. Wide strings go through `LogUtf8()` and `%s`
  (the static CRT's default locale drops a whole `vsnprintf` line on a Chinese path).
- `Log()` must not be called while holding another lock, and no lock may be held across a call
  that can fault (a fault inside the guard leaves the lock held forever).
- The watchdog line 10 s after attaching reads the two counters: `hook calls` (every Present that
  reached the hook) and `game frames` (frames of the bound swap chain). Both zero points at an
  overlay or another mod the hook does not see; only `game frames` zero means no window qualified.
  The line is worded for the owner (overlay examples, no "hook" or "wrapper DLL"); keep it so.
- The first line of every `mod.log` is the "loaded into" identity line; the settings lines come
  after it. `ModThread` logs the identity before `ReadSettings` on purpose.
- `Invoke-Run` in `tools\run-host-test.ps1` lowers `$ErrorActionPreference` to `Continue` around
  the program call: under `Stop`, one line a test program writes to its error stream would abort
  the whole script with no summary.
- Media sessions use the **raw WinRT ABI headers** (the SDK's `winrt` include folder), not
  C++/WinRT: C++/WinRT needs `/EHsc` and adds four static imports. Every combase, shell32 and
  advapi32 function is resolved with `GetProcAddress` on the media thread; calling one directly
  fails the link or the KERNEL32-only gate.
- **No command may reach a real media session while testing.** In the test host,
  `Engine::Send` (the only function that sends) refuses every app id except
  `RACING_MASTER_MOD_TEST_MEDIA_APP` (the fake player). New command paths go through it. Each
  media test run makes the fake player Windows' current session for a few seconds (media keys
  point at it); nothing is shown or heard.
- `remote::Remote::Shutdown()` in the game only lets go and sleeps; it never calls
  `RoUninitialize`, because the mod may have created the process's multithreaded apartment that
  other game threads now belong to. Do not "complete" it into a full stop.
- Track titles and artists never go into `mod.log`; the run-host test fails if the fake title
  appears. App ids and names may.
- The PowerShell tool in this environment may start in the game folder (`C:\Program Files
  (x86)\g112`): always call the scripts by absolute path, never by `build\build.ps1`.
- The music engine refuses WASAPI whenever `RACING_MASTER_MOD_TEST_HOST=1` (null device instead),
  and the picker then never calls `Show()`. `musictest.exe` sets that flag for itself. Never run
  the engine with a real device in an automated test on this PC.
- In engine tests, call `testing::Sync` after a command before pulling audio with
  `testing::Render`: silence pulled before the player thread acted consumes nothing and makes
  positions look late.
- Fixture and test strings use neutral Chinese text ("測試"), never a real artist or song name.
- The media remote's runs (15-22, and 41) depend on Windows' media sessions at that moment and
  failed intermittently on 2026-11-14 (also on main's own commit). When they fail while every
  loader and music check passes, run the suite again before suspecting the music engine. On
  2026-11-15 18 full runs on `integrate` saw no media failure; two engine test cases failed
  once or twice each (a missing `testing::Sync`, a 50 ms window misread at a loop restart) and
  were fixed in their own commits.
- Every panel run (25-38, 40, 41) waits after the host closes its window for the music shutdown
  lines ("music: stopped and saved"), so the panel runs take a few seconds longer than before.

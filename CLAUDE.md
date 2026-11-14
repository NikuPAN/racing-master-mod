# Racing Master Mod

Unofficial mod for the PC version of Racing Master (巔峰極速, NetEase, internal name `g112`).
One DLL loaded into the game that will carry an in-game music panel and NVIDIA DLSS.
Free, MIT, README in English and Traditional Chinese. Repo is private until the owner makes it public.

@docs/agent-notes/README.md

## Current state (2026-11-14, evening)

- **Loader 0.2.0 built and verified in the test host** (`out\version.dll`, 20 of 20 checks in
  `tools\run-host-test.ps1`). It forwards the 17 real `version.dll` functions, wakes up only
  inside `g112-Win64-Shipping.exe` (or `testhost.exe` with the test flag), hooks
  `IDXGISwapChain::Present`/`Present1`, binds to the game's one swap chain, draws a marker square
  for 20 s and writes `%LOCALAPPDATA%\RacingMasterMod\mod.log`.
- **0.2.0 has not been in the real game yet.** It applies the loader review
  (`docs/agent-notes/loader-hardening.md`); the review asked for one more in-person run on the
  throwaway account before the panel work starts, including drag-resize, window-mode switch,
  alt-tab and minimise/restore during the 20 s marker.
- **Loader 0.1.0 verified in the real game (2026-11-14, throwaway account):** the game loads the
  DLL, the launcher leaves the file in place, the marker shows on the login screen, about 60 fps.
  Swap chain: 2558x1360, DXGI format 24 (R10G10B10A2_UNORM), 2 buffers, swap effect 4
  (FLIP_DISCARD), windowed. The game's own log shows no reaction. This does not rule out a ban
  issued later.
- **In design:** music panel and DLSS (verified design briefs are being written in
  `docs/agent-notes/local/design/`).

## Order of work

1. Loader test in the real game (the owner, throwaway account). Read `mod.log` afterwards.
2. In-game music panel: Dear ImGui drawn from the Present hook; local files (MP3, FLAC, WAV,
   OGG) and remote control of other players through Windows media sessions; play, pause, stop,
   skip, loop one, loop all; English and Traditional Chinese.
3. DLSS 4.5 Super Resolution / DLAA through NVIDIA's official DLSS SDK on Direct3D 11,
   replacing the engine's TAA pass.
4. DLSS 5, only from NVIDIA's official release, only after step 3 works.

## Rules that must not be "fixed" away

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
  is why `version.dll` statically imports **KERNEL32 only**: `user32` is delay-loaded, and
  `build.ps1` fails if any other DLL appears in the import table. Every system DLL a later
  feature needs gets its own `/DELAYLOAD:` line or is loaded with `LoadLibraryW` from the
  worker thread. `tools\run-host-test.ps1` checks a bystander program for this.
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
powershell -ExecutionPolicy Bypass -File build\build.ps1          # out\version.dll, out\testhost.exe, out\bystander.exe
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1  # 11 hidden-window runs, 20 checks, exit 0 = pass
```

`build\fetch-deps.ps1` downloads the Windows SDK (Microsoft NuGet packages, SHA-256 pinned in
the script) into `.deps\`. With `-MsvcCrt` it also downloads the Visual C++ runtime headers and
libraries; `build.ps1` asks for that automatically when Visual Studio has the compiler but no
headers. The build also writes `out\version.pdb` and `out\version.map` for turning a fault
offset from the log into a source line. **Never ship the PDB next to the DLL.**

Both scripts run from the repo root. To inspect the DLL's imports by hand use the MSVC linker as
dumpbin: `link.exe /DUMP /NOLOGO /IMPORTS out\version.dll` (the fetched SDK has no `dumpbin.exe`).

## Settings

Two files, both optional, same `[mod]` keys (`stage` 0/1/2, `marker_seconds` 0..600):

1. `%LOCALAPPDATA%\RacingMasterMod\settings.ini` (no administrator rights, survives game updates)
2. `racing-master-mod.ini` next to `version.dll` (overrides 1 when present)

The reader (`src/core/settings.cpp`) accepts UTF-8 with or without a byte order mark and
UTF-16LE with one, `;`/`#` comments, digits-only numbers. A missing file means defaults. A file
that is present but unreadable, or a `stage` that is not 0, 1 or 2, switches the mod **off** with
a log line that quotes the value (fail-closed). `GetPrivateProfile*` is not used: it misses a
section after a BOM and turns `-1` into 4294967295.

## Layout

- `src/loader/` — `version.dll` stand-in: `dllmain.cpp` (decide and start the worker, nothing
  else), `proxy.cpp` (table of the 17 real functions), assembly forwarding stubs, export list,
  version resource.
- `src/core/` — `log` (UTF-8 log, no flush, dated archives, `mod.<pid>.log` fallback),
  `settings`, `vtable_patch` (the only place that writes into a foreign function table),
  `dxgi_probe` (finds DXGI's swap chain table with a throwaway device), `present_hook` (hook,
  swap chain binding, fault guard, counters), `frame.h` (`FrameRefs`), `worker` (start-up
  order, watchdog, periodic status line).
- `tools/testhost/` — `testhost.cpp`: Direct3D 11 program with a never-shown window that stands
  in for the game (game layout, worker render thread, checks T1-T8); `bystander.cpp`: a
  kernel32-only console program that must see plain forwarding and nothing else.
- `build/` — build and dependency scripts. No CMake, no solution file.

## Threads and what each may do

| Thread | Runs | May do | Must not do |
|---|---|---|---|
| Loader (under the loader lock) | `DllMain` | read exe name and one environment variable, load the real `version.dll`, `CreateThread` | wait, log, read files |
| Worker (`ModThread`) | start-up, settings, hook install, watchdog, periodic log | file I/O, `user32`, device creation, sleeping | touch the game's device context or swap chain |
| Game's present thread | `HookedPresent` -> `GuardedOnFrame` | Direct3D on the swap chain it was handed, atomics, a one-off log line | file flushes, sleeping, periodic logging, waiting on a lock the worker holds during slow work |
| Any game thread | forwarding stubs | jump | anything else |

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
- Any `user32` function the frame thread calls must first have been called once on the worker
  (delay-load thunk resolution). Today that is `GetWindowThreadProcessId`, `GetClientRect`,
  `IsWindowVisible`; `ModThread` warms them up right after the window is found.
- No `%ls` under `src\`: `build.ps1` refuses it. Wide strings go through `LogUtf8()` and `%s`
  (the static CRT's default locale drops a whole `vsnprintf` line on a Chinese path).
- `Log()` must not be called while holding another lock, and no lock may be held across a call
  that can fault (a fault inside the guard leaves the lock held forever).
- The watchdog line 10 s after attaching reads the two counters: `hook calls` (every Present that
  reached the hook) and `game frames` (frames of the bound swap chain). Both zero points at an
  overlay or wrapper the hook does not see; only `game frames` zero means no window qualified.
- A later file that needs C++ exceptions (C++/WinRT for media sessions) gets its own flag list
  with `/EHsc`, the `cppwinrt` include folder, `WindowsApp.lib` and `/DELAYLOAD:` for each DLL it
  pulls in (`api-ms-win-core-winrt-l1-1-0`, `api-ms-win-core-winrt-error-l1-1-1`, `OLEAUT32`,
  `api-ms-win-core-com-l1-1-0`). Its exported functions are `noexcept` and wrap their bodies in
  `try/catch(...)`; they run on the worker, never inside `GuardedOnFrame`.

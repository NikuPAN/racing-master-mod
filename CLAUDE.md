# Racing Master Mod

Unofficial mod for the PC version of Racing Master (巔峰極速, NetEase, internal name `g112`).
One DLL loaded into the game that will carry an in-game music panel and NVIDIA DLSS.
Free, MIT, README in English and Traditional Chinese. Repo is private until the owner makes it public.

@docs/agent-notes/README.md

## Current state (2026-11-14)

- **Built and verified in the test host:** the loader (`out\version.dll`). It forwards the 17
  real `version.dll` functions, wakes up only inside `g112-Win64-Shipping.exe`, hooks
  `IDXGISwapChain::Present`/`Present1`, draws a marker square for 20 s and writes
  `%LOCALAPPDATA%\RacingMasterMod\mod.log`.
- **Verified in the real game (2026-11-14, throwaway account):** the game loads the DLL, the
  launcher leaves the file in place, the marker shows on the login screen, the hook reports
  about 60 fps. Swap chain: 2558x1360, DXGI format 24 (R10G10B10A2_UNORM), 2 buffers, swap
  effect 4 (FLIP_DISCARD), windowed. The game's own log shows no reaction. This does not rule
  out a ban issued later.
- **In design:** music panel and DLSS (verified design briefs are being written).

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
  game folder also import `version.dll`; they must get plain forwarding and nothing else.
- **Nothing is held across frames** that would block the game's `ResizeBuffers` (no cached
  back buffer references).
- The README must keep the account-risk warning at the top, in both languages. Chinese is
  written by hand as Traditional Chinese for Taiwan/HK players, never machine-translated.

## Commands

```powershell
powershell -ExecutionPolicy Bypass -File build\build.ps1          # out\version.dll, out\testhost.exe
powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1  # hidden-window check, exit 0 = pass
```

`build\fetch-deps.ps1` downloads the Windows SDK (Microsoft NuGet packages) into `.deps\`.
With `-MsvcCrt` it also downloads the Visual C++ runtime headers and libraries; `build.ps1`
asks for that automatically when Visual Studio has the compiler but no headers.

## Layout

- `src/loader/` — `version.dll` stand-in: assembly forwarding stubs, export list, `DllMain`,
  version resource.
- `src/core/` — log file and the Present hook.
- `tools/testhost/` — Direct3D 11 program with a never-shown window that stands in for the
  game. Imports `version.dll`, reads its back buffer back and checks for the marker.
- `build/` — build and dependency scripts. No CMake, no solution file.

## Gotchas

- The test host only activates the mod when `RACING_MASTER_MOD_TEST_HOST=1` is set.
- `/WX` is on for the mod: warnings fail the build.
- The mod is linked with `/MT` so players need no Visual C++ redistributable.
- `__try/__except` guards the per-frame code; keep C++ objects with destructors out of
  `GuardedOnFrame`.

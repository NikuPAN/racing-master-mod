---
name: game-facts
description: Engine, renderer, audio, protection and launch facts for Racing Master PC (g112), read from the installed files
type: reference
---

Verified on 2026-11-14 from the HMT client's files and logs (build `43bf8d49b58 2f4624708 202610090008`).
Nothing below came from the running process.

- **Engine:** Unreal Engine 4.26.1 (executable version resource `4.26.1.0`), one monolithic
  shipping executable, `Binaries\Win64\g112-Win64-Shipping.exe`, about 198 MB. There is no
  plugin loader for third parties; the `Plugins\` folder only holds two vendor DLLs.
- **Renderer:** Direct3D 11, feature level 11_0 (`LogD3D11RHI`). The executable also contains
  D3D12 and Vulkan RHI strings and a `-dx12` switch, untested and unsupported by the game.
- **Upscaling:** FSR 1 only (`r.FidelityFX.FSR.*`, spatial). `r.TemporalAA.Upsampling=0`,
  `r.ScreenPercentage=100`. No DLSS, NGX, Streamline, FSR 2 or XeSS code in the executable.
- **Audio:** Wwise 2022.1.12 (`LogAkAudio`). Music is driven from Lua (`MusicPlayer`,
  `SoundSongList`, `music_menu` modules; banks such as `music_hall_gta`). The game's own
  settings have a separate music volume slider (the owner confirmed).
- **Scripting/UI:** Lua (`LuaLog`), UMG. UI scripts are inside the `.pak` files.
- **Protection:** the executable has packer sections `.7w10`, `.7w11`, `.7w12` and Detours
  sections; plugins `AntiCheatSDK` and `QSecSdk` are mounted; the string `NEProtect` appears.
  The process cannot be opened from a normal user session (path not readable).
- **Launcher:** `g112` launcher process; at start it downloads `hash_with_size.txt` and checks
  game files against it (`Saved\Logs\g112.launch`). Whether it removes unknown extra files is
  **not known**.
- **Imports that matter:** the executable statically imports `VERSION.dll`, `d3d11.dll`,
  `dxgi.dll`, `WINMM.dll`, `XINPUT1_3.dll`. `version.dll` is not a Windows KnownDLL, so a file
  of that name in `Binaries\Win64` is found first.
- **Other programs in the same folder** (`UniCrashReporter.exe`, `pngquant.exe`, CEF helpers)
  may also load a `version.dll` placed there.

**Why:** every design choice in this repo follows from these facts.

**How to apply:** re-check after a game update (the executable changes roughly monthly) before
trusting any of this, especially the import list and the renderer.

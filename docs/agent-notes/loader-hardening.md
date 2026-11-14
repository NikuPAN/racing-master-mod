---
name: loader-hardening
description: What the 2026-11-14 loader review found in version.dll 0.1.0, what 0.2.0 changed in response, and which proposals were deliberately not applied
type: decision
---

An adversarial review of the loader that ran in the game on 2026-11-14 (0.1.0, commit `6ca5c45`)
found two defects that would have been reused by every later hook, and six medium ones. 0.2.0
applies them. The full brief with probes and measurements is machine-local
(`local/design/loader-review.md`); this note keeps what a later agent must not undo.

## Applied

| Finding | What 0.1.0 did | What 0.2.0 does |
|---|---|---|
| Hook live before the original was saved (crash at address 0 if a frame arrived in between; 33-35 null originals in 200,000 stress installs) | `*original = InterlockedExchangePointer(...)` | `PatchSlot` in `vtable_patch.cpp` publishes the original first, then compare-exchanges the slot; `HookedPresent` reads the pointer once |
| Fault guard swallowed silently, kept running, leaked held references (one leaked back-buffer reference makes the game's `ResizeBuffers` return `DXGI_ERROR_INVALID_CALL`) | `__except { g_markerMs = 0; }` | `FrameRefs` declared outside `__try`, released in the handler; mod switched off for the session; one log line with exception code, module and offset; stack overflow, breakpoint and single-step are not swallowed |
| `FlushFileBuffers` per log line, "running" line written from the present thread | about 1 ms per flush on the frame thread every 30 s | no flush; the frame thread only counts; the worker writes the status line |
| `GetPrivateProfileIntW`: UTF-8 BOM hid the `[mod]` section (mod fully on with `stage=0`), `-1` became 4294967295 | fail-open | own reader, UTF-8/UTF-16, fail-closed with a quoted reason; per-user `settings.ini` plus the game-folder ini as override |
| `%ls` in `Log()` dropped the whole line for a Chinese path | `vsnprintf` returned -1 | `LogUtf8()` and `%s`; `build.ps1` refuses `%ls` under `src\` |
| Host test rotated the real game log | same `LOCALAPPDATA` | one log root per run under the temp folder |
| Test host used 640x360 BGRA8 x1 SEQUENTIAL on one thread | not the game's layout | 2558x1360 R10G10B10A2 x2 FLIP_DISCARD on a worker thread, plus Present1, PRESENT_TEST, three resizes, second swap chain, page protection, injected fault, bystander, ini and Chinese-path runs |
| `USER32.dll` in the import table (loaded into bystanders; error 1114 in a win32k-lockdown process) | eager import | `/DELAYLOAD:user32.dll`, import gate in the build |
| Every swap chain in the process got the marker | class table shared | binds to one swap chain; a 2 s silence lets a new one take over |
| "attached" but no frame was silent | - | watchdog at 10 s with two counters |
| Test flag alone woke the mod in any program | - | also requires the exe name `testhost.exe` |
| Second game process got no log (sharing violation) | - | `mod.<pid>.log` fallback; dated archives, newest five kept |
| `DisableThreadLibraryCalls` in a static-CRT DLL | no-op per Microsoft | removed |
| No PDB; `vc140.pdb` dropped in the current folder | - | `/Zi /Fd`, `/DEBUG /PDB /PDBALTPATH:version.pdb /MAP`; the DLL carries no user folder name |
| NuGet packages downloaded without a checksum | - | SHA-256 pinned in `fetch-deps.ps1` |
| Window poll every 250 ms for ever | - | once a second after two minutes |

## Deliberately not applied

- **Message-only helper window (HWND_MESSAGE).** Would take the mod's one window out of
  enumeration for its 25-45 ms of life. The checker flagged it as a point of judgement against
  the "no hiding" promise; the owner has not decided. Keep the top-level window unless the owner
  says otherwise.
- **Lazy resolution of the 17 forwards (resolver thunks in assembly).** Optional in the brief,
  only when `proxy_exports.asm` is edited for another reason. `DllMain` still loads the real
  `version.dll` and returns `FALSE` if an export is missing.
- **The brief's HDR note** (format 24 under HDR10) is unverified and changes nothing today.

**Why:** both High findings sat in exactly the code the music panel and DLSS will reuse; fixing
them before more is built on the loader was the review's one hard condition.

**How to apply:** new per-frame work extends `FrameRefs`, goes through `GuardedOnFrame`, logs
one-off events only, and never holds a lock across a call that can fault. New hooks go through
`PatchSlot`. New system DLLs get `/DELAYLOAD:` and a warm-up call on the worker for anything the
frame thread will call. 0.2.0 must see one more in-game run on the throwaway account before the
panel starts.

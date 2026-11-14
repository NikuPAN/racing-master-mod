---
name: media-remote-design
description: How src/remote controls other music apps through Windows media sessions, and the choices in it that must not be undone
type: decision
---

`src/remote` lets the panel show and control whatever other app is playing (Spotify, a browser,
any player) through Windows' media sessions (`Windows.Media.Control`). Built 2026-11-14 from
the verified design brief (machine-local: `docs/agent-notes/local/design/media-session-remote.md`
in the main checkout, including its "Checker notes"). Verified only in the test host.

Choices, each with the reason it exists:

- **Raw WinRT ABI headers, not C++/WinRT.** C++/WinRT needs C++ exceptions (fails under the
  mod's `/EHs-c- /WX`) and adds four static imports. The raw headers return HRESULTs. The
  build adds the SDK's `winrt` include folder, never `cppwinrt`.
- **No new static import.** combase, shell32 and advapi32 functions (`RoInitialize`,
  `CoTaskMemFree`, `SHCreateItemInKnownFolder`, `OpenProcessToken`, ...) are resolved with
  `GetProcAddress` on the media thread. A direct call fails to link or breaks the KERNEL32-only
  gate. The bystander check also fails if combase, shell32 or `Windows.Media.MediaControl.dll`
  is loaded into a program that is not the game.
- **One mod-owned thread in the multithreaded apartment** does all WinRT work. The frame thread
  only copies a snapshot (`GetSnapshot`) and queues commands; neither waits for Windows.
- **The game never leaves the apartment.** If the game had no multithreaded apartment, the
  mod's `RoInitialize` creates it and every game thread that never initialised COM becomes an
  implicit member (measured by the brief's checker). So `Shutdown()` in the game only lets go
  of the sessions and sleeps; `Start()` wakes it. Only the test host calls `RoUninitialize`.
- **Every async call has a deadline**: manager 3 s, title 2 s, command 1.5 s, confirmation
  2.5 s. A synchronous call cannot have one; if it hangs, `GetSnapshot()` reports "not
  answering" once the snapshot is 5 s old.
- **Commands are explicit and never substituted.** Play/pause sends play or pause by state and
  uses the toggle only when neither is offered. A control the app does not offer is refused
  with the reason; there is no fallback to media keys or another command. An accepted command
  is confirmed by reading back; "accepted but nothing changed" is its own result. An app that
  ignores two loop changes keeps the loop button off for the run.
- **Change notices are a hint.** A half-second refresh keeps the snapshot right without them.
  A freshness check (a second manager every 10 s) switches to rebuild mode if the long-lived
  objects go stale, which is the expected failure in an elevated game.
- **Titles and artists never reach the log** (players paste it into bug reports). App ids and
  names do.
- **The snapshot carries a message id** (`Message`) and the app it names, besides the English
  `message`, so the panel writes the Traditional Chinese line itself. The English wording of
  each id is listed in `remote.h`.

**Why:** these follow from the mod's rules (no exceptions in the game, KERNEL32-only imports,
never block the frame thread, never touch what the game owns) and from the brief's measurements.

**How to apply:** a later change that adds a Windows call resolves it at run time on the media
thread; anything that reaches another program goes through `Engine::Send`, which holds the test
guard.

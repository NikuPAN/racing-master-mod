---
name: media-remote-elevated-game
description: Open question - whether Windows media sessions work from the elevated game process, and which mod.log lines answer it
type: open-question
---

**Unverified.** Everything in `src/remote` was tested in the unelevated test host only. The game
process is very likely elevated (its `mod.log` is owned by `BUILTIN\Administrators`; the brief
infers this from files, the running game was not inspected).

What may differ in the elevated game:

1. Whether the session list is visible at all (the per-user media service for the elevated half
   of an administrator token).
2. Whether change notices arrive. If not, the cached state may go stale. The freshness check
   and rebuild mode exist for this; they were exercised in the test host by forcing rebuild
   mode, not by a real stale cache.
3. Whether commands reach an unelevated Spotify or browser (high to medium integrity over COM,
   the permitted direction, expected to work).
4. Whether the game already has a multithreaded apartment, and whether the game or its
   protection reacts to the extra threads, DLLs and COM traffic.

The `mod.log` lines that answer it (no titles are ever logged):

- `music apps: starting on the mod's own thread; the process had / already had a multithreaded
  apartment ...; the game runs / does not run as administrator`
- `music apps: Windows answered in N ms; K apps found (ids)` (or "not answering", with a code)
- `music apps: change notices are arriving`
- `music apps: a fresh check agrees with the held session` or
  `music apps: change notices are not arriving ...; re-reading twice a second instead`

**Why:** the brief could not run an elevated probe without a consent prompt, and the owner's
rule forbids touching the running game.

**How to apply:** the first in-game build with the panel should start the remote read-only
(no commands) for about a minute, on the throwaway account, and the owner sends `mod.log`.
Until then, do not tell players that controlling other apps works in the game.

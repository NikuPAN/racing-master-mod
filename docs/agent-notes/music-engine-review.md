---
name: music-engine-review
description: What the unbuilt first draft of the music engine (WIP commit 36be6c4, never pushed, replaced) got wrong and what was changed, so nobody brings the old behaviour back
type: decision
---

A first builder stopped at a usage limit with an unbuilt, untested draft of `src/music`. Its
shape was kept (source split, gate and ring design, playlist logic, store format, picker); the
draft commit was replaced by a sequence of small commits. Defects found on review and fixed:

| Draft | Problem | Now |
|---|---|---|
| The output's shared block (atomics, ring) was freed by `Uninit` | the frame thread stores `userHold`/`volume` at any moment: a use-after-free when the watchdog re-created the device | allocated once, never freed; `Init`/`Uninit` only touch device and ring |
| Manual (test) output treated the ring reset as safe without a handshake | the test thread's `Render` runs concurrently: a data race on the ring | an SRW lock between `Render` and the reset |
| Callback acknowledged a closing gate only after fading | an empty ring during the fade never acknowledged: every flush waited the full 100 ms | acknowledges as soon as nothing is left to fade |
| A decoder fault inside `FillRing` started the next track from inside the decode call | a dangling reference to the cleared track, and recursion as deep as the list | `Decode` only sets flags; `HandleFault`/`StartTrack` walk failures in a loop |
| Converter fed `needed` frames, result ignored the frames it did not take | dropped samples (clicks) whenever the resampler took fewer | leftover input kept in `pending_` |
| At a boundary the queued copy of the logic replaced the real one | a Pause in the last 10 ms of a track turned back into Playing | only `current` and `failures` move at a boundary |
| Every append invalidated the queued successor; a removed successor kept its stale index | needless gaps; a removed track could still play | compare the expected successor's entry id with the decision taken; removed tracks get index -1 |
| End of list and "decide again" shared one path | a list where every successor failed was retried at the end, double notices | `EndKind::Final` vs `Redecide` |
| Watchdog re-created the device and restarted the track at 0:00 | position lost | restarts at the remembered position (tested with a simulated device loss) |
| `settings.ini` read and merged on the player thread, as raw bytes | I/O on the audio thread; a UTF-16 file (the loader accepts it) would have been destroyed | merged on the I/O thread, decoded first (`TextFileToUtf8`) |
| Shutdown saved while the I/O thread could still be saving | two writers on the same temp file | the I/O thread finishes first |
| Duplicate check and tag matching compared every path with every entry | seconds of player-thread time for a large library | hash index on `PathKey` |
| `ToExtendedPath` prefixed paths as given | `\\?\` with `/` or `..` fails | `NormalisePath` (GetFullPathNameW) first; stored paths are normalised |
| Tag reads parsed untrusted files with no fault guard on the I/O thread | a crafted OGG/Opus tag could take the game down | `ReadTags` runs inside the same SEH guard |
| Media Foundation reader ignored a mid-stream format change | buffer overflow if channels grew | the track ends there |
| `OpenAdd*Dialog` returned `bool` | the panel contract says `void` | `void`; `IsDialogOpen()` and `Snapshot::dialogOpen` say when a request was ignored |
| `static Engine engine` | destructor (device uninit, thread handles) at DLL unload under the loader lock | heap object, never destroyed |
| Playlist loaded asynchronously on the player thread | `GetSnapshot` right after `Start` showed an empty list | `Start` loads before returning |
| A truncated file ended silently | no hint for the player | an early end (decoder error, or well short of the stated length) flags the row damaged |
| Opus: endless `OP_HOLE` loop possible | a file of holes keeps the thread busy | at most 64 in a row |

Kept from the brief, not changed: `/DNDEBUG` on all third-party code, `std::atomic<shared_ptr>`
avoided (SRW lock instead), stb_vorbis owns its `FILE*` also on a failed open, Missing and
AccessDenied decided by the engine's own `CreateFileW`, never by miniaudio's result code.

**Why:** these were real bugs, several of them crashes inside the game; the draft had never been
compiled.

**How to apply:** if a change seems to simplify one of these back, read the row first. The tests
in `tools/testhost/musictest_engine.cpp` cover most of them (pause at a boundary is not tested
directly).

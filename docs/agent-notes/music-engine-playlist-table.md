---
name: music-engine-playlist-table
description: The 50-row transition table of the playlist state machine (src/music/playlist_logic.h), the rules it encodes, and where each row is tested
type: reference
---

State: N tracks, current index c (-1 exactly when N = 0), status St / Pl / Pa, loop Off / All /
One, f = failures in a row. The 50 rows are with shuffle off; shuffle is below. Every row below is one or more cases in
`tools/testhost/musictest_logic.cpp` ("row 12: ..."), next to the design brief's invariant sweep
(273 states x 15 events x 7 arguments).

| # | Event | Condition | c after | Status after | Action |
|---|---|---|---|---|---|
| 1 | Play | N = 0 | -1 | St | - |
| 2 | Play | St, N > 0 | c | Pl | Start(c); f = 0 |
| 3 | Play | Pa | c | Pl | Resume |
| 4 | Play | Pl | c | Pl | - |
| 5 | Pause | Pl | c | Pa | Pause |
| 6 | Pause | St or Pa | c | unchanged | - |
| 7 | Stop | Pl or Pa | c | St | Stop |
| 8 | Stop | St | c | St | - |
| 9 | Next | N = 0 | -1 | St | - |
| 10 | Next | c + 1 < N | c + 1 | Pl | Start(c + 1); f = 0 |
| 11 | Next | c = N - 1, loop All or One | 0 | Pl | Start(0); f = 0 |
| 12 | Next | c = N - 1, loop Off, Pl or Pa | 0 | St | Stop |
| 13 | Next | c = N - 1, loop Off, St | 0 | St | - |
| 14 | Previous | N = 0 | -1 | St | - |
| 15 | Previous | Pl or Pa, more than 3 s played | c | Pl | Start(c) |
| 16 | Previous | otherwise, c > 0 | c - 1 | Pl | Start(c - 1) |
| 17 | Previous | otherwise, c = 0, loop Off | 0 | Pl | Start(0) |
| 18 | Previous | otherwise, c = 0, loop All or One | N - 1 | Pl | Start(N - 1) |
| 19 | Play track i | 0 <= i < N | i | Pl | Start(i); f = 0 (also restarts the playing track) |
| 20 | Play track i | i outside the list | c | unchanged | - |
| 21 | Seek | Pl or Pa | c | unchanged | Seek |
| 22 | Seek | St | c | St | - |
| 23 | Set loop | any | c | unchanged | - (the engine re-checks the queued successor) |
| 24 | Track ended | not Pl (stale report) | c | unchanged | - |
| 25 | Track ended | Pl, loop One | c | Pl | Start(c) |
| 26 | Track ended | Pl, loop Off or All, c + 1 < N | c + 1 | Pl | Start(c + 1) |
| 27 | Track ended | Pl, loop All, c = N - 1 | 0 | Pl | Start(0) (with N = 1 the track repeats) |
| 28 | Track ended | Pl, loop Off, c = N - 1 | 0 | St | Stop |
| 29 | Track started | any | c | unchanged | -; f = 0 |
| 30 | Track failed | St (stale report) | c | St | - |
| 31 | Track failed | Pl or Pa, f + 1 >= N | c | St | StopAllFailed; f = 0 |
| 32 | Track failed | Pl or Pa, f + 1 < N, c + 1 < N | c + 1 | Pl | Start(c + 1); f + 1 |
| 33 | Track failed | Pl or Pa, f + 1 < N, c = N - 1, loop All or One | 0 | Pl | Start(0); f + 1 |
| 34 | Track failed | Pl or Pa, f + 1 < N, c = N - 1, loop Off | 0 | St | Stop |
| 35 | Inserted k at p | N = 0 | 0 | St | - (first track cued, nothing plays) |
| 36 | Inserted | p <= c | c + k | unchanged | - |
| 37 | Inserted | p > c | c | unchanged | - |
| 38 | Removed track i | i < c | c - 1 | unchanged | -; f = 0 |
| 39 | Removed track i | i > c | c | unchanged | -; f = 0 |
| 40 | Removed the current track | it was the only one | -1 | St | Stop if it was Pl or Pa, else - |
| 41 | Removed the current track | others remain, St | c, or 0 if it was last | St | - |
| 42 | Removed the current track | others remain, Pa | c, or 0 if it was last | St | Stop |
| 43 | Removed the current track | others remain, Pl, not last | c (the track that followed) | Pl | Start(c) |
| 44 | Removed the current track | others remain, Pl, last, loop All or One | 0 | Pl | Start(0) |
| 45 | Removed the current track | others remain, Pl, last, loop Off | 0 | St | Stop |
| 46 | Moved a to b | a = c | b | unchanged | - |
| 47 | Moved | a < c <= b | c - 1 | unchanged | - |
| 48 | Moved | b <= c < a | c + 1 | unchanged | - |
| 49 | Moved | otherwise | c | unchanged | - |
| 50 | Cleared | any | -1 | St | Stop if it was Pl or Pa, else - |

Rules in words: loop one affects only a natural end (Next always moves on, a broken track is
skipped, never retried); with looping off the end of the list stops and rewinds to the first
track; Next and Previous start playback even from Stopped or Paused, except Next at the very end
with looping off; a list of broken files stops after one pass (`StopAllFailed`), it never spins.

**Why:** the owner's transport choices (open question 6 of the brief: stop at the end with loop
off, Previous restarts after 3 s, Next/Previous start playback) are encoded here and nowhere else.

**How to apply:** change behaviour by changing the row here, the code in `playlist_logic.h` and
its case in `musictest_logic.cpp` together. `Moved` exists in the logic but `Player` has no move
command yet (the panel contract has none).

**Shuffle (0.3.3, the owner's request of 2026-11-15).** With shuffle on the logic walks `order`
(a permutation, this round's play order; `order[pos] == c`) instead of the list: Next, the end
of a song, a failed song and `PeekNextOnEnd` take `order[pos + 1]`; at the end of the round loop
all starts `order[0]` again and loop off stops cued at `order[0]`, both setting `roundEnded` so
the engine hands over a new order that starts with the current song. Previous walks back
through `order`. Choosing a song moves it to just after the current one and starts it, so the
rest of the round keeps its order. Removing the playing song starts the next one in `order`;
added songs join the end of the round. Switching shuffle off continues the list in order from
the current song. `Arrived(index)` is the gapless change-over (decided on a copy) and keeps
`pos` in step. Tested by the shuffle sweep (every order of up to 4 songs, 18 events,
412,522 checks, including Arrived landing where TrackEnded goes) and the named shuffle cases
(musictest 215, 225). How the order is made lives in the engine, not here.

---
name: even-volume
description: "Same volume for every song" (0.3.2) - how songs are measured (EBU R128), where the result is kept, how the gain is applied, and the numbers chosen
type: reference
---

The owner (2026-11-15) asked for the whole list to play at about one level, "like a DAP", by
measuring every track once. The panel's settings row has the checkbox "Same volume for every song"
(`[music] even_volume`, on by default; the engine owns it, the panel reads `Player::EvenVolume()`).

**Measuring** (`src/music/loudness.cpp`): integrated loudness of ITU-R BS.1770-4 / EBU R128 plus
the sample peak. K-weighting designed per sample rate, 400 ms blocks every 100 ms, absolute gate
-70 LUFS, relative gate -10 LU. A mono file counts twice (it plays on both speakers). Musictest
case 380 runs EBU Tech 3341 cases 1-5 (all within 0.1 LU) at 48, 44.1 and 96 kHz.

**Thread:** `rmm-level` (player.cpp, "Measuring thread") runs in `THREAD_MODE_BACKGROUND_BEGIN`
(lowest CPU, disk and memory priority) and decodes one song at a time with the engine's own
`OpenSource` and `GuardedRead`, so a broken file is contained like in playback. The player thread
hands it the songs still to measure, the current song first, then the ones after it, rebuilt when
the list, the current song or the setting changes. Only rows whose tags were read (size and time
known) are measured; cloud placeholders never (that would start a download). It stops within one
4096-frame chunk at shutdown, and the final save does not wait for it.

**Kept** in `playlist.tsv` as an `L` line under the song's `T` line (format in
`playlist_store.h`). Same validity rule as the tags: when a file's size or time changes, its tags
are read again and its loudness is measured again. Measurements reach the file at most every 30 s
while measuring (`kLoudnessSaveIntervalMs`) and at the final save. 0.3.1 reads such a list and drops
the L lines (unknown line types are ignored).

**Applied** on the player thread as the audio is decoded (`ApplyGain`, after `Decode` in
`FillRing`), not in the audio callback: a gapless change to a song of another loudness is then
exact to the sample, with no second boundary to track. A new measurement or the setting switched
moves the gain at most the whole range per second (no click, no jump); the change is heard up to
2 s later because the ring is filled ahead. A song not yet measured plays as it is.

**Numbers** (`loudness.h`): target -14 LUFS (where Spotify and YouTube play, so songs sound as
loud as people expect); never raised more than 12 dB; never raised so far that the loudest sample
passes 0.95. Most commercial songs sit at -6 to -10 LUFS, so turning this on makes them a little
quieter and the volume slider ends up a bit higher than before.

**Why:** a quiet song after a loud one made players reach for the volume mid-race.

**How to apply:** keep the gain at decode time; keep measuring off the player and I/O threads (a
long decode there would hold up playback or saves); tests other than 380/405/620 run with
`testing::Config::evenVolume = false`, so their exact level checks are not moved by a measurement.

---
name: music-engine
description: How the music engine in src/music is built - threads, data flow, gapless, the gate, files on disk, the public interface - and the rules that keep it safe inside the game
type: reference
---

The engine plays the player's own MP3, FLAC, WAV, OGG Vorbis, Opus and M4A/AAC files inside the
game process. The public interface is `src/music/player.h` (`music::Player`, the panel codes
against it) and `src/music/picker.h` (file and folder dialogs). The verified design brief is
machine-local: `docs/agent-notes/local/design/audio-engine.md` in the main repository.

**Decoders** (`source*.cpp`, chosen by file content, then extension, never by trial and error):
miniaudio 0.11.26 for WAV/FLAC/MP3, stb_vorbis 1.24 (pull API, `_wfopen`) for OGG Vorbis,
libopusfile for Opus (see `opus-decoder.md`), the Media Foundation Source Reader for M4A/AAC/ALAC
and raw ADTS. Every decoder call goes through an SEH guard (`GuardedRead/Seek`, `ContainableFault`);
a fault marks the file damaged and the decoder object is leaked on purpose.

**Threads**

| Thread | Does | Never does |
|---|---|---|
| Frame thread (panel) | `Player::*` (posts a command), `GetSnapshot`; `Pause`, `Stop`, `SetVolume` also store an atomic so sound reacts within one 20 ms period | file I/O, decoding, COM, waiting on a lock held during I/O |
| `rmm-player` | commands, playlist logic, opening and decoding into the 2 s ring, track boundaries, device start/stop/re-create, watchdog, snapshot | tag reads, scans, saves |
| miniaudio device thread | the audio callback: copy from the ring, gate fade, volume ramp, count `consumed` | locks, allocation, system calls |
| `rmm-io` | folder scans (background priority), tag reads, crash-safe saves | touching the ring or the device |
| dialog thread | one short-lived STA thread per open file dialog | anything else |

**Gapless:** when the decoding track ends, the player thread asks the playlist logic on a copy
what follows, opens it at once and writes it right behind the last sample, through the same
resampler when the format matches. Each queued track records `startFrame` (in the callback's
`consumed` count); passing it makes the track audible. A change that alters the successor after
it was queued (loop mode, list edit) sets `playLimit` to the boundary and the decision is taken
again there (`InvalidateSuccessorIfChanged`; identity is the entry id, not the index).

**The gate:** every manual change (Stop, Seek, Next, play another track, remove the playing one)
closes the gate, waits for the callback's acknowledgement (at most `kGateTimeoutMs`, 450 ms), only
then resets the ring. On a timeout the ring is not touched; the callback discards up to
`discardBefore` instead. In Manual (test) output an SRW lock between `Render` and the reset gives
the same guarantee.

**Fades (0.3.2):** the gate fades linearly over `AudioOutput::SetFadeMs`. The player sets 250 ms
(`kFadeMs`) for pause, pause when the game loses focus, stop, Next/Previous and playing another
track, and every start fades back in over the same time; no crossfade. Seek fades out in 10 ms
(`kQuickFadeMs`) so it answers at once, then fades in at 250 ms. Shutdown also uses 10 ms, because
the window close waits at most 0.5 s for the final save. A gapless track change never touches the
gate, so albums play on without a dip. The audio keeps moving during a pause fade, so the saved
place is up to 0.25 s past the moment Pause was pressed. Reason: the owner asked for it
(2026-11-15); a hard cut sounded abrupt. Musictest case 500 checks the shape (full, about half at
125 ms, silent from 250 ms) and the fade back in.

**Files** in `%LOCALAPPDATA%\RacingMasterMod\`: `playlist.tsv` (+ `.bak`, `.broken`),
`settings.ini` `[music]` section (volume, loop, pause_when_game_unfocused, current, position_ms;
the loader's `[mod]` lines are kept, any encoding Notepad offers is read), `music\` drop-in folder.
Saves: temp file, flush, then the swap, list 2 s after the last change, settings 0.5 s after.
The list swaps with `ReplaceFileW` (keeps `.bak`, which the loader falls back to); `settings.ini`
swaps with one `MoveFileExW` rename (`SaveFileReplacing`, 0.3.1) and has no backup. Reason:
`ReplaceFileW` is two renames, and a process that ends between them leaves no `settings.ini`;
nothing reads a backup of it, so every section would fall back to defaults and the next save
would make that permanent. Musictest case 115 ends a saving process 100 times at random moments
(the old swap left the file missing after 9 of 100, the single rename after none).
While a song plays its position is saved every 10 s (`kPositionSaveIntervalMs`, 30 s before
0.3.1), and a `[music]` save whose values equal the last one handed to a save is skipped
(`QueueSettingsSave`), so a song that stands still writes nothing. A `[music]` save that did not
reach the file (settings.ini held by another program, or a failed write) is reported back by the
I/O thread (`IoMessage` with `flag`), which clears `haveSaved_`, so the next save writes the same
values again (musictest case 587; before 0.3.1's fix the values counted as saved). Reason: the owner's game
closed twice without the mod seeing it (0.3.0, 2026-11-15), and a forced exit loses at most the
last 10 s of the place in the song. The final save at `Shutdown` runs after the I/O thread has
stopped (one writer per file). Musictest case 585 checks the periodic save with a 300 ms
interval (`testing::Config::positionSaveMs`, test only) and `Counters::settingsSaves`.
`Player::SaveNow()` (0.3.1) skips both delays: the list if it changed and the song with its place
go to the I/O thread at once, and the music plays on. The worker calls it when the game is asked
to close (`integration.md`, "Shutdown"); musictest case 586.

**Start-up:** `Start()` reads playlist and settings on the calling thread, restores the cued track
and position stopped, starts the two threads, scans the drop-in folder. No device and no Media
Foundation DLL until the first Play. The engine object is created on first use and never
destroyed; nothing in `src/music` may have a start-up constructor (build gate 3).

**Why:** the frame thread must never wait on disk or device, a broken file must never take the
game down or spin, and a crash at any moment must never lose or shorten the saved list.

**How to apply:** new engine work keeps these thread rules; new DLL imports get `/DELAYLOAD:` and
an entry in `$delayLoads` in `build.ps1`; new behaviour gets a case in
`tools/testhost/musictest_*.cpp` (see `music-tests.md`).

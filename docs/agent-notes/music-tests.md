---
name: music-tests
description: How out\musictest.exe tests the music engine without sound or windows, how its fixtures are made, and what it cannot check
type: reference
---

`tools\run-host-test.ps1` run 23 starts `out\musictest.exe <work folder> <repo>` with
`LOCALAPPDATA` pointed at its own temp folder and records one check per `PASS`/`FAIL` line;
`SKIPPED` lines are listed with their reason and never count as passes. It then checks the
engine's `mod.log` lines (started / ended / list finished, a skip with "file not found", the
watchdog restart, a Chinese file name written in full, and that only the manual and null outputs
were opened). Run 24 checks the loader's settings reader with a `settings.ini` that holds only
`[music]`. `musictest.exe --fixtures <folder>` (no tests, used by end-to-end run 40) writes four
20 s songs with the same fixture writers (FLAC and MP3 through Media Foundation, tagged Opus and
OGG Vorbis) into `<folder>\music` and a WAV into `<folder>\add`; a missing encoder falls back to
a WAV under the same name and says so.

`musictest.exe` links the same engine objects as `version.dll` (not the DLL itself: the engine is
not wired into the worker yet). It sets `RACING_MASTER_MOD_TEST_HOST=1` for itself, so even a test
mistake cannot open WASAPI or show the file dialog. One file per area, each registering its cases
with `MT_TEST(order, ...)`: `musictest_text` (helpers, crash-safe save), `_logic` (50 table rows,
sweep, named cases), `_miniaudio`, `_vorbis`, `_mf`, `_opus` (decoders at source level, refusals),
`_store` (playlist.tsv, settings merge), `_engine` (the real player thread), `_picker`.

**Fixtures are generated each run**, all our own content (a 0.25-amplitude sine per track:
A 440, B 660, C 880, G 500 Hz), in a folder named with Traditional and Simplified Chinese, a
space, `#` and `%`, plus one WAV under a path longer than 260 characters: WAV by hand; FLAC, MP3,
AAC with Windows' Media Foundation encoders (a missing encoder makes that case SKIPPED); Opus with
libopus + libogg; OGG Vorbis with libvorbis 1.3.7, which only the test program links.

**Engine cases** use the Manual output: the test pulls audio through the real callback in 10 ms
pieces and calls `testing::Sync` (a full player-thread pass) whenever less than 0.5 s is queued.
Tracks are told apart by frequency in 50 ms windows (`mt::Sequence`, e.g. "AB." = A, B,
silence). Gapless is checked as "no sample step above the sine's own, no run of zeros" around the
exact boundary frame, at 48 kHz and through the resampler from 44.1 kHz. One case runs the null
device in real time (about 7 s), including a simulated device loss that the watchdog must cure
without losing the position.

Two cases were flaky and fixed on 2026-11-15: "removing the only track" pulled audio without
`testing::Sync`, and "loop one set late" labelled a 50 ms window that straddled A's restart as
the 500 Hz G; it now labels against the two songs it plays only. When a case labels windows,
give it only the tones it can hear.

Gotchas: call `testing::Sync` after a command and before pulling audio (silence pulled before the
player acted consumes nothing and makes positions look late); a WAV cut in half is not "damaged"
(miniaudio trims its stated length to the bytes present), a FLAC cut in half is.

Runs 15-22 (the media remote, from main) depend on Windows' media sessions at that moment and
failed intermittently on this PC on 2026-11-14, also on main's own commit with no music code; a
media failure next to all-green music checks is not a music engine problem.

**Not checked here (needs a person or the game):** sound quality on real hardware, WASAPI and the
Windows mixer, unplugging a headset (real re-routing), the file dialog once shown, the engine
inside the game process.

**Why:** the owner's rule: nothing audible or visible on this PC, and "done" means it worked.

**How to apply:** new engine behaviour gets a case in the matching file; keep fixtures generated,
not committed, and keep every SKIPPED reason specific.

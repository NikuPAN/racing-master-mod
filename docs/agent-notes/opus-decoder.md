---
name: opus-decoder
description: Why Opus is decoded with libopus + libopusfile + libogg built from Xiph's release tarballs, how they are built without CMake, and why .webm is left out
type: decision
---

The owner made Opus a version 1 format (files saved from YouTube are often Opus). The design
brief had deferred it; this is the choice made when it was built (2026-11-14).

**Choice:** opus 1.6.1 + opusfile 0.12 + libogg 1.3.6, Xiph's own release tarballs, hashes matching
Xiph's published SHA256SUMS, pinned in `build/fetch-deps.ps1`. All three are BSD 3-clause; their
notices are in `THIRD_PARTY.md` and must ship with the DLL.

Alternatives turned down:
- **Media Foundation's Opus decoder:** opening `.opus` depends on an optional Store component,
  and Media Foundation on this PC cannot even open OGG Vorbis (brief E11). Not reliable on a
  player's PC.
- **libopus alone with a hand-written Ogg reader:** the decoder is the easy part; pre-skip,
  end trimming (gapless), granule-position seeking, chained streams and malformed-page recovery
  are what opusfile already does and is fuzzed for.
- **miniaudio's `extras/decoders/libopus`:** only a wrapper around the same libopusfile.

**Build:** `build.ps1` reads the file lists from opus's own `celt_sources.mk`, `silk_sources.mk`
and `opus_sources.mk` (`CELT_SOURCES`, `SILK_SOURCES`, `SILK_SOURCES_FLOAT`, `OPUS_SOURCES`,
`OPUS_SOURCES_FLOAT`), adds libogg's two files and opusfile's `opusfile.c info.c internal.c
stream.c` (no `http.c`, `OP_DISABLE_HTTP`), and builds `out\obj\xiph.lib` with `/MP`, once per
recipe (stamp file). Defines: `OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF`. Plain C: no SIMD,
no run-time CPU detection, none of the neural-network features (deep PLC, DRED, OSCE), no
`ENABLE_HARDENING`/`ENABLE_ASSERTIONS` (they call `abort()`, which would end the game). `/W3`
without `/WX` (upstream code; C4244 and C4005 silenced). Only the decoder objects end up in
`version.dll` (static library + `/OPT:REF`); the test program also links the encoder to make its
fixture. The DLL grew to about 0.95 MB with the whole engine.

**What it gives:** 48 kHz stereo float (`op_read_float_stereo` folds any channel layout), exact
length, sample-exact seeking (tested: seek to 0.75 s leaves exactly 12000 frames), gapless trim
(tested: a 1 s file decodes to exactly 48000 frames), tags (`TITLE`, `ARTIST`).

**Left out: `.webm`.** YouTube downloads come as `.webm` (Opus in Matroska) or `.opus`/`.ogg`.
WebM needs a Matroska/EBML demuxer, which none of these libraries has; writing or adding one is
disproportionate for version 1. A `.webm` file is reported as "not a music file the player can
read". Converting to `.opus` (for example with yt-dlp's `-x --audio-format opus`) remuxes without
re-encoding (UNVERIFIED here: no such tool was run on this PC).

**Why:** Opus was required, and this is the only route that is reliable on every Windows PC, keeps
gapless play, and is MIT-compatible.

**How to apply:** a version bump means new hashes in `fetch-deps.ps1`, a rebuild (the stamp forces
it), and the Opus cases in `musictest_opus.cpp` must still pass.

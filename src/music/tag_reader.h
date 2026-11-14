#pragma once

// Title, artist and duration for the list, read cheaply (no audio is decoded). Runs on the
// I/O thread only (COM must be initialised on it, as the engine's I/O thread does).

#include "source.h"

#include <string>

namespace music {

struct Tags {
    std::wstring title;      // empty = use the file name
    std::wstring artist;
    double durationSeconds = -1;
};

// Reads what the file offers; `kind` is what SniffFile found. Returns false when the file could
// not be opened for tags (the caller keeps the file name). Media Foundation's own property store
// for MP3, FLAC, WAV, M4A and AAC (no shell extension is loaded, no decoder is created); Vorbis
// comments for OGG; Opus tags for Opus.
// A fault inside the parser is contained: the result is false and *faulted is set.
bool ReadTags(const std::wstring& path, Kind kind, Tags* tags, bool* faulted);

// Per-format readers (source_*.cpp).
bool ReadVorbisTags(const std::wstring& path, Tags* tags);
bool ReadOpusTags(const std::wstring& path, Tags* tags);
bool ReadMediaFoundationTags(const std::wstring& path, Tags* tags);

}  // namespace music

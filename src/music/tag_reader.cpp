#include "tag_reader.h"

#include <windows.h>

namespace music {

namespace {

bool ReadByKind(const std::wstring* path, Kind kind, Tags* tags) {
    switch (kind) {
        case Kind::Vorbis: return ReadVorbisTags(*path, tags);
        case Kind::Wav:
        case Kind::Flac:
        case Kind::Mp3:
        case Kind::MediaFoundation: return ReadMediaFoundationTags(*path, tags);
        default: return false;
    }
}

// No object with a destructor in this frame (C2712). A parser that faults is abandoned with
// whatever it held; the leak is the price of keeping the game running.
bool Guarded(const std::wstring* path, Kind kind, Tags* tags, bool* faulted) {
    __try {
        return ReadByKind(path, kind, tags);
    } __except (ContainableFault(GetExceptionCode())) {
        *faulted = true;
        return false;
    }
}

}  // namespace

bool ReadTags(const std::wstring& path, Kind kind, Tags* tags, bool* faulted) {
    *tags = Tags{};
    *faulted = false;
    const bool ok = Guarded(&path, kind, tags, faulted);
    if (*faulted) *tags = Tags{};
    return ok;
}

}  // namespace music

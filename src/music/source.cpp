#include "source.h"

#include "text.h"

#include <windows.h>

#include <cstring>

namespace music {

namespace {

// Reads the first bytes of the file with the engine's own handle. This call decides Missing and
// AccessDenied; the decoders cannot report those reliably.
bool ReadHead(const std::wstring& path, unsigned char* head, DWORD* got, OpenError* error) {
    *got = 0;
    const std::wstring pathX = ToExtendedPath(path);
    const HANDLE file = CreateFileW(pathX.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME ||
            code == ERROR_BAD_NETPATH || code == ERROR_BAD_NET_NAME) {
            *error = OpenError::Missing;
        } else {
            *error = OpenError::AccessDenied;
        }
        return false;
    }
    // Skip an ID3v2 block first: "ID3", flags, then the size as four 7-bit bytes, plus the 10-byte header.
    unsigned char first[10] = {};
    DWORD read = 0;
    if (!ReadFile(file, first, 10, &read, nullptr)) {
        CloseHandle(file);
        *error = OpenError::AccessDenied;
        return false;
    }
    if (read == 10 && first[0] == 'I' && first[1] == 'D' && first[2] == '3' && first[6] < 0x80 && first[7] < 0x80 &&
        first[8] < 0x80 && first[9] < 0x80) {
        const LONGLONG size = 10 + ((static_cast<LONGLONG>(first[6]) << 21) | (static_cast<LONGLONG>(first[7]) << 14) |
                                    (static_cast<LONGLONG>(first[8]) << 7) | first[9]);
        LARGE_INTEGER move;
        move.QuadPart = size;
        SetFilePointerEx(file, move, nullptr, FILE_BEGIN);
        read = 0;
    } else {
        LARGE_INTEGER zero = {};
        SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
        read = 0;
    }
    if (!ReadFile(file, head, 64, &read, nullptr)) {
        CloseHandle(file);
        *error = OpenError::AccessDenied;
        return false;
    }
    CloseHandle(file);
    *got = read;
    return true;
}

bool Starts(const unsigned char* head, DWORD got, const char* magic, DWORD at = 0) {
    const size_t n = strlen(magic);
    return got >= at + n && memcmp(head + at, magic, n) == 0;
}

}  // namespace

Kind SniffFile(const std::wstring& path, OpenError* error) {
    *error = OpenError::None;

    // 1. Does it exist, and is it really on this disk?
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    const std::wstring pathX = ToExtendedPath(path);
    if (!GetFileAttributesExW(pathX.c_str(), GetFileExInfoStandard, &attributes)) {
        const DWORD code = GetLastError();
        *error = (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) ? OpenError::AccessDenied
                                                                                 : OpenError::Missing;
        return Kind::Unknown;
    }
    if (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        *error = OpenError::Unsupported;
        return Kind::Unknown;
    }
    if (attributes.dwFileAttributes &
        (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) {
        *error = OpenError::OnlineOnly;
        return Kind::Unknown;
    }
    if (attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow == 0) {
        *error = OpenError::Empty;
        return Kind::Unknown;
    }

    // 2. Content.
    unsigned char head[64] = {};
    DWORD got = 0;
    if (!ReadHead(path, head, &got, error)) return Kind::Unknown;

    if (Starts(head, got, "fLaC")) return Kind::Flac;
    if ((Starts(head, got, "RIFF") && Starts(head, got, "WAVE", 8)) || (Starts(head, got, "RF64") && Starts(head, got, "WAVE", 8))) {
        return Kind::Wav;
    }
    if (Starts(head, got, "OggS")) {
        if (Starts(head, got, "\x01vorbis", 28)) return Kind::Vorbis;
        if (Starts(head, got, "OpusHead", 28)) return Kind::Opus;
        // Another Ogg payload (FLAC-in-Ogg, Speex, Theora): let the extension decide below.
    }
    if (Starts(head, got, "ftyp", 4)) return Kind::MediaFoundation;
    if (got >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) {
        const unsigned layer = (head[1] >> 1) & 3;
        if (layer == 0) return Kind::MediaFoundation;  // ADTS (AAC) sync: layer bits 00
        return Kind::Mp3;                               // MPEG audio frame sync
    }

    // 3. Extension.
    const std::wstring extension = Extension(path);
    if (extension == L".mp3") return Kind::Mp3;
    if (extension == L".flac") return Kind::Flac;
    if (extension == L".wav") return Kind::Wav;
    if (extension == L".ogg" || extension == L".oga") return Kind::Vorbis;
    if (extension == L".opus") return Kind::Opus;
    if (extension == L".m4a" || extension == L".aac" || extension == L".mp4") return Kind::MediaFoundation;
    *error = OpenError::Unsupported;
    return Kind::Unknown;
}

namespace {

int Containable(DWORD code) {
    return ContainableFault(code);
}

}  // namespace

int ContainableFault(unsigned long code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return EXCEPTION_EXECUTE_HANDLER;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace {

// Raw-pointer inner open so the SEH frame below holds no object with a destructor.
Source* OpenByKind(const std::wstring* path, Kind kind, OpenError* error) {
    std::unique_ptr<Source> source;
    switch (kind) {
        case Kind::Wav:
        case Kind::Flac:
        case Kind::Mp3: source = OpenMiniaudioSource(*path, kind, error); break;
        case Kind::Vorbis: source = OpenVorbisSource(*path, error); break;
        case Kind::Opus: source = OpenOpusSource(*path, error); break;
        case Kind::MediaFoundation: source = OpenMediaFoundationSource(*path, error); break;
        default: *error = OpenError::Unsupported; break;
    }
    return source.release();
}

Source* GuardedOpen(const std::wstring* path, Kind kind, OpenError* error, bool* faulted) {
    *faulted = false;
    __try {
        return OpenByKind(path, kind, error);
    } __except (Containable(GetExceptionCode())) {
        *faulted = true;
        *error = OpenError::Damaged;
        return nullptr;
    }
}

}  // namespace

std::unique_ptr<Source> OpenSource(const std::wstring& path, OpenError* error) {
    const Kind kind = SniffFile(path, error);
    if (kind == Kind::Unknown) return nullptr;
    bool faulted = false;
    std::unique_ptr<Source> source(GuardedOpen(&path, kind, error, &faulted));
    if (source) {
        *error = OpenError::None;
        return source;
    }
    if (!faulted && *error == OpenError::Damaged) {
        // The decoder refused it. Re-check that the file is still there, in case it vanished
        // between the sniff and the open.
        OpenError again = OpenError::None;
        SniffFile(path, &again);
        if (again == OpenError::Missing || again == OpenError::AccessDenied) *error = again;
    }
    return nullptr;
}

const wchar_t* DescribeProblem(OpenError error) {
    switch (error) {
        case OpenError::None: return L"";
        case OpenError::Missing: return L"file not found";
        case OpenError::AccessDenied: return L"the file could not be opened: another program is using it, or Windows does not allow reading it";
        case OpenError::OnlineOnly: return L"the file is only stored online; open its folder in File Explorer and choose \"Always keep on this device\"";
        case OpenError::Unsupported: return L"not a music file the player can read (it plays MP3, FLAC, WAV, OGG, Opus, M4A and AAC)";
        case OpenError::Damaged: return L"the file is damaged or is not what its name says";
        case OpenError::NoAacDecoder: return L"this edition of Windows cannot play M4A or AAC files (add the Media Feature Pack in Windows Settings, under Apps, Optional features)";
        case OpenError::Empty: return L"the file is empty";
    }
    return L"";
}

bool GuardedRead(Source* source, float* out, uint32_t frames, uint32_t* got, bool* failed) {
    *got = 0;
    *failed = false;
    __try {
        *got = source->Read(out, frames, failed);
        return true;
    } __except (Containable(GetExceptionCode())) {
        *got = 0;
        *failed = true;
        return false;
    }
}

bool GuardedSeek(Source* source, int64_t frame, bool* ok) {
    *ok = false;
    __try {
        *ok = source->Seek(frame);
        return true;
    } __except (Containable(GetExceptionCode())) {
        return false;
    }
}

void AbandonSource(std::unique_ptr<Source>& source) {
    // Deliberate leak: the decoder faulted, so its destructor cannot be trusted to run.
    (void)source.release();
}

}  // namespace music

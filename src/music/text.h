#pragma once

// Text helpers for the music engine. Paths are UTF-16 (std::wstring) everywhere inside the
// engine; UTF-8 appears only at two edges: text written to disk and text handed to the log.

#include <string>
#include <string_view>

namespace music {

// UTF-16 -> UTF-8. An unpaired surrogate (legal in an NTFS name, not representable in UTF-8)
// makes this return an empty string and set *ok to false when `ok` is given.
std::string ToUtf8(std::wstring_view text, bool* ok = nullptr);

// UTF-8 -> UTF-16. Invalid UTF-8 returns an empty string.
std::wstring FromUtf8(std::string_view text);

// Turns the bytes of a small text file (settings.ini) into UTF-8: a UTF-8 byte order mark is
// dropped, UTF-16 with a byte order mark in either byte order is converted. These are the
// encodings Windows Notepad offers, and the loader's own settings reader accepts all of them.
std::string TextFileToUtf8(const std::string& bytes);

// The form a path is stored and shown in: absolute, backslashes, "." and ".." resolved, no \\?\
// prefix. Empty in, empty out.
std::wstring NormalisePath(const std::wstring& path);

// Adds the \\?\ prefix (or \\?\UNC\ for \\server\share) to a normalised absolute path so that
// files beyond 260 characters open. The stored and displayed path never carries the prefix; only
// the path handed to CreateFileW, _wfopen, miniaudio or Media Foundation does. The prefix turns
// off Windows' own clean-up of the path, which is why the path is normalised first.
std::wstring ToExtendedPath(const std::wstring& path);

// Ordinal, case-insensitive comparison of two full paths (the duplicate check).
bool SamePath(const std::wstring& a, const std::wstring& b);

// A key for hashing paths so that SamePath-equal paths (almost always) share it: upper case.
std::wstring PathKey(const std::wstring& path);

// "2" before "10": natural order, case-insensitive, for files found in one folder.
bool NaturalLess(const std::wstring& a, const std::wstring& b);

// The file name without its folder, and without its extension when `withoutExtension` is set.
std::wstring FileName(const std::wstring& path, bool withoutExtension);

// Lower-case extension including the dot (L".mp3"), or empty.
std::wstring Extension(const std::wstring& path);

// Replaces tab, CR, LF and other control characters with a space (for the one-line-per-record file).
std::wstring SingleLine(std::wstring text);

}  // namespace music

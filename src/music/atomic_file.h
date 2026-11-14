#pragma once

// Crash-safe small-file persistence. The game process can disappear at any moment, so a save
// never writes into the live file: it writes <name>.<pid>.tmp, flushes it, then swaps it in with
// ReplaceFileW (which leaves the previous version as <name>.bak). A reader that finds the main
// file unreadable tries the backup.

#include <string>

namespace music {

// Writes `content` to `path` as described above. Returns false (and the Windows error) on failure;
// the previous file is then untouched.
bool SaveFileAtomically(const std::wstring& path, const std::string& content, unsigned long* error);

// Reads the whole file (up to `maxBytes`) into `out`. Returns false if it does not exist or
// cannot be read.
bool ReadWholeFile(const std::wstring& path, std::string* out, size_t maxBytes);

// Deletes <folder>\<prefix>*.tmp leftovers from saves that never completed.
void DeleteLeftoverTempFiles(const std::wstring& folder, const std::wstring& prefix);

}  // namespace music

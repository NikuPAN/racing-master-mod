#pragma once

// Crash-safe small-file persistence. The game process can disappear at any moment, so a save
// never writes into the live file: it writes <name>.<pid>.tmp and flushes it, then swaps it in.
//
// SaveFileAtomically (playlist.tsv) swaps with ReplaceFileW, which leaves the previous version as
// <name>.bak. ReplaceFileW is two renames (old file to .bak, new file into place): a process that
// ends between them leaves no <name>, only the .bak, so its reader must fall back to the backup
// (the playlist loader does).
//
// SaveFileReplacing (settings.ini) swaps with a single MoveFileExW rename, so the name always holds
// either the old file or the new one. Nothing that reads settings.ini looks at a backup (the loader,
// the panel and the engine each read it), and 0.3.1 saves it every 10 s while a song plays and at
// the moment the game is asked to close, so it must never be missing. Musictest case 115 ends a
// saving process 100 times at random moments: ReplaceFileW left settings.ini missing after 9 of 100.

#include <string>

namespace music {

// Writes `content` to `path` as described above. Returns false (and the Windows error) on failure;
// the previous file is then untouched.
bool SaveFileAtomically(const std::wstring& path, const std::string& content, unsigned long* error);

// The same temporary file, swapped in with one rename (MoveFileExW, MOVEFILE_REPLACE_EXISTING |
// MOVEFILE_WRITE_THROUGH) and no backup. For settings.ini.
bool SaveFileReplacing(const std::wstring& path, const std::string& content, unsigned long* error);

// Reads the whole file (up to `maxBytes`) into `out`. Returns false if it does not exist or
// cannot be read.
bool ReadWholeFile(const std::wstring& path, std::string* out, size_t maxBytes);

// Deletes <folder>\<prefix>*.tmp leftovers from saves that never completed.
void DeleteLeftoverTempFiles(const std::wstring& folder, const std::wstring& prefix);

}  // namespace music

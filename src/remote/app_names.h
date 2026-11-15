#pragma once

// The name the panel shows for another app, from the id Windows gives its media session.
//
// Windows' ids are not names: Chrome's is "Chrome" or, per profile, "Chrome.UserData.Profile1";
// Firefox's is a hash of its install folder ("308046B0AF4A39CB"); Store apps use their package
// ("SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify"); other programs often just their file name
// ("Spotify.exe"). The owner's game run of 2026-11-15 showed "Chrome.UserData.Profile1: <title>"
// in the panel; the profile must never show, and a raw id never.
//
// Order: the developer test's own players keep their id (the test host only; the game never lists
// them), then a small table of common players and browsers, then Windows' registered name for the
// id (Start menu), then the program file's own description, then the id tidied. An id with nothing
// readable in it (a bare hash) gives an empty name; the panel then shows the song alone.
//
// Pure functions apart from ProgramDescription and AppPathOf, which read the registry and a file
// on disk (never another process). Kernel32 only: anything else is resolved at run time, so
// version.dll keeps its KERNEL32-only import table. Any thread.

#include <string>

namespace remote {

// The table step (and the test players): true with *name set when the id is one of them.
bool TableAppName(const std::wstring& id, std::wstring* name);

// After the table: `registered` is Windows' display name for the id (empty when it has none),
// `description` the program file's FileDescription or ProductName (empty when unknown).
std::wstring ChooseAppName(const std::wstring& id, const std::wstring& registered, const std::wstring& description);

// The id made readable without any lookup: "Chrome.UserData.Profile1" -> "Chrome",
// "C:\\Apps\\musicbee.exe" -> "Musicbee", "Vendor.Player_8wekyb3d8bbwe!App" -> "Player".
// Empty when nothing readable is left (a hash).
std::wstring TidyAppId(const std::wstring& id);

// The full path of a program file the id names: the id itself when it is a full path to an .exe,
// or the App Paths registry entry of a bare "name.exe" (current user, then the machine). Empty
// when neither.
std::wstring AppPathOf(const std::wstring& id);

// FileDescription (else ProductName) from the version resource of the file at `path`; empty when
// it has none. Reads the file through the real Windows version.dll in System32.
std::wstring ProgramDescription(const std::wstring& path);

}  // namespace remote

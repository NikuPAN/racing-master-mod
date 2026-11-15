// The names the panel shows for other apps (src\remote\app_names.*). No media session is read:
// these are the ids Windows gives real players, typed in, plus the two lookups on this PC's files.
#include "musictest.h"

#include "remote/app_names.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

// The whole chain without Windows' lookups: the table, else the id tidied.
std::wstring NameWithoutLookups(const std::wstring& id) {
    std::wstring name;
    if (remote::TableAppName(id, &name)) return name;
    return remote::ChooseAppName(id, L"", L"");
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

void Expect(const wchar_t* id, const wchar_t* expected) {
    const std::wstring got = NameWithoutLookups(id);
    char name[256];
    snprintf(name, sizeof(name), "app names: %s is shown as \"%s\"", Narrow(id).c_str(), Narrow(expected).c_str());
    mt::Check(got == expected, name, "got \"%s\"", Narrow(got).c_str());
}

}  // namespace

MT_TEST(800, AppNames, "app names: common players, browsers and their profiles, never a raw id") {
    // The owner's game run of 2026-11-15: Chrome with a profile.
    Expect(L"Chrome.UserData.Profile1", L"Chrome");
    Expect(L"Chrome", L"Chrome");
    Expect(L"MSEdge", L"Edge");
    Expect(L"MSEdge.UserData.Default", L"Edge");
    Expect(L"Spotify.exe", L"Spotify");
    Expect(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"Spotify");
    Expect(L"308046B0AF4A39CB", L"Firefox");
    Expect(L"Microsoft.ZuneMusic_8wekyb3d8bbwe!Microsoft.ZuneMusic", L"Media Player");
    Expect(L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!App", L"Apple Music");
    Expect(L"C:\\Program Files\\VideoLAN\\VLC\\vlc.exe", L"VLC");
    Expect(L"foobar2000.exe", L"foobar2000");
    Expect(L"wmplayer.exe", L"Windows Media Player");
    Expect(L"com.github.th-ch.youtube-music", L"YouTube Music");
    // Not in the table: tidied, never the raw id.
    Expect(L"Brave.UserData.Profile3", L"Brave");
    Expect(L"Vendor.SomePlayer_1a2b3c4d5e6f!App", L"SomePlayer");
    Expect(L"Vendor.SomePlayer_1a2b3c4d5e6f!Player", L"Player");
    Expect(L"D:\\Tools\\musicbee.exe", L"Musicbee");
    Expect(L"com.example.night-radio", L"Night radio");
    // A hash with nothing readable in it: no name (the panel shows the song alone).
    Expect(L"0123456789ABCDEF", L"");
    // The test host's own players keep their id (the game never lists them: remote.cpp).
    Expect(L"RacingMasterMod.TestHost.FakePlayer.0123456789ab", L"RacingMasterMod.TestHost.FakePlayer.0123456789ab");

    // Windows' registered name wins over the tidied id, but never for a browser profile (its
    // registered name is the profile's shortcut) and never when it is only the id again.
    mt::Check(remote::ChooseAppName(L"Vendor.Player_abc!App", L"Night Radio", L"") == L"Night Radio",
              "app names: Windows' registered name is used for an app not in the table");
    mt::Check(remote::ChooseAppName(L"Brave.UserData.Profile3", L"Person 3 - Brave", L"") == L"Brave",
              "app names: a browser profile's registered name (the profile) is never shown");
    mt::Check(remote::ChooseAppName(L"Chrome._crx_abcdefghijklmnop", L"Sunset Radio", L"") == L"Sunset Radio",
              "app names: a web app installed from a browser shows its own registered name");
    mt::Check(remote::ChooseAppName(L"Chrome._crx_abcdefghijklmnop", L"", L"") == L"Chrome",
              "app names: ... and the browser's name when it has none");
    mt::Check(remote::ChooseAppName(L"musicbee.exe", L"musicbee.exe", L"MusicBee") == L"MusicBee",
              "app names: the program's own description when Windows only repeats the id");
    std::wstring table;
    mt::Check(remote::TableAppName(L"Chrome.UserData.Profile1", &table) && table == L"Chrome",
              "app names: Chrome is named from the table, before any lookup could return a profile");
}

MT_TEST(801, AppNameLookups, "app names: a program's own description, read from its file") {
    wchar_t windows[MAX_PATH] = L"";
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring explorer = std::wstring(windows) + L"\\explorer.exe";
    mt::Check(remote::AppPathOf(explorer) == explorer, "app names: a full path to an existing program is used as it is");
    const std::wstring description = remote::ProgramDescription(explorer);
    mt::Check(!description.empty() && description.find(L'.') == std::wstring::npos,
              "app names: Windows Explorer's file describes itself (version resource read through System32's version.dll)",
              "got \"%s\"", Narrow(description).c_str());
    mt::Check(remote::AppPathOf(L"C:\\No such folder\\none.exe").empty() && remote::AppPathOf(L"Chrome").empty(),
              "app names: no path for a missing program or an id that is not a program");
    // App Paths: Windows lists PowerShell there on every edition this was tried on; skipped otherwise.
    const std::wstring powershell = remote::AppPathOf(L"PowerShell.exe");
    if (powershell.empty()) {
        mt::Skip("app names: a bare program name is found through Windows' App Paths",
                 "this PC has no App Paths entry for PowerShell.exe");
    } else {
        mt::Check(GetFileAttributesW(powershell.c_str()) != INVALID_FILE_ATTRIBUTES,
                  "app names: a bare program name is found through Windows' App Paths");
    }
}

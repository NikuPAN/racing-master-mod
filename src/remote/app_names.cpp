// Friendly names for other apps' media sessions. See app_names.h.

#include "app_names.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <vector>

namespace remote {
namespace {

// How a table entry matches an id (case is ignored everywhere).
enum class Match {
    Family,    // the id is the key, or the key followed by '.' and more ("Chrome.UserData.Profile1")
    Prefix,    // the id starts with the key (a Store package family: "SpotifyAB.SpotifyMusic_...")
    Program,   // the id's file name is the key ("Spotify.exe", also inside a full path)
};

struct Entry {
    const wchar_t* key;
    Match match;
    const wchar_t* name;
};

// Common players and browsers whose ids say little or would show something the player does not
// want (a browser profile). Constant data: no start-up constructor (build gate 3).
constexpr Entry kTable[] = {
    {L"Chrome", Match::Family, L"Chrome"},
    {L"chrome.exe", Match::Program, L"Chrome"},
    {L"MSEdge", Match::Family, L"Edge"},
    {L"Microsoft.MicrosoftEdge", Match::Prefix, L"Edge"},
    {L"msedge.exe", Match::Program, L"Edge"},
    // Firefox names its sessions after a hash of its install folder; these two are the default
    // 64-bit and 32-bit folders. Another folder falls through to Windows' registered name.
    {L"308046B0AF4A39CB", Match::Family, L"Firefox"},
    {L"E7CF176E110C211B", Match::Family, L"Firefox"},
    {L"Firefox", Match::Family, L"Firefox"},
    {L"Mozilla.Firefox", Match::Prefix, L"Firefox"},
    {L"firefox.exe", Match::Program, L"Firefox"},
    {L"Spotify", Match::Family, L"Spotify"},
    {L"SpotifyAB.SpotifyMusic_", Match::Prefix, L"Spotify"},
    {L"Spotify.exe", Match::Program, L"Spotify"},
    {L"AppleInc.AppleMusicWin_", Match::Prefix, L"Apple Music"},
    {L"AppleMusic.exe", Match::Program, L"Apple Music"},
    {L"VLC", Match::Family, L"VLC"},
    {L"VideoLAN.VLC", Match::Prefix, L"VLC"},
    {L"vlc.exe", Match::Program, L"VLC"},
    {L"foobar2000", Match::Family, L"foobar2000"},
    {L"foobar2000.exe", Match::Program, L"foobar2000"},
    // Windows 11's Media Player is the package once called Groove Music; wmplayer.exe is the old one.
    {L"Microsoft.ZuneMusic_", Match::Prefix, L"Media Player"},
    {L"wmplayer.exe", Match::Program, L"Windows Media Player"},
    {L"com.github.th-ch.youtube-music", Match::Family, L"YouTube Music"},
    {L"YouTube Music.exe", Match::Program, L"YouTube Music"},
};

constexpr wchar_t kTestPlayerPrefix[] = L"RacingMasterMod.TestHost.";

bool StartsWithI(const std::wstring& text, const wchar_t* prefix) {
    const size_t n = static_cast<size_t>(lstrlenW(prefix));
    return text.size() >= n && CompareStringOrdinal(text.c_str(), static_cast<int>(n), prefix, static_cast<int>(n), TRUE) == CSTR_EQUAL;
}

bool EqualsI(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool ContainsI(const std::wstring& text, const wchar_t* part) {
    const size_t n = static_cast<size_t>(lstrlenW(part));
    for (size_t i = 0; i + n <= text.size(); ++i) {
        if (CompareStringOrdinal(text.c_str() + i, static_cast<int>(n), part, static_cast<int>(n), TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool EndsWithExe(const std::wstring& text) {
    return text.size() > 4 && EqualsI(text.substr(text.size() - 4), L".exe");
}

bool IsHexOrDigits(const std::wstring& text) {
    for (wchar_t c : text) {
        const bool hex = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        if (!hex) return false;
    }
    return true;
}

// A browser profile ("Chrome.UserData.Profile1"): the profile is never shown, and Windows'
// registered name for such an id is the profile's shortcut ("Person 1 - Chrome").
bool IsBrowserProfile(const std::wstring& id) {
    return ContainsI(id, L".UserData");
}

// A web app installed from a browser ("Chrome._crx_<extension id>"): Windows' registered name is
// the web app's own ("YouTube Music"), so it is asked before the browser's name.
bool IsWebApp(const std::wstring& id) {
    return ContainsI(id, L"_crx_");
}

// True when `name` reads as a name rather than another id: something, not the id itself, no
// package or path marks, and not a single dotted or underscored word.
bool Usable(const std::wstring& name, const std::wstring& id) {
    if (name.empty() || EqualsI(name, id) || name.find_first_of(L"!\\/") != std::wstring::npos) {
        return false;
    }
    const bool hasSpace = name.find(L' ') != std::wstring::npos;
    if (!hasSpace && (name.find(L'.') != std::wstring::npos || name.find(L'_') != std::wstring::npos)) {
        return false;
    }
    if (IsHexOrDigits(name)) {
        return false;
    }
    return true;
}

}  // namespace

bool TableAppName(const std::wstring& id, std::wstring* name) {
    if (StartsWithI(id, kTestPlayerPrefix)) {
        *name = id;   // the test host's own players: the tests read their id back
        return true;
    }
    if (IsWebApp(id)) {
        return false;
    }
    const std::wstring file = FileNameOf(id);
    for (const Entry& entry : kTable) {
        bool hit = false;
        switch (entry.match) {
            case Match::Family: {
                const size_t n = static_cast<size_t>(lstrlenW(entry.key));
                hit = StartsWithI(id, entry.key) && (id.size() == n || id[n] == L'.');
                break;
            }
            case Match::Prefix: hit = StartsWithI(id, entry.key); break;
            case Match::Program: hit = EqualsI(file, entry.key); break;
        }
        if (hit) {
            *name = entry.name;
            return true;
        }
    }
    return false;
}

std::wstring TidyAppId(const std::wstring& id) {
    std::wstring base;
    const size_t bang = id.find(L'!');
    if (bang != std::wstring::npos) {
        // A Store app: "Publisher.Product_hash!AppId". The app's own id when it is a word,
        // otherwise (often just "App") the product part of the package.
        const std::wstring app = id.substr(bang + 1);
        std::wstring package = id.substr(0, bang);
        const size_t underscore = package.find(L'_');
        if (underscore != std::wstring::npos) package.resize(underscore);
        const size_t dot = package.find(L'.');
        const std::wstring product = dot == std::wstring::npos ? package : package.substr(dot + 1);
        bool generic = app.empty() || (app.size() >= 3 && EqualsI(app.substr(0, 3), L"App"));
        for (size_t i = 3; generic && i < app.size(); ++i) {
            generic = app[i] >= L'0' && app[i] <= L'9';
        }
        base = generic ? product : app;
        const size_t last = base.find_last_of(L'.');
        if (last != std::wstring::npos && last + 1 < base.size()) base = base.substr(last + 1);
    } else {
        base = FileNameOf(id);
        if (EndsWithExe(base)) base.resize(base.size() - 4);
        if (base.find(L' ') == std::wstring::npos && base.find(L'.') != std::wstring::npos) {
            // A dotted id. Reverse-domain ids ("com.vendor.player") end in the name; others
            // ("Chrome.UserData.Profile1", "Vendor.Player") start with it.
            const size_t first = base.find(L'.');
            const std::wstring head = base.substr(0, first);
            static const wchar_t* const kDomains[] = {L"com", L"org", L"net", L"io", L"app", L"dev", L"me", L"co"};
            bool domain = false;
            for (const wchar_t* d : kDomains) domain = domain || EqualsI(head, d);
            if (domain) {
                const size_t last = base.find_last_of(L'.');
                base = base.substr(last + 1);
            } else {
                base = head;
            }
        }
    }
    for (wchar_t& c : base) {
        if (c == L'-' || c == L'_') c = L' ';
    }
    while (!base.empty() && base.back() == L' ') base.pop_back();
    while (!base.empty() && base.front() == L' ') base.erase(0, 1);
    if (base.empty() || (base.size() >= 8 && IsHexOrDigits(base))) {
        return std::wstring();   // nothing readable (a hash)
    }
    if (base[0] >= L'a' && base[0] <= L'z') {
        base[0] = static_cast<wchar_t>(base[0] - (L'a' - L'A'));
    }
    return base;
}

std::wstring ChooseAppName(const std::wstring& id, const std::wstring& registered, const std::wstring& description) {
    if (IsBrowserProfile(id)) {
        return TidyAppId(id);
    }
    if (Usable(registered, id)) {
        return registered;
    }
    if (Usable(description, id)) {
        return description;
    }
    return TidyAppId(id);
}

std::wstring AppPathOf(const std::wstring& id) {
    if (!EndsWithExe(id)) {
        return std::wstring();
    }
    const bool fullPath = (id.size() > 3 && id[1] == L':' && (id[2] == L'\\' || id[2] == L'/')) ||
                          (id.size() > 2 && id[0] == L'\\' && id[1] == L'\\');
    if (fullPath) {
        const DWORD attributes = GetFileAttributesW(id.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? id : std::wstring();
    }
    if (id.find_first_of(L"\\/") != std::wstring::npos) {
        return std::wstring();
    }
    // App Paths: where Windows itself looks up a program started by its bare file name.
    using RegGetValueFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    const HMODULE advapi32 = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto regGetValue =
        advapi32 ? reinterpret_cast<RegGetValueFn>(reinterpret_cast<void*>(GetProcAddress(advapi32, "RegGetValueW"))) : nullptr;
    if (!regGetValue) {
        return std::wstring();
    }
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + id;
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        wchar_t value[MAX_PATH * 2] = L"";
        DWORD bytes = sizeof(value);
        if (regGetValue(root, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value, &bytes) ==
            ERROR_SUCCESS) {
            std::wstring path = value;
            if (path.size() >= 2 && path.front() == L'"') {
                const size_t close = path.find(L'"', 1);
                path = path.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
            }
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (!path.empty() && attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                return path;
            }
        }
    }
    return std::wstring();
}

std::wstring ProgramDescription(const std::wstring& path) {
    if (path.empty()) {
        return std::wstring();
    }
    // The real version.dll from System32 by its full path: inside the game "version.dll" is the
    // mod itself, which forwards to this one anyway.
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length > MAX_PATH - 16) {
        return std::wstring();
    }
    lstrcatW(system, L"\\version.dll");
    const HMODULE version = LoadLibraryExW(system, nullptr, 0);
    if (!version) {
        return std::wstring();
    }
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto size = reinterpret_cast<SizeFn>(reinterpret_cast<void*>(GetProcAddress(version, "GetFileVersionInfoSizeW")));
    const auto info = reinterpret_cast<InfoFn>(reinterpret_cast<void*>(GetProcAddress(version, "GetFileVersionInfoW")));
    const auto query = reinterpret_cast<QueryFn>(reinterpret_cast<void*>(GetProcAddress(version, "VerQueryValueW")));
    std::wstring result;
    DWORD ignored = 0;
    const DWORD bytes = size && info && query ? size(path.c_str(), &ignored) : 0;
    if (bytes > 0 && bytes < 4 * 1024 * 1024) {
        std::vector<BYTE> block(bytes);
        if (info(path.c_str(), 0, bytes, block.data())) {
            struct Translation {
                WORD language;
                WORD codePage;
            };
            Translation* translations = nullptr;
            UINT translationBytes = 0;
            std::vector<Translation> tried;
            if (query(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationBytes) &&
                translations) {
                for (UINT i = 0; i < translationBytes / sizeof(Translation) && i < 8; ++i) tried.push_back(translations[i]);
            }
            tried.push_back({0x0409, 0x04B0});   // US English, Unicode: the most common block
            for (const wchar_t* field : {L"FileDescription", L"ProductName"}) {
                for (const Translation& t : tried) {
                    wchar_t name[96];
                    swprintf_s(name, L"\\StringFileInfo\\%04x%04x\\", t.language, t.codePage);
                    wcscat_s(name, field);
                    wchar_t* text = nullptr;
                    UINT chars = 0;
                    if (query(block.data(), name, reinterpret_cast<void**>(&text), &chars) && text && chars > 1) {
                        result.assign(text, lstrlenW(text));
                        while (!result.empty() && result.back() == L' ') result.pop_back();
                        if (!result.empty()) break;
                    }
                }
                if (!result.empty()) break;
            }
        }
    }
    FreeLibrary(version);
    return result;
}

}  // namespace remote

// Stands in for the other programs in the game folder (the crash reporter and helpers) that
// import version.dll but are not the game. In such a program the stand-in must only forward:
// no extra DLL loaded, no thread, no log file.
//
// This program links kernel32 and version only, so any user32, gdi32, d3d11 or dxgi found in
// the process was brought in by the stand-in. Exit code 0 = every check passed.

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "version.lib")

namespace {

const char* const kExportNames[17] = {
    "GetFileVersionInfoA",      "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",    "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",   "GetFileVersionInfoW",
    "VerFindFileA",             "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",          "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",           "VerQueryValueW",
};

bool g_allPassed = true;

void Check(const char* name, bool ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) g_allPassed = false;
}

// Each stand-in export is a 6-byte "jmp [address]" stub; this follows it to the function it
// jumps to.
void* StubTarget(HMODULE standIn, const char* name) {
    const BYTE* stub = reinterpret_cast<const BYTE*>(GetProcAddress(standIn, name));
    if (!stub || stub[0] != 0xFF || stub[1] != 0x25) {
        return nullptr;
    }
    INT32 offset = 0;
    memcpy(&offset, stub + 2, 4);
    void* const* slot = reinterpret_cast<void* const*>(stub + 6 + offset);
    return *slot;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);

    // Use version.dll once so the import is real and the stand-in is loaded.
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    wchar_t kernel[MAX_PATH];
    lstrcpyW(kernel, system);
    lstrcatW(kernel, L"\\kernel32.dll");
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(kernel, &handle);
    Check("forwarding: GetFileVersionInfoSizeW returns data", size != 0);

    // Give a wrongly woken mod time to start and load its libraries before looking.
    Sleep(1500);

    const wchar_t* const extraModules[] = {L"user32.dll", L"gdi32.dll", L"win32u.dll", L"d3d11.dll", L"dxgi.dll"};
    for (const wchar_t* module : extraModules) {
        char name[64];
        snprintf(name, sizeof(name), "no %ls loaded into a bystander", module);
        Check(name, GetModuleHandleW(module) == nullptr);
    }

    const HMODULE standIn = GetModuleHandleW(L"version.dll");
    wchar_t loaded[MAX_PATH] = L"";
    wchar_t expected[MAX_PATH] = L"";
    GetModuleFileNameW(standIn, loaded, MAX_PATH);
    GetModuleFileNameW(nullptr, expected, MAX_PATH);
    wchar_t* slash = wcsrchr(expected, L'\\');
    if (slash) lstrcpyW(slash + 1, L"version.dll");
    char loaded8[MAX_PATH * 3];
    if (WideCharToMultiByte(CP_UTF8, 0, loaded, -1, loaded8, sizeof(loaded8), nullptr, nullptr) <= 0) loaded8[0] = '\0';
    printf("version.dll in use: %s\n", loaded8);
    Check("stand-in in use", lstrcmpiW(loaded, expected) == 0);

    wchar_t realPath[MAX_PATH];
    lstrcpyW(realPath, system);
    lstrcatW(realPath, L"\\version.dll");
    const HMODULE real = LoadLibraryW(realPath);
    int matching = 0;
    if (real) {
        for (const char* name : kExportNames) {
            const void* target = StubTarget(standIn, name);
            const void* genuine = reinterpret_cast<const void*>(GetProcAddress(real, name));
            if (target && genuine && target == genuine) {
                ++matching;
            } else {
                printf("  %s: stub target %p, Windows function %p\n", name, target, genuine);
            }
        }
    }
    printf("stub targets equal to the Windows functions: %d of 17\n", matching);
    Check("17 of 17 forwards point at the Windows functions", matching == 17);

    printf("result: %s\n", g_allPassed ? "PASS" : "FAIL");
    return g_allPassed ? 0 : 1;
}

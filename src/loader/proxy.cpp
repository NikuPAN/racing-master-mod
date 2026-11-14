// Loads the real Windows version.dll and fills the table the assembly stubs jump through.

#include "proxy.h"

#include <windows.h>

extern "C" void* g_realVersionFn[17] = {};

namespace {

// Order must match the indices in proxy_exports.asm.
const char* const kExportNames[17] = {
    "GetFileVersionInfoA",
    "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA",
    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW",
    "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",
    "VerFindFileA",
    "VerFindFileW",
    "VerInstallFileA",
    "VerInstallFileW",
    "VerLanguageNameA",
    "VerLanguageNameW",
    "VerQueryValueA",
    "VerQueryValueW",
};

}  // namespace

bool LoadRealVersionDll() {
    wchar_t path[MAX_PATH];
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 16) {
        return false;
    }
    lstrcatW(path, L"\\version.dll");

    // Full path, so Windows loads the genuine DLL from System32 and not this file again.
    const HMODULE real = LoadLibraryW(path);
    if (!real) {
        return false;
    }
    for (int i = 0; i < 17; ++i) {
        g_realVersionFn[i] = reinterpret_cast<void*>(GetProcAddress(real, kExportNames[i]));
        if (!g_realVersionFn[i]) {
            return false;
        }
    }
    return true;
}

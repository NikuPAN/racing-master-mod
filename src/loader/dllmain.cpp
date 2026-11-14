// Entry point of the stand-in version.dll.
//
// In every process it only forwards the 17 version.dll functions to Windows. The mod itself
// starts in exactly one case: the process is the game (g112-Win64-Shipping.exe), or it is the
// developer test host (testhost.exe) AND RACING_MASTER_MOD_TEST_HOST=1 is set.
//
// DllMain runs under the Windows loader lock: it decides and starts the worker thread, nothing
// else. Loading the real version.dll here is the one exception (every stand-in does it; the
// game's first version query must find the table filled).

#include "proxy.h"

#include "../core/worker.h"

#include <windows.h>

#include <cwchar>

namespace {

constexpr wchar_t kGameExe[] = L"g112-Win64-Shipping.exe";
constexpr wchar_t kTestHostExe[] = L"testhost.exe";

bool ExeNameIs(const wchar_t* expected) {
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    const wchar_t* name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return lstrcmpiW(name, expected) == 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    if (!LoadRealVersionDll()) {
        return FALSE;
    }

    wchar_t flag[8];
    const bool testHost = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_HOST", flag, 8) == 1 && flag[0] == L'1' &&
                          ExeNameIs(kTestHostExe);
    if (testHost || ExeNameIs(kGameExe)) {
        StartModThread(instance, testHost);
    }
    return TRUE;
}

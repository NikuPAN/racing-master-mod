#include "picker.h"

#include "../core/log.h"

#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <atomic>
#include <new>

namespace music {

namespace {

// Remembers its own last folder in the dialog's per-application settings.
const GUID kMusicDialogGuid = {0x7c3f2a51, 0x2e6b, 0x4a0d, {0x9b, 0x8e, 0x3d, 0x5a, 0x1f, 0x60, 0x72, 0x11}};

std::atomic<bool> g_open{false};
std::atomic<bool> g_suppress{false};

struct Request {
    HWND owner;
    bool folder;
    void (*filesDone)(const std::vector<std::wstring>&);
    void (*folderDone)(const std::wstring&);
};

bool Suppressed() {
    if (g_suppress.load()) return true;
    wchar_t flag[8];
    return GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_HOST", flag, 8) == 1 && flag[0] == L'1';
}

DWORD WINAPI DialogThread(LPVOID param) {
    Request* request = static_cast<Request*>(param);
    std::vector<std::wstring> paths;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comReady = SUCCEEDED(hr);
    IFileOpenDialog* dialog = nullptr;
    if (comReady) hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr)) {
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        // FOS_NOCHANGEDIR matters in a game: the process's working directory must not move.
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT;
        options |= request->folder ? FOS_PICKFOLDERS : (FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
        dialog->SetOptions(options);
        if (!request->folder) {
            const COMDLG_FILTERSPEC types[2] = {{L"Music", L"*.mp3;*.flac;*.wav;*.ogg;*.oga;*.opus;*.m4a;*.aac"},
                                                {L"All files", L"*.*"}};
            dialog->SetFileTypes(2, types);
        }
        dialog->SetClientGuid(kMusicDialogGuid);
        IShellItem* musicFolder = nullptr;
        if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_Music, 0, nullptr, IID_PPV_ARGS(&musicFolder)))) {
            dialog->SetDefaultFolder(musicFolder);
            musicFolder->Release();
        }
        dialog->SetTitle(request->folder ? L"Add a music folder" : L"Add music files");

        if (Suppressed()) {
            Log("music: file dialog suppressed (test mode); reporting cancel");
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        } else {
            hr = dialog->Show(request->owner);
        }
        if (SUCCEEDED(hr)) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dialog->GetResults(&items))) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    if (FAILED(items->GetItemAt(i, &item))) continue;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        paths.push_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
                items->Release();
            }
        } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            Log("music: the file dialog could not be shown (0x%08lx)", static_cast<unsigned long>(hr));
        }
        dialog->Release();
    } else {
        Log("music: the file dialog could not be created (0x%08lx)", static_cast<unsigned long>(hr));
    }
    if (comReady) CoUninitialize();

    g_open.store(false);
    if (request->folder) {
        request->folderDone(paths.empty() ? std::wstring() : paths.front());
    } else {
        request->filesDone(paths);
    }
    delete request;
    return 0;
}

bool Launch(Request* request) {
    if (!request) return false;
    bool expected = false;
    if (!g_open.compare_exchange_strong(expected, true)) {
        delete request;
        return false;  // one dialog at a time
    }
    const HANDLE thread = CreateThread(nullptr, 0, DialogThread, request, 0, nullptr);
    if (!thread) {
        g_open.store(false);
        delete request;
        return false;
    }
    CloseHandle(thread);
    return true;
}

}  // namespace

void OpenAddFilesDialog(HWND owner, void (*done)(const std::vector<std::wstring>&)) {
    if (!done) return;
    if (!Launch(new (std::nothrow) Request{owner, false, done, nullptr})) Log("music: a file dialog is already open; the second request was ignored");
}

void OpenAddFolderDialog(HWND owner, void (*done)(const std::wstring&)) {
    if (!done) return;
    if (!Launch(new (std::nothrow) Request{owner, true, nullptr, done})) Log("music: a file dialog is already open; the second request was ignored");
}

bool IsDialogOpen() {
    return g_open.load();
}

namespace testing {
void SuppressDialogs(bool suppress) {
    g_suppress.store(suppress);
}
}  // namespace testing

}  // namespace music

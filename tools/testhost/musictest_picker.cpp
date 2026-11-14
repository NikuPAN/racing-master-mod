// The "Add files" / "Add folder" pickers (src\music\picker.cpp). The dialog is created and
// configured on its own STA thread exactly as in the game, but never shown: in test-host mode the
// thread skips Show() and reports a cancel. What a person sees when it is shown (owner window,
// focus, z-order) can only be checked in the game.
#include "musictest.h"

#include "music/picker.h"

#include <windows.h>

#include <atomic>

namespace {

std::atomic<int> g_filesCalls{0};
std::atomic<int> g_folderCalls{0};
std::atomic<size_t> g_filesCount{99};
std::atomic<bool> g_folderEmpty{false};
std::atomic<DWORD> g_callbackThread{0};

void FilesDone(const std::vector<std::wstring>& paths) {
    g_filesCount = paths.size();
    g_callbackThread = GetCurrentThreadId();
    ++g_filesCalls;
}

void FolderDone(const std::wstring& folder) {
    g_folderEmpty = folder.empty();
    ++g_folderCalls;
}

bool WaitFor(const std::atomic<int>& counter, int value) {
    for (int i = 0; i < 500 && counter.load() < value; ++i) Sleep(10);
    return counter.load() >= value;
}

}  // namespace

MT_TEST(700, Picker, "file picker: own thread, one at a time, never shown in tests") {
    music::testing::SuppressDialogs(true);
    music::OpenAddFilesDialog(nullptr, FilesDone);
    const bool openNow = music::IsDialogOpen();
    music::OpenAddFolderDialog(nullptr, FolderDone);  // refused: one dialog at a time
    const bool filesDone = WaitFor(g_filesCalls, 1);
    mt::Check(filesDone && g_filesCount.load() == 0, "Add files: the dialog thread reports a cancel (empty list) without showing anything");
    mt::Check(g_callbackThread.load() != 0 && g_callbackThread.load() != GetCurrentThreadId(),
              "Add files: the answer comes from the dialog's own thread");
    Sleep(100);
    if (openNow) {
        mt::Check(g_folderCalls.load() == 0, "a second dialog while one is open is not started");
    } else {
        mt::Skip("a second dialog while one is open is not started", "the first dialog thread had already finished");
    }
    for (int i = 0; i < 100 && music::IsDialogOpen(); ++i) Sleep(10);
    music::OpenAddFolderDialog(nullptr, FolderDone);
    mt::Check(WaitFor(g_folderCalls, 1) && g_folderEmpty.load(), "Add folder: works once the first dialog has closed, reports a cancel");
    for (int i = 0; i < 100 && music::IsDialogOpen(); ++i) Sleep(10);
    mt::Check(!music::IsDialogOpen(), "no dialog is left open");
}

namespace {

std::vector<std::wstring> g_reported;
std::wstring g_reportedFolder;

void FilesReported(const std::vector<std::wstring>& paths) {
    g_reported = paths;
    ++g_filesCalls;
}

void FolderReported(const std::wstring& folder) {
    g_reportedFolder = folder;
    ++g_folderCalls;
}

}  // namespace

// The test host's way to "choose" files without a window: RACING_MASTER_MOD_TEST_PICKER_FILES.
MT_TEST(701, PickerTestPaths, "file picker: the test host can hand it paths, still without a window") {
    music::testing::SuppressDialogs(true);
    SetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_PICKER_FILES", L"C:\\Music\\one 測試.mp3|C:\\Music\\two.flac");
    const int files = g_filesCalls.load();
    music::OpenAddFilesDialog(nullptr, FilesReported);
    const bool filesDone = WaitFor(g_filesCalls, files + 1);
    mt::Check(filesDone && g_reported.size() == 2 && g_reported[0] == L"C:\\Music\\one 測試.mp3" &&
                  g_reported[1] == L"C:\\Music\\two.flac",
              "Add files reports the paths from RACING_MASTER_MOD_TEST_PICKER_FILES, in order");
    for (int i = 0; i < 100 && music::IsDialogOpen(); ++i) Sleep(10);
    const int folders = g_folderCalls.load();
    music::OpenAddFolderDialog(nullptr, FolderReported);
    mt::Check(WaitFor(g_folderCalls, folders + 1) && g_reportedFolder == L"C:\\Music\\one 測試.mp3",
              "Add folder reports the first of those paths");
    for (int i = 0; i < 100 && music::IsDialogOpen(); ++i) Sleep(10);
    SetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_PICKER_FILES", nullptr);
}

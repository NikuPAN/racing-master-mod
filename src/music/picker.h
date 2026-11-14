#pragma once

// The "Add files" and "Add folder" pickers: one IFileOpenDialog on its own short-lived STA thread.
//
// `owner` is the game's top-level window (GetAncestor(gameWindow, GA_ROOT)); an owned dialog
// stays above the game and gives focus back when it closes. While it is open Windows disables
// the owner, so the game takes no input; the panel says so before opening it. Only one dialog
// at a time: a second request while one is open does nothing (IsDialogOpen() and the snapshot's
// dialogOpen say so, so the panel can explain it).
//
// `done` is called on the dialog thread with the chosen paths (empty when cancelled). It should
// only post to the engine (Player::AddFiles / AddFolder do exactly that).
//
// Automated tests never show a dialog: in test-host mode (RACING_MASTER_MOD_TEST_HOST=1) and
// after testing::SuppressDialogs(true), the thread configures the dialog, skips Show and reports
// a cancel.

#include <string>
#include <vector>

#include <windows.h>

namespace music {

void OpenAddFilesDialog(HWND owner, void (*done)(const std::vector<std::wstring>&));
void OpenAddFolderDialog(HWND owner, void (*done)(const std::wstring&));
bool IsDialogOpen();  // any thread; cheap

namespace testing {
void SuppressDialogs(bool suppress);
}

}  // namespace music

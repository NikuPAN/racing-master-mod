#include "global_hotkeys.h"

#include "input_queue.h"   // SharedInput, Shared(), PostCommand

#include "../core/log.h"

#include <windows.h>

namespace global_hotkeys {
namespace {

// The hotkey id is the command index (Command / UiKey order).
void RunCommand(Command command) {
    if (command == Command::TogglePanel) {
        // Flipped here, like the window-procedure path, so it works even while frames are slow.
        SharedInput& shared = Shared();
        shared.panelOpen.store(!shared.panelOpen.load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
        PostCommand(command);   // drained by the frame thread, which runs the music command
    }
}

DWORD WINAPI HotkeyThread(LPVOID param) {
    const HotkeyTable* table = static_cast<const HotkeyTable*>(param);
    // Force this thread to own a message queue before RegisterHotKey posts WM_HOTKEY to it.
    MSG message;
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    static const char* const kActionNames[kCommandCount] = {"panel", "stop", "previous", "play_pause", "next", "loop", "shuffle"};
    int registered = 0;
    for (int i = 0; i < kCommandCount; ++i) {
        const Hotkey& key = table->keys[i];
        if (key.vk == 0) {
            continue;
        }
        const UINT mods = MOD_NOREPEAT | (key.ctrl ? MOD_CONTROL : 0u) | (key.shift ? MOD_SHIFT : 0u) |
                          (key.alt ? MOD_ALT : 0u);
        if (RegisterHotKey(nullptr, i, mods, key.vk)) {
            ++registered;
        } else {
            Log("panel: the global hotkey for %s could not be registered (error %lu); another program may already "
                "use it, so that key does nothing in minimal mode", kActionNames[i], GetLastError());
        }
    }
    Log("panel: minimal mode ([panel] mode=keys): %d of the keys work as global hotkeys, the panel is drawn but not "
        "clickable, and the game window is not touched", registered);

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_HOTKEY) {
            const int id = static_cast<int>(message.wParam);
            if (id >= 0 && id < kCommandCount) {
                RunCommand(static_cast<Command>(id));
            }
        }
    }
    for (int i = 0; i < kCommandCount; ++i) {
        UnregisterHotKey(nullptr, i);
    }
    return 0;
}

}  // namespace

void Start(const HotkeyTable* table) {
    const HANDLE thread = CreateThread(nullptr, 0, HotkeyThread, const_cast<HotkeyTable*>(table), 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        Log("panel: minimal mode could not start its hotkey thread (error %lu); the keys will not work",
            GetLastError());
    }
}

}  // namespace global_hotkeys

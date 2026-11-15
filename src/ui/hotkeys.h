#pragma once

// Hotkeys: what the player typed in settings.ini under [keys], turned into virtual-key codes.
//
// Syntax: [Ctrl+][Shift+][Alt+]KeyName, or "none" to switch an action off. Key names: A-Z, 0-9,
// F1-F24, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock, Space, Tab, Enter,
// Escape, Backspace, Up, Down, Left, Right, Num0-Num9, NumAdd, NumSubtract, NumMultiply,
// NumDivide, MediaPlayPause, MediaNext, MediaPrevious, MediaStop, or VK:0x.. for anything else.
//
// The default keys are single keys without modifiers on purpose: Shift, Ctrl and Alt are game
// controls by themselves, so a combination would also fire the game's own action.
//
// Matching: the key must be the one pressed and every modifier a hotkey names must be held.
// Modifiers it does not name are ignored, so F7 still works while the player holds Shift (turbo).

// The order matches UiKey in src/core/settings.h. Shuffle (F10) since 0.3.3: F10 reaches the window
// as WM_SYSKEYDOWN (the menu key), which the input path treats like any key, so a matched F10
// never opens the window menu.
enum class Command { TogglePanel = 0, Stop, Previous, PlayPause, Next, LoopMode, Shuffle, Count };
constexpr int kCommandCount = static_cast<int>(Command::Count);

struct Hotkey {
    unsigned short vk = 0;   // 0 = no key
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

// Immutable once published: the window thread reads it through an atomic pointer and the old
// table is never freed.
struct HotkeyTable {
    Hotkey keys[kCommandCount];
    bool mediaKeys = true;   // also react to the keyboard's own media keys (local files only)
};

// Returns false when the text is not a key name the mod knows; *out is then unchanged.
bool ParseHotkey(const char* text, Hotkey* out);

// "Ctrl+F7", "Insert", or the given word for "no key".
void HotkeyDisplayName(const Hotkey& key, const char* noneWord, char* buffer, int size);

// The accepted names, for the log line that explains a refused value.
const char* HotkeyAcceptedNames();

// The default table (Insert, F5, F6, F7, F8, F9, F10).
HotkeyTable DefaultHotkeys();

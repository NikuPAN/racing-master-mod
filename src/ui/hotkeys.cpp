#include "hotkeys.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

struct KeyName {
    const char* name;
    unsigned short vk;
};

// Looked up case-insensitively. Letters and digits are handled in code.
const KeyName kNames[] = {
    {"Insert", VK_INSERT},       {"Delete", VK_DELETE},     {"Home", VK_HOME},
    {"End", VK_END},             {"PageUp", VK_PRIOR},      {"PageDown", VK_NEXT},
    {"Pause", VK_PAUSE},         {"ScrollLock", VK_SCROLL}, {"Space", VK_SPACE},
    {"Tab", VK_TAB},             {"Enter", VK_RETURN},      {"Escape", VK_ESCAPE},
    {"Backspace", VK_BACK},      {"Up", VK_UP},             {"Down", VK_DOWN},
    {"Left", VK_LEFT},           {"Right", VK_RIGHT},       {"NumAdd", VK_ADD},
    {"NumSubtract", VK_SUBTRACT}, {"NumMultiply", VK_MULTIPLY}, {"NumDivide", VK_DIVIDE},
    {"MediaPlayPause", VK_MEDIA_PLAY_PAUSE}, {"MediaNext", VK_MEDIA_NEXT_TRACK},
    {"MediaPrevious", VK_MEDIA_PREV_TRACK},  {"MediaStop", VK_MEDIA_STOP},
};

bool EqualsNoCase(const char* a, const char* b) {
    return _stricmp(a, b) == 0;
}

// "F7" -> 0x76, "Num3" -> VK_NUMPAD3, "VK:0x2D" -> 0x2D.
bool ParseKeyName(const char* name, unsigned short* vk) {
    const size_t length = strlen(name);
    if (length == 0) {
        return false;
    }
    if (length == 1) {
        const char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            *vk = static_cast<unsigned short>(c);
            return true;
        }
        return false;
    }
    if ((name[0] == 'F' || name[0] == 'f') && length <= 3) {
        unsigned number = 0;
        for (size_t i = 1; i < length; ++i) {
            if (name[i] < '0' || name[i] > '9') {
                return false;
            }
            number = number * 10 + static_cast<unsigned>(name[i] - '0');
        }
        if (number >= 1 && number <= 24) {
            *vk = static_cast<unsigned short>(VK_F1 + number - 1);
            return true;
        }
        return false;
    }
    if (length == 4 && _strnicmp(name, "Num", 3) == 0 && name[3] >= '0' && name[3] <= '9') {
        *vk = static_cast<unsigned short>(VK_NUMPAD0 + (name[3] - '0'));
        return true;
    }
    if (_strnicmp(name, "VK:", 3) == 0) {
        const char* digits = name + 3;
        if (_strnicmp(digits, "0x", 2) == 0) {
            digits += 2;
        }
        unsigned value = 0;
        if (*digits == '\0') {
            return false;
        }
        for (; *digits; ++digits) {
            const char c = *digits;
            unsigned nibble;
            if (c >= '0' && c <= '9') nibble = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = static_cast<unsigned>(c - 'A' + 10);
            else return false;
            value = value * 16 + nibble;
            if (value > 0xFE) {
                return false;
            }
        }
        if (value == 0) {
            return false;
        }
        *vk = static_cast<unsigned short>(value);
        return true;
    }
    for (const KeyName& entry : kNames) {
        if (EqualsNoCase(entry.name, name)) {
            *vk = entry.vk;
            return true;
        }
    }
    return false;
}

}  // namespace

bool ParseHotkey(const char* text, Hotkey* out) {
    char copy[64];
    size_t n = 0;
    for (const char* p = text; *p && n < sizeof(copy) - 1; ++p) {
        if (*p != ' ' && *p != '\t') {
            copy[n++] = *p;
        }
    }
    copy[n] = '\0';
    if (n == 0) {
        return false;
    }
    if (EqualsNoCase(copy, "none") || EqualsNoCase(copy, "off")) {
        *out = Hotkey{};
        return true;
    }
    Hotkey key;
    char* rest = copy;
    for (;;) {
        char* plus = strchr(rest, '+');
        if (!plus || plus[1] == '\0') {   // a trailing '+' is the key itself ("Shift++" is not supported)
            break;
        }
        *plus = '\0';
        if (EqualsNoCase(rest, "Ctrl") || EqualsNoCase(rest, "Control")) key.ctrl = true;
        else if (EqualsNoCase(rest, "Shift")) key.shift = true;
        else if (EqualsNoCase(rest, "Alt")) key.alt = true;
        else return false;
        rest = plus + 1;
    }
    if (!ParseKeyName(rest, &key.vk)) {
        return false;
    }
    *out = key;
    return true;
}

void HotkeyDisplayName(const Hotkey& key, const char* noneWord, char* buffer, int size) {
    if (size <= 0) {
        return;
    }
    if (key.vk == 0) {
        snprintf(buffer, static_cast<size_t>(size), "%s", noneWord);
        return;
    }
    char name[32] = "";
    const unsigned short vk = key.vk;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(name, sizeof(name), "%c", static_cast<char>(vk));
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(name, sizeof(name), "F%u", static_cast<unsigned>(vk - VK_F1 + 1));
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(name, sizeof(name), "Num%u", static_cast<unsigned>(vk - VK_NUMPAD0));
    } else {
        bool found = false;
        for (const KeyName& entry : kNames) {
            if (entry.vk == vk) {
                snprintf(name, sizeof(name), "%s", entry.name);
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(name, sizeof(name), "VK:0x%02X", static_cast<unsigned>(vk));
        }
    }
    snprintf(buffer, static_cast<size_t>(size), "%s%s%s%s", key.ctrl ? "Ctrl+" : "", key.shift ? "Shift+" : "",
             key.alt ? "Alt+" : "", name);
}

const char* HotkeyAcceptedNames() {
    return "A-Z, 0-9, F1-F24, Insert, Delete, Home, End, PageUp, PageDown, Pause, ScrollLock, Space, Tab, Enter, "
           "Escape, Backspace, Up, Down, Left, Right, Num0-Num9, NumAdd, NumSubtract, NumMultiply, NumDivide, "
           "MediaPlayPause, MediaNext, MediaPrevious, MediaStop, VK:0x.. or none; a modifier goes in front, as "
           "in Ctrl+F7";
}

HotkeyTable DefaultHotkeys() {
    HotkeyTable table;
    table.keys[static_cast<int>(Command::TogglePanel)].vk = VK_INSERT;
    table.keys[static_cast<int>(Command::Stop)].vk = VK_F5;
    table.keys[static_cast<int>(Command::Previous)].vk = VK_F6;
    table.keys[static_cast<int>(Command::PlayPause)].vk = VK_F7;
    table.keys[static_cast<int>(Command::Next)].vk = VK_F8;
    table.keys[static_cast<int>(Command::LoopMode)].vk = VK_F9;
    table.mediaKeys = true;
    return table;
}

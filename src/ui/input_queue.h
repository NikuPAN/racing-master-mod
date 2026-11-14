#pragma once

// What the game's window thread and the thread that presents frames share. Everything here is
// an atomic or a single-producer, single-consumer queue: neither thread ever takes a lock.
//
//   window thread -> frame thread: mouse and key events for the panel (queue), the latest mouse
//                                  position, the panel open flag, commands from hotkeys
//   frame thread  -> window thread: heartbeat (time of the last drawn panel frame), the panel's
//                                  rectangles for the hit test, whether a text field wants keys
//
// Only the window thread pushes into the queue and only the frame thread pops from it. The game
// presents from one thread at a time, so there is one consumer at any moment.

#include "hotkeys.h"

#include <atomic>
#include <cstdint>

struct InputEvent {
    enum Type : uint8_t {
        MouseButton,    // code = button 0..4 (left, right, middle, X1, X2), x/y = client position
        MouseWheel,     // code 0 = vertical, 1 = horizontal; wheel = steps, + = up or right
        Key,            // code = Windows virtual-key code, down = 1/0 (only while a text field has focus)
        Char,           // code = one UTF-16 unit (only while a text field has focus)
        FocusLost,      // the game is no longer the active app
        ClickOutside,   // a click went to the game while the panel was open: close open drop-downs
    };
    Type type;
    uint8_t down;
    uint16_t code;
    int16_t x, y;
    float wheel;
};
static_assert(sizeof(InputEvent) == 12, "InputEvent is meant to stay a small plain record");

struct InputQueue {
    static constexpr unsigned kCapacity = 256;
    InputEvent events[kCapacity];
    std::atomic<unsigned> head{0};
    std::atomic<unsigned> tail{0};

    // Window thread only. A full queue drops the event rather than wait.
    bool Push(const InputEvent& event) {
        const unsigned t = tail.load(std::memory_order_relaxed);
        const unsigned next = (t + 1) % kCapacity;
        if (next == head.load(std::memory_order_acquire)) {
            return false;
        }
        events[t] = event;
        tail.store(next, std::memory_order_release);
        return true;
    }
    // Frame thread only.
    bool Pop(InputEvent* event) {
        const unsigned h = head.load(std::memory_order_relaxed);
        if (h == tail.load(std::memory_order_acquire)) {
            return false;
        }
        *event = events[h];
        head.store((h + 1) % kCapacity, std::memory_order_release);
        return true;
    }
};

// A panel window's rectangle in client coordinates, packed into one 64-bit word so that it is
// published atomically. w == 0 means "no window in this slot".
struct PanelRect {
    int16_t x, y, w, h;
};
inline uint64_t PackRect(PanelRect r) {
    return static_cast<uint64_t>(static_cast<uint16_t>(r.x)) |
           (static_cast<uint64_t>(static_cast<uint16_t>(r.y)) << 16) |
           (static_cast<uint64_t>(static_cast<uint16_t>(r.w)) << 32) |
           (static_cast<uint64_t>(static_cast<uint16_t>(r.h)) << 48);
}
inline PanelRect UnpackRect(uint64_t v) {
    return {static_cast<int16_t>(v & 0xFFFF), static_cast<int16_t>((v >> 16) & 0xFFFF),
            static_cast<int16_t>((v >> 32) & 0xFFFF), static_cast<int16_t>((v >> 48) & 0xFFFF)};
}
inline uint32_t PackPoint(int x, int y) {
    return static_cast<uint32_t>(static_cast<uint16_t>(x)) | (static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
}

constexpr uint32_t kMouseUnknown = 0x80008000u;   // (-32768, -32768): a position that cannot occur
constexpr int kMaxPanelRects = 8;
constexpr unsigned long long kHeartbeatMs = 500;  // the mouse goes back to the game this long after the last panel frame

struct SharedInput {
    InputQueue queue;
    // False until the panel is running with clicks and keys ("[panel] mode=on"), and again for
    // good after a fault: the window procedure then forwards every message untouched.
    std::atomic<bool> enabled{false};
    std::atomic<bool> panelOpen{false};
    std::atomic<unsigned long long> lastDrawTick{0};   // GetTickCount64 of the last drawn panel frame
    std::atomic<uint32_t> latestMouse{kMouseUnknown};  // client x (low 16 bits) and y (high) of the last mouse move
    std::atomic<uint64_t> rects[kMaxPanelRects];       // the panel's clickable windows, client coordinates
    std::atomic<bool> wantTextInput{false};
    std::atomic<bool> gameFocused{true};
    std::atomic<uint32_t> pending[kCommandCount];      // commands waiting for the frame thread
    std::atomic<const HotkeyTable*> hotkeys{nullptr};  // never freed once published
    std::atomic<bool> localSourceActive{true};         // "My files" selected: the keyboard's media keys are ours
};

SharedInput& Shared();

// Any thread. Hotkeys (window thread) and panel buttons (frame thread) use the same entry point.
void PostCommand(Command command);

// True when (x, y), in client coordinates, lies inside one of the published panel rectangles.
bool PointOverPanel(int x, int y);

// True while the panel is open and was drawn within the last half second.
bool PanelOwnsMouse();

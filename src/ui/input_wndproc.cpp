// The game window's subclassed procedure. Runs on the game's window thread.
//
// The owner's rule: clicks outside the panel go to the game. So, while the panel is open and
// being drawn ("owned" below):
//
//   message                        over a panel window           elsewhere
//   mouse button down              queued for the panel, kept    forwarded to the game
//   mouse button up                goes to whoever got its down (either state)
//   mouse move                     kept from the game            forwarded
//   mouse wheel                    queued for the panel, kept    forwarded
//
// A drag stays with the side it started on: while the panel holds a button (dragging the
// volume or seek line) moves and the release are the panel's wherever the cursor is; while the
// game holds one (camera look) everything goes to the game, even over the panel. A release that
// happened outside the game window (windowed mode) is noticed from the next mouse message's
// button flags and ends the drag there.
//
// In every state:
//   key down matching a hotkey     consumed, command posted (auto-repeat consumed, ignored)
//   key up of a consumed key       consumed
//   other keys                     forwarded, except while a panel text field has the keyboard
//   raw input (WM_INPUT)           forwarded (camera look, wheels and pads are never touched)
//   game activated / deactivated   noted, the player told, forwarded
//   everything else                forwarded
//
// "Owned" is a heartbeat: the frame thread stamps the time of every panel frame it draws, and
// this procedure only takes mouse input while that stamp is less than half a second old, so a
// panel that stops being drawn (fault, device loss, minimised, loading screen) can never keep
// the mouse. The hit test uses the panel's window rectangles from the last drawn frame against
// the message's own position, which is what ImGui's WantCaptureMouse would say for that point.
//
// Nothing here calls ImGui or Direct3D, takes a lock, or logs per message. The mod's own part of
// each message runs inside a fault guard; the game's procedure is always called outside it.

#include "input_wndproc.h"

#include "input_queue.h"
#include "media_api.h"

#include "../core/log.h"

#include <windowsx.h>

#include <cstring>

namespace {

struct SubclassedWindow {
    HWND window;
    WNDPROC previous;
};
constexpr int kMaxWindows = 4;
SubclassedWindow g_windows[kMaxWindows] = {};
volatile LONG g_windowCount = 0;

enum class ButtonOwner : unsigned char { None, Game, Panel };

// Window thread only.
ButtonOwner g_buttonOwner[5] = {};   // who received each button's "down"
bool g_consumedKeys[256] = {};       // keys whose "down" the mod consumed
bool g_textKeys[256] = {};           // keys whose "down" went to a panel text field
ULONGLONG g_lastMediaCommandTick = 0;
volatile LONG g_procedureFaulted = 0;

SharedInput g_shared;

WNDPROC PreviousProcedureFor(HWND window) {
    const LONG count = g_windowCount;
    for (LONG i = 0; i < count && i < kMaxWindows; ++i) {
        if (g_windows[i].window == window) {
            return g_windows[i].previous;
        }
    }
    return nullptr;
}

int ButtonIndex(UINT message, WPARAM wParam, bool* down) {
    switch (message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: *down = true; return 0;
        case WM_LBUTTONUP: *down = false; return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: *down = true; return 1;
        case WM_RBUTTONUP: *down = false; return 1;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: *down = true; return 2;
        case WM_MBUTTONUP: *down = false; return 2;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: *down = true; return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
        case WM_XBUTTONUP: *down = false; return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
        default: return -1;
    }
}

bool AnyButtonHeldBy(ButtonOwner owner) {
    for (const ButtonOwner held : g_buttonOwner) {
        if (held == owner) {
            return true;
        }
    }
    return false;
}

// A hotkey matches when its key is the one pressed and every modifier it names is held. Other
// modifiers are ignored on purpose: Shift is the turbo key and Ctrl and Alt are game controls,
// so a racing player may well be holding one while pressing F7.
bool HotkeyMatches(const Hotkey& key, WPARAM vk) {
    if (key.vk == 0 || key.vk != vk) {
        return false;
    }
    if (key.ctrl && GetKeyState(VK_CONTROL) >= 0) return false;
    if (key.shift && GetKeyState(VK_SHIFT) >= 0) return false;
    if (key.alt && GetKeyState(VK_MENU) >= 0) return false;
    return true;
}

int ModifierCount(const Hotkey& key) {
    return (key.ctrl ? 1 : 0) + (key.shift ? 1 : 0) + (key.alt ? 1 : 0);
}

// Keys that produce a character. For these, TranslateMessage has already queued a WM_CHAR when
// the key-down reaches us; it is removed so the game does not get a stray letter. No other key
// queues one, so nothing else is removed.
bool KeyMakesCharacter(WPARAM vk) {
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) ||
           vk == VK_SPACE || (vk >= VK_OEM_1 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

void DropQueuedCharacter(HWND window, WPARAM vk) {
    if (!KeyMakesCharacter(vk)) {
        return;
    }
    MSG message;
    PeekMessageW(&message, window, WM_CHAR, WM_DEADCHAR, PM_REMOVE);
    PeekMessageW(&message, window, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE);
}

bool MediaCommandFor(unsigned code, bool appCommand, Command* out) {
    if (appCommand) {
        switch (code) {
            case APPCOMMAND_MEDIA_PLAY_PAUSE: *out = Command::PlayPause; return true;
            case APPCOMMAND_MEDIA_NEXTTRACK: *out = Command::Next; return true;
            case APPCOMMAND_MEDIA_PREVIOUSTRACK: *out = Command::Previous; return true;
            case APPCOMMAND_MEDIA_STOP: *out = Command::Stop; return true;
            default: return false;
        }
    }
    switch (code) {
        case VK_MEDIA_PLAY_PAUSE: *out = Command::PlayPause; return true;
        case VK_MEDIA_NEXT_TRACK: *out = Command::Next; return true;
        case VK_MEDIA_PREV_TRACK: *out = Command::Previous; return true;
        case VK_MEDIA_STOP: *out = Command::Stop; return true;
        default: return false;
    }
}

// The keyboard's media keys are only taken while "My files" is the source: for another app,
// Windows routes them to that app as usual. One press can arrive both as a key and as
// WM_APPCOMMAND; only the first within 50 ms acts.
bool TakeMediaKey(const HotkeyTable* table) {
    if (!table || !table->mediaKeys || !g_shared.localSourceActive.load(std::memory_order_relaxed)) {
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    const bool duplicate = now - g_lastMediaCommandTick < 50;
    g_lastMediaCommandTick = now;
    return !duplicate;
}

void RunCommand(Command command) {
    if (command == Command::TogglePanel) {
        // Flipped here rather than on the frame thread, so it works even while frames are slow.
        g_shared.panelOpen.store(!g_shared.panelOpen.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return;
    }
    PostCommand(command);
}

void PushMouseEvent(InputEvent::Type type, int code, bool down, int x, int y, float wheel) {
    InputEvent event = {};
    event.type = type;
    event.down = down ? 1 : 0;
    event.code = static_cast<uint16_t>(code);
    event.x = static_cast<int16_t>(x);
    event.y = static_cast<int16_t>(y);
    event.wheel = wheel;
    g_shared.queue.Push(event);
}

void NotifyPlayerOfFocus(bool focused) {
    music::Player::Get().OnGameFocusChanged(focused);
}

void ActiveChanged(bool active) {
    if (g_shared.gameFocused.exchange(active, std::memory_order_relaxed) == active) {
        return;
    }
    if (!active) {
        InputEvent event = {};
        event.type = InputEvent::FocusLost;
        g_shared.queue.Push(event);
        memset(g_consumedKeys, 0, sizeof(g_consumedKeys));
        memset(g_textKeys, 0, sizeof(g_textKeys));
        for (ButtonOwner& owner : g_buttonOwner) {
            owner = ButtonOwner::None;
        }
    }
    NotifyPlayerOfFocus(active);
}

// What to do with one message: Forward it to the game, or Consume it and return `result`.
enum class Verdict { Forward, Consume };

Verdict HandleKeyDown(HWND window, WPARAM wParam, LPARAM lParam) {
    if (wParam >= 256) {
        return Verdict::Forward;
    }
    const bool repeat = (lParam & (1LL << 30)) != 0;
    const HotkeyTable* table = g_shared.hotkeys.load(std::memory_order_acquire);
    if (table) {
        // The most specific match wins (Ctrl+F7 before F7 when both are set).
        int best = -1;
        for (int i = 0; i < kCommandCount; ++i) {
            if (HotkeyMatches(table->keys[i], wParam) &&
                (best < 0 || ModifierCount(table->keys[i]) > ModifierCount(table->keys[best]))) {
                best = i;
            }
        }
        if (best >= 0) {
            if (!repeat) {
                RunCommand(static_cast<Command>(best));
            }
            g_consumedKeys[wParam] = true;
            DropQueuedCharacter(window, wParam);
            return Verdict::Consume;
        }
        Command media;
        if (MediaCommandFor(static_cast<unsigned>(wParam), false, &media) && TakeMediaKey(table)) {
            if (!repeat) {
                PostCommand(media);
            }
            g_consumedKeys[wParam] = true;
            return Verdict::Consume;
        }
    }
    // A text field in the panel has the keyboard: the key is the panel's, not the game's.
    if (PanelOwnsMouse() && g_shared.wantTextInput.load(std::memory_order_relaxed)) {
        InputEvent event = {};
        event.type = InputEvent::Key;
        event.down = 1;
        event.code = static_cast<uint16_t>(wParam);
        g_shared.queue.Push(event);
        g_textKeys[wParam] = true;
        return Verdict::Consume;
    }
    return Verdict::Forward;
}

Verdict HandleKeyUp(WPARAM wParam) {
    if (wParam >= 256) {
        return Verdict::Forward;
    }
    if (g_consumedKeys[wParam]) {
        g_consumedKeys[wParam] = false;
        return Verdict::Consume;
    }
    if (g_textKeys[wParam]) {
        g_textKeys[wParam] = false;
        InputEvent event = {};
        event.type = InputEvent::Key;
        event.down = 0;
        event.code = static_cast<uint16_t>(wParam);
        g_shared.queue.Push(event);
        return Verdict::Consume;
    }
    return Verdict::Forward;
}

// The flag each button has in a mouse message's key state, by button index.
constexpr WPARAM kButtonFlags[5] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2};

// Every mouse message says which buttons are down at that moment. In windowed or borderless
// mode a button can be let go after the cursor has left the game window; that release goes to
// another window and never reaches this procedure. So a button still noted as held whose flag is
// clear was released elsewhere: the panel gets its release now (a slider stops following the
// cursor, and the next click is not taken for it), and a drag the game owned simply ends.
// `skip` is the button the message itself is about (its own up has the flag clear on purpose).
void ForgetButtonsReleasedElsewhere(WPARAM keyState, int x, int y, int skip) {
    for (int i = 0; i < 5; ++i) {
        if (i == skip || g_buttonOwner[i] == ButtonOwner::None || (keyState & kButtonFlags[i]) != 0) {
            continue;
        }
        if (g_buttonOwner[i] == ButtonOwner::Panel) {
            PushMouseEvent(InputEvent::MouseButton, i, false, x, y, 0.0f);
        }
        g_buttonOwner[i] = ButtonOwner::None;
    }
}

Verdict HandleMouse(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // The key state is the low word for every mouse message (the wheel and extra-button messages
    // carry other data in the high word).
    const WPARAM keyState = GET_KEYSTATE_WPARAM(wParam);
    if (message == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        ForgetButtonsReleasedElsewhere(keyState, x, y, -1);
        // Remembered in every state, so the panel knows where the cursor is the moment it opens.
        g_shared.latestMouse.store(PackPoint(x, y), std::memory_order_relaxed);
        if (!PanelOwnsMouse() || AnyButtonHeldBy(ButtonOwner::Game)) {
            return Verdict::Forward;
        }
        return (AnyButtonHeldBy(ButtonOwner::Panel) || PointOverPanel(x, y)) ? Verdict::Consume : Verdict::Forward;
    }
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};   // wheel messages carry screen coordinates
        ScreenToClient(window, &point);
        ForgetButtonsReleasedElsewhere(keyState, point.x, point.y, -1);
        if (!PanelOwnsMouse() || AnyButtonHeldBy(ButtonOwner::Game)) {
            return Verdict::Forward;
        }
        if (!PointOverPanel(point.x, point.y)) {
            return Verdict::Forward;
        }
        PushMouseEvent(InputEvent::MouseWheel, message == WM_MOUSEHWHEEL ? 1 : 0, false, point.x, point.y,
                       static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
        return Verdict::Consume;
    }
    bool down = false;
    const int button = ButtonIndex(message, wParam, &down);
    if (button < 0) {
        return Verdict::Forward;
    }
    const int x = GET_X_LPARAM(lParam);
    const int y = GET_Y_LPARAM(lParam);
    ForgetButtonsReleasedElsewhere(keyState, x, y, button);
    if (down) {
        const bool owned = PanelOwnsMouse();
        if (owned && !AnyButtonHeldBy(ButtonOwner::Game) &&
            (AnyButtonHeldBy(ButtonOwner::Panel) || PointOverPanel(x, y))) {
            g_buttonOwner[button] = ButtonOwner::Panel;
            PushMouseEvent(InputEvent::MouseButton, button, true, x, y, 0.0f);
            return Verdict::Consume;
        }
        g_buttonOwner[button] = ButtonOwner::Game;
        if (owned) {
            // The game gets this click; an open drop-down list in the panel closes, as it would
            // for any click outside it.
            PushMouseEvent(InputEvent::ClickOutside, button, true, x, y, 0.0f);
        }
        return Verdict::Forward;
    }
    // Button up: it belongs to whoever received the down. An up whose down nobody saw (pressed
    // before the mod attached) goes to the game.
    const ButtonOwner owner = g_buttonOwner[button];
    g_buttonOwner[button] = ButtonOwner::None;
    if (owner == ButtonOwner::Panel) {
        // Queued even if the panel closed meanwhile; the frame thread clears ImGui's mouse
        // state when the panel is drawn again after a gap, so nothing stays held.
        PushMouseEvent(InputEvent::MouseButton, button, false, x, y, 0.0f);
        return Verdict::Consume;
    }
    return Verdict::Forward;
}

// The mod's decision for one message. *result is what to return when consumed.
Verdict Decide(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    *result = 0;
    switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            return HandleKeyDown(window, wParam, lParam);
        case WM_KEYUP:
        case WM_SYSKEYUP:
            return HandleKeyUp(wParam);
        case WM_CHAR:
        case WM_SYSCHAR:
            if (PanelOwnsMouse() && g_shared.wantTextInput.load(std::memory_order_relaxed)) {
                InputEvent event = {};
                event.type = InputEvent::Char;
                event.code = static_cast<uint16_t>(wParam);
                g_shared.queue.Push(event);
                return Verdict::Consume;
            }
            return Verdict::Forward;
        case WM_APPCOMMAND: {
            Command media;
            if (MediaCommandFor(static_cast<unsigned>(GET_APPCOMMAND_LPARAM(lParam)), true, &media) &&
                TakeMediaKey(g_shared.hotkeys.load(std::memory_order_acquire))) {
                PostCommand(media);
                *result = TRUE;   // "handled", as Windows asks
                return Verdict::Consume;
            }
            return Verdict::Forward;
        }
        case WM_ACTIVATEAPP:
            // The whole game app, not one of its windows: the file picker the panel opens is a
            // window of the game process, and choosing files must not count as leaving the game.
            ActiveChanged(wParam != FALSE);
            return Verdict::Forward;
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: {
            const Verdict verdict = HandleMouse(window, message, wParam, lParam);
            if (verdict == Verdict::Consume && (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                                                message == WM_XBUTTONDBLCLK)) {
                *result = TRUE;   // Windows' rule for the extra mouse buttons
            }
            return verdict;
        }
        default:
            return Verdict::Forward;
    }
}

// SEH guard around the mod's part only. A fault here switches the panel's input handling off
// for good (every later message is forwarded) and is reported once by the worker thread.
Verdict GuardedDecide(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT* result) {
    __try {
        return Decide(window, message, wParam, lParam, result);
    } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER) {
        g_shared.enabled.store(false, std::memory_order_relaxed);
        InterlockedExchange(&g_procedureFaulted, 1);
        return Verdict::Forward;
    }
}

LRESULT CALLBACK PanelWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const WNDPROC previous = PreviousProcedureFor(window);
    if (!previous) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (message == WM_NCDESTROY) {
        const LRESULT result = CallWindowProcW(previous, window, message, wParam, lParam);
        const LONG count = g_windowCount;
        for (LONG i = 0; i < count && i < kMaxWindows; ++i) {
            if (g_windows[i].window == window) {
                g_windows[i].window = nullptr;
            }
        }
        return result;
    }
    if (g_shared.enabled.load(std::memory_order_relaxed)) {
        LRESULT result = 0;
        if (GuardedDecide(window, message, wParam, lParam, &result) == Verdict::Consume) {
            return result;
        }
    }
    return CallWindowProcW(previous, window, message, wParam, lParam);
}

}  // namespace

SharedInput& Shared() {
    return g_shared;
}

void PostCommand(Command command) {
    const int index = static_cast<int>(command);
    if (index >= 0 && index < kCommandCount) {
        g_shared.pending[index].fetch_add(1, std::memory_order_relaxed);
    }
}

bool PointOverPanel(int x, int y) {
    for (int i = 0; i < kMaxPanelRects; ++i) {
        const PanelRect r = UnpackRect(g_shared.rects[i].load(std::memory_order_relaxed));
        if (r.w <= 0 || r.h <= 0) {
            continue;
        }
        if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
            return true;
        }
    }
    return false;
}

bool PanelOwnsMouse() {
    if (!g_shared.panelOpen.load(std::memory_order_relaxed)) {
        return false;
    }
    const unsigned long long last = g_shared.lastDrawTick.load(std::memory_order_relaxed);
    return last != 0 && GetTickCount64() - last < kHeartbeatMs;
}

bool InstallWindowSubclass(HWND window) {
    if (PreviousProcedureFor(window)) {
        return true;
    }
    const LONG index = g_windowCount;
    if (index >= kMaxWindows) {
        Log("panel: the game has drawn into more than %d windows; the newest one is not watched for clicks and keys",
            kMaxWindows);
        return false;
    }
    if (!IsWindowUnicode(window)) {
        Log("panel: the game window is not a Unicode window; clicks and keys are not watched");
        return false;
    }
    // The previous procedure is published before the switch, so a message that arrives in
    // between (on the window's own thread) already finds something to chain to.
    SubclassedWindow& slot = g_windows[index];
    slot.previous = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    if (!slot.previous) {
        Log("panel: could not read the game window's procedure (error %lu); clicks and keys are not watched",
            GetLastError());
        return false;
    }
    slot.window = window;
    InterlockedIncrement(&g_windowCount);
    SetLastError(0);
    const LONG_PTR replaced = SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PanelWndProc));
    if (replaced == 0 && GetLastError() != 0) {
        Log("panel: could not attach to the game window (error %lu); the panel is drawn but cannot be clicked and "
            "its keys do nothing",
            GetLastError());
        slot.window = nullptr;
        return false;
    }
    if (reinterpret_cast<WNDPROC>(replaced) != slot.previous) {
        slot.previous = reinterpret_cast<WNDPROC>(replaced);   // someone else changed it in between
    }
    DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &pid);
    Log("panel: watching the game window for clicks and keys (window thread %lu, frame thread %lu)", thread,
        GetCurrentThreadId());
    return true;
}

bool WindowProcedureFaulted() {
    return g_procedureFaulted != 0;
}

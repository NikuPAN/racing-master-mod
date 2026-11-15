// Dear ImGui inside the game's frame: start-up, the per-frame sequence, input hand-off, and the
// fault latch. The widgets themselves are in panel.cpp.
//
// Per frame, while something is visible (panel open or a short notice):
//   1. device, immediate context, buffer 0 of the swap chain and a render-target view on it,
//      all into FrameRefs (released at the end of the frame or by the fault handler)
//   2. input from the window thread's queue, scale for the back-buffer height, ImGui frame
//   3. SwapDeviceContextState(ours): the game's entire pipeline state is parked
//   4. the stock DX11 backend draws
//   5. ClearState (our state lets go of the back buffer), SwapDeviceContextState(game's) back
//   6. release; publish the heartbeat and the panel's rectangles to the window thread
//
// Without step 5's ClearState the game's next ResizeBuffers fails (DXGI_ERROR_INVALID_CALL),
// which Unreal treats as fatal.

#include "overlay.h"

#include "fonts.h"
#include "imgui_errors.h"
#include "global_hotkeys.h"
#include "input_queue.h"
#include "input_wndproc.h"
#include "media_api.h"
#include "other_app.h"
#include "panel.h"
#include "scale.h"
#include "ui_common.h"
#include "ui_settings.h"

#include "../core/frame.h"
#include "../core/log.h"
#include "../core/present_hook.h"
#include "../core/present_probe.h"
#include "../core/settings.h"

#include "imgui_impl_dx11.h"
#include "imgui_internal.h"

#include <windows.h>

#include <d3d11_1.h>
#include <delayimp.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace {

// Set by the worker before the hook exists; read-only afterwards.
bool g_started = false;
bool g_testHost = false;
unsigned g_mode = UiModeOff;
bool g_compilerLoaded = false;
unsigned g_testFaultAtPanelFrame = 0;   // test host: fault between the two state swaps at this panel frame
volatile int* volatile g_faultTarget = nullptr;

// Frame thread only.
enum class InitState { NotYet, Ready, Unavailable };
InitState g_init = InitState::NotYet;
std::atomic<bool> g_dead{false};          // fault or ImGui assertion: never call ImGui again
ID3D11Device* g_device = nullptr;          // AddRef'd; compared with the swap chain's device every frame
ID3DDeviceContextState* g_state = nullptr; // our pipeline state; holds no bindings between frames
HWND g_window = nullptr;
std::atomic<HWND> g_publishedWindow{nullptr};   // g_window for the worker (PanelGameWindow)
IDXGISwapChain* g_lastSwapChain = nullptr; // compared only, never dereferenced across frames
bool g_wasDrawing = false;                 // false -> true is a gap: ImGui's mouse state is cleared
bool g_imguiFocused = true;
LARGE_INTEGER g_frequency = {};
LARGE_INTEGER g_lastFrameTime = {};
unsigned g_consecutiveFailures = 0;
UINT g_failedWidth = 0, g_failedHeight = 0;
bool g_failureLogged = false;
bool g_formatLogged = false;
bool g_openWithoutDrawLogged = false;
unsigned g_panelFrames = 0;
bool g_exclusiveFullscreen = false;
char g_lastLayout[32768] = "";   // test host only

// ---- small helpers -------------------------------------------------------------------------------

bool ReadEnv(const wchar_t* name, wchar_t* value, DWORD size) {
    const DWORD n = GetEnvironmentVariableW(name, value, size);
    return n > 0 && n < size;
}

unsigned ParseDigits(const wchar_t* text) {
    unsigned value = 0;
    for (const wchar_t* p = text; *p; ++p) {
        if (*p < L'0' || *p > L'9' || value > 100000000) {
            return 0;
        }
        value = value * 10 + static_cast<unsigned>(*p - L'0');
    }
    return value;
}

// Delay-loaded functions are resolved here, on the worker, so their first call never happens on
// the game's window thread or frame thread. The name must match the import table exactly.
void ResolveDelayLoaded(const char* const* names, int count, const char* what) {
    for (int i = 0; i < count; ++i) {
        if (SUCCEEDED(__HrLoadAllImportsForDll(names[i]))) {
            return;
        }
    }
    Log("panel: could not prepare %s in advance; it is prepared on first use instead", what);
}

// The panel's own look: Dear ImGui's dark style, a little more opaque and rounded.
void ApplyBaseStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.ItemSpacing = ImVec2(6.0f, 6.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.92f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.18f);
    style.Colors[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.12f);
    // Slider tracks, the checkbox box and drop-down fields: an opaque grey-blue that stands out
    // from the window background (ImGui's default is a faint translucent blue that all but
    // disappears on it).
    style.DisabledAlpha = 0.4f;   // greyed controls look clearly unavailable (ImGui's default is 0.6)
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.24f, 0.31f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.31f, 0.40f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.36f, 0.47f, 1.0f);
}

// ---- start-up on the frame thread ------------------------------------------------------------------

bool CreateDeviceObjects(ID3D11Device* device) {
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const bool initOk = ImGui_ImplDX11_Init(device, context);
    context->Release();   // the backend keeps its own reference
    if (!initOk) {
        Log("panel: the drawing backend did not start; the panel is unavailable");
        return false;
    }
    // Compiles the two small shaders now (login screen) rather than when the panel first opens
    // during a race.
    if (!ImGui_ImplDX11_CreateDeviceObjects()) {
        Log("panel: the panel's shaders could not be prepared; the panel is unavailable");
        ImGui_ImplDX11_Shutdown();
        return false;
    }
    ID3D11Device1* device1 = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
    if (SUCCEEDED(hr)) {
        D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                               : 0;
        hr = device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                               &g_state);
        device1->Release();
    }
    if (FAILED(hr) || !g_state) {
        Log("panel: Direct3D 11.1 state switching is not available (error 0x%08lx); the panel is unavailable",
            static_cast<unsigned long>(hr));
        ImGui_ImplDX11_Shutdown();
        return false;
    }
    device->AddRef();
    g_device = device;
    return true;
}

void ReleaseDeviceObjects() {
    ImGui_ImplDX11_Shutdown();
    if (g_state) {
        g_state->Release();
        g_state = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

void LogDisplayFacts(IDXGISwapChain* swapChain) {
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&chain1)))) {
        DXGI_SWAP_CHAIN_DESC1 desc1 = {};
        if (SUCCEEDED(chain1->GetDesc1(&desc1))) {
            Log("panel: swap chain scaling mode %u, alpha mode %u", static_cast<unsigned>(desc1.Scaling),
                static_cast<unsigned>(desc1.AlphaMode));
        }
        chain1->Release();
    }
    IDXGIOutput* output = nullptr;
    if (SUCCEEDED(swapChain->GetContainingOutput(&output))) {
        IDXGIOutput6* output6 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), reinterpret_cast<void**>(&output6)))) {
            DXGI_OUTPUT_DESC1 desc = {};
            if (SUCCEEDED(output6->GetDesc1(&desc))) {
                Log("panel: display colour space %u%s", static_cast<unsigned>(desc.ColorSpace),
                    desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                        ? " (Windows HDR is on for this display; the panel's colours may look wrong if the game "
                          "also outputs HDR)"
                        : "");
            }
            output6->Release();
        }
        output->Release();
    }
}

bool InitOnFrameThread(IDXGISwapChain* swapChain, FrameRefs& refs) {
    if (!g_compilerLoaded) {
        Log("panel: unavailable, because d3dcompiler_47.dll could not be loaded; the game is unaffected");
        return false;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc))) {
        return false;
    }
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&refs.device)))) {
        Log("panel: unavailable, because the game does not draw with Direct3D 11");
        return false;
    }
    g_window = desc.OutputWindow;
    g_publishedWindow.store(g_window, std::memory_order_release);
    g_lastSwapChain = swapChain;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // nothing is written into the game folder
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // the mod never changes the system cursor
    io.BackendPlatformName = "racing-master-mod";
    InstallImGuiErrorPolicy(g_testHost);
    ApplyBaseStyle();
    ScaleCaptureBaseStyle();
    LoadFontChain();

    if (!CreateDeviceObjects(refs.device)) {
        ImGui::DestroyContext();
        return false;
    }
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_lastFrameTime);

    RECT client = {};
    GetClientRect(g_window, &client);
    const char* modeText = g_mode == UiModeKeys ? "minimal (drawn, global keys, the game window is not touched)" : "on";
    Log("panel: ready on the frame thread %lu; back buffer %ux%u, window client area %ldx%ld, mode %s",
        GetCurrentThreadId(), desc.BufferDesc.Width, desc.BufferDesc.Height, client.right, client.bottom, modeText);
    LogDisplayFacts(swapChain);

    if (g_mode == UiModeOn) {
        if (InstallWindowSubclass(g_window)) {
            Shared().enabled.store(true, std::memory_order_release);
        }
    }
    // Minimal mode (keys): the panel starts closed and the Insert global hotkey toggles it; the
    // hotkey thread was started in PanelStart. The game window is never subclassed.
    PanelFirstFrame();
    return true;
}

// ---- input -----------------------------------------------------------------------------------------

ImGuiKey KeyFromVirtualKey(unsigned vk) {
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_CONTROL: return ImGuiMod_Ctrl;
        case VK_SHIFT: return ImGuiMod_Shift;
        case VK_MENU: return ImGuiMod_Alt;
        default: return ImGuiKey_None;
    }
}

void PositionToBuffer(int x, int y, float sx, float sy, ImVec2* out) {
    out->x = static_cast<float>(x) * sx;
    out->y = static_cast<float>(y) * sy;
}

// Window thread's queue -> ImGui. `gap` = the panel was not drawn on the previous frame.
void FeedInput(ImGuiIO& io, UINT bufferWidth, UINT bufferHeight, bool gap, bool* clickedOutside) {
    RECT client = {};
    float sx = 1.0f, sy = 1.0f;
    if (GetClientRect(g_window, &client) && client.right > 0 && client.bottom > 0) {
        sx = static_cast<float>(bufferWidth) / static_cast<float>(client.right);
        sy = static_cast<float>(bufferHeight) / static_cast<float>(client.bottom);
    }
    if (gap) {
        // A button pressed on the panel and released after it closed would otherwise stay held
        // when it opens again, and a slider would follow the cursor (design brief, checker note 10).
        io.ClearInputMouse();
    }
    SharedInput& shared = Shared();
    const bool focused = shared.gameFocused.load(std::memory_order_relaxed);
    if (focused && !g_imguiFocused) {
        io.AddFocusEvent(true);
        g_imguiFocused = true;
    }
    InputEvent event;
    while (shared.queue.Pop(&event)) {
        ImVec2 p;
        switch (event.type) {
            case InputEvent::MouseButton:
                PositionToBuffer(event.x, event.y, sx, sy, &p);
                io.AddMousePosEvent(p.x, p.y);
                io.AddMouseButtonEvent(event.code, event.down != 0);
                break;
            case InputEvent::MouseWheel:
                PositionToBuffer(event.x, event.y, sx, sy, &p);
                io.AddMousePosEvent(p.x, p.y);
                // Windows: horizontal + = tilted right; ImGui: wheel_x + = scroll left.
                io.AddMouseWheelEvent(event.code == 1 ? -event.wheel : 0.0f, event.code == 0 ? event.wheel : 0.0f);
                break;
            case InputEvent::Key: {
                const ImGuiKey key = KeyFromVirtualKey(event.code);
                if (key != ImGuiKey_None) {
                    io.AddKeyEvent(key, event.down != 0);
                }
                break;
            }
            case InputEvent::Char:
                io.AddInputCharacterUTF16(static_cast<ImWchar16>(event.code));
                break;
            case InputEvent::FocusLost:
                io.AddFocusEvent(false);
                g_imguiFocused = false;
                break;
            case InputEvent::ClickOutside:
                *clickedOutside = true;
                break;
        }
    }
    const uint32_t packed = shared.latestMouse.load(std::memory_order_relaxed);
    if (packed == kMouseUnknown) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    } else {
        ImVec2 p;
        PositionToBuffer(static_cast<int16_t>(packed & 0xFFFF), static_cast<int16_t>(packed >> 16), sx, sy, &p);
        io.AddMousePosEvent(p.x, p.y);
    }
    if (!g_testHost) {
        // In the game: nothing in the panel lights up while another program is in front. (The
        // test host's window is never shown or focused, and its pointer comes only from the
        // messages the test posts, so the owner's real cursor cannot change a screenshot.)
        if (GetForegroundWindow() != g_window) {
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
        // The panel draws its own arrow only when the game has hidden the system cursor while
        // the panel is open. The mod never shows, hides, moves or confines the system cursor.
        CURSORINFO cursor = {sizeof(cursor)};
        const bool systemCursorShown = GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING) && cursor.hCursor;
        io.MouseDrawCursor = PanelOwnsMouse() && !systemCursorShown;
    } else {
        io.MouseDrawCursor = false;
    }
}

// The windows the player can click, as client rectangles for the window thread's hit test.
void PublishRects(float sx, float sy) {
    SharedInput& shared = Shared();
    int slot = 0;
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* window : g.Windows) {
        if (slot >= kMaxPanelRects) {
            break;
        }
        if (!window->Active || window->Hidden ||
            (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoMouseInputs | ImGuiWindowFlags_Tooltip))) {
            continue;
        }
        PanelRect r;
        r.x = static_cast<int16_t>(window->Pos.x / sx);
        r.y = static_cast<int16_t>(window->Pos.y / sy);
        r.w = static_cast<int16_t>(window->Size.x / sx + 1.0f);
        r.h = static_cast<int16_t>(window->Size.y / sy + 1.0f);
        shared.rects[slot++].store(PackRect(r), std::memory_order_relaxed);
    }
    for (; slot < kMaxPanelRects; ++slot) {
        shared.rects[slot].store(0, std::memory_order_relaxed);
    }
}

void ClearRects() {
    for (int i = 0; i < kMaxPanelRects; ++i) {
        Shared().rects[i].store(0, std::memory_order_relaxed);
    }
}

// ---- test host: layout file --------------------------------------------------------------------------

// Writes %LOCALAPPDATA%\RacingMasterMod\test-layout.txt when it changed: the panel's state, its
// windows (back-buffer pixels, for the pixel checks) and widget centres (client coordinates, for
// the messages the test posts). Test host only; never written in the game.
void WriteTestLayout(bool drawn, float sx, float sy) {
    static char text[32768];
    // "thread": the thread doing the frame work (this one); "otherthread": presents of the game's
    // swap chain from another thread, passed on untouched (present_hook.cpp).
    const PresentHookCounters counters = PresentHookGetCounters();
    int used = snprintf(text, sizeof(text),
                        "open %d\ndrawn %d\nenabled %d\nsaves %ld\nskips %llu\notherthread %llu\nthread %lu\nprobe %llu\n",
                        Shared().panelOpen.load() ? 1 : 0, drawn ? 1 : 0, Shared().enabled.load() ? 1 : 0,
                        UiSettingsSavesWritten(), counters.concurrentSkips, counters.otherThreadPresents,
                        GetCurrentThreadId(), ProbeRecordedCalls());
    used += PanelAppendTestState(text + used, static_cast<int>(sizeof(text)) - used);
    if (drawn) {
        ImGuiContext& g = *ImGui::GetCurrentContext();
        used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "display %d %d\n",
                         static_cast<int>(g.IO.DisplaySize.x), static_cast<int>(g.IO.DisplaySize.y));
        for (ImGuiWindow* window : g.Windows) {
            if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_ChildWindow)) {
                continue;
            }
            // "bar": the panel's own window (the bar, with the list and settings row when open).
            used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "%s %d %d %d %d\n",
                             strstr(window->Name, "###rmm_panel") ? "bar" : "window", static_cast<int>(window->Pos.x),
                             static_cast<int>(window->Pos.y), static_cast<int>(window->Size.x + 0.5f),
                             static_cast<int>(window->Size.y + 0.5f));
        }
        for (int i = 0; i < kMaxPanelRects; ++i) {
            const PanelRect r = UnpackRect(Shared().rects[i].load());
            if (r.w > 0) {
                used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "hit %d %d %d %d\n", r.x, r.y,
                                 r.w, r.h);
            }
        }
        const PanelTestItem* items = nullptr;
        const int count = PanelTestItems(&items);
        for (int i = 0; i < count && used < static_cast<int>(sizeof(text)) - 64; ++i) {
            used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "item %s %d %d\n", items[i].name,
                             static_cast<int>(items[i].x / sx), static_cast<int>(items[i].y / sy));
        }
        // Texts in fixed-width boxes (the times), back-buffer pixels: "fit <name> <text> <box>"; and
        // every widget's centre in back-buffer pixels with a tenth of a pixel, for the alignment
        // checks ("at <name> <x> <y>"; the "item" lines above are rounded client coordinates).
        for (int i = 0; i < count && used < static_cast<int>(sizeof(text)) - 96; ++i) {
            if (items[i].box > 0.0f) {
                used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "fit %s %.1f %.1f\n",
                                 items[i].name, items[i].text, items[i].box);
            }
            used += snprintf(text + used, sizeof(text) - static_cast<size_t>(used), "at %s %.1f %.1f\n", items[i].name,
                             items[i].x, items[i].y);
        }
    }
    if (strcmp(text, g_lastLayout) == 0) {
        return;
    }
    strcpy_s(g_lastLayout, text);
    const wchar_t* dir = LogDirectory();
    if (dir[0] == L'\0') {
        return;
    }
    wchar_t path[MAX_PATH], temporary[MAX_PATH];
    swprintf_s(path, L"%s\\test-layout.txt", dir);
    swprintf_s(temporary, L"%s\\test-layout.new", dir);
    const HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(file);
    MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING);
}

// ---- the frame --------------------------------------------------------------------------------------

void LogFrameFailure(const char* step, HRESULT hr, UINT width, UINT height) {
    ++g_consecutiveFailures;
    g_failedWidth = width;
    g_failedHeight = height;
    if (!g_failureLogged) {
        g_failureLogged = true;
        Log("panel: could not draw (%s, error 0x%08lx); the game is unaffected and the panel tries again",
            step, static_cast<unsigned long>(hr));
    }
}

// Returns true when the panel was drawn.
bool DrawFrame(IDXGISwapChain* swapChain, FrameRefs& refs, bool panelOpen) {
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&refs.device));
    if (FAILED(hr)) {
        LogFrameFailure("device", hr, 0, 0);
        return false;
    }
    if (refs.device->GetDeviceRemovedReason() != S_OK) {
        return false;   // the game is recreating its device; do not touch the context
    }
    if (refs.device != g_device) {
        // The game created a new device: the panel's device objects move to it.
        Log("panel: the game changed its Direct3D device; the panel follows it");
        ReleaseDeviceObjects();
        if (!CreateDeviceObjects(refs.device)) {
            g_init = InitState::Unavailable;
            Shared().enabled.store(false);
            return false;
        }
    }
    hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&refs.backBuffer));
    if (FAILED(hr)) {
        LogFrameFailure("back buffer", hr, 0, 0);
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    refs.backBuffer->GetDesc(&desc);
    // After many failures in a row, wait for something to change (size or device) before trying again.
    if (g_consecutiveFailures >= 120 && desc.Width == g_failedWidth && desc.Height == g_failedHeight) {
        return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
    const D3D11_RENDER_TARGET_VIEW_DESC* viewDescPointer = nullptr;
    switch (desc.Format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            if (!g_formatLogged) {
                g_formatLogged = true;
                Log("panel: the game outputs HDR colour (format %u); the panel is not drawn in this mode",
                    static_cast<unsigned>(desc.Format));
            }
            return false;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            // ImGui's colours are already display-encoded; write them through a plain view.
            viewDesc.Format = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                              : DXGI_FORMAT_B8G8R8A8_UNORM;
            viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            viewDescPointer = &viewDesc;
            break;
        default:
            break;
    }
    hr = refs.device->CreateRenderTargetView(refs.backBuffer, viewDescPointer, &refs.target);
    if (FAILED(hr)) {
        LogFrameFailure("render target", hr, desc.Width, desc.Height);
        return false;
    }
    refs.device->GetImmediateContext(&refs.context);
    hr = refs.context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&refs.context1));
    if (FAILED(hr)) {
        LogFrameFailure("context", hr, desc.Width, desc.Height);
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float delta = static_cast<float>(static_cast<double>(now.QuadPart - g_lastFrameTime.QuadPart) /
                                     static_cast<double>(g_frequency.QuadPart));
    g_lastFrameTime = now;
    io.DeltaTime = delta < 0.001f ? 0.001f : (delta > 0.1f ? 0.1f : delta);

    bool clickedOutside = false;
    FeedInput(io, desc.Width, desc.Height, !g_wasDrawing, &clickedOutside);
    ApplyScale(desc.Height, PanelTextSize());
    FontAtlasValve();

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (clickedOutside) {
        PanelClickedOutside();
    }
    PanelFrameInfo info;
    info.window = g_window;
    info.displayWidth = io.DisplaySize.x;
    info.displayHeight = io.DisplaySize.y;
    info.displayOnly = g_mode != UiModeOn;   // display and minimal (keys) modes: not clickable
    // Exclusive full screen is checked about once a second while the panel is drawn (the file
    // window is not offered there).
    if (g_panelFrames % 60 == 0) {
        BOOL fullscreen = FALSE;
        g_exclusiveFullscreen = SUCCEEDED(swapChain->GetFullscreenState(&fullscreen, nullptr)) && fullscreen;
    }
    info.exclusiveFullscreen = g_exclusiveFullscreen;
    BuildPanel(info, panelOpen);
    ImGui::Render();

    RECT client = {};
    float sx = 1.0f, sy = 1.0f;
    if (GetClientRect(g_window, &client) && client.right > 0 && client.bottom > 0) {
        sx = io.DisplaySize.x / static_cast<float>(client.right);
        sy = io.DisplaySize.y / static_cast<float>(client.bottom);
    }
    PublishRects(sx, sy);
    Shared().wantTextInput.store(io.WantTextInput, std::memory_order_relaxed);

    ++g_panelFrames;
    if (g_testHost && InputTakeTestAssertRequest()) {
        ModImGuiAssertFail("test host: a deliberate assertion", __FILE__, __LINE__);   // test host only
    }
    refs.context1->SwapDeviceContextState(g_state, &refs.gameState);   // the game's whole pipeline state is parked
    if (g_testFaultAtPanelFrame != 0 && g_panelFrames == g_testFaultAtPanelFrame) {
        *g_faultTarget = 1;   // test host only: fault while our state is swapped in
    }
    refs.context->OMSetRenderTargets(1, &refs.target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());   // also uploads new glyphs
    refs.context->ClearState();                             // our state lets go of the back buffer
    refs.context1->SwapDeviceContextState(refs.gameState, nullptr);   // the game's state back, bit for bit
    refs.gameState->Release();
    refs.gameState = nullptr;

    g_consecutiveFailures = 0;
    g_failureLogged = false;
    if (g_testHost) {
        WriteTestLayout(true, sx, sy);
    }
    return true;
}

}  // namespace

void PanelStart(const UiSettings& settings, bool testHost) {
    g_testHost = testHost;
    g_mode = settings.mode;
    if (g_mode == UiModeOff) {
        Log("panel: off ([panel] mode=off)");
        return;
    }
    UiSettings effective = settings;

    // Test host only: overrides that let tools\run-host-test.ps1 produce every panel state.
    if (testHost) {
        wchar_t value[64];
        if (ReadEnv(L"RACING_MASTER_MOD_LANG", value, 64)) {
            effective.language = lstrcmpiW(value, L"zh-TW") == 0 ? 2u : (lstrcmpiW(value, L"en") == 0 ? 1u : 0u);
        }
        if (ReadEnv(L"RACING_MASTER_MOD_TEXT_SIZE", value, 64)) {
            effective.textSize = lstrcmpiW(value, L"small") == 0 ? 0u : (lstrcmpiW(value, L"large") == 0 ? 2u : 1u);
        }
        if (ReadEnv(L"RACING_MASTER_MOD_PANEL", value, 64) && lstrcmpiW(value, L"open") == 0) {
            Shared().panelOpen.store(true);
        }
        if (ReadEnv(L"RACING_MASTER_MOD_TEST_PANEL_FAULT", value, 64)) {
            g_testFaultAtPanelFrame = ParseDigits(value);
            if (g_testFaultAtPanelFrame) {
                Log("test host: a deliberate fault is scheduled for panel frame %u, between the two state swaps",
                    g_testFaultAtPanelFrame);
            }
        }
        if (ReadEnv(L"RACING_MASTER_MOD_TEST_PANEL_OFF", value, 64)) {
            if (lstrcmpiW(value, L"wndproc") == 0) {
                InputScheduleTestPanelOff(TestPanelOff::WindowProcedureFault);
                Log("test host: the mod's part of the window procedure will fail deliberately at the first F12");
            } else if (lstrcmpiW(value, L"assert") == 0) {
                InputScheduleTestPanelOff(TestPanelOff::ImGuiAssert);
                Log("test host: the panel will raise a deliberate Dear ImGui assertion after the first F12");
            }
        }
    }

    // Hotkeys: each [keys] value is checked here; a refused one keeps its default and says why.
    static const char* const kActionNames[kCommandCount] = {"panel", "stop", "previous", "play_pause", "next", "loop", "shuffle"};
    HotkeyTable* table = new HotkeyTable(DefaultHotkeys());   // published once, never freed
    for (int i = 0; i < kCommandCount; ++i) {
        Hotkey key;
        if (ParseHotkey(effective.keys[i], &key)) {
            table->keys[i] = key;
        } else {
            Log("settings: \"%s=%s\" under [keys] is not a key name the mod knows; the default stays. Accepted: %s",
                kActionNames[i], effective.keys[i], HotkeyAcceptedNames());
        }
    }
    table->mediaKeys = effective.mediaKeys;
    char names[kCommandCount][32];
    for (int i = 0; i < kCommandCount; ++i) {
        HotkeyDisplayName(table->keys[i], "none", names[i], sizeof(names[i]));
    }
    {
        Log("panel: keys %s panel, %s stop, %s previous, %s play or pause, %s next, %s repeat, %s shuffle; media keys %s",
            names[0], names[1], names[2], names[3], names[4], names[5], names[6], table->mediaKeys ? "on" : "off");
    }
    Shared().hotkeys.store(table, std::memory_order_release);

    PanelConfigure(effective, *table, testHost);

    // Loaded here by full path so the System32 copy is the one used and a missing file becomes a
    // log line, not a delay-load fault on the frame thread.
    wchar_t compiler[MAX_PATH];
    const UINT length = GetSystemDirectoryW(compiler, MAX_PATH);
    if (length > 0 && length < MAX_PATH - 24) {
        lstrcatW(compiler, L"\\d3dcompiler_47.dll");
        g_compilerLoaded = LoadLibraryExW(compiler, nullptr, 0) != nullptr;
        if (!g_compilerLoaded) {
            Log("panel: d3dcompiler_47.dll could not be loaded from the Windows folder (error %lu)", GetLastError());
        }
    }
    if (g_compilerLoaded) {
        static const char* const kCompilerNames[] = {"d3dcompiler_47.dll", "D3DCOMPILER_47.dll"};
        ResolveDelayLoaded(kCompilerNames, 2, "the shader compiler");
    }
    static const char* const kUser32Names[] = {"USER32.dll", "user32.dll"};
    ResolveDelayLoaded(kUser32Names, 2, "the window functions");
    if (g_mode == UiModeKeys) {
        // Minimal mode: the keys are global hotkeys on the mod's own thread, not the window
        // procedure. user32 is resolved just above (on the frame thread), so the hotkey thread
        // never resolves a delay-loaded DLL first.
        global_hotkeys::Start(table);
    }
    UiSettingsStartWriter();
    g_started = true;
}

bool PanelOnFrame(IDXGISwapChain* swapChain, UINT presentFlags, FrameRefs& refs) {
    if (!g_started || g_dead.load(std::memory_order_relaxed)) {
        return false;
    }
    if (WindowProcedureFaulted()) {
        // Without the window procedure nothing could close an open panel or reach the music any
        // more (Insert and F5-F9 now go to the game), so the panel closes for the session, as
        // after any other panel error, and the worker pauses the music.
        Log("panel: the mod's part of the game window procedure failed; clicks and keys go to the game from now on, "
            "and the panel is closed for this session");
        PanelOnFault();
        return false;
    }
    // Hotkeys run every frame, whether or not the panel is drawn.
    SharedInput& shared = Shared();
    for (int i = 0; i < kCommandCount; ++i) {
        const uint32_t times = shared.pending[i].exchange(0, std::memory_order_relaxed);
        for (uint32_t t = 0; t < times && t < 8; ++t) {
            RunMusicCommand(static_cast<Command>(i));
        }
    }

    if (g_init == InitState::NotYet) {
        // Start-up waits for an ordinary frame (not a re-shown one) on the login screen, so its
        // few milliseconds never land in a race.
        if (presentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
            return false;
        }
        const bool ok = InitOnFrameThread(swapChain, refs);
        ReleaseFrameRefs(refs);
        g_init = ok ? InitState::Ready : InitState::Unavailable;
        if (!ok) {
            return false;
        }
        if (g_testHost) {
            WriteTestLayout(false, 1.0f, 1.0f);   // tells the test the panel is ready
        }
    }
    const bool open = shared.panelOpen.load(std::memory_order_relaxed);
    if (g_init != InitState::Ready) {
        if (open && !g_openWithoutDrawLogged) {
            g_openWithoutDrawLogged = true;
            Log("panel: the panel was asked to open but cannot be drawn (see the earlier panel line); the music keys "
                "still work");
        }
        return false;
    }
    if (swapChain != g_lastSwapChain) {
        // The game presents through a new swap chain: follow its window.
        g_lastSwapChain = swapChain;
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow != g_window) {
            g_window = desc.OutputWindow;
            g_publishedWindow.store(g_window, std::memory_order_release);
            if (g_mode == UiModeOn) {
                InstallWindowSubclass(g_window);
            }
        }
    }

    const ULONGLONG now = GetTickCount64();
    if (!PanelWantsFrame(open, now)) {
        if (g_wasDrawing) {
            ClearRects();
            g_wasDrawing = false;
        }
        if (g_testHost) {
            WriteTestLayout(false, 1.0f, 1.0f);   // state still changes through hotkeys; written only when it did
        }
        // Events queued while nothing is drawn are stale by the time the panel opens again.
        InputEvent stale;
        while (shared.queue.Pop(&stale)) {
            if (stale.type == InputEvent::FocusLost) {
                g_imguiFocused = false;
                ImGui::GetIO().AddFocusEvent(false);
            }
        }
        return false;
    }
    const bool drawn = DrawFrame(swapChain, refs, open);
    ReleaseFrameRefs(refs);
    if (drawn) {
        if (open) {
            shared.lastDrawTick.store(GetTickCount64(), std::memory_order_relaxed);
        }
        g_wasDrawing = true;
    }
    if (ImGuiAssertTripped()) {
        // Switched off after the frame completed, so the state swap above finished normally.
        PanelOnFault();
    }
    return drawn;
}

void PanelOnFault() {
    g_dead.store(true, std::memory_order_relaxed);
    SharedInput& shared = Shared();
    shared.enabled.store(false, std::memory_order_relaxed);
    shared.panelOpen.store(false, std::memory_order_relaxed);
    shared.lastDrawTick.store(0, std::memory_order_relaxed);
    for (int i = 0; i < kMaxPanelRects; ++i) {
        shared.rects[i].store(0, std::memory_order_relaxed);
    }
}

HWND PanelGameWindow() {
    return g_publishedWindow.load(std::memory_order_acquire);
}

bool PanelSwitchedOff() {
    return g_dead.load(std::memory_order_relaxed) || WindowProcedureFaulted();
}

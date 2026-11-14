// Developer test host. Stands in for the game so the loader can be checked without it.
//
// It imports version.dll (so the stand-in next to this exe is loaded the way the game loads it),
// renders with Direct3D 11 to a window that is never shown, and reports what it found. The mod
// only wakes up when RACING_MASTER_MOD_TEST_HOST=1 is set and this file is named testhost.exe.
//
// Usage: testhost.exe <mode> <output folder> [seconds]
//   game    The game's swap chain layout (2558x1360, R10G10B10A2_UNORM, 2 buffers, FLIP_DISCARD),
//           presented from a worker thread like the engine does. Runs checks T1-T7 and exits 0 if
//           every one passed. T7 (fault recovery) runs only when RACING_MASTER_MOD_TEST_FAULT=<frame>
//           is set; [seconds] is how long to wait for the hook to attach.
//   legacy  The old layout (640x360, B8G8R8A8_UNORM, 1 buffer, SEQUENTIAL): marker check only (T8).
//   quick   Game layout; presents for [seconds] and prints whether the marker appeared. Exits 0 if
//           forwarding works and the stand-in is in use; the caller decides what the marker means.
//   panel   Game layout; the music panel. testhost.exe panel <output folder> <seconds> <script> [name]
//           (scripts and checks: see "music panel runs" below). Screenshots go to <output folder>\screens.
//   media   testhost.exe media <output folder> <seconds> <scenario>. No rendering. Publishes the
//           silent fake player (fake_player.h) and waits up to [seconds] for the mod's media test
//           script (RACING_MASTER_MOD_TEST_MEDIA_SCRIPT) to log "music apps test: finished", then
//           prints what the fake player received. Scenarios: basic, ignore-pause, no-repeat,
//           vanish, late (the player appears 2 s after start), guard, lifecycle. The mod's test
//           script decides what is sent; the scenario decides how the fake player behaves.
//           Exits 0 if the script finished.
//
// Reading a flip-model frame back: the hook draws into buffer 0 before the real Present runs.
// A normal Present then rotates the buffers, so the host clears buffer 0 and presents it with
// DXGI_PRESENT_DO_NOT_SEQUENCE: the hook still draws, DXGI does not rotate, and buffer 0 can be
// copied to a staging texture. At least one ordinary Present must come first on a flip-model chain.

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wincodec.h>

#include "fake_player.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

struct Layout {
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT buffers;
    DXGI_SWAP_EFFECT effect;
};

const Layout kGameLayout = {2558, 1360, DXGI_FORMAT_R10G10B10A2_UNORM, 2, DXGI_SWAP_EFFECT_FLIP_DISCARD};
const Layout kLegacyLayout = {640, 360, DXGI_FORMAT_B8G8R8A8_UNORM, 1, DXGI_SWAP_EFFECT_SEQUENTIAL};

const FLOAT kClearColour[4] = {0.10f, 0.20f, 0.45f, 1.0f};
const FLOAT kPanelClearColour[4] = {0.45f, 0.45f, 0.45f, 1.0f};   // mid-grey behind the panel screenshots
const FLOAT* g_clearColour = kClearColour;

bool g_allPassed = true;

void Check(const char* name, bool ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) g_allPassed = false;
}

// Paths may contain Chinese characters, which printf's %ls cannot write; print them as UTF-8.
const char* Utf8(const wchar_t* text, char* buffer, int size) {
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, size, nullptr, nullptr) <= 0) buffer[0] = '\0';
    return buffer;
}

// ---- version.dll checks -------------------------------------------------------------------------

bool CheckForwarding() {
    wchar_t kernel[MAX_PATH];
    GetSystemDirectoryW(kernel, MAX_PATH);
    lstrcatW(kernel, L"\\kernel32.dll");

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(kernel, &handle);
    if (size == 0) {
        printf("forwarding: FAIL (GetFileVersionInfoSizeW returned 0, error %lu)\n", GetLastError());
        return false;
    }
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoLen = 0;
    if (!GetFileVersionInfoW(kernel, 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoLen) || !info) {
        printf("forwarding: FAIL (could not read kernel32 version)\n");
        return false;
    }
    printf("forwarding: OK (kernel32.dll is %u.%u.%u.%u)\n", HIWORD(info->dwFileVersionMS),
           LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return true;
}

// The version.dll in use must be the one next to this exe, not the Windows one.
bool CheckStandInInUse() {
    wchar_t loaded[MAX_PATH] = L"";
    GetModuleFileNameW(GetModuleHandleW(L"version.dll"), loaded, MAX_PATH);
    wchar_t expected[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, expected, MAX_PATH);
    wchar_t* slash = wcsrchr(expected, L'\\');
    if (slash) lstrcpyW(slash + 1, L"version.dll");
    const bool ok = lstrcmpiW(loaded, expected) == 0;
    char loaded8[MAX_PATH * 3];
    printf("stand-in in use: %s (%s)\n", ok ? "yes" : "no", Utf8(loaded, loaded8, sizeof(loaded8)));
    return ok;
}

// ---- Direct3D helpers -----------------------------------------------------------------------------

struct Host {
    HWND window = nullptr;
    HWND secondWindow = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIFactory* factory = nullptr;
    IDXGISwapChain* chain = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned presented = 0;   // presents on the main chain that the mod also counts (TEST excluded)
};

bool CreateDeviceAndFactory(Host& host) {
    const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &wanted, 1, D3D11_SDK_VERSION,
                                   &host.device, nullptr, &host.context);
    if (FAILED(hr)) {
        printf("could not create the Direct3D 11 device (0x%08lx)\n", static_cast<unsigned long>(hr));
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    hr = host.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&host.factory));
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();
    if (FAILED(hr)) {
        printf("could not reach the DXGI factory (0x%08lx)\n", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

IDXGISwapChain* CreateChain(Host& host, HWND window, const Layout& layout) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = layout.buffers;
    desc.BufferDesc.Width = layout.width;
    desc.BufferDesc.Height = layout.height;
    desc.BufferDesc.Format = layout.format;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = layout.effect;
    IDXGISwapChain* chain = nullptr;
    const HRESULT hr = host.factory->CreateSwapChain(host.device, &desc, &chain);
    if (FAILED(hr)) {
        printf("could not create the swap chain (0x%08lx)\n", static_cast<unsigned long>(hr));
        return nullptr;
    }
    return chain;
}

bool AcquireBackBuffer(Host& host) {
    HRESULT hr = host.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&host.backBuffer));
    if (SUCCEEDED(hr)) hr = host.device->CreateRenderTargetView(host.backBuffer, nullptr, &host.view);
    return SUCCEEDED(hr);
}

void ReleaseBackBuffer(Host& host) {
    if (host.view) host.view->Release();
    if (host.backBuffer) host.backBuffer->Release();
    host.view = nullptr;
    host.backBuffer = nullptr;
}

void Clear(Host& host) {
    host.context->ClearRenderTargetView(host.view, g_clearColour);
}

HRESULT Present(Host& host, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) ++host.presented;
    return host.chain->Present(0, flags);
}

struct Rgb {
    float r, g, b;
};

// Reads one pixel of a texture through a staging copy, as a 0..1 colour whatever the format.
bool ReadPixel(Host& host, ID3D11Texture2D* texture, UINT x, UINT y, Rgb* out) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    const DXGI_FORMAT format = desc.Format;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(host.device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    host.context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    bool ok = false;
    if (SUCCEEDED(host.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const BYTE* p = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch + x * 4;
        UINT32 v = 0;
        memcpy(&v, p, 4);
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            *out = {(v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f};
        } else if (format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            *out = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f};
        } else {
            *out = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
        }
        host.context->Unmap(staging, 0);
        ok = true;
    }
    staging->Release();
    return ok;
}

// 1 = the marker is in buffer 0 of the chain, 0 = it is not, -1 = could not read.
// Marker = white edge at (28,28), dark centre at (48,48), untouched clear colour at (200,200).
int MarkerState(Host& host, IDXGISwapChain* chain, bool print) {
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return -1;
    Rgb edge = {}, centre = {}, outside = {};
    const bool ok = ReadPixel(host, buffer, 28, 28, &edge) && ReadPixel(host, buffer, 48, 48, &centre) &&
                    ReadPixel(host, buffer, 200, 200, &outside);
    buffer->Release();
    if (!ok) return -1;
    if (print) {
        printf("  pixel on marker edge (28,28): %.2f %.2f %.2f; centre (48,48): %.2f %.2f %.2f; outside (200,200): "
               "%.2f %.2f %.2f\n",
               edge.r, edge.g, edge.b, centre.r, centre.g, centre.b, outside.r, outside.g, outside.b);
    }
    const bool marker = edge.r > 0.94f && edge.g > 0.94f && edge.b > 0.94f && centre.r < 0.15f && centre.g < 0.15f &&
                        centre.b < 0.15f && outside.b > 0.35f && outside.r < 0.25f;
    return marker ? 1 : 0;
}

// The read-back recipe for the main chain: one ordinary present, then a cleared buffer 0
// presented without sequencing, then the pixel read.
int MarkerAfterPresent(Host& host, bool print) {
    Clear(host);
    Present(host, 0);
    Clear(host);
    Present(host, DXGI_PRESENT_DO_NOT_SEQUENCE);
    return MarkerState(host, host.chain, print);
}

// Writes buffer 0 of the main chain as an 8-bit BMP so a failing run can be looked at.
void SaveFrame(Host& host, const char* path) {
    D3D11_TEXTURE2D_DESC desc = {};
    host.backBuffer->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(host.device->CreateTexture2D(&desc, nullptr, &staging))) return;
    host.context->CopyResource(staging, host.backBuffer);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(host.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        FILE* out = nullptr;
        if (fopen_s(&out, path, "wb") == 0 && out) {
            BITMAPFILEHEADER file = {};
            BITMAPINFOHEADER info = {};
            info.biSize = sizeof(info);
            info.biWidth = static_cast<LONG>(desc.Width);
            info.biHeight = -static_cast<LONG>(desc.Height);
            info.biPlanes = 1;
            info.biBitCount = 32;
            file.bfType = 0x4D42;
            file.bfOffBits = sizeof(file) + sizeof(info);
            file.bfSize = file.bfOffBits + desc.Width * desc.Height * 4;
            fwrite(&file, sizeof(file), 1, out);
            fwrite(&info, sizeof(info), 1, out);
            std::vector<BYTE> row(desc.Width * 4);
            for (UINT y = 0; y < desc.Height; ++y) {
                const BYTE* src = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch;
                for (UINT x = 0; x < desc.Width; ++x) {
                    UINT32 v = 0;
                    memcpy(&v, src + x * 4, 4);
                    BYTE* dst = row.data() + x * 4;
                    if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                        dst[0] = static_cast<BYTE>(((v >> 20) & 1023) >> 2);
                        dst[1] = static_cast<BYTE>(((v >> 10) & 1023) >> 2);
                        dst[2] = static_cast<BYTE>((v & 1023) >> 2);
                        dst[3] = 255;
                    } else {
                        memcpy(dst, src + x * 4, 4);
                    }
                }
                fwrite(row.data(), 4, desc.Width, out);
            }
            fclose(out);
        }
        host.context->Unmap(staging, 0);
    }
    staging->Release();
}

bool WaitForMarker(Host& host, int seconds) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    while (GetTickCount64() < end) {
        if (MarkerAfterPresent(host, false) == 1) return true;
        Sleep(4);
    }
    return false;
}

// ---- the runs -------------------------------------------------------------------------------------

struct RenderJob {
    Host* host;
    const char* mode;
    const char* outputFolder;
    int seconds;
    const char* script;   // panel mode
    const char* name;     // panel mode
    volatile LONG done;
};

void RunGameChecks(Host& host, const char* outputFolder, int seconds) {
    host.chain = CreateChain(host, host.window, kGameLayout);
    if (!host.chain || !AcquireBackBuffer(host)) {
        g_allPassed = false;
        return;
    }
    host.format = kGameLayout.format;

    // T1: the hook attaches while this thread presents, and the marker lands in the frame.
    const bool hooked = WaitForMarker(host, seconds);
    MarkerState(host, host.chain, true);
    Check("T1 marker on the game layout (2558x1360 R10G10B10A2 x2 FLIP_DISCARD, worker thread)", hooked);
    if (outputFolder) {
        char bmp[MAX_PATH];
        snprintf(bmp, sizeof(bmp), "%s\\game-frame.bmp", outputFolder);
        SaveFrame(host, bmp);
    }
    if (!hooked) {
        ReleaseBackBuffer(host);
        return;   // nothing below can pass without the hook
    }

    // T2: Present1 is hooked as well.
    IDXGISwapChain1* chain1 = nullptr;
    if (SUCCEEDED(host.chain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&chain1)))) {
        DXGI_PRESENT_PARAMETERS parameters = {};
        Clear(host);
        chain1->Present1(0, 0, &parameters);
        ++host.presented;
        Clear(host);
        chain1->Present1(0, DXGI_PRESENT_DO_NOT_SEQUENCE, &parameters);
        ++host.presented;
        Check("T2 marker drawn through Present1", MarkerState(host, host.chain, false) == 1);
        chain1->Release();
    } else {
        Check("T2 marker drawn through Present1 (IDXGISwapChain1 not available)", false);
    }

    // T3: a Present with DXGI_PRESENT_TEST is not a frame; nothing is drawn.
    Clear(host);
    Present(host, DXGI_PRESENT_TEST);
    Check("T3 nothing drawn on Present(DXGI_PRESENT_TEST)", MarkerState(host, host.chain, false) == 0);

    // T4: the game can resize while the mod is active, without any Flush; the marker comes back.
    const UINT sizes[3][2] = {{1920, 1080}, {1280, 720}, {2558, 1360}};
    for (int round = 0; round < 3; ++round) {
        Clear(host);
        Present(host, 0);
        ReleaseBackBuffer(host);
        const HRESULT hr = host.chain->ResizeBuffers(kGameLayout.buffers, sizes[round][0], sizes[round][1],
                                                     DXGI_FORMAT_UNKNOWN, 0);
        char name[96];
        snprintf(name, sizeof(name), "T4 ResizeBuffers to %ux%u with the mod active (0x%08lx)", sizes[round][0],
                 sizes[round][1], static_cast<unsigned long>(hr));
        const bool resized = SUCCEEDED(hr) && AcquireBackBuffer(host);
        Check(name, resized && MarkerAfterPresent(host, false) == 1);
        if (!resized) {
            return;
        }
    }

    // T5: a second swap chain in the same process (its window would qualify) is left alone.
    IDXGISwapChain* second = CreateChain(host, host.secondWindow, kGameLayout);
    if (second) {
        ID3D11Texture2D* secondBuffer = nullptr;
        ID3D11RenderTargetView* secondView = nullptr;
        second->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&secondBuffer));
        host.device->CreateRenderTargetView(secondBuffer, nullptr, &secondView);
        for (int i = 0; i < 5; ++i) {
            Clear(host);
            Present(host, 0);
            host.context->ClearRenderTargetView(secondView, kClearColour);
            second->Present(0, 0);
        }
        host.context->ClearRenderTargetView(secondView, kClearColour);
        second->Present(0, DXGI_PRESENT_DO_NOT_SEQUENCE);
        Check("T5 second swap chain in the process is not drawn on", MarkerState(host, second, false) == 0);
        Check("T5 main swap chain keeps the marker", MarkerAfterPresent(host, false) == 1);
        secondView->Release();
        secondBuffer->Release();
        second->Release();
    } else {
        Check("T5 second swap chain could be created", false);
    }

    // T6: the function table is read-only again after the hook.
    MEMORY_BASIC_INFORMATION memory = {};
    VirtualQuery(*reinterpret_cast<void**>(host.chain), &memory, sizeof(memory));
    printf("  swap chain table page protection: 0x%lx\n", memory.Protect);
    Check("T6 swap chain table is PAGE_READONLY after hooking", memory.Protect == PAGE_READONLY);

    // T7: the mod was told to fault at a frame while it holds references. It must log, release,
    // switch itself off and leave this process alive and able to resize.
    char faultText[16] = "";
    if (GetEnvironmentVariableA("RACING_MASTER_MOD_TEST_FAULT", faultText, sizeof(faultText)) > 0) {
        const unsigned faultFrame = static_cast<unsigned>(atoi(faultText));
        const ULONGLONG end = GetTickCount64() + 15000;
        bool markerGone = false;
        while (GetTickCount64() < end) {
            for (int i = 0; i < 20; ++i) {
                Clear(host);
                Present(host, 0);
            }
            if (host.presented > faultFrame && MarkerAfterPresent(host, false) == 0) {
                markerGone = true;
                break;
            }
        }
        printf("  presented %u frames; fault was scheduled for frame %u\n", host.presented, faultFrame);
        Check("T7 after the injected fault the process is alive and the marker is gone", markerGone);
        ReleaseBackBuffer(host);
        const HRESULT hr = host.chain->ResizeBuffers(kGameLayout.buffers, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0);
        printf("  ResizeBuffers after the fault: 0x%08lx\n", static_cast<unsigned long>(hr));
        Check("T7 ResizeBuffers succeeds after the fault (no reference leaked)", SUCCEEDED(hr));
        if (SUCCEEDED(hr)) AcquireBackBuffer(host);
    }

    ReleaseBackBuffer(host);
}

void RunLegacyCheck(Host& host, int seconds) {
    host.chain = CreateChain(host, host.window, kLegacyLayout);
    if (!host.chain || !AcquireBackBuffer(host)) {
        g_allPassed = false;
        return;
    }
    host.format = kLegacyLayout.format;
    // SEQUENTIAL with one buffer keeps the contents after Present, so buffer 0 can be read directly.
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    bool found = false;
    while (GetTickCount64() < end && !found) {
        Clear(host);
        Present(host, 0);
        found = MarkerState(host, host.chain, false) == 1;
        Sleep(4);
    }
    MarkerState(host, host.chain, true);
    Check("T8 marker on the old layout (640x360 B8G8R8A8 x1 SEQUENTIAL)", found);
    ReleaseBackBuffer(host);
}

void RunQuick(Host& host, int seconds) {
    host.chain = CreateChain(host, host.window, kGameLayout);
    if (!host.chain || !AcquireBackBuffer(host)) {
        g_allPassed = false;
        return;
    }
    host.format = kGameLayout.format;
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    bool found = false;
    while (GetTickCount64() < end) {
        if (MarkerAfterPresent(host, false) == 1) found = true;
        Sleep(8);
    }
    printf("presented %u frames\n", host.presented);
    printf("marker: %s\n", found ? "found" : "not found");
    ReleaseBackBuffer(host);
}

// ---- music panel runs -------------------------------------------------------------------------------
//
// testhost.exe panel <output folder> <seconds> <script> [name]
//   shots   <prefix>  bar, list, settings row and "Other app" screenshots (PNG) with pixel checks
//   shot1   <name>    one screenshot of the state the environment sets up
//   resize            ResizeBuffers to four sizes with the panel and list open
//   fault             a fault inside the panel code (RACING_MASTER_MOD_TEST_PANEL_FAULT)
//   display           [panel] mode=display: drawn, nothing taken from the game
//   input             clicks, pass-through, hotkeys, Insert, ClearInputMouse after a gap, text field
//
// Every message is posted to this program's own hidden window with PostMessageW: it goes through
// the mod's window procedure exactly as the game's messages would. Nothing leaves the process and
// nothing is shown. The mod reports its state and widget centres in test-layout.txt.
//
// Before every Present the host binds a set of "game" pipeline state and checks afterwards that
// all of it is still bound (the panel draws inside its own swapped state).

std::atomic<int> g_gotKeyDown[256];
std::atomic<int> g_gotKeyUp[256];
std::atomic<int> g_gotLDown{0}, g_gotLUp{0}, g_gotMove{0}, g_gotWheel{0}, g_gotChar{0}, g_gotActivateApp{0};
std::atomic<unsigned> g_gotSync{0};
constexpr UINT kSyncMessage = WM_APP + 7;

// The host window's own procedure: what "the game" receives after the mod's window procedure.
LRESULT CALLBACK HostWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: if (wParam < 256) ++g_gotKeyDown[wParam]; break;
        case WM_KEYUP: case WM_SYSKEYUP: if (wParam < 256) ++g_gotKeyUp[wParam]; break;
        case WM_CHAR: ++g_gotChar; break;
        case WM_LBUTTONDOWN: ++g_gotLDown; break;
        case WM_LBUTTONUP: ++g_gotLUp; break;
        case WM_MOUSEMOVE: ++g_gotMove; break;
        case WM_MOUSEWHEEL: ++g_gotWheel; break;
        case WM_ACTIVATEAPP: ++g_gotActivateApp; break;
        case kSyncMessage: g_gotSync.store(static_cast<unsigned>(wParam)); return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct GameState {
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11DepthStencilState* depthStencil = nullptr;
    ID3D11ComputeShader* compute = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11Texture2D* sceneTexture = nullptr;
    ID3D11RenderTargetView* sceneTarget = nullptr;
};

GameState g_game;
int g_stateChecks = 0;
int g_stateFailures = 0;
unsigned g_syncSequence = 0;
const FLOAT kBlendFactor[4] = {0.1f, 0.2f, 0.3f, 0.4f};
const D3D11_VIEWPORT kViewport = {3.0f, 5.0f, 17.0f, 19.0f, 0.25f, 0.75f};
const D3D11_RECT kScissor = {1, 2, 33, 44};

bool CreateGameState(Host& host) {
    D3D11_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D11_FILL_WIREFRAME;
    rasterizer.CullMode = D3D11_CULL_FRONT;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_GREATER;
    depth.StencilEnable = TRUE;
    depth.StencilReadMask = 0x0f;
    depth.StencilWriteMask = 0xff;
    depth.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    depth.FrontFace.StencilPassOp = depth.FrontFace.StencilFailOp = depth.FrontFace.StencilDepthFailOp =
        D3D11_STENCIL_OP_KEEP;
    depth.BackFace = depth.FrontFace;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_ANISOTROPIC;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR;
    sampler.MaxAnisotropy = 8;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC buffer = {};
    buffer.ByteWidth = 64;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = texture.Height = 64;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    static const char kCompute[] = "[numthreads(1,1,1)] void main() {}";
    ID3DBlob* blob = nullptr;
    HRESULT hr = D3DCompile(kCompute, sizeof(kCompute) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob,
                            nullptr);
    if (SUCCEEDED(hr)) {
        hr = host.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_game.compute);
    }
    if (blob) blob->Release();
    if (SUCCEEDED(hr)) hr = host.device->CreateRasterizerState(&rasterizer, &g_game.rasterizer);
    if (SUCCEEDED(hr)) hr = host.device->CreateBlendState(&blend, &g_game.blend);
    if (SUCCEEDED(hr)) hr = host.device->CreateDepthStencilState(&depth, &g_game.depthStencil);
    if (SUCCEEDED(hr)) hr = host.device->CreateSamplerState(&sampler, &g_game.sampler);
    if (SUCCEEDED(hr)) hr = host.device->CreateBuffer(&buffer, nullptr, &g_game.constants);
    if (SUCCEEDED(hr)) hr = host.device->CreateTexture2D(&texture, nullptr, &g_game.sceneTexture);
    if (SUCCEEDED(hr)) hr = host.device->CreateRenderTargetView(g_game.sceneTexture, nullptr, &g_game.sceneTarget);
    return SUCCEEDED(hr);
}

void SetGameState(ID3D11DeviceContext* context) {
    context->OMSetRenderTargets(1, &g_game.sceneTarget, nullptr);
    context->RSSetState(g_game.rasterizer);
    context->RSSetViewports(1, &kViewport);
    context->RSSetScissorRects(1, &kScissor);
    context->OMSetBlendState(g_game.blend, kBlendFactor, 0x0000ffff);
    context->OMSetDepthStencilState(g_game.depthStencil, 7);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ);
    context->CSSetShader(g_game.compute, nullptr, 0);
    context->PSSetSamplers(0, 1, &g_game.sampler);
    context->PSSetSamplers(2, 1, &g_game.sampler);
    context->VSSetConstantBuffers(0, 1, &g_game.constants);
    context->VSSetConstantBuffers(1, 1, &g_game.constants);
}

// The 14 pieces of state SetGameState bound, read back after Present.
bool GameStateIntact(ID3D11DeviceContext* context) {
    bool ok = true;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11DepthStencilView* depthView = nullptr;
    context->OMGetRenderTargets(1, &target, &depthView);
    ok = ok && target == g_game.sceneTarget && depthView == nullptr;
    if (target) target->Release();
    if (depthView) depthView->Release();
    ID3D11RasterizerState* rasterizer = nullptr;
    context->RSGetState(&rasterizer);
    ok = ok && rasterizer == g_game.rasterizer;
    if (rasterizer) rasterizer->Release();
    UINT count = 1;
    D3D11_VIEWPORT viewport = {};
    context->RSGetViewports(&count, &viewport);
    ok = ok && count == 1 && memcmp(&viewport, &kViewport, sizeof(viewport)) == 0;
    count = 1;
    D3D11_RECT scissor = {};
    context->RSGetScissorRects(&count, &scissor);
    ok = ok && count == 1 && memcmp(&scissor, &kScissor, sizeof(scissor)) == 0;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4] = {};
    UINT mask = 0;
    context->OMGetBlendState(&blend, factor, &mask);
    ok = ok && blend == g_game.blend && memcmp(factor, kBlendFactor, sizeof(factor)) == 0 && mask == 0x0000ffff;
    if (blend) blend->Release();
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    context->OMGetDepthStencilState(&depth, &stencilRef);
    ok = ok && depth == g_game.depthStencil && stencilRef == 7;
    if (depth) depth->Release();
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&topology);
    ok = ok && topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
    ID3D11ComputeShader* compute = nullptr;
    context->CSGetShader(&compute, nullptr, nullptr);
    ok = ok && compute == g_game.compute;
    if (compute) compute->Release();
    ID3D11SamplerState* samplers[3] = {};
    context->PSGetSamplers(0, 3, samplers);
    ok = ok && samplers[0] == g_game.sampler && samplers[1] == nullptr && samplers[2] == g_game.sampler;
    for (ID3D11SamplerState* s : samplers) if (s) s->Release();
    ID3D11Buffer* buffers[2] = {};
    context->VSGetConstantBuffers(0, 2, buffers);
    ok = ok && buffers[0] == g_game.constants && buffers[1] == g_game.constants;
    for (ID3D11Buffer* b : buffers) if (b) b->Release();
    ID3D11VertexShader* vertex = nullptr;
    context->VSGetShader(&vertex, nullptr, nullptr);
    ok = ok && vertex == nullptr;
    if (vertex) vertex->Release();
    ID3D11PixelShader* pixel = nullptr;
    context->PSGetShader(&pixel, nullptr, nullptr);
    ok = ok && pixel == nullptr;
    if (pixel) pixel->Release();
    ID3D11InputLayout* layout = nullptr;
    context->IAGetInputLayout(&layout);
    ok = ok && layout == nullptr;
    if (layout) layout->Release();
    ID3D11ShaderResourceView* views[4] = {};
    context->PSGetShaderResources(0, 4, views);
    ok = ok && !views[0] && !views[1] && !views[2] && !views[3];
    for (ID3D11ShaderResourceView* v : views) if (v) v->Release();
    return ok;
}

// One "game frame" on the main chain: clear, bind the game's state, present, check the state.
HRESULT PanelFrame(Host& host, UINT flags) {
    Clear(host);
    SetGameState(host.context);
    const HRESULT hr = Present(host, flags);
    ++g_stateChecks;
    if (!GameStateIntact(host.context)) {
        ++g_stateFailures;
    }
    return hr;
}

void Pump(Host& host, int frames) {
    for (int i = 0; i < frames; ++i) {
        PanelFrame(host, 0);
        Sleep(2);
    }
}

// Waits until the window thread has handled every message posted before this call, presenting
// frames meanwhile (or not, to let the panel's heartbeat lapse), then lets the panel run a few
// frames so it has taken the queued input in.
bool Sync(Host& host, bool present = true) {
    const unsigned sequence = ++g_syncSequence;
    PostMessageW(host.window, kSyncMessage, sequence, 0);
    const ULONGLONG end = GetTickCount64() + 3000;
    while (g_gotSync.load() != sequence && GetTickCount64() < end) {
        if (present) PanelFrame(host, 0);
        Sleep(2);
    }
    if (present) Pump(host, 6);
    return g_gotSync.load() == sequence;
}

struct LayoutFile {
    std::map<std::string, std::string> values;
    std::map<std::string, POINT> items;
    std::vector<RECT> windows;   // back-buffer pixels
    std::vector<RECT> hits;      // client coordinates
    bool ok = false;
    int Int(const char* key, int fallback = -999) const {
        auto it = values.find(key);
        return it == values.end() ? fallback : atoi(it->second.c_str());
    }
    std::string Text(const char* key) const {
        auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    }
    bool Has(const char* item) const { return items.count(item) != 0; }
};

std::wstring ModFolder() {
    wchar_t base[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    return std::wstring(base) + L"\\RacingMasterMod";
}

std::string ReadWholeFile(const std::wstring& path) {
    std::string text;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return text;
    char buffer[16384];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) text.append(buffer, read);
    CloseHandle(file);
    return text;
}

LayoutFile ReadLayout() {
    LayoutFile layout;
    const std::string text = ReadWholeFile(ModFolder() + L"\\test-layout.txt");
    if (text.empty()) return layout;
    layout.ok = true;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        const std::string key = line.substr(0, space);
        const std::string rest = line.substr(space + 1);
        if (key == "item") {
            char name[64] = "";
            int x = 0, y = 0;
            if (sscanf_s(rest.c_str(), "%63s %d %d", name, static_cast<unsigned>(sizeof(name)), &x, &y) == 3) {
                layout.items[name] = POINT{x, y};
            }
        } else if (key == "window" || key == "hit") {
            int x = 0, y = 0, w = 0, h = 0;
            if (sscanf_s(rest.c_str(), "%d %d %d %d", &x, &y, &w, &h) == 4) {
                (key == "window" ? layout.windows : layout.hits).push_back(RECT{x, y, x + w, y + h});
            }
        } else {
            layout.values[key] = rest;
        }
    }
    return layout;
}

// Presents frames until the layout file satisfies `condition` or the time runs out.
template <typename Condition>
bool WaitLayout(Host& host, Condition condition, int milliseconds = 4000) {
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    for (;;) {
        const LayoutFile layout = ReadLayout();
        if (layout.ok && condition(layout)) return true;
        if (GetTickCount64() >= end) return false;
        PanelFrame(host, 0);
        Sleep(3);
    }
}

POINT Item(const char* name) {
    const LayoutFile layout = ReadLayout();
    auto it = layout.items.find(name);
    if (it == layout.items.end()) {
        printf("  (no widget named %s in the layout file)\n", name);
        return POINT{-1, -1};
    }
    return it->second;
}

void PostKey(Host& host, UINT vk, bool down, bool repeat = false) {
    LPARAM l = 1 | (static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16);
    if (repeat) l |= 1LL << 30;
    if (!down) l |= (1LL << 30) | (1LL << 31);
    PostMessageW(host.window, down ? WM_KEYDOWN : WM_KEYUP, vk, l);
}

void Key(Host& host, UINT vk) {
    PostKey(host, vk, true);
    PostKey(host, vk, false);
    Sync(host);
}

void Move(Host& host, POINT p) {
    PostMessageW(host.window, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
}

// While the panel is open, waits until it has published its clickable rectangles: on its first
// drawn frame ImGui keeps auto-sized windows hidden, so nothing is clickable yet (correctly: nothing
// is on screen either), and a click posted then goes to the game.
void Click(Host& host, POINT p) {
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") != 1 || !l.hits.empty(); }, 1000);
    Move(host, p);
    PostMessageW(host.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
    PostMessageW(host.window, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
    Sync(host);
}

void Wheel(Host& host, POINT client, int steps) {
    POINT screen = client;
    ClientToScreen(host.window, &screen);
    PostMessageW(host.window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * steps), MAKELPARAM(screen.x, screen.y));
    Sync(host);
}

// A point in the client area that no panel window covers.
POINT OutsidePanel() {
    return POINT{120, 700};
}

// ---- screenshots ----

std::wstring g_screensFolder;

bool SavePng(const std::vector<uint32_t>& bgra, UINT width, UINT height, const std::wstring& path) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                  reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size() * 4),
                                reinterpret_cast<BYTE*>(const_cast<uint32_t*>(bgra.data())));
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return SUCCEEDED(hr);
}

struct ShotResult {
    bool saved = false;
    long long stray = 0;          // pixels outside every panel window that are not the clear colour
    double changedInside = 0.0;   // share of pixels inside the panel windows that differ from it
    long long bright = 0;         // near-white pixels inside (text)
    size_t windows = 0;
    UINT width = 0, height = 0;
};

// The host's read-back rule for a flip-model chain: one ordinary Present, then a cleared buffer 0
// presented without sequencing (the mod draws into it, DXGI does not rotate), then a copy of
// buffer 0. The layout file written by that last frame says where the panel's windows are.
ShotResult Shoot(Host& host, const char* name) {
    ShotResult result;
    PanelFrame(host, 0);
    PanelFrame(host, DXGI_PRESENT_DO_NOT_SEQUENCE);
    const LayoutFile layout = ReadLayout();
    result.windows = layout.windows.size();
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(host.chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return result;
    D3D11_TEXTURE2D_DESC desc = {};
    buffer->GetDesc(&desc);
    const DXGI_FORMAT format = desc.Format;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    std::vector<uint32_t> raw;
    if (SUCCEEDED(host.device->CreateTexture2D(&desc, nullptr, &staging))) {
        host.context->CopyResource(staging, buffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(host.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
            raw.resize(static_cast<size_t>(desc.Width) * desc.Height);
            for (UINT y = 0; y < desc.Height; ++y) {
                memcpy(raw.data() + static_cast<size_t>(y) * desc.Width,
                       static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, desc.Width * 4);
            }
            host.context->Unmap(staging, 0);
        }
        staging->Release();
    }
    buffer->Release();
    if (raw.empty()) return result;
    result.width = desc.Width;
    result.height = desc.Height;
    const bool r10 = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    auto channels = [&](uint32_t v, int* r, int* g, int* b) {
        if (r10) {
            *r = static_cast<int>(((v & 1023) * 255 + 511) / 1023);
            *g = static_cast<int>((((v >> 10) & 1023) * 255 + 511) / 1023);
            *b = static_cast<int>((((v >> 20) & 1023) * 255 + 511) / 1023);
        } else {
            *b = static_cast<int>(v & 255);
            *g = static_cast<int>((v >> 8) & 255);
            *r = static_cast<int>((v >> 16) & 255);
        }
    };
    const uint32_t clearValue = raw[static_cast<size_t>(desc.Height - 2) * desc.Width + 1];   // bottom-left corner
    long long inside = 0, changed = 0;
    std::vector<uint32_t> bgra(raw.size());
    for (UINT y = 0; y < desc.Height; ++y) {
        for (UINT x = 0; x < desc.Width; ++x) {
            const uint32_t v = raw[static_cast<size_t>(y) * desc.Width + x];
            int r, g, b;
            channels(v, &r, &g, &b);
            bgra[static_cast<size_t>(y) * desc.Width + x] =
                0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            bool inWindow = false;
            for (const RECT& w : layout.windows) {
                if (static_cast<LONG>(x) >= w.left - 2 && static_cast<LONG>(x) < w.right + 2 &&
                    static_cast<LONG>(y) >= w.top - 2 && static_cast<LONG>(y) < w.bottom + 2) {
                    inWindow = true;
                    break;
                }
            }
            if (inWindow) {
                ++inside;
                if (v != clearValue) ++changed;
                if (r > 200 && g > 200 && b > 200) ++result.bright;
            } else if (v != clearValue) {
                ++result.stray;
            }
        }
    }
    result.changedInside = inside ? static_cast<double>(changed) / static_cast<double>(inside) : 0.0;
    const std::wstring path = g_screensFolder + L"\\" + std::wstring(name, name + strlen(name)) + L".png";
    result.saved = SavePng(bgra, desc.Width, desc.Height, path);
    int cr, cg, cb;
    channels(clearValue, &cr, &cg, &cb);
    printf("  shot %s: %ux%u, %zu panel window(s), %.1f%% of panel pixels drawn, %lld text pixels, %lld stray pixels "
           "outside the panel, clear colour %d %d %d, %s\n",
           name, desc.Width, desc.Height, result.windows, result.changedInside * 100.0, result.bright, result.stray, cr,
           cg, cb, result.saved ? "saved" : "NOT SAVED");
    return result;
}

void CheckShot(const char* label, const ShotResult& shot, bool expectPanel) {
    char name[160];
    if (expectPanel) {
        snprintf(name, sizeof(name), "%s: panel drawn (saved, over 90%% of its pixels drawn, text present, nothing "
                 "drawn outside it)", label);
        Check(name, shot.saved && shot.windows >= 1 && shot.changedInside > 0.9 && shot.bright > 150 && shot.stray == 0);
    } else {
        snprintf(name, sizeof(name), "%s: nothing drawn anywhere", label);
        Check(name, shot.saved && shot.stray == 0 && shot.bright == 0 && shot.changedInside == 0.0);
    }
}

void MoveAway(Host& host) {
    Move(host, OutsidePanel());
    Sync(host);
}

bool PanelReady(Host& host, int seconds) {
    return WaitLayout(host, [](const LayoutFile& l) { return l.values.count("open") != 0; }, seconds * 1000);
}

void ScriptShots(Host& host, const std::string& prefix) {
    const bool ready = WaitLayout(host, [](const LayoutFile& l) { return l.Int("drawn") == 1 && l.Has("list"); });
    Check("S0 the panel opens and reports its layout", ready);
    if (!ready) return;
    MoveAway(host);
    CheckShot("S1 bar", Shoot(host, (prefix + "-bar").c_str()), true);

    Click(host, Item("list"));
    Check("S2 Show list opens the list", WaitLayout(host, [](const LayoutFile& l) { return l.Int("list") == 1 && l.Has("row0"); }));
    MoveAway(host);
    CheckShot("S2 list", Shoot(host, (prefix + "-list").c_str()), true);
    Click(host, Item("list"));

    Click(host, Item("settings"));
    Check("S3 Settings opens the settings row",
          WaitLayout(host, [](const LayoutFile& l) { return l.Int("settings") == 1 && l.Int("list") == 0 && l.Has("language"); }));
    MoveAway(host);
    CheckShot("S3 settings row", Shoot(host, (prefix + "-settings").c_str()), true);
    Click(host, Item("settings"));

    Click(host, Item("source_other_app"));
    Check("S4 Other app is selected",
          WaitLayout(host, [](const LayoutFile& l) { return l.Int("source") == 1 && l.Int("settings") == 0; }));
    MoveAway(host);
    CheckShot("S4 other app", Shoot(host, (prefix + "-other-app").c_str()), true);
}

void ScriptShot1(Host& host, const std::string& name) {
    const bool ready = WaitLayout(host, [](const LayoutFile& l) { return l.Int("drawn") == 1 && l.Has("next"); });
    Check("S0 the panel opens and reports its layout", ready);
    if (!ready) return;
    MoveAway(host);
    CheckShot(("S5 " + name).c_str(), Shoot(host, name.c_str()), true);
}

void ScriptResize(Host& host) {
    const bool ready = WaitLayout(host, [](const LayoutFile& l) { return l.Int("drawn") == 1 && l.Has("row0"); });
    Check("R0 the panel and its list are drawn", ready);
    if (!ready) return;
    const UINT sizes[4][2] = {{1920, 1080}, {3840, 2160}, {1280, 720}, {2558, 1360}};
    for (const auto& size : sizes) {
        Pump(host, 2);
        ReleaseBackBuffer(host);
        const HRESULT hr = host.chain->ResizeBuffers(kGameLayout.buffers, size[0], size[1], DXGI_FORMAT_UNKNOWN, 0);
        char name[128];
        snprintf(name, sizeof(name), "R1 ResizeBuffers to %ux%u with the panel drawing (0x%08lx)", size[0], size[1],
                 static_cast<unsigned long>(hr));
        const bool resized = SUCCEEDED(hr) && AcquireBackBuffer(host);
        Check(name, resized);
        if (!resized) return;
        Pump(host, 8);
        char shot[64];
        snprintf(shot, sizeof(shot), "resize-%ux%u", size[0], size[1]);
        const ShotResult result = Shoot(host, shot);
        snprintf(name, sizeof(name), "R2 at %ux%u", size[0], size[1]);
        CheckShot(name, result, true);
        Check("R2 the screenshot has the new size", result.width == size[0] && result.height == size[1]);
    }
}

void ScriptFault(Host& host) {
    const bool ready = WaitLayout(host, [](const LayoutFile& l) { return l.Int("drawn") == 1 && l.Has("next"); });
    Check("F0 the panel is drawn before the fault", ready);
    if (!ready) return;
    const POINT next = Item("next");
    Pump(host, 120);   // the fault is scheduled for panel frame 60
    CheckShot("F1 after the fault", Shoot(host, "after-fault"), false);
    Check("F2 the game's pipeline state was intact after every frame, the faulting one included", g_stateFailures == 0);
    ReleaseBackBuffer(host);
    const HRESULT hr = host.chain->ResizeBuffers(kGameLayout.buffers, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0);
    printf("  ResizeBuffers after the panel fault: 0x%08lx\n", static_cast<unsigned long>(hr));
    Check("F3 ResizeBuffers succeeds after the panel fault (no back-buffer reference leaked)", SUCCEEDED(hr));
    if (SUCCEEDED(hr)) AcquireBackBuffer(host);
    const int insertDown = g_gotKeyDown[VK_INSERT].load();
    const int lDown = g_gotLDown.load();
    Key(host, VK_INSERT);
    Click(host, next);
    Check("F4 after the fault the game gets every key and click again (Insert and a click on the old panel spot)",
          g_gotKeyDown[VK_INSERT].load() == insertDown + 1 && g_gotLDown.load() == lDown + 1);
}

void ScriptDisplay(Host& host) {
    const bool ready = WaitLayout(host, [](const LayoutFile& l) { return l.Int("drawn") == 1 && l.Has("next"); });
    Check("D0 display mode: the panel is drawn without being asked", ready);
    if (!ready) return;
    CheckShot("D1 display mode", Shoot(host, "display-mode"), true);
    const LayoutFile layout = ReadLayout();
    const int insertDown = g_gotKeyDown[VK_INSERT].load();
    const int lDown = g_gotLDown.load();
    Key(host, VK_INSERT);
    Click(host, Item("next"));
    Check("D2 display mode takes nothing from the game (Insert and a click on the panel reach it)",
          layout.Int("enabled") == 0 && g_gotKeyDown[VK_INSERT].load() == insertDown + 1 && g_gotLDown.load() == lDown + 1);
}

void ScriptInput(Host& host) {
    const bool ready = WaitLayout(host, [](const LayoutFile& l) { return l.Int("enabled") == 1; }, 10000);
    Check("I0 the panel is ready and watching the window", ready);
    if (!ready) return;

    // Insert opens the panel; the game never sees the key.
    int down = g_gotKeyDown[VK_INSERT].load(), up = g_gotKeyUp[VK_INSERT].load();
    Key(host, VK_INSERT);
    bool ok = WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 1 && l.Int("drawn") == 1 && l.Has("next"); });
    Check("I1 Insert opens the panel and is kept from the game",
          ok && g_gotKeyDown[VK_INSERT].load() == down && g_gotKeyUp[VK_INSERT].load() == up);

    // A driving key goes to the game.
    down = g_gotKeyDown['W'].load();
    up = g_gotKeyUp['W'].load();
    Key(host, 'W');
    Check("I2 a driving key (W) reaches the game while the panel is open",
          g_gotKeyDown['W'].load() == down + 1 && g_gotKeyUp['W'].load() == up + 1);

    const POINT outside = OutsidePanel();
    int moves = g_gotMove.load();
    Move(host, outside);
    Sync(host);
    Check("I3 a mouse move outside the panel reaches the game", g_gotMove.load() == moves + 1);
    moves = g_gotMove.load();
    Move(host, Item("next"));
    Sync(host);
    Check("I4 a mouse move over the panel is kept from the game", g_gotMove.load() == moves);

    int lDown = g_gotLDown.load(), lUp = g_gotLUp.load();
    Click(host, outside);
    Check("I5 a click outside the panel reaches the game (down and up)",
          g_gotLDown.load() == lDown + 1 && g_gotLUp.load() == lUp + 1);

    LayoutFile layout = ReadLayout();
    const int tracks = layout.Int("tracks");
    int index = layout.Int("index");
    lDown = g_gotLDown.load();
    lUp = g_gotLUp.load();
    Click(host, Item("next"));
    int expected = (index + 1) % tracks;
    ok = WaitLayout(host, [&](const LayoutFile& l) { return l.Int("index") == expected; });
    Check("I6 a click on Next presses it and is kept from the game",
          ok && g_gotLDown.load() == lDown && g_gotLUp.load() == lUp);

    // Hotkeys with the panel open. None of them reaches the game.
    const UINT keys[5] = {VK_F5, VK_F6, VK_F7, VK_F8, VK_F9};
    int keyDowns[5], keyUps[5];
    for (int i = 0; i < 5; ++i) {
        keyDowns[i] = g_gotKeyDown[keys[i]].load();
        keyUps[i] = g_gotKeyUp[keys[i]].load();
    }
    index = ReadLayout().Int("index");
    Key(host, VK_F8);
    expected = (index + 1) % tracks;
    Check("I7 F8 plays the next song", WaitLayout(host, [&](const LayoutFile& l) { return l.Int("index") == expected; }));
    Key(host, VK_F6);
    Check("I7 F6 plays the previous song", WaitLayout(host, [&](const LayoutFile& l) { return l.Int("index") == index; }));
    const int state = ReadLayout().Int("state");
    Key(host, VK_F7);
    Check("I7 F7 plays or pauses", WaitLayout(host, [&](const LayoutFile& l) { return l.Int("state") != state; }));
    const int loop = ReadLayout().Int("loop");
    Key(host, VK_F9);
    Check("I7 F9 changes what repeats",
          WaitLayout(host, [&](const LayoutFile& l) { return l.Int("loop") == (loop + 1) % 3; }));
    Key(host, VK_F7);   // back to playing, so Stop has something to stop
    Key(host, VK_F5);
    Check("I7 F5 stops", WaitLayout(host, [](const LayoutFile& l) { return l.Int("state") == 0; }));
    bool none = true;
    for (int i = 0; i < 5; ++i) {
        none = none && g_gotKeyDown[keys[i]].load() == keyDowns[i] && g_gotKeyUp[keys[i]].load() == keyUps[i];
    }
    Check("I7 none of F5-F9 reached the game", none);

    // Auto-repeat of a held hotkey is kept from the game and does not act again.
    index = ReadLayout().Int("index");
    PostKey(host, VK_F8, true);
    PostKey(host, VK_F8, true, true);
    PostKey(host, VK_F8, true, true);
    PostKey(host, VK_F8, false);
    Sync(host);
    Pump(host, 6);
    Check("I8 a held F8 (with auto-repeat) acts once",
          ReadLayout().Int("index") == (index + 1) % tracks && g_gotKeyDown[VK_F8].load() == keyDowns[3]);

    // Insert closes the panel; hotkeys still work; clicks on the empty spot go to the game.
    const POINT nextPoint = Item("next");
    Key(host, VK_INSERT);
    Check("I9 Insert closes the panel", WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 0; }));
    index = ReadLayout().Int("index");
    Key(host, VK_F8);
    expected = (index + 1) % tracks;
    Check("I9 F8 works with the panel closed", WaitLayout(host, [&](const LayoutFile& l) { return l.Int("index") == expected; }));
    Key(host, VK_F9);
    Check("I9 F9 with the panel closed shows a short notice of what now repeats",
          WaitLayout(host, [](const LayoutFile& l) { return l.Int("toast") == 1; }));
    lDown = g_gotLDown.load();
    Click(host, nextPoint);
    Check("I9 with the panel closed a click where it was reaches the game", g_gotLDown.load() == lDown + 1);

    // The list, and the wheel over it.
    Key(host, VK_INSERT);
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 1 && l.Has("list"); });
    Click(host, Item("list"));
    ok = WaitLayout(host, [](const LayoutFile& l) { return l.Int("list") == 1 && l.Has("row3"); });
    Check("I10 Show list opens the list", ok);
    int wheels = g_gotWheel.load();
    Wheel(host, Item("row0"), -1);
    Check("I10 the wheel over the list is kept from the game", g_gotWheel.load() == wheels);
    Wheel(host, outside, -1);
    Check("I10 the wheel outside the panel reaches the game", g_gotWheel.load() == wheels + 1);
    Wheel(host, Item("row3"), 1);   // scroll the list back to the top
    WaitLayout(host, [](const LayoutFile& l) { return l.Has("row0") && l.Has("remove0"); });

    const int before = ReadLayout().Int("tracks");
    Click(host, Item("add_files"));
    Check("I11 Add files adds the chosen files",
          WaitLayout(host, [&](const LayoutFile& l) { return l.Int("tracks") == before + 2; }));
    Click(host, Item("remove0"));
    Check("I11 the remove button takes a song off the list",
          WaitLayout(host, [&](const LayoutFile& l) { return l.Int("tracks") == before + 1; }));
    Click(host, Item("row3"));
    Check("I11 clicking a song plays it", WaitLayout(host, [](const LayoutFile& l) { return l.Int("index") == 3; }));

    // ClearInputMouse after a gap: press on the volume slider, close the panel, release, reopen,
    // move along the slider. If the press were still held, the volume would follow the pointer.
    const POINT volume = Item("volume");
    Move(host, volume);
    PostMessageW(host.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(volume.x, volume.y));
    Sync(host);
    const int volumeAfterPress = ReadLayout().Int("volume");
    lUp = g_gotLUp.load();
    Key(host, VK_INSERT);
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 0; });
    PostMessageW(host.window, WM_LBUTTONUP, 0, MAKELPARAM(volume.x, volume.y));
    Sync(host);
    Key(host, VK_INSERT);
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 1 && l.Int("drawn") == 1; });
    Move(host, POINT{volume.x - 60, volume.y});
    Sync(host);
    Move(host, POINT{volume.x - 90, volume.y});
    Sync(host);
    const int volumeAfterGap = ReadLayout().Int("volume");
    printf("  volume after the press %d, after closing, releasing, reopening and moving %d\n", volumeAfterPress,
           volumeAfterGap);
    Check("I12 a press held across closing the panel is let go when it reopens (the slider does not follow)",
          volumeAfterGap == volumeAfterPress && g_gotLUp.load() == lUp);

    // Settings row: language drop-down, saved to settings.ini.
    Click(host, Item("list"));
    Click(host, Item("settings"));
    ok = WaitLayout(host, [](const LayoutFile& l) { return l.Int("settings") == 1 && l.Has("language"); });
    Click(host, Item("language"));
    ok = ok && WaitLayout(host, [](const LayoutFile& l) { return l.Has("language_item2"); });
    Click(host, Item("language_item2"));
    ok = ok && WaitLayout(host, [](const LayoutFile& l) { return l.Int("language") == 2 && l.Int("saves") >= 1; });
    const std::string ini = ReadWholeFile(ModFolder() + L"\\settings.ini");
    printf("  settings.ini after the change:\n%s\n", ini.c_str());
    Check("I13 choosing Traditional Chinese in the settings row switches the panel and saves language=zh-TW",
          ok && ini.find("language=zh-TW") != std::string::npos && ini.find("marker_seconds=0") != std::string::npos);
    Click(host, Item("language"));
    WaitLayout(host, [](const LayoutFile& l) { return l.Has("language_item1"); });
    Click(host, Item("language_item1"));
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("language") == 1; });

    // A click outside closes an open drop-down, and reaches the game.
    Click(host, Item("text_size"));
    ok = WaitLayout(host, [](const LayoutFile& l) { return l.Has("text_size_item0") && l.windows.size() >= 2; });
    lDown = g_gotLDown.load();
    Click(host, outside);
    ok = ok && WaitLayout(host, [](const LayoutFile& l) { return !l.Has("text_size_item0") && l.windows.size() == 1; });
    Check("I14 a click outside closes the open drop-down and reaches the game", ok && g_gotLDown.load() == lDown + 1);

    // Text field: keys are the panel's only while it has focus.
    Click(host, Item("test_field"));
    ok = WaitLayout(host, [](const LayoutFile& l) { return l.Int("wanttext") == 1; });
    down = g_gotKeyDown['A'].load();
    const int chars = g_gotChar.load();
    PostKey(host, 'A', true);
    PostMessageW(host.window, WM_CHAR, 'a', 1);
    PostKey(host, 'A', false);
    Sync(host);
    ok = ok && WaitLayout(host, [](const LayoutFile& l) { return l.Text("textfield") == "a"; });
    Check("I15 typing into a focused text field goes to the panel, not the game",
          ok && g_gotKeyDown['A'].load() == down && g_gotChar.load() == chars);
    Click(host, outside);
    ok = WaitLayout(host, [](const LayoutFile& l) { return l.Int("wanttext") == 0; });
    PostKey(host, 'A', true);
    PostMessageW(host.window, WM_CHAR, 'a', 1);
    PostKey(host, 'A', false);
    Sync(host);
    Check("I15 after clicking into the game, typing goes to the game again",
          ok && g_gotKeyDown['A'].load() == down + 1 && g_gotChar.load() == chars + 1);

    // The game losing and regaining focus is passed on (to the game and to the player).
    const int activations = g_gotActivateApp.load();
    PostMessageW(host.window, WM_ACTIVATEAPP, FALSE, 0);
    Sync(host);
    PostMessageW(host.window, WM_ACTIVATEAPP, TRUE, 0);
    Sync(host);
    Check("I16 focus changes reach the game", g_gotActivateApp.load() == activations + 2);

    // "Other app": hotkeys go to the other app; the keyboard's media keys are left to Windows.
    Click(host, Item("settings"));
    Click(host, Item("source_other_app"));
    ok = WaitLayout(host, [](const LayoutFile& l) { return l.Int("source") == 1; });
    Key(host, VK_F7);   // the example browser allows play and pause
    Key(host, VK_F8);   // ... but not next: refused, nothing sent to it
    const int mediaDown = g_gotKeyDown[VK_MEDIA_NEXT_TRACK].load();
    Key(host, VK_MEDIA_NEXT_TRACK);
    Check("I17 with Other app chosen the keyboard's media keys are left to Windows",
          ok && g_gotKeyDown[VK_MEDIA_NEXT_TRACK].load() == mediaDown + 1);
    Key(host, VK_INSERT);
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 0; });
    Key(host, VK_F5);   // the example app does not allow Stop
    Check("I17 a key the other app refuses shows a notice with the reason",
          WaitLayout(host, [](const LayoutFile& l) { return l.Int("toast") == 1; }));
    Key(host, VK_INSERT);
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("open") == 1 && l.Has("source_my_files"); });
    Click(host, Item("source_my_files"));
    WaitLayout(host, [](const LayoutFile& l) { return l.Int("source") == 0; });
    index = ReadLayout().Int("index");
    const int tracksNow = ReadLayout().Int("tracks");
    Key(host, VK_MEDIA_NEXT_TRACK);
    Check("I17 with My files chosen the media key Next is the panel's",
          WaitLayout(host, [&](const LayoutFile& l) { return l.Int("index") == (index + 1) % tracksNow; }) &&
              g_gotKeyDown[VK_MEDIA_NEXT_TRACK].load() == mediaDown + 1);

    // Heartbeat: when the panel stops being drawn, the mouse goes back to the game within half a
    // second, even though the panel is still "open".
    const POINT nextAgain = Item("next");
    Sleep(800);
    lDown = g_gotLDown.load();
    Move(host, nextAgain);
    PostMessageW(host.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(nextAgain.x, nextAgain.y));
    PostMessageW(host.window, WM_LBUTTONUP, 0, MAKELPARAM(nextAgain.x, nextAgain.y));
    Sync(host, false);
    Check("I18 a click over the panel reaches the game when the panel has not been drawn for 0.8 s",
          g_gotLDown.load() == lDown + 1);
    Pump(host, 10);
}

void RunPanel(Host& host, const char* outputFolder, int seconds, const char* script, const char* name) {
    host.chain = CreateChain(host, host.window, kGameLayout);
    if (!host.chain || !AcquireBackBuffer(host) || !CreateGameState(host)) {
        g_allPassed = false;
        return;
    }
    host.format = kGameLayout.format;
    g_clearColour = kPanelClearColour;
    wchar_t folder[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, outputFolder, -1, folder, MAX_PATH);
    g_screensFolder = std::wstring(folder) + L"\\screens";
    CreateDirectoryW(g_screensFolder.c_str(), nullptr);
    if (!PanelReady(host, seconds)) {
        Check("the panel started (test-layout.txt appeared)", false);
    } else {
        const std::string what = script;
        if (what == "shots") ScriptShots(host, name ? name : "panel");
        else if (what == "shot1") ScriptShot1(host, name ? name : "panel");
        else if (what == "resize") ScriptResize(host);
        else if (what == "fault") ScriptFault(host);
        else if (what == "display") ScriptDisplay(host);
        else if (what == "input") ScriptInput(host);
        else Check("known panel script", false);
    }
    printf("  game pipeline state checked after %d presents, %d differed\n", g_stateChecks, g_stateFailures);
    if (strcmp(script, "fault") != 0) {
        Check("P1 the game's pipeline state is unchanged after every present with the panel", g_stateFailures == 0);
    }
    ReleaseBackBuffer(host);
}

DWORD WINAPI RenderThread(LPVOID param) {
    auto* job = static_cast<RenderJob*>(param);
    Host& host = *job->host;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // PNG writing (Windows Imaging Component)
    if (CreateDeviceAndFactory(host)) {
        if (strcmp(job->mode, "panel") == 0) {
            RunPanel(host, job->outputFolder, job->seconds, job->script, job->name);
        } else if (strcmp(job->mode, "game") == 0) {
            RunGameChecks(host, job->outputFolder, job->seconds);
        } else if (strcmp(job->mode, "legacy") == 0) {
            RunLegacyCheck(host, job->seconds);
        } else {
            RunQuick(host, job->seconds);
        }
    } else {
        g_allPassed = false;
    }
    if (host.chain) host.chain->Release();
    if (host.factory) host.factory->Release();
    if (host.context) host.context->Release();
    if (host.device) host.device->Release();
    InterlockedExchange(&job->done, 1);
    return 0;
}

HWND MakeHiddenWindow(const wchar_t* className, int width, int height) {
    // Created without WS_VISIBLE and never shown.
    return CreateWindowExW(0, className, L"Racing Master Mod test host", WS_OVERLAPPEDWINDOW, 0, 0, width, height,
                           nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}


// ---- media mode ------------------------------------------------------------------------------------

// True when the mod's log in this run's LOCALAPPDATA contains the text. The mod keeps the file
// open for writing and allows reading.
bool ModLogContains(const char* text) {
    wchar_t path[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 40) return false;
    lstrcatW(path, L"\\RacingMasterMod\\mod.log");
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<char> data(1 << 20);
    DWORD read = 0;
    const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size() - 1), &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    data[read] = '\0';
    return strstr(data.data(), text) != nullptr;
}

void PumpMessages() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        DispatchMessageW(&message);
    }
}

// The fake player lives on this (main) thread, which pumps its messages; nothing is rendered.
int RunMedia(const char* scenario, int seconds, bool forwardingOk, bool standInOk) {
    fake_player::Behaviour behaviour = fake_player::Behaviour::ApplyAll;
    bool late = false;
    if (strcmp(scenario, "ignore-pause") == 0) behaviour = fake_player::Behaviour::IgnorePause;
    else if (strcmp(scenario, "no-repeat") == 0) behaviour = fake_player::Behaviour::NoRepeatNoNext;
    else if (strcmp(scenario, "vanish") == 0) behaviour = fake_player::Behaviour::VanishOnPause;
    else if (strcmp(scenario, "late") == 0) late = true;
    else if (strcmp(scenario, "basic") != 0 && strcmp(scenario, "guard") != 0 && strcmp(scenario, "lifecycle") != 0) {
        printf("unknown media scenario %s\n", scenario);
        return 2;
    }

    // Before any window: the process app id and this thread's COM apartment.
    if (!fake_player::PrepareProcess()) return 1;

    // A window the size of the game's, so the mod's start-up finds "the game window" as usual.
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RacingMasterModTestHost";
    RegisterClassExW(&windowClass);
    const HWND window = MakeHiddenWindow(windowClass.lpszClassName, 2558, 1360);

    const ULONGLONG start = GetTickCount64();
    bool published = false;
    if (!late) {
        published = fake_player::Start(behaviour);
        printf("fake player published at start: %s\n", published ? "yes" : "no");
    }
    bool finished = false;
    ULONGLONG lastLook = 0;
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000) {
        PumpMessages();
        const ULONGLONG now = GetTickCount64();
        if (late && !published && now - start >= 2000) {
            published = fake_player::Start(behaviour);
            printf("fake player published after %llu ms: %s\n", now - start, published ? "yes" : "no");
        }
        if (now - lastLook >= 100) {
            lastLook = now;
            if (ModLogContains("music apps test: finished")) {
                finished = true;
                break;
            }
        }
        Sleep(5);
    }
    // Apply anything still queued before reporting.
    const ULONGLONG settle = GetTickCount64();
    while (GetTickCount64() - settle < 200) {
        PumpMessages();
        Sleep(5);
    }
    printf("fake player received: %s\n", fake_player::Received()[0] ? fake_player::Received() : "nothing");
    printf("fake player state at the end: %s\n", fake_player::StatusWord());
    printf("mod test script finished: %s (after %llu ms)\n", finished ? "yes" : "no", GetTickCount64() - start);
    fake_player::Stop();
    DestroyWindow(window);
    fake_player::FinishProcess();

    const bool ok = forwardingOk && standInOk && published && finished;
    printf("result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        printf("usage: testhost.exe <game|legacy|quick|panel> <output folder> [seconds] [panel script] [name]\n"
               "       testhost.exe media <output folder> <seconds> <scenario>\n");
        return 2;
    }
    const char* mode = argv[1];
    if (strcmp(mode, "game") != 0 && strcmp(mode, "legacy") != 0 && strcmp(mode, "quick") != 0 &&
        strcmp(mode, "media") != 0 && strcmp(mode, "panel") != 0) {
        printf("unknown mode %s\n", mode);
        return 2;
    }
    const int seconds = argc > 3 ? atoi(argv[3]) : 6;

    const bool forwardingOk = CheckForwarding();
    const bool standInOk = CheckStandInInUse();

    if (strcmp(mode, "media") == 0) {
        return RunMedia(argc > 4 ? argv[4] : "basic", seconds, forwardingOk, standInOk);
    }

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = HostWndProc;   // counts what reaches "the game" past the mod's window procedure
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RacingMasterModTestHost";
    RegisterClassExW(&windowClass);

    Host host;
    const Layout& layout = strcmp(mode, "legacy") == 0 ? kLegacyLayout : kGameLayout;
    host.window = MakeHiddenWindow(windowClass.lpszClassName, static_cast<int>(layout.width),
                                   static_cast<int>(layout.height));
    host.secondWindow = MakeHiddenWindow(windowClass.lpszClassName, 800, 600);

    // Direct3D runs on a worker thread, as the engine's render thread does; this thread owns the
    // windows and pumps their messages.
    RenderJob job = {&host, mode, argv[2], seconds, argc > 4 ? argv[4] : "", argc > 5 ? argv[5] : nullptr, 0};
    const HANDLE thread = CreateThread(nullptr, 0, RenderThread, &job, 0, nullptr);
    while (!job.done) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&message);
        }
        Sleep(2);
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    DestroyWindow(host.secondWindow);
    DestroyWindow(host.window);

    const bool ok = forwardingOk && standInOk && (strcmp(mode, "quick") == 0 || g_allPassed);
    printf("result: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

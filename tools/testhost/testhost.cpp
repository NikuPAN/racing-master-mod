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
#include <dxgi1_2.h>

#include "fake_player.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

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
    host.context->ClearRenderTargetView(host.view, kClearColour);
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

DWORD WINAPI RenderThread(LPVOID param) {
    auto* job = static_cast<RenderJob*>(param);
    Host& host = *job->host;
    if (CreateDeviceAndFactory(host)) {
        if (strcmp(job->mode, "game") == 0) {
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
        printf("usage: testhost.exe <game|legacy|quick> <output folder> [seconds]\n"
               "       testhost.exe media <output folder> <seconds> <scenario>\n");
        return 2;
    }
    const char* mode = argv[1];
    if (strcmp(mode, "game") != 0 && strcmp(mode, "legacy") != 0 && strcmp(mode, "quick") != 0 &&
        strcmp(mode, "media") != 0) {
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
    windowClass.lpfnWndProc = DefWindowProcW;
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
    RenderJob job = {&host, mode, argv[2], seconds, 0};
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

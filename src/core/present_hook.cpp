#include "present_hook.h"

#include "log.h"

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

// Positions of Present and Present1 in the IDXGISwapChain1 function table.
constexpr int kSlotPresent = 8;
constexpr int kSlotPresent1 = 22;

PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;

ULONGLONG g_markerMs = 0;
ULONGLONG g_firstFrameTick = 0;
ULONGLONG g_lastReportTick = 0;
unsigned long long g_frames = 0;
unsigned long long g_framesAtLastReport = 0;
bool g_drawFailedLogged = false;

void LogFirstFrame(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        Log("first frame: %ux%u, format %u, buffers %u, swap effect %u, windowed %d, samples %u",
            desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<unsigned>(desc.BufferDesc.Format),
            desc.BufferCount, static_cast<unsigned>(desc.SwapEffect), desc.Windowed,
            desc.SampleDesc.Count);
    }
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        Log("renderer: Direct3D 11, feature level 0x%x", static_cast<unsigned>(device->GetFeatureLevel()));
        device->Release();
    } else {
        Log("renderer: the swap chain is not Direct3D 11 (the marker will not be drawn)");
    }
}

// Draws a white square with a dark centre in the top-left corner. Uses ClearView, which
// touches no pipeline state, so the game's own rendering is unaffected.
void DrawMarker(IDXGISwapChain* swapChain) {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    if (SUCCEEDED(hr)) {
        device->GetImmediateContext(&context);
        hr = context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1));
    }
    if (SUCCEEDED(hr)) {
        hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    }
    if (SUCCEEDED(hr)) {
        hr = device->CreateRenderTargetView(backBuffer, nullptr, &target);
    }
    if (SUCCEEDED(hr)) {
        const FLOAT white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const FLOAT dark[4] = {0.05f, 0.05f, 0.05f, 1.0f};
        const D3D11_RECT outer = {24, 24, 72, 72};
        const D3D11_RECT inner = {36, 36, 60, 60};
        context1->ClearView(target, white, &outer, 1);
        context1->ClearView(target, dark, &inner, 1);
    } else if (!g_drawFailedLogged) {
        g_drawFailedLogged = true;
        Log("marker: could not draw (error 0x%08lx); frames are still being counted",
            static_cast<unsigned long>(hr));
    }
    // Nothing is kept between frames, so the game can resize its buffers freely.
    if (target) target->Release();
    if (backBuffer) backBuffer->Release();
    if (context1) context1->Release();
    if (context) context->Release();
    if (device) device->Release();
}

void OnFrame(IDXGISwapChain* swapChain, UINT flags) {
    if (flags & DXGI_PRESENT_TEST) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_frames == 0) {
        g_firstFrameTick = now;
        g_lastReportTick = now;
        LogFirstFrame(swapChain);
    }
    ++g_frames;

    if (now - g_firstFrameTick < g_markerMs) {
        DrawMarker(swapChain);
    }
    if (now - g_lastReportTick >= 30000) {
        const double seconds = static_cast<double>(now - g_lastReportTick) / 1000.0;
        Log("running: %llu frames so far, %.0f frames per second over the last %.0f s", g_frames,
            static_cast<double>(g_frames - g_framesAtLastReport) / seconds, seconds);
        g_lastReportTick = now;
        g_framesAtLastReport = g_frames;
    }
}

void GuardedOnFrame(IDXGISwapChain* swapChain, UINT flags) {
    // Never let a fault in the mod take the game down.
    __try {
        OnFrame(swapChain, flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_markerMs = 0;
    }
}

// One of the two functions may call the other internally; the depth counter makes sure such a
// frame is handled once.
thread_local int t_presentDepth = 0;

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (t_presentDepth++ == 0) {
        GuardedOnFrame(swapChain, flags);
    }
    const HRESULT result = g_originalPresent(swapChain, syncInterval, flags);
    --t_presentDepth;
    return result;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    if (t_presentDepth++ == 0) {
        GuardedOnFrame(swapChain, flags);
    }
    const HRESULT result = g_originalPresent1(swapChain, syncInterval, flags, parameters);
    --t_presentDepth;
    return result;
}

bool PatchSlot(void** table, int slot, void* replacement, void** original) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    *original = InterlockedExchangePointer(&table[slot], replacement);
    VirtualProtect(&table[slot], sizeof(void*), oldProtect, &oldProtect);
    return true;
}

// Creates a throwaway swap chain on a hidden window only to learn where DXGI keeps its
// swap chain function table. Everything is released again before returning.
void** FindSwapChainTable(bool* hasPresent1) {
    const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) {
        Log("hook: d3d11.dll could not be loaded (error %lu)", GetLastError());
        return nullptr;
    }
    using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                      UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                      ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create) {
        Log("hook: D3D11CreateDeviceAndSwapChain not found");
        return nullptr;
    }

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RacingMasterModProbe";
    RegisterClassExW(&windowClass);
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr,
                                        nullptr, windowClass.hInstance, nullptr);
    if (!window) {
        Log("hook: could not create the hidden helper window (error %lu)", GetLastError());
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
                        &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) {
        // Software fallback: the function table is the same whichever adapter is used.
        hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain,
                    &device, nullptr, &context);
    }

    void** table = nullptr;
    if (SUCCEEDED(hr)) {
        table = *reinterpret_cast<void***>(swapChain);
        IDXGISwapChain1* swapChain1 = nullptr;
        *hasPresent1 = SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1),
                                                           reinterpret_cast<void**>(&swapChain1))) &&
                       *reinterpret_cast<void***>(swapChain1) == table;
        if (swapChain1) swapChain1->Release();
    } else {
        Log("hook: could not create the helper swap chain (error 0x%08lx)", static_cast<unsigned long>(hr));
    }
    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    return table;
}

}  // namespace

bool InstallPresentHook(unsigned markerSeconds) {
    g_markerMs = static_cast<ULONGLONG>(markerSeconds) * 1000;

    bool hasPresent1 = false;
    void** table = FindSwapChainTable(&hasPresent1);
    if (!table) {
        return false;
    }
    if (!PatchSlot(table, kSlotPresent, reinterpret_cast<void*>(&HookedPresent),
                   reinterpret_cast<void**>(&g_originalPresent))) {
        Log("hook: could not attach to Present (error %lu)", GetLastError());
        return false;
    }
    if (hasPresent1) {
        if (!PatchSlot(table, kSlotPresent1, reinterpret_cast<void*>(&HookedPresent1),
                       reinterpret_cast<void**>(&g_originalPresent1))) {
            Log("hook: attached to Present but not Present1 (error %lu)", GetLastError());
        }
    }
    Log("hook: attached to the frame presentation function%s", hasPresent1 ? "s (Present and Present1)" : "");
    return true;
}

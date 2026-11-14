#include "dxgi_probe.h"

#include "log.h"

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

void** FindSwapChainTable(bool* hasPresent1) {
    *hasPresent1 = false;
    // The game imports d3d11.dll itself, so this returns the module it already has loaded.
    const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    if (!d3d11) {
        Log("hook: d3d11.dll could not be loaded (error %lu)", GetLastError());
        return nullptr;
    }
    using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                      UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                      D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create) {
        Log("hook: D3D11CreateDeviceAndSwapChain not found");
        return nullptr;
    }

    // An ordinary top-level window that is never shown. It exists for about 50 ms. (A message-only
    // window would work too, but it would not be enumerable; this mod hides nothing.)
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
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain,
                        &device, nullptr, &context);
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

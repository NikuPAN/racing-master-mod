// Developer test host. Stands in for the game so the loader can be checked without it.
//
// It imports version.dll (so the stand-in next to this exe is loaded), renders with Direct3D 11
// to a window that is never shown, and reports whether
//   1. version.dll calls still return real data (forwarding works), and
//   2. the mod's marker square ended up in the frame (the frame hook works).
//
// Usage: testhost.exe <output.bmp> [seconds]
// Exit code 0 = both checks passed.

#include <windows.h>

#include <d3d11.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 360;

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

    wchar_t loaded[MAX_PATH] = L"";
    GetModuleFileNameW(GetModuleHandleW(L"version.dll"), loaded, MAX_PATH);
    printf("version.dll in use: %ls\n", loaded);
    return true;
}

bool SaveBmp(const char* path, const BYTE* pixels, UINT rowPitch) {
    BITMAPFILEHEADER file = {};
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info);
    info.biWidth = kWidth;
    info.biHeight = -static_cast<LONG>(kHeight);
    info.biPlanes = 1;
    info.biBitCount = 32;
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + kWidth * kHeight * 4;

    FILE* out = nullptr;
    if (fopen_s(&out, path, "wb") != 0 || !out) {
        return false;
    }
    fwrite(&file, sizeof(file), 1, out);
    fwrite(&info, sizeof(info), 1, out);
    for (UINT y = 0; y < kHeight; ++y) {
        fwrite(pixels + y * rowPitch, 4, kWidth, out);
    }
    fclose(out);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: testhost.exe <output.bmp> [seconds]\n");
        return 2;
    }
    const int seconds = argc > 2 ? atoi(argv[2]) : 4;
    const bool forwardingOk = CheckForwarding();

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RacingMasterModTestHost";
    RegisterClassExW(&windowClass);
    // Created but never shown.
    const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"test host", WS_OVERLAPPEDWINDOW, 0, 0,
                                        kWidth, kHeight, nullptr, nullptr, windowClass.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = kWidth;
    desc.BufferDesc.Height = kHeight;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    // Sequential keeps the back buffer contents after Present, so it can be read back below.
    desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &wanted, 1,
                                               D3D11_SDK_VERSION, &desc, &swapChain, &device, nullptr, &context);
    if (FAILED(hr)) {
        printf("could not create the Direct3D 11 device (0x%08lx)\n", static_cast<unsigned long>(hr));
        return 3;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    device->CreateRenderTargetView(backBuffer, nullptr, &target);

    const FLOAT clearColour[4] = {0.10f, 0.20f, 0.45f, 1.0f};
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    unsigned frames = 0;
    while (GetTickCount64() < end) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&message);
        }
        context->ClearRenderTargetView(target, clearColour);
        swapChain->Present(0, 0);
        ++frames;
        Sleep(8);
    }
    printf("presented %u frames\n", frames);

    D3D11_TEXTURE2D_DESC textureDesc = {};
    backBuffer->GetDesc(&textureDesc);
    textureDesc.Usage = D3D11_USAGE_STAGING;
    textureDesc.BindFlags = 0;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    device->CreateTexture2D(&textureDesc, nullptr, &staging);
    context->CopyResource(staging, backBuffer);

    bool markerOk = false;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const BYTE* pixels = static_cast<const BYTE*>(mapped.pData);
        auto at = [&](UINT x, UINT y) { return pixels + y * mapped.RowPitch + x * 4; };
        const BYTE* ring = at(28, 28);      // white edge of the marker
        const BYTE* centre = at(48, 48);    // dark centre of the marker
        const BYTE* outside = at(200, 200); // untouched clear colour
        printf("pixel on marker edge   (28,28):   B%u G%u R%u\n", ring[0], ring[1], ring[2]);
        printf("pixel in marker centre (48,48):   B%u G%u R%u\n", centre[0], centre[1], centre[2]);
        printf("pixel outside marker   (200,200): B%u G%u R%u\n", outside[0], outside[1], outside[2]);
        markerOk = ring[0] > 240 && ring[1] > 240 && ring[2] > 240 && centre[0] < 40 && centre[1] < 40 &&
                   centre[2] < 40 && outside[0] > 90 && outside[2] < 60;
        SaveBmp(argv[1], pixels, mapped.RowPitch);
        context->Unmap(staging, 0);
    }
    printf("frame hook: %s\n", markerOk ? "OK (marker found in the frame)" : "FAIL (marker not found)");

    staging->Release();
    target->Release();
    backBuffer->Release();
    swapChain->Release();
    context->Release();
    device->Release();
    DestroyWindow(window);
    return forwardingOk && markerOk ? 0 : 1;
}

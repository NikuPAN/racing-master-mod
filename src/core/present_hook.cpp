#include "present_hook.h"

#include "dxgi_probe.h"
#include "frame.h"
#include "log.h"
#include "vtable_patch.h"

#include "../ui/overlay.h"

#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

// Positions of Present and Present1 in the IDXGISwapChain1 function table.
constexpr int kSlotPresent = 8;
constexpr int kSlotPresent1 = 22;

// Written by the worker thread before the table entry is switched, read by the present thread.
void* volatile g_originalPresent = nullptr;
void* volatile g_originalPresent1 = nullptr;

// Set before the hook is installed, read-only afterwards.
ULONGLONG g_markerMs = 0;
bool g_acceptHiddenWindow = false;
unsigned g_testFaultAtFrame = 0;

// Written by whichever thread presents, read by the worker thread.
volatile LONG64 g_hookCalls = 0;
volatile LONG64 g_frames = 0;
volatile LONG g_frameWorkOff = 0;   // 1 after a fault: the mod no longer touches frames

// The game's swap chain. Compared against the caller's pointer, never dereferenced, no reference
// held. Two swap chains can present from two threads, so both are volatile and written atomically.
IDXGISwapChain* volatile g_boundSwapChain = nullptr;
volatile ULONGLONG g_lastBoundFrameTick = 0;

// Present thread of the bound swap chain only.
ULONGLONG g_firstFrameTick = 0;
bool g_drawFailedLogged = false;

DWORD g_faultCode = 0;
void* g_faultAddress = nullptr;
// Test host only: writing through this null pointer is the deliberate fault.
volatile int* volatile g_faultTarget = nullptr;

void LogFirstFrame(IDXGISwapChain* swapChain, FrameRefs& refs) {
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        Log("first frame: %ux%u, format %u, buffers %u, swap effect %u, windowed %d, samples %u",
            desc.BufferDesc.Width, desc.BufferDesc.Height, static_cast<unsigned>(desc.BufferDesc.Format),
            desc.BufferCount, static_cast<unsigned>(desc.SwapEffect), desc.Windowed, desc.SampleDesc.Count);
    }
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&refs.device)))) {
        Log("renderer: Direct3D 11, feature level 0x%x", static_cast<unsigned>(refs.device->GetFeatureLevel()));
    } else {
        Log("renderer: the swap chain is not Direct3D 11 (the marker will not be drawn)");
    }
    ReleaseFrameRefs(refs);
}

// Draws a white square with a dark centre in the top-left corner. Uses ClearView, which
// touches no pipeline state, so the game's own rendering is unaffected.
void DrawMarker(IDXGISwapChain* swapChain, FrameRefs& refs, bool injectFault) {
    HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&refs.device));
    if (SUCCEEDED(hr)) {
        refs.device->GetImmediateContext(&refs.context);
        hr = refs.context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&refs.context1));
    }
    if (SUCCEEDED(hr)) {
        hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&refs.backBuffer));
    }
    if (SUCCEEDED(hr)) {
        hr = refs.device->CreateRenderTargetView(refs.backBuffer, nullptr, &refs.target);
    }
    if (injectFault) {
        *g_faultTarget = 1;   // test host only: fault while references are held
    }
    if (SUCCEEDED(hr)) {
        const FLOAT white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const FLOAT dark[4] = {0.05f, 0.05f, 0.05f, 1.0f};
        const D3D11_RECT outer = {24, 24, 72, 72};
        const D3D11_RECT inner = {36, 36, 60, 60};
        refs.context1->ClearView(refs.target, white, &outer, 1);
        refs.context1->ClearView(refs.target, dark, &inner, 1);
    } else if (!g_drawFailedLogged) {
        g_drawFailedLogged = true;
        Log("marker: could not draw (error 0x%08lx); frames are still being counted",
            static_cast<unsigned long>(hr));
    }
    // Nothing that refers to a swap chain buffer is kept between frames.
    ReleaseFrameRefs(refs);
}

// The three user32 functions below are delay-loaded. The worker thread calls each of them once
// before the hook is installed, so their first call never happens on the game's frame thread.
bool WindowQualifies(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId()) {
        return false;
    }
    RECT rect = {};
    if (!GetClientRect(window, &rect) || rect.right < 320 || rect.bottom < 200) {
        return false;
    }
    return g_acceptHiddenWindow || IsWindowVisible(window);
}

bool IsGameSwapChain(IDXGISwapChain* swapChain, ULONGLONG now) {
    IDXGISwapChain* bound = g_boundSwapChain;
    if (swapChain == bound) {
        g_lastBoundFrameTick = now;
        return true;
    }
    if (bound && now - g_lastBoundFrameTick < 2000) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swapChain->GetDesc(&desc)) || !WindowQualifies(desc.OutputWindow)) {
        return false;
    }
    // First one, or the game rebuilt its swap chain; only one of two racing threads wins.
    if (InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_boundSwapChain), swapChain, bound) !=
        bound) {
        return false;
    }
    g_lastBoundFrameTick = now;
    if (bound) {
        Log("the game presents through a new swap chain; following it");
    }
    return true;
}

void OnFrame(IDXGISwapChain* swapChain, UINT flags, FrameRefs& refs) {
    if (flags & DXGI_PRESENT_TEST) {
        return;
    }
    InterlockedIncrement64(&g_hookCalls);
    const ULONGLONG now = GetTickCount64();
    if (!IsGameSwapChain(swapChain, now)) {
        return;
    }
    const LONG64 frame = InterlockedIncrement64(&g_frames);
    if (frame == 1) {
        g_firstFrameTick = now;
        LogFirstFrame(swapChain, refs);
    }
    if (now - g_firstFrameTick < g_markerMs) {
        DrawMarker(swapChain, refs, g_testFaultAtFrame != 0 && frame == static_cast<LONG64>(g_testFaultAtFrame));
    }
    // The music panel (does nothing when [panel] mode=off). Runs after the marker, which has
    // already given back everything it held.
    PanelOnFrame(swapChain, flags, refs);
}

LONG FrameFaultFilter(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Not ours to swallow: the stack guard page would stay consumed, and debuggers need theirs.
    if (code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_faultCode = code;
    g_faultAddress = info->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// One log line: exception code, the module the faulting address belongs to, and the offset
// inside it (out\version.map or the PDB turns that into a source line).
void ReportFault() {
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = L"";
    char path8[MAX_PATH * 3] = "unknown module";
    unsigned long long offset = 0;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(g_faultAddress), &module) &&
        module) {
        offset = static_cast<unsigned long long>(static_cast<char*>(g_faultAddress) - reinterpret_cast<char*>(module));
        if (GetModuleFileNameW(module, path, MAX_PATH)) {
            LogUtf8(path, path8, sizeof(path8));
        }
    }
    Log("the mod's frame code failed (exception 0x%08lx at %s+0x%llx) and has switched itself off; the game "
        "continues without it until it is restarted",
        static_cast<unsigned long>(g_faultCode), path8, offset);
}

void GuardedOnFrame(IDXGISwapChain* swapChain, UINT flags) {
    if (g_frameWorkOff) {
        return;
    }
    FrameRefs refs = {};
    // Never let a fault in the mod take the game down - but say so, stop for good, and give back
    // every reference, or the game's next ResizeBuffers fails. Keep C++ objects with
    // destructors out of this function (the compiler refuses them next to __try).
    __try {
        OnFrame(swapChain, flags, refs);
    } __except (FrameFaultFilter(GetExceptionInformation())) {
        InterlockedExchange(&g_frameWorkOff, 1);
        // Order matters: the game's pipeline state goes back on the context first (if the fault
        // came while the panel's own state was swapped in), then every reference is released,
        // then the panel hands the mouse and keys back to the game.
        RestoreGameStateAfterFault(refs);
        __try {
            ReleaseFrameRefs(refs);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        PanelOnFault();
        ReportFault();
    }
}

// One of the two functions may call the other internally; the depth counter makes sure such a
// frame is handled once.
thread_local int t_presentDepth = 0;

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    // Read once: the worker published this before the table entry pointed here.
    const auto original = reinterpret_cast<PresentFn>(g_originalPresent);
    if (t_presentDepth++ == 0) {
        GuardedOnFrame(swapChain, flags);
    }
    const HRESULT result = original(swapChain, syncInterval, flags);
    --t_presentDepth;
    return result;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    const auto original = reinterpret_cast<Present1Fn>(g_originalPresent1);
    if (t_presentDepth++ == 0) {
        GuardedOnFrame(swapChain, flags);
    }
    const HRESULT result = original(swapChain, syncInterval, flags, parameters);
    --t_presentDepth;
    return result;
}

}  // namespace

void ReleaseFrameRefs(FrameRefs& refs) {
    if (refs.gameState) refs.gameState->Release();
    if (refs.target) refs.target->Release();
    if (refs.backBuffer) refs.backBuffer->Release();
    if (refs.context1) refs.context1->Release();
    if (refs.context) refs.context->Release();
    if (refs.device) refs.device->Release();
    refs = FrameRefs{};
}

void RestoreGameStateAfterFault(FrameRefs& refs) {
    if (!refs.gameState || !refs.context || !refs.context1) {
        return;
    }
    __try {
        refs.context->ClearState();   // the panel's state lets go of the back buffer
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        refs.context1->SwapDeviceContextState(refs.gameState, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool InstallPresentHook(const PresentHookOptions& options) {
    g_markerMs = static_cast<ULONGLONG>(options.markerSeconds) * 1000;
    g_acceptHiddenWindow = options.acceptHiddenWindow;
    g_testFaultAtFrame = options.testFaultAtFrame;

    bool hasPresent1 = false;
    void** table = FindSwapChainTable(&hasPresent1);
    if (!table) {
        return false;
    }
    if (!PatchSlot(table, kSlotPresent, reinterpret_cast<void*>(&HookedPresent), &g_originalPresent)) {
        Log("hook: could not attach to Present (error %lu)", GetLastError());
        return false;
    }
    if (hasPresent1) {
        if (!PatchSlot(table, kSlotPresent1, reinterpret_cast<void*>(&HookedPresent1), &g_originalPresent1)) {
            Log("hook: attached to Present but not Present1 (error %lu)", GetLastError());
        }
    }
    Log("hook: attached to the frame presentation function%s", hasPresent1 ? "s (Present and Present1)" : "");
    return true;
}

PresentHookCounters PresentHookGetCounters() {
    PresentHookCounters counters;
    counters.hookCalls = static_cast<unsigned long long>(g_hookCalls);
    counters.gameFrames = static_cast<unsigned long long>(g_frames);
    return counters;
}

bool PresentHookSwitchedOff() {
    return g_frameWorkOff != 0;
}

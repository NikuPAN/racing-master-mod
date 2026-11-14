// Silent fake music player for the test host. See fake_player.h.
//
// Raw WinRT ABI like the mod. The player side numbers its states differently from the reader
// side (MediaPlaybackStatus: Closed 0, Changing 1, Stopped 2, Playing 3, Paused 4; the mod reads
// GlobalSystemMediaTransportControlsSessionPlaybackStatus: Closed 0 ... Paused 5), so everything
// here goes by name, never by number.

#include "fake_player.h"

#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.media.h>
#include <SystemMediaTransportControlsInterop.h>
#include <shobjidl_core.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace wf = ABI::Windows::Foundation;
namespace wm = ABI::Windows::Media;

namespace fake_player {
namespace {

enum : UINT { kButtonMessage = WM_APP + 41, kRepeatMessage };

HWND g_window = nullptr;
Behaviour g_behaviour = Behaviour::ApplyAll;
wm::ISystemMediaTransportControls* g_smtc = nullptr;
wm::ISystemMediaTransportControls2* g_smtc2 = nullptr;
EventRegistrationToken g_buttonToken = {};
EventRegistrationToken g_repeatToken = {};
bool g_repeatSubscribed = false;
wm::MediaPlaybackStatus g_status = wm::MediaPlaybackStatus_Closed;
int g_track = 1;
char g_received[1024] = "";
wchar_t g_appId[128] = L"";
char g_appId8[128] = "";

struct StringRef {
    HSTRING_HEADER header = {};
    HSTRING value = nullptr;
    explicit StringRef(const wchar_t* text) {
        WindowsCreateStringReference(text, static_cast<UINT32>(lstrlenW(text)), &header, &value);
    }
};

void Record(const char* what) {
    if (g_received[0]) strncat_s(g_received, ", ", _TRUNCATE);
    strncat_s(g_received, what, _TRUNCATE);
}

// The handlers run on Windows' threads and only forward to the window, in arrival order.
class ButtonHandler final
    : public wf::ITypedEventHandler<wm::SystemMediaTransportControls*, wm::SystemMediaTransportControlsButtonPressedEventArgs*> {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) ||
            iid == __uuidof(wf::ITypedEventHandler<wm::SystemMediaTransportControls*,
                                                   wm::SystemMediaTransportControlsButtonPressedEventArgs*>)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Invoke(wm::ISystemMediaTransportControls*,
                                     wm::ISystemMediaTransportControlsButtonPressedEventArgs* args) override {
        wm::SystemMediaTransportControlsButton button = wm::SystemMediaTransportControlsButton_Play;
        if (args && SUCCEEDED(args->get_Button(&button))) {
            PostMessageW(g_window, kButtonMessage, static_cast<WPARAM>(button), 0);
        }
        return S_OK;
    }
};

class RepeatHandler final
    : public wf::ITypedEventHandler<wm::SystemMediaTransportControls*, wm::AutoRepeatModeChangeRequestedEventArgs*> {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) ||
            iid == __uuidof(wf::ITypedEventHandler<wm::SystemMediaTransportControls*,
                                                   wm::AutoRepeatModeChangeRequestedEventArgs*>)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Invoke(wm::ISystemMediaTransportControls*,
                                     wm::IAutoRepeatModeChangeRequestedEventArgs* args) override {
        wm::MediaPlaybackAutoRepeatMode mode = wm::MediaPlaybackAutoRepeatMode_None;
        if (args && SUCCEEDED(args->get_RequestedAutoRepeatMode(&mode))) {
            PostMessageW(g_window, kRepeatMessage, static_cast<WPARAM>(mode), 0);
        }
        return S_OK;
    }
};

ButtonHandler g_onButton;
RepeatHandler g_onRepeat;

void SetStatus(wm::MediaPlaybackStatus status) {
    g_status = status;
    if (g_smtc) g_smtc->put_PlaybackStatus(status);
}

void SetTitle() {
    wm::ISystemMediaTransportControlsDisplayUpdater* updater = nullptr;
    if (FAILED(g_smtc->get_DisplayUpdater(&updater)) || !updater) return;
    updater->put_Type(wm::MediaPlaybackType_Music);
    wm::IMusicDisplayProperties* music = nullptr;
    if (SUCCEEDED(updater->get_MusicProperties(&music)) && music) {
        wchar_t title[32];
        swprintf_s(title, L"Test track %d", g_track);
        StringRef titleRef(title), artistRef(L"Racing Master Mod test");
        music->put_Title(titleRef.value);
        music->put_Artist(artistRef.value);
        music->Release();
    }
    updater->Update();
    updater->Release();
}

void ApplyButton(wm::SystemMediaTransportControlsButton button) {
    switch (button) {
        case wm::SystemMediaTransportControlsButton_Play:
            Record("play");
            SetStatus(wm::MediaPlaybackStatus_Playing);
            break;
        case wm::SystemMediaTransportControlsButton_Pause:
            Record("pause");
            if (g_behaviour == Behaviour::IgnorePause) break;   // says nothing, keeps playing
            if (g_behaviour == Behaviour::VanishOnPause) {
                g_smtc->put_IsEnabled(false);   // the session leaves Windows' list
                break;
            }
            SetStatus(wm::MediaPlaybackStatus_Paused);
            break;
        case wm::SystemMediaTransportControlsButton_Stop:
            Record("stop");
            SetStatus(wm::MediaPlaybackStatus_Stopped);
            break;
        case wm::SystemMediaTransportControlsButton_Next:
            Record("next");
            ++g_track;
            SetTitle();
            break;
        case wm::SystemMediaTransportControlsButton_Previous:
            Record("previous");
            if (g_track > 1) --g_track;
            SetTitle();
            break;
        default:
            Record("other button");
            break;
    }
}

void ApplyRepeat(wm::MediaPlaybackAutoRepeatMode mode) {
    Record(mode == wm::MediaPlaybackAutoRepeatMode_None    ? "loop off"
           : mode == wm::MediaPlaybackAutoRepeatMode_Track ? "loop one"
                                                           : "loop all");
    if (g_smtc2) g_smtc2->put_AutoRepeatMode(mode);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kButtonMessage && g_smtc) {
        ApplyButton(static_cast<wm::SystemMediaTransportControlsButton>(wparam));
        return 0;
    }
    if (message == kRepeatMessage && g_smtc) {
        ApplyRepeat(static_cast<wm::MediaPlaybackAutoRepeatMode>(wparam));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

const wchar_t* AppId() {
    return g_appId;
}

const char* AppIdUtf8() {
    return g_appId8;
}

bool PrepareProcess() {
    // The mod's remote reads app ids into 128 characters (other_app.cpp), so the id must fit.
    const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID", g_appId, 128);
    if (n >= 128) {
        printf("fake player: RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID is %lu characters; at most 127 fit\n", n);
        return false;
    }
    if (n == 0) {
        swprintf_s(g_appId, L"%s.%lu", kAppIdPrefix, GetCurrentProcessId());
    }
    WideCharToMultiByte(CP_UTF8, 0, g_appId, -1, g_appId8, sizeof(g_appId8), nullptr, nullptr);
    printf("fake player: app id %s%s\n", g_appId8, n == 0 ? " (RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID unset, so the process id was added)" : "");
    HRESULT hr = SetCurrentProcessExplicitAppUserModelID(g_appId);
    if (FAILED(hr)) {
        printf("fake player: could not set the app id (0x%08lx)\n", static_cast<unsigned long>(hr));
        return false;
    }
    // Without this the session factory below either fails with CO_E_NOTINITIALIZED or lands in
    // the mod's apartment, depending on which thread gets there first.
    hr = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(hr)) {
        printf("fake player: could not initialise COM on the main thread (0x%08lx)\n", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool Start(Behaviour behaviour) {
    g_behaviour = behaviour;
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"RacingMasterModTestFakePlayer";
    RegisterClassExW(&windowClass);
    // Created without WS_VISIBLE and never shown.
    g_window = CreateWindowExW(0, windowClass.lpszClassName, L"Racing Master Mod test player", WS_OVERLAPPEDWINDOW, 0,
                               0, 320, 200, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!g_window) {
        printf("fake player: could not create its window (error %lu)\n", GetLastError());
        return false;
    }

    StringRef classId(RuntimeClass_Windows_Media_SystemMediaTransportControls);
    ISystemMediaTransportControlsInterop* interop = nullptr;
    HRESULT hr = RoGetActivationFactory(classId.value, __uuidof(ISystemMediaTransportControlsInterop),
                                        reinterpret_cast<void**>(&interop));
    if (SUCCEEDED(hr)) {
        hr = interop->GetForWindow(g_window, __uuidof(wm::ISystemMediaTransportControls), reinterpret_cast<void**>(&g_smtc));
        interop->Release();
    }
    if (FAILED(hr) || !g_smtc) {
        printf("fake player: could not publish a media session (0x%08lx)\n", static_cast<unsigned long>(hr));
        g_smtc = nullptr;
        return false;
    }
    g_smtc->QueryInterface(__uuidof(wm::ISystemMediaTransportControls2), reinterpret_cast<void**>(&g_smtc2));
    g_smtc->add_ButtonPressed(&g_onButton, &g_buttonToken);
    g_smtc->put_IsEnabled(true);
    g_smtc->put_IsPlayEnabled(true);
    g_smtc->put_IsPauseEnabled(true);
    g_smtc->put_IsStopEnabled(true);
    g_smtc->put_IsNextEnabled(behaviour != Behaviour::NoRepeatNoNext);
    g_smtc->put_IsPreviousEnabled(true);
    SetStatus(wm::MediaPlaybackStatus_Paused);
    if (g_smtc2 && behaviour != Behaviour::NoRepeatNoNext) {
        // Windows reports "loop allowed" only once the player listens for loop requests.
        g_smtc2->put_AutoRepeatMode(wm::MediaPlaybackAutoRepeatMode_None);
        g_repeatSubscribed = SUCCEEDED(g_smtc2->add_AutoRepeatModeChangeRequested(&g_onRepeat, &g_repeatToken));
    }
    SetTitle();
    return true;
}

void Stop() {
    if (g_smtc) {
        g_smtc->remove_ButtonPressed(g_buttonToken);
        if (g_smtc2 && g_repeatSubscribed) g_smtc2->remove_AutoRepeatModeChangeRequested(g_repeatToken);
        g_smtc->put_IsEnabled(false);
    }
    if (g_smtc2) g_smtc2->Release();
    if (g_smtc) g_smtc->Release();
    g_smtc2 = nullptr;
    g_smtc = nullptr;
    if (g_window) DestroyWindow(g_window);
    g_window = nullptr;
}

const char* Received() {
    return g_received;
}

const char* StatusWord() {
    switch (g_status) {
        case wm::MediaPlaybackStatus_Closed: return "closed";
        case wm::MediaPlaybackStatus_Changing: return "changing";
        case wm::MediaPlaybackStatus_Stopped: return "stopped";
        case wm::MediaPlaybackStatus_Playing: return "playing";
        case wm::MediaPlaybackStatus_Paused: return "paused";
    }
    return "unknown";
}

void FinishProcess() {
    RoUninitialize();
}

}  // namespace fake_player

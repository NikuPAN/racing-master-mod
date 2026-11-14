// Remote control of other music apps through Windows media sessions (Windows.Media.Control).
//
// Shape of this file, and why:
// - Raw WinRT ABI headers from the SDK, no C++/WinRT: every call returns an HRESULT, so this
//   builds with the mod's /EHs-c- flags and no C++ exception is ever thrown inside the game.
// - Every function outside kernel32 (combase, shell32, advapi32) is resolved with GetProcAddress
//   on the media thread. version.dll gains no static import and stays plain forwarding in the
//   other programs that load it; the build script checks the import table.
// - One thread owned by the mod does all the COM work, in the multithreaded apartment. The frame
//   thread only copies a snapshot and queues commands; it never waits for the media thread.
// - Change notices from Windows arrive on COM threads and do nothing but wake the media thread.
//   The periodic refresh keeps the snapshot right even when a notice is lost.
// - Titles and artists are never written to the log: players paste the log into bug reports.
// - In the game the thread never leaves its apartment. If the game had no multithreaded
//   apartment, the mod's RoInitialize creates it and every game thread that never initialised
//   COM becomes an implicit member; leaving would take that away from under the game.
//
// The media thread is the only thread that touches a WinRT object. "Media thread only" below
// means exactly that.

#include "remote.h"

#include "../core/log.h"
#include "../core/worker.h"

#include <windows.h>

#include <roapi.h>
#include <winstring.h>
#include <asyncinfo.h>
#include <windows.foundation.h>
#include <windows.media.control.h>

#include <shobjidl_core.h>
#include <shlobj_core.h>
#include <knownfolders.h>

#include <utility>

namespace wf = ABI::Windows::Foundation;
namespace wfc = ABI::Windows::Foundation::Collections;
namespace wm = ABI::Windows::Media;
namespace wmc = ABI::Windows::Media::Control;

namespace remote {
namespace {

using SessionManagerStatics = wmc::IGlobalSystemMediaTransportControlsSessionManagerStatics;
using SessionManager = wmc::IGlobalSystemMediaTransportControlsSessionManager;
using Session = wmc::IGlobalSystemMediaTransportControlsSession;
using PlaybackInfo = wmc::IGlobalSystemMediaTransportControlsSessionPlaybackInfo;
using PlaybackControls = wmc::IGlobalSystemMediaTransportControlsSessionPlaybackControls;
using Timeline = wmc::IGlobalSystemMediaTransportControlsSessionTimelineProperties;
using MediaProperties = wmc::IGlobalSystemMediaTransportControlsSessionMediaProperties;
using PlaybackStatus = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using RepeatMode = wm::MediaPlaybackAutoRepeatMode;
using SessionList = wfc::IVectorView<wmc::GlobalSystemMediaTransportControlsSession*>;
using BoolOperation = wf::IAsyncOperation<bool>;

constexpr PlaybackStatus kClosed = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Closed;
constexpr PlaybackStatus kStopped = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Stopped;
constexpr PlaybackStatus kPlaying = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing;
constexpr PlaybackStatus kPaused = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Paused;

constexpr DWORD kManagerTimeoutMs = 3000;   // RequestAsync
constexpr DWORD kTitleTimeoutMs = 2000;     // TryGetMediaPropertiesAsync
constexpr DWORD kCommandTimeoutMs = 1500;   // Try...Async
constexpr DWORD kConfirmTimeoutMs = 2500;   // the expected change after an accepted command
constexpr DWORD kRefreshMs = 500;           // periodic refresh while running
constexpr ULONGLONG kTitleRefreshMs = 5000;
constexpr ULONGLONG kAuditEveryMs = 10000;
constexpr ULONGLONG kAuditRecheckMs = 1000;
constexpr ULONGLONG kResultShownMs = 5000;
constexpr ULONGLONG kClosedShownMs = 4000;
constexpr ULONGLONG kDoubleClickMs = 150;
constexpr ULONGLONG kStaleMs = 5000;        // a snapshot older than this means the thread is stuck
constexpr unsigned kQueueSize = 8;
constexpr unsigned kMaxApps = 16;
constexpr unsigned kAppIdChars = 132;       // an app id may be 128 characters, plus room

// Returned by Send() when the test-host guard refused; never a code Windows returns.
constexpr HRESULT kRefusedByTestGuard = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0201);

// TestHang and TestRebuild exist only for the test host's "hang" and "rebuild" steps.
enum class Command { Play, Pause, Toggle, Stop, Next, Previous, RepeatOff, RepeatOne, RepeatAll, PickApp, FollowWindows,
                     TestHang, TestRebuild };

// ---- functions resolved at run time ----------------------------------------------------------------

struct Api {
    HRESULT(WINAPI* RoInitialize)(RO_INIT_TYPE) = nullptr;
    void(WINAPI* RoUninitialize)() = nullptr;
    HRESULT(WINAPI* RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* WindowsCreateStringReference)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*) = nullptr;
    PCWSTR(WINAPI* WindowsGetStringRawBuffer)(HSTRING, UINT32*) = nullptr;
    HRESULT(WINAPI* WindowsDeleteString)(HSTRING) = nullptr;
    void(WINAPI* CoTaskMemFree)(LPVOID) = nullptr;
    HRESULT(WINAPI* CoGetApartmentType)(APTTYPE*, APTTYPEQUALIFIER*) = nullptr;
    HRESULT(WINAPI* SHCreateItemInKnownFolder)(REFKNOWNFOLDERID, DWORD, PCWSTR, REFIID, void**) = nullptr;
    BOOL(WINAPI* OpenProcessToken)(HANDLE, DWORD, PHANDLE) = nullptr;
    BOOL(WINAPI* GetTokenInformation)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD) = nullptr;
};
Api g_api;

template <typename F>
void Resolve(HMODULE module, const char* name, F& target) {
    target = module ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(module, name))) : nullptr;
}

// Media thread only. Returns false when the seven combase functions are missing (then this
// Windows has no WinRT and the feature is unsupported). shell32 and advapi32 are optional: without
// them the app names fall back to a tidied id and the log leaves out the administrator part.
bool LoadApi() {
    const HMODULE combase = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE shell32 = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE advapi32 = LoadLibraryExW(L"advapi32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Resolve(combase, "RoInitialize", g_api.RoInitialize);
    Resolve(combase, "RoUninitialize", g_api.RoUninitialize);
    Resolve(combase, "RoGetActivationFactory", g_api.RoGetActivationFactory);
    Resolve(combase, "WindowsCreateStringReference", g_api.WindowsCreateStringReference);
    Resolve(combase, "WindowsGetStringRawBuffer", g_api.WindowsGetStringRawBuffer);
    Resolve(combase, "WindowsDeleteString", g_api.WindowsDeleteString);
    Resolve(combase, "CoTaskMemFree", g_api.CoTaskMemFree);
    Resolve(combase, "CoGetApartmentType", g_api.CoGetApartmentType);
    Resolve(shell32, "SHCreateItemInKnownFolder", g_api.SHCreateItemInKnownFolder);
    Resolve(advapi32, "OpenProcessToken", g_api.OpenProcessToken);
    Resolve(advapi32, "GetTokenInformation", g_api.GetTokenInformation);
    return g_api.RoInitialize && g_api.RoUninitialize && g_api.RoGetActivationFactory &&
           g_api.WindowsCreateStringReference && g_api.WindowsGetStringRawBuffer && g_api.WindowsDeleteString &&
           g_api.CoTaskMemFree;
}

// ---- small helpers (media thread only) ---------------------------------------------------------------

// A string reference for a class id: no allocation, lives as long as the object.
struct StringRef {
    HSTRING_HEADER header = {};
    HSTRING value = nullptr;
    explicit StringRef(const wchar_t* text) {
        g_api.WindowsCreateStringReference(text, static_cast<UINT32>(lstrlenW(text)), &header, &value);
    }
};

// Copies an HSTRING out and frees it.
std::wstring TakeString(HSTRING text) {
    std::wstring result;
    if (text) {
        UINT32 length = 0;
        const wchar_t* raw = g_api.WindowsGetStringRawBuffer(text, &length);
        if (raw && length) {
            result.assign(raw, length);
        }
        g_api.WindowsDeleteString(text);
    }
    return result;
}

template <typename T>
void ReleaseAndClear(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

const char* Utf8(const std::wstring& text, char* buffer, int size) {
    return LogUtf8(text.c_str(), buffer, size);
}

// Waits for a WinRT operation with a deadline, without a completion delegate. ERROR_TIMEOUT on
// expiry (the operation is cancelled), the operation's own error code on failure.
template <typename Op>
HRESULT WaitAsync(Op* op, DWORD timeoutMs) {
    IAsyncInfo* info = nullptr;
    HRESULT hr = op->QueryInterface(__uuidof(IAsyncInfo), reinterpret_cast<void**>(&info));
    if (FAILED(hr)) {
        return hr;
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    AsyncStatus status = AsyncStatus::Started;
    for (;;) {
        hr = info->get_Status(&status);
        if (FAILED(hr) || status != AsyncStatus::Started) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            info->Cancel();
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            break;
        }
        Sleep(1);
    }
    if (SUCCEEDED(hr) && status == AsyncStatus::Error) {
        HRESULT error = E_FAIL;
        info->get_ErrorCode(&error);
        hr = FAILED(error) ? error : E_FAIL;
    } else if (SUCCEEDED(hr) && status == AsyncStatus::Canceled) {
        hr = E_ABORT;
    }
    info->Release();
    return hr;
}

// ---- change notices: immortal delegates that only wake the media thread -----------------------------

HANDLE g_wake = nullptr;          // auto-reset: a notice arrived or a command was queued
volatile LONG g_notices = 0;      // notices received so far (any kind)

template <typename TDelegate, typename TSender, typename TArgs>
class WakeHandler final : public TDelegate {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAgileObject) || iid == __uuidof(TDelegate)) {
            *out = static_cast<TDelegate*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // Static lifetime: a notice that arrives after the handler was removed cannot touch freed
    // memory, because nothing is ever freed. g_wake is never closed for the same reason.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Invoke(TSender*, TArgs*) override {
        InterlockedIncrement(&g_notices);
        if (g_wake) {
            SetEvent(g_wake);
        }
        return S_OK;
    }
};

WakeHandler<wf::ITypedEventHandler<wmc::GlobalSystemMediaTransportControlsSessionManager*, wmc::SessionsChangedEventArgs*>,
            SessionManager, wmc::ISessionsChangedEventArgs>
    g_onSessions;
WakeHandler<wf::ITypedEventHandler<wmc::GlobalSystemMediaTransportControlsSessionManager*, wmc::CurrentSessionChangedEventArgs*>,
            SessionManager, wmc::ICurrentSessionChangedEventArgs>
    g_onCurrent;
WakeHandler<wf::ITypedEventHandler<wmc::GlobalSystemMediaTransportControlsSession*, wmc::PlaybackInfoChangedEventArgs*>,
            Session, wmc::IPlaybackInfoChangedEventArgs>
    g_onPlayback;
WakeHandler<wf::ITypedEventHandler<wmc::GlobalSystemMediaTransportControlsSession*, wmc::MediaPropertiesChangedEventArgs*>,
            Session, wmc::IMediaPropertiesChangedEventArgs>
    g_onMedia;
WakeHandler<wf::ITypedEventHandler<wmc::GlobalSystemMediaTransportControlsSession*, wmc::TimelinePropertiesChangedEventArgs*>,
            Session, wmc::ITimelinePropertiesChangedEventArgs>
    g_onTimeline;

// ---- state shared between threads ----------------------------------------------------------------------

// The published snapshot plus what GetSnapshot() needs to work out the position. Lives on the
// heap and is never freed, so no destructor runs at process exit while a thread may still read it.
struct Published {
    Snapshot snapshot;
    long long positionTicks = 0;   // position at positionAsOf, 100 ns units
    long long endTicks = 0;
    long long positionAsOf = 0;    // 100 ns since 1601 UTC, the clock of GetSystemTimeAsFileTime
    double rate = 1.0;
    ULONGLONG publishedTick = 0;   // GetTickCount64 when the media thread wrote this
    bool watched = false;          // the media thread is supposed to be refreshing it
};
SRWLOCK g_snapshotLock = SRWLOCK_INIT;

Published& Store() {
    static Published* const store = new Published;
    return *store;
}

struct Queued {
    Command command;
    wchar_t appId[kAppIdChars];
};
SRWLOCK g_queueLock = SRWLOCK_INIT;
Queued g_queue[kQueueSize];
unsigned g_queueHead = 0;
unsigned g_queueCount = 0;
Command g_lastQueued = Command::FollowWindows;
ULONGLONG g_lastQueuedTick = 0;

volatile LONG g_started = 0;       // Start() created the thread (or tried to)
volatile LONG g_dropLogged = 0;
volatile LONG g_queueDrops = 0;    // commands lost because the queue was full
HANDLE g_thread = nullptr;
bool g_testHost = false;
wchar_t g_testApp[kAppIdChars] = L"";

void Post(Command command, const wchar_t* appId) {
    if (!g_started) {
        if (InterlockedExchange(&g_dropLogged, 1) == 0) {
            Log("music apps: a command arrived while the media side was not started; ignored");
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    bool queued = false;
    AcquireSRWLockExclusive(&g_queueLock);
    // The same button twice within 150 ms is one click (double-click guard).
    const bool duplicate = command == g_lastQueued && now - g_lastQueuedTick < kDoubleClickMs &&
                           command != Command::PickApp && command != Command::FollowWindows;
    if (!duplicate) {
        if (g_queueCount < kQueueSize) {
            Queued& slot = g_queue[(g_queueHead + g_queueCount) % kQueueSize];
            slot.command = command;
            slot.appId[0] = L'\0';
            if (appId) {
                lstrcpynW(slot.appId, appId, kAppIdChars);
            }
            ++g_queueCount;
            g_lastQueued = command;
            g_lastQueuedTick = now;
            queued = true;
        } else {
            InterlockedIncrement(&g_queueDrops);   // logged by the media thread
        }
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    if (queued) {
        SetEvent(g_wake);
    }
}

bool Pop(Queued* out) {
    bool got = false;
    AcquireSRWLockExclusive(&g_queueLock);
    if (g_queueCount) {
        *out = g_queue[g_queueHead];
        g_queueHead = (g_queueHead + 1) % kQueueSize;
        --g_queueCount;
        got = true;
    }
    ReleaseSRWLockExclusive(&g_queueLock);
    return got;
}

// ---- words ------------------------------------------------------------------------------------------------

Status ToStatus(PlaybackStatus status) {
    switch (status) {
        case kPlaying: return Status::Playing;
        case kPaused: return Status::Paused;
        case kStopped: return Status::Stopped;
        case kClosed: return Status::NoSession;
        default: return Status::Unknown;
    }
}

const char* StatusWord(PlaybackStatus status) {
    switch (status) {
        case wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Closed: return "closed";
        case wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Opened: return "opened";
        case wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Changing: return "changing";
        case wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Stopped: return "stopped";
        case wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Playing: return "playing";
        case wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus_Paused: return "paused";
    }
    return "unknown";
}

const char* RepeatWord(RepeatMode mode) {
    switch (mode) {
        case wm::MediaPlaybackAutoRepeatMode_None: return "off";
        case wm::MediaPlaybackAutoRepeatMode_Track: return "one";
        case wm::MediaPlaybackAutoRepeatMode_List: return "all";
    }
    return "unknown";
}

const char* CommandWord(Command command) {
    switch (command) {
        case Command::Play: return "play";
        case Command::Pause: return "pause";
        case Command::Toggle: return "play/pause";
        case Command::Stop: return "stop";
        case Command::Next: return "next track";
        case Command::Previous: return "previous track";
        case Command::RepeatOff: return "loop off";
        case Command::RepeatOne: return "loop one";
        case Command::RepeatAll: return "loop all";
        case Command::PickApp: return "pick app";
        case Command::FollowWindows: return "follow Windows";
        case Command::TestHang: return "test hang";
        case Command::TestRebuild: return "test rebuild";
    }
    return "?";
}

const char* ResultWord(Result result) {
    switch (result) {
        case Result::None: return "none";
        case Result::Pending: return "pending";
        case Result::Done: return "done";
        case Result::NotOffered: return "not offered";
        case Result::Refused: return "refused";
        case Result::NoReply: return "no reply";
        case Result::NoEffect: return "no effect";
        case Result::SessionGone: return "session gone";
        case Result::NoSession: return "no session";
    }
    return "?";
}

const char* StateWord(State state) {
    switch (state) {
        case State::Off: return "off";
        case State::Starting: return "starting";
        case State::Unsupported: return "unsupported";
        case State::NotAnswering: return "not answering";
        case State::NoSession: return "no session";
        case State::WaitingForApp: return "waiting for the app";
        case State::Ready: return "ready";
        case State::Faulted: return "faulted";
    }
    return "?";
}

// The English line for a message id; {app} is replaced by the app's friendly name. The same
// wording is listed next to each id in remote.h, for the panel's translations.
std::wstring Text(Message id, const std::wstring& app) {
    const wchar_t* pattern = L"";
    switch (id) {
        case Message::None: pattern = L""; break;
        case Message::LookingForApps: pattern = L"Looking for music apps..."; break;
        case Message::Unsupported:
            pattern = L"Controlling other apps needs Windows 10 (version 1809) or newer. Local files still work.";
            break;
        case Message::NotAnswering:
            pattern = L"Windows is not answering about music apps right now. The panel keeps trying. Local files still "
                      L"work.";
            break;
        case Message::Faulted:
            pattern = L"Controlling other apps stopped working in this session. Local files still work.";
            break;
        case Message::NoApp:
            pattern = L"No music app found. Start playing something in Spotify, a browser or another player and it "
                      L"will show up here.";
            break;
        case Message::WaitingForApp: pattern = L"Waiting for {app}. Start it and it will show up here."; break;
        case Message::PickedNotRunning: pattern = L"{app} is not running. Start it, or pick another app below."; break;
        case Message::PickedClosed: pattern = L"{app} has closed. Start it again, or pick another app below."; break;
        case Message::AppClosed: pattern = L"{app} has closed."; break;
        case Message::IsPlaying: pattern = L"{app} is playing."; break;
        case Message::IsPaused: pattern = L"{app} is paused."; break;
        case Message::IsStopped: pattern = L"{app} is stopped."; break;
        case Message::AlreadyPlaying: pattern = L"{app} is already playing."; break;
        case Message::AlreadyPaused: pattern = L"{app} is already paused."; break;
        case Message::Sending: pattern = L"Sending to {app}..."; break;
        case Message::DonePlaying: pattern = L"{app} is playing."; break;
        case Message::DonePaused: pattern = L"{app} is paused."; break;
        case Message::DoneStopped: pattern = L"{app} has stopped."; break;
        case Message::DonePlayPause: pattern = L"{app} accepted play/pause."; break;
        case Message::DoneNext: pattern = L"Asked {app} for the next track."; break;
        case Message::DonePrevious: pattern = L"Asked {app} for the previous track."; break;
        case Message::DoneLoopOff: pattern = L"Looping is off in {app}."; break;
        case Message::DoneLoopOne: pattern = L"Loop one is on in {app}."; break;
        case Message::DoneLoopAll: pattern = L"Loop all is on in {app}."; break;
        case Message::NoPlay: pattern = L"{app} does not offer \"play\" right now."; break;
        case Message::NoPause: pattern = L"{app} does not offer \"pause\" right now."; break;
        case Message::NoPlayPause: pattern = L"{app} does not offer \"play/pause\" right now."; break;
        case Message::NoStop: pattern = L"{app} has no stop. Use pause."; break;
        case Message::NoNext: pattern = L"{app} does not offer \"next track\" right now."; break;
        case Message::NoPrevious: pattern = L"{app} does not offer \"previous track\" right now."; break;
        case Message::LoopNotAllowed:
            pattern = L"{app} does not let other programs change looping. Change it in {app} itself.";
            break;
        case Message::LoopIgnored: pattern = L"{app} did not change looping. Change it in {app} itself."; break;
        case Message::Refused: pattern = L"{app} did not accept that. Try again, or use {app} itself."; break;
        case Message::NoReply: pattern = L"{app} did not answer. Try again, or use {app} itself."; break;
        case Message::NoEffect: pattern = L"{app} did not respond. Try again, or use {app} itself."; break;
        case Message::TestGuard: pattern = L"The test host may not send commands to {app}."; break;
    }
    std::wstring text = pattern;
    const std::wstring placeholder = L"{app}";
    for (size_t at = text.find(placeholder); at != std::wstring::npos; at = text.find(placeholder, at + app.size())) {
        text.replace(at, placeholder.size(), app);
    }
    return text;
}

// ---- the media thread's engine --------------------------------------------------------------------------

enum class Expect { Nothing, StatusIs, StoppedOrGone, RepeatIs };
enum class Call { Play, Pause, Toggle, Stop, Next, Previous, Repeat };

// What one session looked like at one moment.
struct Reading {
    bool present = false;          // listed and not closed
    PlaybackStatus status = kClosed;
    bool repeatReported = false;
    RepeatMode repeatMode = wm::MediaPlaybackAutoRepeatMode_None;
};

class Engine {
public:
    void Run();

private:
    // Windows side
    bool SetupManager();
    void TeardownManager();
    SessionManager* RequestManager(HRESULT* result, unsigned* tookMs);
    bool ListSessions(std::vector<App>* apps, std::vector<Session*>* objects, SessionManager* manager);
    std::wstring CurrentSessionId(SessionManager* manager);
    bool ReadStatusOf(Session* session, Reading* reading);
    Reading ReadNow(const std::wstring& id);
    Reading ReadThroughFreshManager(const std::wstring& id);
    void SwitchTarget(Session* session, const std::wstring& id);
    void ReleaseSession();
    void DropTarget();
    void ForgetTarget();
    void ReadTarget(ULONGLONG now);
    void ReadTitle();
    void Audit(ULONGLONG now);
    void Refresh();
    void ScheduleRetry(HRESULT hr);
    void LogStartFacts();
    std::wstring AppName(const std::wstring& id);
    bool RepeatBlocked(const std::wstring& id) const;

    // commands
    void DrainCommands();
    void Execute(const Queued& queued);
    HRESULT Send(Call call, RepeatMode mode, boolean* accepted);
    Result Confirm(Expect expect, PlaybackStatus wantStatus, RepeatMode wantMode, ULONGLONG* tookMs);
    void SetResult(Result result, Message message, const std::wstring& app);

    // publishing
    void Publish();
    void ComposeMessage(ULONGLONG now, Message* id, std::wstring* app);

    SessionManagerStatics* statics_ = nullptr;
    SessionManager* manager_ = nullptr;
    EventRegistrationToken sessionsToken_ = {};
    EventRegistrationToken currentToken_ = {};
    Session* session_ = nullptr;   // the controlled session object
    bool subscribed_ = false;      // the three session handlers are registered on session_
    EventRegistrationToken playbackToken_ = {};
    EventRegistrationToken mediaToken_ = {};
    EventRegistrationToken timelineToken_ = {};

    // What is known about the target app. Survives a change of session object for the same app
    // (rebuild mode fetches a new object on every refresh).
    std::wstring targetId_;
    std::wstring targetName_;
    PlaybackStatus status_ = kClosed;
    bool play_ = false, pause_ = false, toggle_ = false, stop_ = false, next_ = false, previous_ = false,
         repeatAllowed_ = false;
    bool repeatReported_ = false;
    RepeatMode repeatMode_ = wm::MediaPlaybackAutoRepeatMode_None;
    bool hasTimeline_ = false;
    long long positionTicks_ = 0, endTicks_ = 0, positionAsOf_ = 0;
    double rate_ = 1.0;
    std::wstring title_, artist_;
    bool titleDirty_ = true;
    ULONGLONG titleTick_ = 0;
    int repeatMisses_ = 0;

    std::vector<App> apps_;
    bool following_ = true;
    std::wstring pickedId_, pickedName_;

    State state_ = State::Starting;
    unsigned retryStage_ = 0;
    unsigned failedRequests_ = 0;
    ULONGLONG retryTick_ = 0;
    bool announced_ = false;
    bool noticesLogged_ = false;
    int elevated_ = -1;            // 1 yes, 0 no, -1 could not tell
    ULONGLONG auditTick_ = 0;
    int auditMisses_ = 0;
    bool auditAgreedLogged_ = false;
    bool rebuild_ = false;
    std::wstring closedName_;
    ULONGLONG closedTick_ = 0;

    Result result_ = Result::None;
    Message resultMessage_ = Message::None;
    std::wstring resultApp_;
    ULONGLONG resultTick_ = 0;
    unsigned resultSequence_ = 0;

    std::vector<std::pair<std::wstring, std::wstring>> names_;   // app id -> friendly name
    std::vector<std::wstring> repeatBlocked_;                     // apps that ignored looping twice
};

void Engine::Run() {
    Publish();
    if (!LoadApi()) {
        state_ = State::Unsupported;
        Log("music apps: this Windows has no media session API (combase functions missing); other apps cannot be "
            "controlled");
        Publish();
        return;
    }
    LogStartFacts();
    const HRESULT init = g_api.RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(init)) {
        state_ = State::NotAnswering;
        Log("music apps: could not enter the COM apartment (0x%08lx); other apps cannot be controlled in this run",
            static_cast<unsigned long>(init));
        Publish();
        return;
    }
    auditTick_ = GetTickCount64() + kAuditEveryMs;
    Refresh();

    // The thread runs until the process ends.
    for (;;) {
        if (WaitForSingleObject(g_wake, kRefreshMs) == WAIT_OBJECT_0) {
            Sleep(30);   // notices come in bursts; one refresh covers them all
            if (!noticesLogged_ && g_notices) {
                noticesLogged_ = true;
                Log("music apps: change notices are arriving");
            }
        }
        const LONG drops = InterlockedExchange(&g_queueDrops, 0);
        if (drops) {
            Log("music apps: %ld button press%s came faster than they could be sent and %s dropped", drops,
                drops == 1 ? "" : "es", drops == 1 ? "was" : "were");
        }
        DrainCommands();
        Refresh();
    }
}

void Engine::LogStartFacts() {
    const char* apartment = "could not tell whether the process already had a multithreaded apartment";
    if (g_api.CoGetApartmentType) {
        APTTYPE type = {};
        APTTYPEQUALIFIER qualifier = {};
        const HRESULT hr = g_api.CoGetApartmentType(&type, &qualifier);
        apartment = SUCCEEDED(hr) ? "the process already had a multithreaded apartment, which this thread joins"
                                  : "the process had no multithreaded apartment; this thread creates one";
    }
    if (g_api.OpenProcessToken && g_api.GetTokenInformation) {
        HANDLE token = nullptr;
        if (g_api.OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION elevation = {};
            DWORD size = 0;
            if (g_api.GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
                elevated_ = elevation.TokenIsElevated ? 1 : 0;
            }
            CloseHandle(token);
        }
    }
    const char* who = g_testHost ? "the test host" : "the game";
    Log("music apps: starting on the mod's own thread; %s; %s %s", apartment, who,
        elevated_ == 1   ? "runs as administrator"
        : elevated_ == 0 ? "does not run as administrator"
                         : "may or may not run as administrator (could not tell)");
}

SessionManager* Engine::RequestManager(HRESULT* result, unsigned* tookMs) {
    wf::IAsyncOperation<wmc::GlobalSystemMediaTransportControlsSessionManager*>* op = nullptr;
    SessionManager* manager = nullptr;
    const ULONGLONG start = GetTickCount64();
    HRESULT hr = statics_->RequestAsync(&op);
    if (SUCCEEDED(hr) && op) {
        hr = WaitAsync(op, kManagerTimeoutMs);
        if (SUCCEEDED(hr)) {
            hr = op->GetResults(&manager);
        }
        op->Release();
    } else if (SUCCEEDED(hr)) {
        hr = E_POINTER;
    }
    if (tookMs) {
        *tookMs = static_cast<unsigned>(GetTickCount64() - start);
    }
    if (SUCCEEDED(hr) && !manager) {
        hr = E_POINTER;
    }
    if (FAILED(hr)) {
        ReleaseAndClear(manager);
    }
    *result = hr;
    return manager;
}

// After 2, 5, 15 s, then every 30 s. Logged on the first failure and every tenth after it.
void Engine::ScheduleRetry(HRESULT hr) {
    static const unsigned kDelaysSeconds[] = {2, 5, 15, 30};
    const unsigned delay = kDelaysSeconds[retryStage_ < 3 ? retryStage_ : 3];
    ++retryStage_;
    retryTick_ = GetTickCount64() + static_cast<ULONGLONG>(delay) * 1000;
    state_ = State::NotAnswering;
    if (failedRequests_++ % 10 == 0) {
        Log("music apps: Windows is not answering about music apps (0x%08lx); trying again in %u s",
            static_cast<unsigned long>(hr), delay);
    }
}

bool Engine::SetupManager() {
    if (!statics_) {
        StringRef classId(RuntimeClass_Windows_Media_Control_GlobalSystemMediaTransportControlsSessionManager);
        const HRESULT hr = g_api.RoGetActivationFactory(classId.value, __uuidof(SessionManagerStatics),
                                                        reinterpret_cast<void**>(&statics_));
        if (FAILED(hr) || !statics_) {
            statics_ = nullptr;
            if (hr == REGDB_E_CLASSNOTREG || hr == CLASS_E_CLASSNOTAVAILABLE) {
                state_ = State::Unsupported;
                Log("music apps: this Windows has no media session API (0x%08lx); it needs Windows 10 version 1809 "
                    "or newer",
                    static_cast<unsigned long>(hr));
                return false;
            }
            ScheduleRetry(FAILED(hr) ? hr : E_POINTER);
            return false;
        }
    }
    unsigned tookMs = 0;
    HRESULT hr = S_OK;
    manager_ = RequestManager(&hr, &tookMs);
    if (!manager_) {
        ScheduleRetry(hr);
        return false;
    }
    manager_->add_SessionsChanged(&g_onSessions, &sessionsToken_);
    manager_->add_CurrentSessionChanged(&g_onCurrent, &currentToken_);
    if (failedRequests_ && !rebuild_) {
        Log("music apps: Windows answered again");
    }
    retryStage_ = 0;
    failedRequests_ = 0;
    if (!announced_) {
        announced_ = true;
        std::vector<App> apps;
        std::vector<Session*> objects;
        ListSessions(&apps, &objects, manager_);
        std::string ids;
        for (const App& app : apps) {
            char id8[kAppIdChars * 3];
            if (!ids.empty()) {
                ids += ", ";
            }
            ids += Utf8(app.id, id8, sizeof(id8));
        }
        for (Session* object : objects) {
            object->Release();
        }
        Log("music apps: Windows answered in %u ms; %u app%s found%s%s%s", tookMs, static_cast<unsigned>(apps.size()),
            apps.size() == 1 ? "" : "s", apps.empty() ? "" : " (", ids.c_str(), apps.empty() ? "" : ")");
    }
    return true;
}

void Engine::TeardownManager() {
    if (manager_) {
        manager_->remove_SessionsChanged(sessionsToken_);
        manager_->remove_CurrentSessionChanged(currentToken_);
        ReleaseAndClear(manager_);
    }
}

// Fills the app list and hands back one reference per listed session (same order), which the
// caller releases. Closed sessions are skipped.
bool Engine::ListSessions(std::vector<App>* apps, std::vector<Session*>* objects, SessionManager* manager) {
    apps->clear();
    objects->clear();
    SessionList* list = nullptr;
    if (FAILED(manager->GetSessions(&list)) || !list) {
        return false;
    }
    unsigned size = 0;
    list->get_Size(&size);
    for (unsigned i = 0; i < size && apps->size() < kMaxApps; ++i) {
        Session* session = nullptr;
        if (FAILED(list->GetAt(i, &session)) || !session) {
            continue;
        }
        HSTRING id = nullptr;
        session->get_SourceAppUserModelId(&id);
        App app;
        app.id = TakeString(id);
        Reading reading;
        if (app.id.empty() || !ReadStatusOf(session, &reading) || reading.status == kClosed) {
            session->Release();
            continue;
        }
        app.status = ToStatus(reading.status);
        app.name = AppName(app.id);
        apps->push_back(std::move(app));
        objects->push_back(session);
    }
    list->Release();
    return true;
}

std::wstring Engine::CurrentSessionId(SessionManager* manager) {
    Session* current = nullptr;
    std::wstring id;
    // S_OK with no session means there is none; a failure is treated the same way.
    if (SUCCEEDED(manager->GetCurrentSession(&current)) && current) {
        HSTRING text = nullptr;
        current->get_SourceAppUserModelId(&text);
        id = TakeString(text);
        current->Release();
    }
    return id;
}

bool Engine::ReadStatusOf(Session* session, Reading* reading) {
    PlaybackInfo* info = nullptr;
    if (FAILED(session->GetPlaybackInfo(&info)) || !info) {
        return false;
    }
    info->get_PlaybackStatus(&reading->status);
    wf::IReference<RepeatMode>* repeat = nullptr;
    reading->repeatReported = false;
    if (SUCCEEDED(info->get_AutoRepeatMode(&repeat)) && repeat) {
        reading->repeatReported = SUCCEEDED(repeat->get_Value(&reading->repeatMode));
        repeat->Release();
    }
    info->Release();
    return true;
}

// The target app as the long-lived manager sees it now: present only if it is still listed and
// not closed.
Reading Engine::ReadNow(const std::wstring& id) {
    Reading reading;
    std::vector<App> apps;
    std::vector<Session*> objects;
    if (manager_ && ListSessions(&apps, &objects, manager_)) {
        for (size_t i = 0; i < apps.size() && !reading.present; ++i) {
            if (apps[i].id == id && ReadStatusOf(objects[i], &reading)) {
                reading.present = reading.status != kClosed;
            }
        }
    }
    for (Session* object : objects) {
        object->Release();
    }
    return reading;
}

// The same through a second, independent manager (about 15 ms). Used by the freshness audit and,
// in rebuild mode, to confirm commands.
Reading Engine::ReadThroughFreshManager(const std::wstring& id) {
    Reading reading;
    HRESULT hr = S_OK;
    SessionManager* fresh = RequestManager(&hr, nullptr);
    if (!fresh) {
        return reading;
    }
    std::vector<App> apps;
    std::vector<Session*> objects;
    if (ListSessions(&apps, &objects, fresh)) {
        for (size_t i = 0; i < apps.size() && !reading.present; ++i) {
            if (apps[i].id == id && ReadStatusOf(objects[i], &reading)) {
                reading.present = reading.status != kClosed;
            }
        }
    }
    for (Session* object : objects) {
        object->Release();
    }
    fresh->Release();
    return reading;
}

// Start menu lookup of the friendly name ("Chrome" -> "Google Chrome"); for an id Windows does not
// know (a player that uses its file name), a tidied version of the id. Cached per id.
std::wstring Engine::AppName(const std::wstring& id) {
    for (const auto& entry : names_) {
        if (entry.first == id) {
            return entry.second;
        }
    }
    std::wstring name;
    if (g_api.SHCreateItemInKnownFolder) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(g_api.SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, id.c_str(),
                                                      __uuidof(IShellItem), reinterpret_cast<void**>(&item))) &&
            item) {
            PWSTR display = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) && display) {
                name = display;
                g_api.CoTaskMemFree(display);
            }
            item->Release();
        }
    }
    if (name.empty() || name == id) {
        name = id;
        const size_t bang = name.find(L'!');
        if (bang != std::wstring::npos && bang + 1 < name.size()) {
            name = name.substr(bang + 1);
        }
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < name.size()) {
            name = name.substr(slash + 1);
        }
        if (name.size() > 4 && lstrcmpiW(name.c_str() + name.size() - 4, L".exe") == 0) {
            name.resize(name.size() - 4);
        }
        if (!name.empty() && name[0] >= L'a' && name[0] <= L'z') {
            name[0] = static_cast<wchar_t>(name[0] - (L'a' - L'A'));
        }
        if (name.empty()) {
            name = id;
        }
    }
    if (names_.size() < 64) {
        names_.emplace_back(id, name);
    }
    return name;
}

bool Engine::RepeatBlocked(const std::wstring& id) const {
    for (const std::wstring& blocked : repeatBlocked_) {
        if (blocked == id) {
            return true;
        }
    }
    return false;
}

// Holds a new session object. When it belongs to the app already targeted (a new object for the
// same app, or rebuild mode), what is known about the app is kept.
void Engine::SwitchTarget(Session* session, const std::wstring& id) {
    const bool sameApp = !targetId_.empty() && targetId_ == id;
    ReleaseSession();
    if (!sameApp) {
        ForgetTarget();
        targetId_ = id;
        targetName_ = AppName(id);
        repeatMisses_ = 0;
        char id8[kAppIdChars * 3], name8[256];
        Log("music apps: now controlling %s, shown as \"%s\" (%s)", Utf8(id, id8, sizeof(id8)),
            Utf8(targetName_, name8, sizeof(name8)), following_ ? "Windows' choice" : "picked in the panel");
    }
    session->AddRef();
    session_ = session;
    if (!rebuild_) {
        session_->add_PlaybackInfoChanged(&g_onPlayback, &playbackToken_);
        session_->add_MediaPropertiesChanged(&g_onMedia, &mediaToken_);
        session_->add_TimelinePropertiesChanged(&g_onTimeline, &timelineToken_);
        subscribed_ = true;
    }
    // Rebuild mode fetches a new object for the same app on every refresh; its title is then
    // re-read on the 5 s rule rather than every time.
    if (!sameApp || !rebuild_) {
        titleDirty_ = true;
    }
}

// Unsubscribes from and releases the held session object; keeps what is known about the app.
void Engine::ReleaseSession() {
    if (session_) {
        if (subscribed_) {
            session_->remove_PlaybackInfoChanged(playbackToken_);
            session_->remove_MediaPropertiesChanged(mediaToken_);
            session_->remove_TimelinePropertiesChanged(timelineToken_);
            subscribed_ = false;
        }
        ReleaseAndClear(session_);
    }
}

// Lets go of the session and forgets the app.
void Engine::DropTarget() {
    ReleaseSession();
    ForgetTarget();
}

void Engine::ForgetTarget() {
    targetId_.clear();
    targetName_.clear();
    title_.clear();
    artist_.clear();
    hasTimeline_ = false;
    status_ = kClosed;
    play_ = pause_ = toggle_ = stop_ = next_ = previous_ = repeatAllowed_ = false;
    repeatReported_ = false;
}

void Engine::ReadTarget(ULONGLONG now) {
    PlaybackInfo* info = nullptr;
    if (SUCCEEDED(session_->GetPlaybackInfo(&info)) && info) {
        info->get_PlaybackStatus(&status_);
        PlaybackControls* controls = nullptr;
        if (SUCCEEDED(info->get_Controls(&controls)) && controls) {
            boolean play = 0, pause = 0, toggle = 0, stop = 0, next = 0, previous = 0, repeat = 0;
            controls->get_IsPlayEnabled(&play);
            controls->get_IsPauseEnabled(&pause);
            controls->get_IsPlayPauseToggleEnabled(&toggle);
            controls->get_IsStopEnabled(&stop);
            controls->get_IsNextEnabled(&next);
            controls->get_IsPreviousEnabled(&previous);
            controls->get_IsRepeatEnabled(&repeat);
            play_ = play != 0;
            pause_ = pause != 0;
            toggle_ = toggle != 0;
            stop_ = stop != 0;
            next_ = next != 0;
            previous_ = previous != 0;
            repeatAllowed_ = repeat != 0;
            controls->Release();
        }
        wf::IReference<RepeatMode>* repeatRef = nullptr;
        repeatReported_ = false;
        if (SUCCEEDED(info->get_AutoRepeatMode(&repeatRef)) && repeatRef) {
            repeatReported_ = SUCCEEDED(repeatRef->get_Value(&repeatMode_));
            repeatRef->Release();
        }
        // Chrome reports 1.0 while playing and 0.0 while paused, and some apps report nothing.
        // The position is only extrapolated while playing, and then null, zero or a negative
        // rate counts as normal speed.
        wf::IReference<double>* rateRef = nullptr;
        rate_ = 1.0;
        if (SUCCEEDED(info->get_PlaybackRate(&rateRef)) && rateRef) {
            double rate = 0.0;
            if (SUCCEEDED(rateRef->get_Value(&rate)) && rate > 0.0) {
                rate_ = rate;
            }
            rateRef->Release();
        }
        info->Release();
    } else {
        status_ = kClosed;
    }

    Timeline* timeline = nullptr;
    hasTimeline_ = false;
    if (SUCCEEDED(session_->GetTimelineProperties(&timeline)) && timeline) {
        wf::TimeSpan end = {}, position = {};
        wf::DateTime updated = {};
        timeline->get_EndTime(&end);
        timeline->get_Position(&position);
        timeline->get_LastUpdatedTime(&updated);
        // Shown only when the app gives both a length and the time its position was taken.
        if (end.Duration > 0 && updated.UniversalTime > 0) {
            hasTimeline_ = true;
            endTicks_ = end.Duration;
            positionTicks_ = position.Duration;
            positionAsOf_ = updated.UniversalTime;
        }
        timeline->Release();
    }

    if (titleDirty_ || now - titleTick_ >= kTitleRefreshMs) {
        ReadTitle();
        titleTick_ = now;
    }
}

void Engine::ReadTitle() {
    wf::IAsyncOperation<wmc::GlobalSystemMediaTransportControlsSessionMediaProperties*>* op = nullptr;
    HRESULT hr = session_->TryGetMediaPropertiesAsync(&op);
    if (FAILED(hr) || !op) {
        return;   // keep the previous title, try again on the next refresh
    }
    hr = WaitAsync(op, kTitleTimeoutMs);
    MediaProperties* properties = nullptr;
    if (SUCCEEDED(hr)) {
        hr = op->GetResults(&properties);
    }
    if (SUCCEEDED(hr) && properties) {
        HSTRING title = nullptr, artist = nullptr;
        properties->get_Title(&title);
        properties->get_Artist(&artist);
        title_ = TakeString(title);
        artist_ = TakeString(artist);
        properties->Release();
        titleDirty_ = false;
    }
    op->Release();
}

// Every 10 s (and 1 s after an accepted command) a second, independent manager reads the target's
// status. The cheap getters serve a cache fed by notices from Windows' service; if two checks a
// second apart disagree, the notices are not getting through (a possibility in an elevated game)
// and the engine fetches a new manager on every refresh instead.
void Engine::Audit(ULONGLONG now) {
    auditTick_ = now + kAuditEveryMs;
    if (rebuild_ || !session_ || targetId_.empty()) {
        return;
    }
    const Reading fresh = ReadThroughFreshManager(targetId_);
    Reading mine;
    mine.present = ReadStatusOf(session_, &mine) && mine.status != kClosed;
    const bool agree = fresh.present == mine.present && (!fresh.present || fresh.status == mine.status);
    if (agree) {
        auditMisses_ = 0;
        if (!auditAgreedLogged_) {
            auditAgreedLogged_ = true;
            Log("music apps: a fresh check agrees with the held session (\"%s\")",
                fresh.present ? StatusWord(fresh.status) : "closed");
        }
        return;
    }
    ++auditMisses_;
    if (auditMisses_ >= 2) {
        rebuild_ = true;
        Log("music apps: change notices are not arriving (a fresh check reports \"%s\" while the held session reports "
            "\"%s\"); re-reading twice a second instead",
            fresh.present ? StatusWord(fresh.status) : "closed", mine.present ? StatusWord(mine.status) : "closed");
    } else {
        auditTick_ = now + kAuditRecheckMs;
    }
}

void Engine::Refresh() {
    const ULONGLONG now = GetTickCount64();
    if (state_ == State::Unsupported) {
        Publish();
        return;
    }
    if (!manager_) {
        if (now < retryTick_ || !SetupManager()) {
            Publish();
            return;
        }
    } else if (rebuild_) {
        // Rebuild mode: the long-lived objects may be stale, so every refresh starts from a new
        // manager. What is known about the target app is kept (ReleaseSession, not DropTarget).
        ReleaseSession();
        TeardownManager();
        if (!SetupManager()) {
            Publish();
            return;
        }
    }

    std::vector<App> apps;
    std::vector<Session*> objects;
    ListSessions(&apps, &objects, manager_);
    const std::wstring currentId = CurrentSessionId(manager_);
    size_t pick = apps.size();
    for (size_t i = 0; i < apps.size(); ++i) {
        apps[i].isWindowsChoice = apps[i].id == currentId;
        if (!following_ && pick == apps.size() && apps[i].id == pickedId_) {
            pick = i;
        }
    }
    if (following_) {
        // Windows' choice; if it names none, the first app that is playing, else the first listed.
        for (size_t i = 0; i < apps.size() && pick == apps.size(); ++i) {
            if (apps[i].isWindowsChoice) pick = i;
        }
        for (size_t i = 0; i < apps.size() && pick == apps.size(); ++i) {
            if (apps[i].status == Status::Playing) pick = i;
        }
        if (pick == apps.size() && !apps.empty()) {
            pick = 0;
        }
    }

    const bool hadTarget = !targetId_.empty();
    if (pick == apps.size()) {
        if (hadTarget) {
            closedName_ = targetName_;
            closedTick_ = now;
            char id8[kAppIdChars * 3];
            Log("music apps: %s closed; %u music app%s left", Utf8(targetId_, id8, sizeof(id8)),
                static_cast<unsigned>(apps.size()), apps.size() == 1 ? "" : "s");
            DropTarget();
        }
        state_ = following_ ? State::NoSession : State::WaitingForApp;
    } else {
        if (!session_ || session_ != objects[pick] || targetId_ != apps[pick].id) {
            SwitchTarget(objects[pick], apps[pick].id);
        }
        ReadTarget(now);
        if (status_ == kClosed) {
            closedName_ = targetName_;
            closedTick_ = now;
            DropTarget();
            state_ = following_ ? State::NoSession : State::WaitingForApp;
        } else {
            if (closedName_ == targetName_) {
                closedName_.clear();   // the app came back
            }
            state_ = State::Ready;
        }
    }
    for (Session* object : objects) {
        object->Release();
    }
    apps_ = std::move(apps);

    if (now >= auditTick_) {
        Audit(now);
    }
    Publish();
}

void Engine::DrainCommands() {
    Queued queued = {};
    for (unsigned i = 0; i < kQueueSize && Pop(&queued); ++i) {
        Execute(queued);
    }
}

void Engine::SetResult(Result result, Message message, const std::wstring& app) {
    result_ = result;
    resultMessage_ = message;
    resultApp_ = app;
    resultTick_ = GetTickCount64();
    ++resultSequence_;
}

// Polls until the expected change shows or 2.5 s pass. In rebuild mode the held objects may be
// stale, so each poll goes through a fresh manager.
Result Engine::Confirm(Expect expect, PlaybackStatus wantStatus, RepeatMode wantMode, ULONGLONG* tookMs) {
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG deadline = start + kConfirmTimeoutMs;
    for (;;) {
        const Reading now = rebuild_ ? ReadThroughFreshManager(targetId_) : ReadNow(targetId_);
        *tookMs = GetTickCount64() - start;
        if (expect == Expect::StoppedOrGone) {
            if (!now.present || now.status == kStopped) {
                return Result::Done;
            }
        } else if (!now.present) {
            return Result::SessionGone;
        } else if (expect == Expect::StatusIs && now.status == wantStatus) {
            return Result::Done;
        } else if (expect == Expect::RepeatIs && now.repeatReported && now.repeatMode == wantMode) {
            return Result::Done;
        }
        if (GetTickCount64() >= deadline) {
            return Result::NoEffect;
        }
        Sleep(rebuild_ ? 250 : 50);
    }
}

// The one function that sends anything to another program.
//
// Hard guard for the developer test host: the only session a test may ever command is the host's
// own silent player, named in RACING_MASTER_MOD_TEST_MEDIA_APP. A real Spotify or browser session
// on the developer's PC cannot receive a test command, even when Windows calls it current.
HRESULT Engine::Send(Call call, RepeatMode mode, boolean* accepted) {
    *accepted = 0;
    if (g_testHost && (g_testApp[0] == L'\0' || targetId_ != g_testApp)) {
        return kRefusedByTestGuard;
    }
    if (!session_) {
        return E_POINTER;
    }
    BoolOperation* op = nullptr;
    HRESULT hr = S_OK;
    switch (call) {
        case Call::Play: hr = session_->TryPlayAsync(&op); break;
        case Call::Pause: hr = session_->TryPauseAsync(&op); break;
        case Call::Toggle: hr = session_->TryTogglePlayPauseAsync(&op); break;
        case Call::Stop: hr = session_->TryStopAsync(&op); break;
        case Call::Next: hr = session_->TrySkipNextAsync(&op); break;
        case Call::Previous: hr = session_->TrySkipPreviousAsync(&op); break;
        case Call::Repeat: hr = session_->TryChangeAutoRepeatModeAsync(mode, &op); break;
    }
    if (SUCCEEDED(hr) && op) {
        hr = WaitAsync(op, kCommandTimeoutMs);
        if (SUCCEEDED(hr)) {
            hr = op->GetResults(accepted);
        }
        op->Release();
    } else if (SUCCEEDED(hr)) {
        hr = E_POINTER;
    }
    return hr;
}

void Engine::Execute(const Queued& queued) {
    if (queued.command == Command::PickApp) {
        following_ = false;
        pickedId_ = queued.appId;
        pickedName_ = AppName(pickedId_);
        closedName_.clear();
        char id8[kAppIdChars * 3];
        Log("music apps: the panel picked %s", Utf8(pickedId_, id8, sizeof(id8)));
        Refresh();
        return;
    }
    if (queued.command == Command::FollowWindows) {
        following_ = true;
        pickedId_.clear();
        pickedName_.clear();
        closedName_.clear();
        Log("music apps: the panel follows Windows' choice again");
        Refresh();
        return;
    }
    if (queued.command == Command::TestHang) {
        // Test host only: stands in for a Windows call that never returns, so the test can see
        // GetSnapshot() report "not answering" and recover afterwards.
        if (g_testHost) {
            Log("music apps test: the media thread stops answering for 6 s on purpose");
            Sleep(6000);
        }
        return;
    }
    if (queued.command == Command::TestRebuild) {
        // Test host only: switches on the mode the freshness check falls back to when change
        // notices do not arrive, so its refresh and confirmation paths run in the test.
        if (g_testHost && !rebuild_) {
            rebuild_ = true;
            Log("music apps test: re-reading through a new manager on every refresh from now on");
            Refresh();
        }
        return;
    }

    const Command command = queued.command;
    const char* word = CommandWord(command);
    if (!session_ || state_ != State::Ready) {
        Log("music apps: \"%s\" requested but no music app is being controlled", word);
        if (!following_ && !pickedName_.empty()) {
            SetResult(Result::NoSession, closedName_.empty() ? Message::PickedNotRunning : Message::PickedClosed,
                      pickedName_);
        } else {
            SetResult(Result::NoSession, Message::NoApp, L"");
        }
        Publish();
        return;
    }
    const std::wstring app = targetName_;
    char id8[kAppIdChars * 3];
    Utf8(targetId_, id8, sizeof(id8));

    // Which call, what counts as "it worked", and why it might not be sent at all.
    Call call = Call::Play;
    Expect expect = Expect::Nothing;
    PlaybackStatus wantStatus = kPlaying;
    RepeatMode wantMode = wm::MediaPlaybackAutoRepeatMode_None;
    Message notOffered = Message::None;
    Message already = Message::None;
    switch (command) {
        case Command::Play:
            if (status_ == kPlaying) already = Message::AlreadyPlaying;
            else if (!play_) notOffered = Message::NoPlay;
            call = Call::Play;
            expect = Expect::StatusIs;
            break;
        case Command::Pause:
            if (status_ == kPaused) already = Message::AlreadyPaused;
            else if (!pause_) notOffered = Message::NoPause;
            call = Call::Pause;
            expect = Expect::StatusIs;
            wantStatus = kPaused;
            break;
        case Command::Toggle:
            // Explicit calls first: a toggle does the opposite of what was meant when the snapshot
            // is a moment out of date. The toggle itself only when neither is offered.
            if (status_ == kPlaying && pause_) {
                call = Call::Pause;
                expect = Expect::StatusIs;
                wantStatus = kPaused;
            } else if (status_ != kPlaying && play_) {
                call = Call::Play;
                expect = Expect::StatusIs;
            } else if (toggle_) {
                call = Call::Toggle;
                expect = (status_ == kPlaying || status_ == kPaused) ? Expect::StatusIs : Expect::Nothing;
                wantStatus = status_ == kPlaying ? kPaused : kPlaying;
            } else {
                notOffered = Message::NoPlayPause;
            }
            break;
        case Command::Stop:
            if (!stop_) notOffered = Message::NoStop;   // never replaced by pause without the player's say
            call = Call::Stop;
            expect = Expect::StoppedOrGone;
            break;
        case Command::Next:
            if (!next_) notOffered = Message::NoNext;
            call = Call::Next;
            break;
        case Command::Previous:
            if (!previous_) notOffered = Message::NoPrevious;
            call = Call::Previous;
            break;
        case Command::RepeatOff:
        case Command::RepeatOne:
        case Command::RepeatAll:
            call = Call::Repeat;
            expect = Expect::RepeatIs;
            wantMode = command == Command::RepeatOff   ? wm::MediaPlaybackAutoRepeatMode_None
                       : command == Command::RepeatOne ? wm::MediaPlaybackAutoRepeatMode_Track
                                                       : wm::MediaPlaybackAutoRepeatMode_List;
            if (!repeatAllowed_) {
                notOffered = Message::LoopNotAllowed;
            } else if (RepeatBlocked(targetId_)) {
                notOffered = Message::LoopIgnored;
            }
            break;
        default:
            return;
    }
    if (already != Message::None) {
        Log("music apps: \"%s\" not sent to %s: it is already in that state", word, id8);
        SetResult(Result::Done, already, app);
        Publish();
        return;
    }
    if (notOffered != Message::None) {
        Log("music apps: \"%s\" is not offered by %s right now", word, id8);
        SetResult(Result::NotOffered, notOffered, app);
        Publish();
        return;
    }

    SetResult(Result::Pending, Message::Sending, app);
    Publish();

    const PlaybackStatus statusAtSend = status_;
    boolean accepted = 0;
    const HRESULT hr = Send(call, wantMode, &accepted);
    if (hr == kRefusedByTestGuard) {
        Log("music apps: refused to send \"%s\" to %s: the test host may only command the test player", word, id8);
        SetResult(Result::Refused, Message::TestGuard, app);
        Publish();
        return;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        Log("music apps: \"%s\" sent to %s: no reply after %lu ms", word, id8, kCommandTimeoutMs);
        SetResult(Result::NoReply, Message::NoReply, app);
        Refresh();
        return;
    }
    if (FAILED(hr) || !accepted) {
        if (FAILED(hr)) {
            Log("music apps: \"%s\" sent to %s: failed (0x%08lx)", word, id8, static_cast<unsigned long>(hr));
        } else {
            Log("music apps: \"%s\" sent to %s: refused", word, id8);
        }
        SetResult(Result::Refused, Message::Refused, app);
        Refresh();
        return;
    }

    auditTick_ = GetTickCount64() + kAuditRecheckMs;
    ULONGLONG tookMs = 0;
    const Result result = expect == Expect::Nothing ? Result::Done : Confirm(expect, wantStatus, wantMode, &tookMs);
    if (call == Call::Next || call == Call::Previous) {
        titleDirty_ = true;   // no reliable signal for a skip; re-read the title
    }

    switch (result) {
        case Result::Done: {
            if (expect == Expect::Nothing) {
                Log("music apps: \"%s\" sent to %s: accepted", word, id8);
            } else {
                Log("music apps: \"%s\" sent to %s: accepted, confirmed after %llu ms", word, id8, tookMs);
            }
            Message done = Message::DonePlayPause;
            if (call == Call::Repeat) {
                repeatMisses_ = 0;
                done = wantMode == wm::MediaPlaybackAutoRepeatMode_None    ? Message::DoneLoopOff
                       : wantMode == wm::MediaPlaybackAutoRepeatMode_Track ? Message::DoneLoopOne
                                                                             : Message::DoneLoopAll;
            } else if (call == Call::Next) {
                done = Message::DoneNext;
            } else if (call == Call::Previous) {
                done = Message::DonePrevious;
            } else if (call == Call::Stop) {
                done = Message::DoneStopped;
            } else if (expect != Expect::Nothing) {
                done = wantStatus == kPlaying ? Message::DonePlaying : Message::DonePaused;
            }
            SetResult(Result::Done, done, app);
            break;
        }
        case Result::NoEffect:
            if (call == Call::Repeat) {
                const Reading now = ReadNow(targetId_);
                Log("music apps: \"%s\" sent to %s: accepted, but it still reports looping \"%s\" after %.1f s", word,
                    id8, now.repeatReported ? RepeatWord(now.repeatMode) : "not reported", kConfirmTimeoutMs / 1000.0);
                // Two misses in a row: the button stays off with this reason for the rest of the run.
                if (++repeatMisses_ >= 2 && !RepeatBlocked(targetId_)) {
                    repeatBlocked_.push_back(targetId_);
                    Log("music apps: looping in %s cannot be changed from the panel for the rest of this run", id8);
                }
                SetResult(Result::NoEffect, Message::LoopIgnored, app);
            } else {
                const Reading now = ReadNow(targetId_);
                Log("music apps: \"%s\" sent to %s: accepted, but it still reports \"%s\" after %.1f s (it was \"%s\")",
                    word, id8, now.present ? StatusWord(now.status) : "closed", kConfirmTimeoutMs / 1000.0,
                    StatusWord(statusAtSend));
                SetResult(Result::NoEffect, Message::NoEffect, app);
            }
            break;
        case Result::SessionGone:
            // Nothing is re-sent to another app.
            Log("music apps: \"%s\" sent to %s: accepted, but the app closed before it took effect", word, id8);
            SetResult(Result::SessionGone, following_ ? Message::AppClosed : Message::PickedClosed, app);
            break;
        default:
            SetResult(result, Message::None, L"");
            break;
    }
    Refresh();
}

// Which line the panel shows when no title is showing, and for which app.
void Engine::ComposeMessage(ULONGLONG now, Message* id, std::wstring* app) {
    *app = L"";
    switch (state_) {
        case State::Off: *id = Message::None; return;
        case State::Unsupported: *id = Message::Unsupported; return;
        case State::NotAnswering: *id = Message::NotAnswering; return;
        case State::Starting: *id = Message::LookingForApps; return;
        case State::Faulted: *id = Message::Faulted; return;
        default: break;
    }
    // The outcome of the last command first, for a few seconds (a pending one until it settles).
    if (result_ == Result::Pending ||
        (resultMessage_ != Message::None && now - resultTick_ < kResultShownMs)) {
        *id = resultMessage_;
        *app = resultApp_;
        return;
    }
    if (!following_ && !session_) {
        *id = closedName_.empty() ? Message::WaitingForApp : Message::PickedClosed;
        *app = closedName_.empty() ? pickedName_ : closedName_;
        return;
    }
    if (following_ && !closedName_.empty() && now - closedTick_ < kClosedShownMs) {
        *id = Message::AppClosed;
        *app = closedName_;
        return;
    }
    if (!session_) {
        *id = Message::NoApp;
        return;
    }
    *id = Message::None;
    if (title_.empty()) {
        *app = targetName_;
        switch (status_) {
            case kPlaying: *id = Message::IsPlaying; break;
            case kPaused: *id = Message::IsPaused; break;
            case kStopped: *id = Message::IsStopped; break;
            default: app->clear(); break;
        }
    }
}

void Engine::Publish() {
    const ULONGLONG now = GetTickCount64();
    Published fresh;
    fresh.publishedTick = now;
    fresh.watched = state_ != State::Off && state_ != State::Unsupported && state_ != State::Faulted;
    Snapshot& s = fresh.snapshot;
    s.state = state_;
    s.available = session_ != nullptr && state_ == State::Ready;
    s.followingWindows = following_;
    s.apps = apps_;
    s.lastResult = result_;
    s.resultSequence = resultSequence_;
    if (s.available) {
        s.status = ToStatus(status_);
        s.app = targetName_;
        s.appId = targetId_;
        s.title = title_;
        s.artist = artist_;
        s.repeat = !repeatReported_                                       ? Repeat::Unknown
                   : repeatMode_ == wm::MediaPlaybackAutoRepeatMode_None  ? Repeat::Off
                   : repeatMode_ == wm::MediaPlaybackAutoRepeatMode_Track ? Repeat::One
                                                                            : Repeat::All;
        s.controls = {play_, pause_, stop_, next_, previous_, repeatAllowed_ && !RepeatBlocked(targetId_)};
        s.hasTimeline = hasTimeline_;
        if (hasTimeline_) {
            fresh.positionTicks = positionTicks_;
            fresh.endTicks = endTicks_;
            fresh.positionAsOf = positionAsOf_;
            fresh.rate = rate_;
        }
    } else if (!following_) {
        s.app = pickedName_;
        s.appId = pickedId_;
    }
    ComposeMessage(now, &s.messageId, &s.messageApp);
    s.message = Text(s.messageId, s.messageApp);

    Published& store = Store();
    AcquireSRWLockExclusive(&g_snapshotLock);
    std::swap(store, fresh);   // moves only, no allocation under the lock; the old copy is freed after it
    ReleaseSRWLockExclusive(&g_snapshotLock);
}

// ---- the thread -------------------------------------------------------------------------------------------

DWORD g_faultCode = 0;
void* g_faultAddress = nullptr;

LONG FaultFilter(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_faultCode = code;
    g_faultAddress = info->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

// After a fault nothing COM-related is touched again in this run; the objects are leaked on purpose.
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
    Log("music apps: the media thread failed (exception 0x%08lx at %s+0x%llx) and has stopped; other apps cannot be "
        "controlled until the game is restarted",
        static_cast<unsigned long>(g_faultCode), path8, offset);
    Published fresh;
    fresh.publishedTick = GetTickCount64();
    fresh.snapshot.state = State::Faulted;
    fresh.snapshot.messageId = Message::Faulted;
    fresh.snapshot.message = Text(Message::Faulted, L"");
    Published& store = Store();
    AcquireSRWLockExclusive(&g_snapshotLock);
    std::swap(store, fresh);
    ReleaseSRWLockExclusive(&g_snapshotLock);
}

void RunEngine() {
    Engine* engine = new Engine;
    engine->Run();
    delete engine;
}

// Keep C++ objects with destructors out of this function (the compiler refuses them next to __try).
DWORD WINAPI MediaThread(LPVOID) {
    __try {
        RunEngine();
    } __except (FaultFilter(GetExceptionInformation())) {
        ReportFault();
    }
    return 0;
}

}  // namespace

// ---- public interface ----------------------------------------------------------------------------------------

Remote& Remote::Get() {
    static Remote instance;
    return instance;
}

void Remote::Start() {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) {
        return;   // already created
    }
    g_testHost = ModIsTestHost();
    if (g_testHost) {
        const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_MEDIA_APP", g_testApp, kAppIdChars);
        if (n == 0 || n >= kAppIdChars) {
            g_testApp[0] = L'\0';
        }
    }
    Store();   // allocate before any reader can race with the thread
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) {
        Log("music apps: could not create the thread events; other apps cannot be controlled");
        return;
    }
    g_thread = CreateThread(nullptr, 0, MediaThread, nullptr, 0, nullptr);
    if (!g_thread) {
        Log("music apps: could not start the media thread (error %lu); other apps cannot be controlled",
            GetLastError());
    }
}

void Remote::Shutdown() {
    // Not built yet: the media thread runs until the process ends.
}

void Remote::Play() { Post(Command::Play, nullptr); }
void Remote::Pause() { Post(Command::Pause, nullptr); }
void Remote::TogglePlayPause() { Post(Command::Toggle, nullptr); }
void Remote::Stop() { Post(Command::Stop, nullptr); }
void Remote::Next() { Post(Command::Next, nullptr); }
void Remote::Previous() { Post(Command::Previous, nullptr); }

void Remote::SetRepeat(Repeat r) {
    switch (r) {
        case Repeat::Off: Post(Command::RepeatOff, nullptr); break;
        case Repeat::One: Post(Command::RepeatOne, nullptr); break;
        case Repeat::All: Post(Command::RepeatAll, nullptr); break;
        default: break;
    }
}

void Remote::SelectApp(const std::wstring& appId) {
    if (!appId.empty() && appId.size() < kAppIdChars) {
        Post(Command::PickApp, appId.c_str());
    }
}

void Remote::FollowWindows() { Post(Command::FollowWindows, nullptr); }

Snapshot Remote::GetSnapshot() const {
    if (!g_started) {
        return Snapshot();
    }
    Snapshot snapshot;
    long long positionTicks = 0, endTicks = 0, positionAsOf = 0;
    double rate = 1.0;
    ULONGLONG publishedTick = 0;
    bool watched = false;
    {
        const Published& store = Store();
        AcquireSRWLockShared(&g_snapshotLock);
        snapshot = store.snapshot;
        positionTicks = store.positionTicks;
        endTicks = store.endTicks;
        positionAsOf = store.positionAsOf;
        rate = store.rate;
        publishedTick = store.publishedTick;
        watched = store.watched;
        ReleaseSRWLockShared(&g_snapshotLock);
    }
    // A synchronous call into Windows cannot be given a deadline. If one hangs, only the media
    // thread hangs, and the snapshot stops being renewed: say so instead of showing old state.
    if (watched && publishedTick && GetTickCount64() - publishedTick > kStaleMs) {
        snapshot.state = State::NotAnswering;
        snapshot.available = false;
        snapshot.controls = {};
        snapshot.messageId = Message::NotAnswering;
        snapshot.messageApp.clear();
        snapshot.message = Text(Message::NotAnswering, L"");
    }
    if (snapshot.available && snapshot.hasTimeline) {
        long long ticks = positionTicks;
        if (snapshot.status == Status::Playing) {
            // The reported position is the position at its time stamp, not now.
            FILETIME nowFt = {};
            GetSystemTimeAsFileTime(&nowFt);
            const long long now = static_cast<long long>((static_cast<unsigned long long>(nowFt.dwHighDateTime) << 32) |
                                                         nowFt.dwLowDateTime);
            const long long elapsed = now > positionAsOf ? now - positionAsOf : 0;
            ticks += static_cast<long long>(static_cast<double>(elapsed) * rate);
        }
        if (ticks < 0) ticks = 0;
        if (ticks > endTicks) ticks = endTicks;
        snapshot.positionSeconds = static_cast<double>(ticks) / 1e7;
        snapshot.durationSeconds = static_cast<double>(endTicks) / 1e7;
    }
    return snapshot;
}

// ---- test script (developer test host only) ------------------------------------------------------------------

namespace {

const char* PublicStatusWord(Status status) {
    switch (status) {
        case Status::NoSession: return "no session";
        case Status::Playing: return "playing";
        case Status::Paused: return "paused";
        case Status::Stopped: return "stopped";
        case Status::Unknown: return "unknown";
    }
    return "?";
}

const char* PublicRepeatWord(Repeat repeat) {
    switch (repeat) {
        case Repeat::Unknown: return "not reported";
        case Repeat::Off: return "off";
        case Repeat::One: return "one";
        case Repeat::All: return "all";
    }
    return "?";
}

// Waits until the predicate holds or the time is up; returns whether it held.
template <typename F>
bool WaitFor(DWORD timeoutMs, F predicate) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (predicate()) return true;
        if (GetTickCount64() - start >= timeoutMs) return false;
        Sleep(20);
    }
}

// Runs the steps of RACING_MASTER_MOD_TEST_MEDIA_SCRIPT one after another. The next step goes out
// only once the previous one has settled (up to 1.5 s for a reply plus 2.5 s for the
// confirmation), then 400 ms later.
DWORD WINAPI TestScriptThread(LPVOID) {
    Remote& remote = Remote::Get();
    wchar_t script[1024] = L"";
    const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_MEDIA_SCRIPT", script, 1024);
    if (n == 0 || n >= 1024) {
        return 0;
    }
    remote.Start();
    if (g_testApp[0] == L'\0') {
        Log("music apps test: RACING_MASTER_MOD_TEST_MEDIA_APP is not set; the script is not run");
        return 0;
    }
    // The app to pick defaults to the one commands may go to. A different pick tests the guard.
    wchar_t pick[kAppIdChars] = L"";
    const DWORD p = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_MEDIA_PICK", pick, kAppIdChars);
    if (p == 0 || p >= kAppIdChars) {
        lstrcpynW(pick, g_testApp, kAppIdChars);
    }
    char script8[1024 * 3], pick8[kAppIdChars * 3];
    Log("music apps test: script \"%s\", picking %s", LogUtf8(script, script8, sizeof(script8)),
        LogUtf8(pick, pick8, sizeof(pick8)));

    remote.SelectApp(pick);
    const ULONGLONG waitStart = GetTickCount64();
    WaitFor(5000, [&] {
        const Snapshot s = remote.GetSnapshot();
        return !s.followingWindows && s.state != State::Starting;
    });
    Log("music apps test: the pick took effect; state: %s", StateWord(remote.GetSnapshot().state));
    const bool ready = WaitFor(10000, [&] { return remote.GetSnapshot().available; });
    Log("music apps test: %s after %llu ms", ready ? "the test player is ready" : "the test player did not appear",
        GetTickCount64() - waitStart);

    unsigned step = 0;
    wchar_t* cursor = script;
    while (*cursor) {
        wchar_t* end = cursor;
        while (*end && *end != L';') {
            ++end;
        }
        const bool more = *end != L'\0';
        *end = L'\0';
        const wchar_t* name = cursor;
        cursor = more ? end + 1 : end;
        if (*name == L'\0') {
            continue;
        }
        ++step;
        char name8[64];
        LogUtf8(name, name8, sizeof(name8));
        if (lstrcmpW(name, L"read") == 0) {
            // What the panel would see. Whether a title is shared, never the title itself.
            const Snapshot s = remote.GetSnapshot();
            std::string controls;
            const struct { bool on; const char* word; } offered[] = {
                {s.controls.play, "play"}, {s.controls.pause, "pause"}, {s.controls.stop, "stop"},
                {s.controls.next, "next"}, {s.controls.previous, "previous"}, {s.controls.repeat, "loop"}};
            for (const auto& control : offered) {
                if (control.on) {
                    controls += controls.empty() ? "" : " ";
                    controls += control.word;
                }
            }
            char app8[256];
            Log("music apps test: step %u \"read\": state: %s; app: \"%s\"; status: %s; offers: %s; loop: %s; "
                "timeline: %s; title shared: %s",
                step, StateWord(s.state), LogUtf8(s.app.c_str(), app8, sizeof(app8)), PublicStatusWord(s.status),
                controls.empty() ? "nothing" : controls.c_str(), PublicRepeatWord(s.repeat),
                s.hasTimeline ? "yes" : "no", s.title.empty() ? "no" : "yes");
            Sleep(400);
            continue;
        }
        if (lstrcmpW(name, L"rebuild") == 0) {
            Post(Command::TestRebuild, nullptr);
            Sleep(600);
            Log("music apps test: step %u \"rebuild\": state: %s", step, StateWord(remote.GetSnapshot().state));
            continue;
        }
        if (lstrcmpW(name, L"hang") == 0) {
            const ULONGLONG hangStart = GetTickCount64();
            Post(Command::TestHang, nullptr);
            const bool noticed = WaitFor(8000, [&] { return remote.GetSnapshot().state == State::NotAnswering; });
            const ULONGLONG noticedMs = GetTickCount64() - hangStart;
            const bool recovered = WaitFor(8000, [&] { return remote.GetSnapshot().available; });
            Log("music apps test: step %u \"hang\": %s after %llu ms; %s", step,
                noticed ? "the snapshot said Windows is not answering" : "the snapshot never said Windows is not answering",
                noticedMs, recovered ? "ready again afterwards" : "did not recover");
            Sleep(400);
            continue;
        }
        const unsigned before = remote.GetSnapshot().resultSequence;
        if (lstrcmpW(name, L"play") == 0) remote.Play();
        else if (lstrcmpW(name, L"pause") == 0) remote.Pause();
        else if (lstrcmpW(name, L"toggle") == 0) remote.TogglePlayPause();
        else if (lstrcmpW(name, L"stop") == 0) remote.Stop();
        else if (lstrcmpW(name, L"next") == 0) remote.Next();
        else if (lstrcmpW(name, L"previous") == 0) remote.Previous();
        else if (lstrcmpW(name, L"repeat-off") == 0) remote.SetRepeat(Repeat::Off);
        else if (lstrcmpW(name, L"repeat-one") == 0) remote.SetRepeat(Repeat::One);
        else if (lstrcmpW(name, L"repeat-all") == 0) remote.SetRepeat(Repeat::All);
        else {
            Log("music apps test: step %u \"%s\": unknown step, skipped", step, name8);
            continue;
        }
        Snapshot snapshot;
        const bool settled = WaitFor(8000, [&] {
            snapshot = remote.GetSnapshot();
            return snapshot.resultSequence != before && snapshot.lastResult != Result::Pending;
        });
        if (!settled) {
            Log("music apps test: step %u \"%s\": no outcome after 8 s", step, name8);
        } else {
            // The message never holds a title (only app names), so it may go into the log.
            char message8[512];
            Log("music apps test: step %u \"%s\": %s; state: %s; message: \"%s\"", step, name8,
                ResultWord(snapshot.lastResult), StateWord(snapshot.state),
                LogUtf8(snapshot.message.c_str(), message8, sizeof(message8)));
        }
        Sleep(400);
    }
    Log("music apps test: finished; the media thread keeps running");
    return 0;
}

}  // namespace

void StartTestScriptIfAsked() {
    if (!ModIsTestHost()) {
        return;
    }
    wchar_t probe[4];
    const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_MEDIA_SCRIPT", probe, 4);
    if (n == 0) {
        return;
    }
    const HANDLE thread = CreateThread(nullptr, 0, TestScriptThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        Log("music apps test: could not start the script thread (error %lu)", GetLastError());
    }
}

}  // namespace remote

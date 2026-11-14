// "Other app": the real media remote, or in the developer test host an example app. See other_app.h.

#include "other_app.h"

#include "../core/log.h"
#include "../core/worker.h"

#include <windows.h>

#include <atomic>

namespace {

enum class Kind { Real, ExampleLimited, ExampleFull, ExampleNone };

// Decided once: the test host flag and the variable do not change while the process runs.
Kind DecideKind() {
    if (!ModIsTestHost()) {
        return Kind::Real;
    }
    wchar_t value[16] = L"";
    const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_REMOTE", value, 16);
    if (n > 0 && n < 16) {
        if (lstrcmpiW(value, L"real") == 0) return Kind::Real;
        if (lstrcmpiW(value, L"full") == 0) return Kind::ExampleFull;
        if (lstrcmpiW(value, L"none") == 0) return Kind::ExampleNone;
    }
    return Kind::ExampleLimited;
}

Kind TheKind() {
    static const Kind kind = DecideKind();
    return kind;
}

bool Real() {
    return TheKind() == Kind::Real;
}

// The example: a web browser playing a song. "limited" allows play and pause only and shows no
// timeline; "full" allows everything and shows one; "none" means no app is playing. Frame thread
// only. Every command is logged as "test app: ...", which the host test reads.
remote::Snapshot& Example() {
    static remote::Snapshot snapshot;
    static bool filled = false;
    if (!filled) {
        filled = true;
        const Kind kind = TheKind();
        const bool full = kind == Kind::ExampleFull;
        snapshot.state = kind == Kind::ExampleNone ? remote::State::NoSession : remote::State::Ready;
        snapshot.available = kind != Kind::ExampleNone;
        snapshot.status = kind == Kind::ExampleNone ? remote::Status::NoSession : remote::Status::Playing;
        if (kind != Kind::ExampleNone) {
            snapshot.app = L"Web browser";
            snapshot.title = L"Sunset Drive";
            snapshot.artist = L"Example Artist";
        }
        snapshot.hasTimeline = full;
        snapshot.positionSeconds = full ? 64.0 : 0.0;
        snapshot.durationSeconds = full ? 215.0 : 0.0;
        snapshot.repeat = full ? remote::Repeat::All : remote::Repeat::Unknown;
        const bool any = kind != Kind::ExampleNone;
        // play, pause, stop, next, previous, repeat
        snapshot.controls = {any, any, full, full, full, full};
    }
    return snapshot;
}

std::atomic<bool> g_started{false};

}  // namespace

namespace other_app {

void Start() {
    if (g_started.exchange(true)) {
        return;
    }
    if (!Real()) {
        Log("other app: test host, so \"Other app\" is an example app inside the mod; nothing reads Windows' media "
            "sessions");
        return;
    }
    Log("other app: starting the control of other music apps (Windows media sessions)");
    remote::Remote::Get().Start();
}

remote::Snapshot GetSnapshot() {
    if (Real()) {
        return remote::Remote::Get().GetSnapshot();
    }
    return Example();
}

void Play() {
    if (Real()) return remote::Remote::Get().Play();
    Example().status = remote::Status::Playing;
    Log("test app: play");
}

void Pause() {
    if (Real()) return remote::Remote::Get().Pause();
    Example().status = remote::Status::Paused;
    Log("test app: pause");
}

void Stop() {
    if (Real()) return remote::Remote::Get().Stop();
    Example().status = remote::Status::Stopped;
    Log("test app: stop");
}

void Next() {
    if (Real()) return remote::Remote::Get().Next();
    Log("test app: next");
}

void Previous() {
    if (Real()) return remote::Remote::Get().Previous();
    Log("test app: previous");
}

void SetRepeat(remote::Repeat repeat) {
    if (Real()) return remote::Remote::Get().SetRepeat(repeat);
    Example().repeat = repeat;
    Log("test app: repeat changed");
}

void Shutdown() {
    if (Real() && g_started.load()) {
        remote::Remote::Get().Shutdown();
    }
}

}  // namespace other_app

// STAND-IN implementations of music::Player and the file pickers, compiled only
// when the build defines RMM_UI_STANDALONE (build\build.ps1 does so while the real modules are
// not in the tree). They let the panel be built, drawn and tested on its own:
//
//   - an example playlist: four fictional songs in English and Chinese, the second playing at
//     1:24 of 3:50, repeat all, volume 70 %. RACING_MASTER_MOD_TEST_PLAYLIST=long gives ten
//     songs instead (Japanese, Korean, a Hong Kong character and CJK Extension B, a file that
//     cannot be played, a very long title) to check fonts and the list's edge cases.
//     No audio device is opened, nothing is played, the position does not move.
//   - "Add files" / "Add folder" add example entries at once instead of opening a window
//   - "Other app" is not here: the real media remote is used, with an example app in the test
//     host (src/ui/other_app.cpp)
//
// Every call is written to the log ("test player: ..."), which the host test reads.

#if defined(RMM_UI_STANDALONE)

#include "interfaces.h"

#include "../../core/log.h"

#include <windows.h>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;

struct PlayerState {
    std::vector<music::TrackInfo> tracks;
    music::State state = music::State::Stopped;
    int current = -1;
    double position = 0.0;
    music::LoopMode loop = music::LoopMode::Off;
    float volume = 0.7f;
    bool pauseWhenUnfocused = false;
    bool focused = true;
    std::wstring message;
    bool filled = false;
};

PlayerState& Data() {
    static PlayerState state;
    return state;
}

music::TrackInfo Track(const wchar_t* path, const wchar_t* title, const wchar_t* artist, double seconds,
                       const wchar_t* problem = nullptr) {
    music::TrackInfo track;
    track.path = path;
    track.title = title;
    track.artist = artist;
    track.durationSeconds = seconds;
    track.supported = problem == nullptr;
    track.problem = problem ? problem : L"";
    return track;
}

// Called with the lock held.
void FillExamples(PlayerState& s) {
    if (s.filled) {
        return;
    }
    s.filled = true;
    wchar_t kind[16] = L"";
    const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_PLAYLIST", kind, 16);
    if (n == 0 || n >= 16 || lstrcmpiW(kind, L"long") != 0) {
        s.tracks.push_back(Track(L"C:\\Music\\midnight-highway.mp3", L"Midnight Highway", L"Example Band", 222));
        s.tracks.push_back(Track(L"C:\\Music\\city-lights.flac", L"城市的燈火", L"範例歌手", 230));
        s.tracks.push_back(Track(L"C:\\Music\\final-lap.mp3", L"Final Lap", L"Example Band", 179));
        s.tracks.push_back(Track(L"C:\\Music\\night-road.flac", L"夜空下的公路", L"示範樂團", 251));
        s.current = 1;
        s.state = music::State::Playing;
        s.position = 84.0;
        s.loop = music::LoopMode::All;
        return;
    }
    s.tracks.push_back(Track(L"C:\\Music\\midnight-highway.mp3", L"Midnight Highway", L"Example Band", 222));
    s.tracks.push_back(Track(L"C:\\Music\\city-lights.flac", L"城市的燈火", L"範例歌手", 245));
    s.tracks.push_back(Track(L"C:\\Music\\dawn-drive.ogg", L"夜明けのドライブ", L"サンプル", 198));
    s.tracks.push_back(Track(L"C:\\Music\\slow-lane.m4a", L"Slow Lane Sunset", L"", 301));
    s.tracks.push_back(Track(L"C:\\Music\\track_05_final.opus", L"", L"", 167));
    s.tracks.push_back(Track(L"C:\\Music\\seoul-night.mp3", L"서울의 밤", L"예시", 213));
    s.tracks.push_back(Track(L"C:\\Music\\broken.xyz", L"broken.xyz", L"", 0, L"this kind of file is not supported"));
    s.tracks.push_back(Track(L"C:\\Music\\long.mp3",
                             L"A very long title that keeps going well past the edge of the panel to show how it ends",
                             L"Someone With A Long Name", 380));
    s.tracks.push_back(Track(L"C:\\Music\\hk.mp3", L"粵語歌 㗎喎 \U0002070E", L"示範", 199));
    s.tracks.push_back(Track(L"C:\\Music\\final-lap.wav", L"Final Lap", L"Example Band", 179));
    s.current = 1;
    s.state = music::State::Playing;
    s.position = 83.0;
}

class Locked {
public:
    Locked() { AcquireSRWLockExclusive(&g_lock); }
    ~Locked() { ReleaseSRWLockExclusive(&g_lock); }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
};

const char* StateWord(music::State state) {
    switch (state) {
        case music::State::Playing: return "playing";
        case music::State::Paused: return "paused";
        default: return "stopped";
    }
}

void LogPlayer(const char* action) {
    PlayerState copy;
    {
        Locked lock;
        copy.state = Data().state;
        copy.current = Data().current;
        copy.loop = Data().loop;
        copy.volume = Data().volume;
        copy.tracks.resize(Data().tracks.size());
    }
    // Logged outside the lock (Log must not be called while holding another lock).
    Log("test player: %s (now %s, song %d of %d, repeat %d, volume %d)", action, StateWord(copy.state),
        copy.current + 1, static_cast<int>(copy.tracks.size()), static_cast<int>(copy.loop),
        static_cast<int>(copy.volume * 100.0f + 0.5f));
}

}  // namespace

namespace music {

Player& Player::Get() {
    static Player player;
    return player;
}

void Player::Start() { LogPlayer("start"); }
void Player::Shutdown() {}

void Player::Play() {
    {
        Locked lock;
        PlayerState& s = Data();
        FillExamples(s);
        if (!s.tracks.empty()) {
            if (s.current < 0) s.current = 0;
            s.state = State::Playing;
        }
    }
    LogPlayer("play");
}

void Player::Pause() {
    {
        Locked lock;
        if (Data().state == State::Playing) Data().state = State::Paused;
    }
    LogPlayer("pause");
}

void Player::TogglePlayPause() {
    {
        Locked lock;
        PlayerState& s = Data();
        FillExamples(s);
        if (s.state == State::Playing) {
            s.state = State::Paused;
        } else if (!s.tracks.empty()) {
            if (s.current < 0) s.current = 0;
            s.state = State::Playing;
        }
    }
    LogPlayer("play or pause");
}

void Player::Stop() {
    {
        Locked lock;
        Data().state = State::Stopped;
        Data().position = 0.0;
    }
    LogPlayer("stop");
}

void Player::Next() {
    {
        Locked lock;
        PlayerState& s = Data();
        FillExamples(s);
        if (!s.tracks.empty()) {
            s.current = (s.current + 1) % static_cast<int>(s.tracks.size());
            s.position = 0.0;
        }
    }
    LogPlayer("next");
}

void Player::Previous() {
    {
        Locked lock;
        PlayerState& s = Data();
        FillExamples(s);
        if (!s.tracks.empty()) {
            const int n = static_cast<int>(s.tracks.size());
            s.current = (s.current + n - 1) % n;
            s.position = 0.0;
        }
    }
    LogPlayer("previous");
}

void Player::PlayIndex(int index) {
    {
        Locked lock;
        PlayerState& s = Data();
        if (index >= 0 && index < static_cast<int>(s.tracks.size())) {
            s.current = index;
            s.position = 0.0;
            s.state = State::Playing;
        }
    }
    LogPlayer("play a song from the list");
}

void Player::Seek(double seconds) {
    {
        Locked lock;
        Data().position = seconds;
    }
    Log("test player: seek to %.1f s", seconds);
}

void Player::SetVolume(float volume) {
    Locked lock;
    Data().volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    // Not logged: a drag changes it many times a frame.
}

void Player::SetLoop(LoopMode mode) {
    {
        Locked lock;
        Data().loop = mode;
    }
    LogPlayer("repeat changed");
}

void Player::SetPauseWhenGameUnfocused(bool pause) {
    {
        Locked lock;
        Data().pauseWhenUnfocused = pause;
    }
    Log("test player: pause when the game is not in focus: %s", pause ? "yes" : "no");
}

void Player::OnGameFocusChanged(bool focused) {
    {
        Locked lock;
        Data().focused = focused;
    }
    Log("test player: the game is %s", focused ? "in focus" : "not in focus");
}

void Player::AddFiles(const std::vector<std::wstring>& files) {
    {
        Locked lock;
        PlayerState& s = Data();
        FillExamples(s);
        for (const std::wstring& file : files) {
            s.tracks.push_back(Track(file.c_str(), L"", L"", 180));
        }
    }
    LogPlayer("files added");
}

void Player::AddFolder(const std::wstring& folder) {
    {
        Locked lock;
        PlayerState& s = Data();
        FillExamples(s);
        for (int i = 1; i <= 3; ++i) {
            std::wstring path = folder + L"\\folder song " + std::to_wstring(i) + L".mp3";
            s.tracks.push_back(Track(path.c_str(), L"", L"", 200));
        }
    }
    LogPlayer("folder added");
}

void Player::Remove(int index) {
    {
        Locked lock;
        PlayerState& s = Data();
        if (index >= 0 && index < static_cast<int>(s.tracks.size())) {
            s.tracks.erase(s.tracks.begin() + index);
            if (s.current == index) {
                s.current = -1;
                s.state = State::Stopped;
            } else if (s.current > index) {
                --s.current;
            }
        }
    }
    LogPlayer("song removed");
}

void Player::Clear() {
    {
        Locked lock;
        Data().tracks.clear();
        Data().current = -1;
        Data().state = State::Stopped;
    }
    LogPlayer("list cleared");
}

Snapshot Player::GetSnapshot() const {
    Locked lock;
    PlayerState& s = Data();
    FillExamples(s);
    Snapshot snapshot;
    snapshot.state = s.state;
    snapshot.currentIndex = s.current;
    snapshot.positionSeconds = s.position;
    snapshot.durationSeconds =
        s.current >= 0 && s.current < static_cast<int>(s.tracks.size()) ? s.tracks[static_cast<size_t>(s.current)].durationSeconds : 0.0;
    snapshot.loop = s.loop;
    snapshot.volume = s.volume;
    snapshot.pauseWhenGameUnfocused = s.pauseWhenUnfocused;
    snapshot.tracks = s.tracks;
    snapshot.message = s.message;
    return snapshot;
}

void OpenAddFilesDialog(HWND, void (*done)(const std::vector<std::wstring>&)) {
    // The real picker opens a window on its own thread; the stand-in answers at once.
    Log("test picker: add files");
    const std::vector<std::wstring> files = {L"C:\\Music\\Added song one.mp3", L"C:\\Music\\Added song two.flac"};
    done(files);
}

void OpenAddFolderDialog(HWND, void (*done)(const std::wstring&)) {
    Log("test picker: add folder");
    done(L"C:\\Music\\Example folder");
}

}  // namespace music


#endif  // RMM_UI_STANDALONE

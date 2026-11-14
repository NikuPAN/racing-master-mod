#pragma once

// STAND-IN DECLARATIONS, used only when the build defines RMM_UI_STANDALONE.
//
// The music player's interface, copied exactly as agreed with the module that builds it in
// parallel (real headers: src/music/player.h and src/music/picker.h). The media remote is real
// (src/remote/remote.h) and is not declared here.
//
// The panel never includes this file directly; it includes src/ui/media_api.h, which picks this
// file or the real headers. The stand-in implementation is in stubs.cpp next to this file.
// Do not change a declaration here without changing it in the real module as well.

#include <windows.h>

#include <string>
#include <vector>

namespace music {

enum class LoopMode { Off, One, All };
enum class State { Stopped, Playing, Paused };

struct TrackInfo {
    std::wstring path, title, artist;
    double durationSeconds;
    bool supported;
    std::wstring problem;
};

struct Snapshot {
    State state;
    int currentIndex;
    double positionSeconds, durationSeconds;
    LoopMode loop;
    float volume;
    bool pauseWhenGameUnfocused;
    std::vector<TrackInfo> tracks;
    std::wstring message;
};

class Player {
public:
    static Player& Get();
    void Start();
    void Shutdown();
    void Play();
    void Pause();
    void TogglePlayPause();
    void Stop();
    void Next();
    void Previous();
    void PlayIndex(int);
    void Seek(double);
    void SetVolume(float);
    void SetLoop(LoopMode);
    void SetPauseWhenGameUnfocused(bool);
    void OnGameFocusChanged(bool);
    void AddFiles(const std::vector<std::wstring>&);
    void AddFolder(const std::wstring&);
    void Remove(int);
    void Clear();
    Snapshot GetSnapshot() const;
};

void OpenAddFilesDialog(HWND owner, void (*done)(const std::vector<std::wstring>&));
void OpenAddFolderDialog(HWND owner, void (*done)(const std::wstring&));

}  // namespace music

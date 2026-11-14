#pragma once

// Music player engine: plays the player's own files (MP3, FLAC, WAV, OGG Vorbis, Opus, M4A/AAC)
// inside the game process. Plain types only; no Windows headers.
//
// Threads: every method except Start, Shutdown and the snapshot readers posts a command to the
// engine's own player thread and returns at once, so the frame thread never waits on a file or a
// device. Pause, Stop and SetVolume also take effect immediately through atomics, even while the
// player thread is busy opening a file. GetSnapshot / GetSnapshotInto copy a few values and the
// published list under a lock that is only ever held for a few assignments; they never wait on
// file or device work.
//
// Files: the playlist and the [music] settings live in %LOCALAPPDATA%\RacingMasterMod
// (playlist.tsv, settings.ini), saved crash-safe. %LOCALAPPDATA%\RacingMasterMod\music is the
// drop-in folder: files put there are added at Start and whenever RescanMusicFolder() is called.
//
// Start() reads the playlist and settings before it returns and opens no audio device and loads
// no Media Foundation DLL until the first Play. After Start the list is restored with the last
// track cued, stopped; nothing plays by itself.

#include <cstdint>
#include <string>
#include <vector>

namespace music {

enum class LoopMode { Off, One, All };
enum class State { Stopped, Playing, Paused };

struct TrackInfo {
    std::wstring path;
    std::wstring title;            // tag title, else the file name without its extension
    std::wstring artist;           // may be empty
    double durationSeconds = -1;   // -1 until known
    bool supported = true;         // false once the file failed to play; `problem` says why
    std::wstring problem;          // plain-English reason when a file cannot be played, empty otherwise
};

struct Snapshot {
    State state = State::Stopped;
    int currentIndex = -1;          // -1 only when the list is empty
    double positionSeconds = 0;
    double durationSeconds = -1;    // -1 while unknown
    LoopMode loop = LoopMode::All;
    float volume = 0.6f;            // 0..1, the slider position (the gain applied is volume squared)
    bool pauseWhenGameUnfocused = false;
    std::vector<TrackInfo> tracks;
    std::wstring message;           // last plain-English status for the panel, e.g. "Skipped 03.mp3: file not found"

    // Added by the engine (not in the panel contract, safe to ignore):
    uint32_t listVersion = 0;       // changes whenever `tracks` changes; GetSnapshotInto uses it to skip the copy
    uint32_t messageSerial = 0;     // changes whenever `message` is replaced, even by the same text
    bool outputFailed = false;      // no audio device could be opened; the engine retries every few seconds
    bool scanning = false;          // a folder scan, tag read or save is in progress on the I/O thread
    bool dialogOpen = false;        // an "Add files" / "Add folder" dialog is open (picker.h)
};

class Player {
public:
    static Player& Get();

    void Start();     // from the mod's worker thread; loads playlist and settings, no audio device until first Play
    void Shutdown();  // stops the sound, saves, joins the threads; safe to call twice. Not for DllMain.

    void Play();
    void Pause();
    void TogglePlayPause();
    void Stop();
    void Next();
    void Previous();      // restarts the current track when more than 3 s have played
    void PlayIndex(int index);
    void Seek(double seconds);
    void SetVolume(float v);  // 0..1
    void SetLoop(LoopMode m);
    void SetPauseWhenGameUnfocused(bool on);
    void OnGameFocusChanged(bool focused);

    void AddFiles(const std::vector<std::wstring>& paths);  // files, or folders (scanned like AddFolder)
    void AddFolder(const std::wstring& folder);             // recursive, natural order, duplicates skipped
    void Remove(int index);
    void Clear();
    void RescanMusicFolder();  // re-reads the drop-in folder; new files are appended

    Snapshot GetSnapshot() const;  // cheap, callable from the frame thread every frame

    // Same data; `out.tracks` is refreshed only when the list changed since `out.listVersion`
    // was taken, so a panel that keeps one Snapshot across frames pays nothing for an unchanged list.
    void GetSnapshotInto(Snapshot& out) const;

    // Full path of the drop-in folder (empty before Start or when LOCALAPPDATA is not set).
    std::wstring MusicFolder() const;

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

private:
    Player() = default;
    ~Player() = default;
};

}  // namespace music

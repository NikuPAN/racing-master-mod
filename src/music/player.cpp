// The music engine: command queue, player thread, I/O thread and the published snapshot.
//
// Player thread ("rmm-player"): drains commands, runs the playlist logic (playlist_logic.h),
// opens and decodes tracks into the ring (audio_output.h), tracks the boundaries between tracks
// for gapless play, starts and stops the device, publishes the snapshot, debounces saves.
//
// I/O thread ("rmm-io"): folder scans, tag reads, crash-safe saves. Runs at background priority
// while scanning so it yields disk and CPU to the game. It never touches the ring or the device.
//
// The frame thread only posts commands and reads the snapshot. Pause, Stop and SetVolume also
// store into the output's atomics directly, so they take effect within one audio period even
// when the player thread is blocked on a slow file.
//
// Gapless: when the decoding track reaches its end, the player thread asks the playlist logic, on
// a copy, what follows, opens it at once and keeps writing into the ring behind the last sample
// of the old track, through the same resampler. Each queued track records the output frame count
// at which its first sample sits (`startFrame`); when the callback's `consumed` counter passes it,
// the new track is the audible one. Nothing waits for the device to run dry.
#include "player.h"

#include "../core/log.h"
#include "../core/settings_file_lock.h"
#include "atomic_file.h"
#include "audio_output.h"
#include "picker.h"
#include "playlist_logic.h"
#include "playlist_store.h"
#include "source.h"
#include "tag_reader.h"
#include "testing.h"
#include "text.h"

#include "miniaudio_config.h"
#include "miniaudio.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <utility>

namespace music {

namespace {

constexpr wchar_t kPlaylistName[] = L"playlist.tsv";
constexpr wchar_t kSettingsName[] = L"settings.ini";
constexpr wchar_t kMusicFolderName[] = L"music";
constexpr size_t kMaxScanFiles = 20000;
constexpr int kMaxScanDepth = 8;
constexpr unsigned kGateTimeoutMs = 100;
constexpr unsigned kListSaveDelayMs = 2000;
constexpr unsigned kSettingsSaveDelayMs = 500;
// While a song plays, its position is saved this often (only when it moved), so a game that ends
// without the mod seeing it close loses at most this much of the place in the song.
constexpr unsigned kPositionSaveIntervalMs = 10000;
constexpr unsigned kPausedDeviceIdleMs = 60000;
constexpr unsigned kStoppedDeviceIdleMs = 5000;
constexpr unsigned kWatchdogMs = 3000;
constexpr unsigned kDeviceRetryMs = 3000;
constexpr double kRestartPointSeconds = 3.0;
constexpr uint32_t kDecodeChunk = 4096;       // frames per decode call
constexpr uint32_t kSuccessorHeadroom = 2048;  // free ring frames needed before the next track is queued

const wchar_t* const kExtensions[] = {L".mp3", L".flac", L".wav", L".ogg", L".oga", L".opus", L".m4a", L".aac"};

bool HasMusicExtension(const std::wstring& path) {
    const std::wstring extension = Extension(path);
    for (const wchar_t* candidate : kExtensions) {
        if (extension == candidate) return true;
    }
    return false;
}

void NameThread(const wchar_t* name) {
    // SetThreadDescription exists since Windows 10 1607; looked up at run time so the DLL does not
    // import it and still loads on older systems.
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return;
    const Fn fn = reinterpret_cast<Fn>(GetProcAddress(kernel, "SetThreadDescription"));
    if (fn) fn(GetCurrentThread(), name);
}

std::string Utf8(const std::wstring& text) {
    return ToUtf8(text);
}

uint64_t FileTimeValue(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool SameSettings(const MusicSettings& a, const MusicSettings& b) {
    return a.volumePercent == b.volumePercent && a.loop == b.loop && a.pauseWhenGameUnfocused == b.pauseWhenGameUnfocused &&
           a.currentPath == b.currentPath && a.positionMs == b.positionMs;
}

// ---- commands -------------------------------------------------------------------------------------

enum class CommandKind {
    Play, Pause, TogglePlayPause, Stop, Next, Previous, PlayIndex, Seek, SetVolume, SetLoop,
    SetPauseWhenUnfocused, FocusChanged, AddFiles, AddFolder, Remove, Clear, Rescan,
    FilesFound, TagsReady, IoMessage, SaveNow, Nop, Shutdown,
};

struct TagJobItem {
    std::wstring path;
    uint64_t fileSize = 0;
    uint64_t lastWrite = 0;
    bool cached = false;  // the list already holds tags for this size and time
};

struct TagResult {
    std::wstring path;
    std::wstring key;      // PathKey(path), computed on the I/O thread
    Tags tags;
    bool tagsRead = false;
    bool unchanged = false;  // cached tags are still valid
    OpenError problem = OpenError::None;
    uint64_t fileSize = 0;
    uint64_t lastWrite = 0;
};

struct Command {
    CommandKind kind = CommandKind::Nop;
    int index = 0;
    double seconds = 0;
    float value = 0;
    bool flag = false;
    std::wstring text;
    std::vector<std::wstring> paths;
    std::vector<TagResult> tags;
};

struct IoJob {
    enum class Kind { ScanFolder, ReadTags, SaveFile, SaveSettings, Quit } kind = Kind::Quit;
    std::wstring path;               // folder to scan, or file to save
    std::vector<TagJobItem> items;   // files whose tags to read
    std::string content;             // SaveFile
    MusicSettings settings;          // SaveSettings
    bool fromDropIn = false;
};

// ---- the engine --------------------------------------------------------------------------------------

struct Entry {
    uint64_t id = 0;       // stable while the engine runs; never stored
    std::wstring path;
    std::wstring key;      // PathKey(path)
    std::wstring title;    // empty until tags were read; the file name is shown meanwhile
    std::wstring artist;
    int64_t durationMs = -1;
    uint64_t fileSize = 0;
    uint64_t lastWrite = 0;
    bool tagsKnown = false;
    OpenError problem = OpenError::None;
};

// One open track in the play chain: tracks_[0] is audible, tracks_[1] (if any) is queued behind it.
struct OpenTrack {
    std::unique_ptr<Source> source;  // null once abandoned after a decoder fault
    SourceInfo info;
    uint64_t id = 0;
    int index = -1;               // position in entries_; -1 once the entry was removed
    std::wstring path;
    uint64_t startFrame = 0;      // in `consumed` units: where its first sample sits in the output
    uint64_t offsetFrames = 0;    // pipeline frames skipped by a seek
    uint64_t framesRead = 0;      // source frames decoded so far (from the start of the file)
    bool eof = false;             // nothing more will be decoded from it
    bool readFailed = false;      // the decoder reported an error before the end
    bool faulted = false;         // the decoder faulted (access violation); handled by HandleFault
    bool endChecked = false;
};

enum class EndKind {
    None,
    Final,     // the list ends behind the decoded audio (loop off, or every successor failed)
    Redecide,  // the queued successor was dropped; decide again when the audio reaches endFrame_
};

struct View {
    State state = State::Stopped;
    int currentIndex = -1;
    double positionSeconds = 0;
    double durationSeconds = -1;
    LoopMode loop = LoopMode::All;
    float volume = 0.6f;
    bool pauseWhenGameUnfocused = false;
    bool outputFailed = false;
    bool scanning = false;
    std::wstring message;
    uint32_t messageSerial = 0;
    std::shared_ptr<const std::vector<TrackInfo>> list;
    uint32_t listVersion = 0;
};

class Engine {
public:
    Engine() {
        InitializeSRWLock(&commandLock_);
        InitializeSRWLock(&ioLock_);
        InitializeSRWLock(&viewLock_);
        view_.list = std::make_shared<const std::vector<TrackInfo>>();
    }

    // ---- lifetime (worker / test thread) ----
    void Configure(const testing::Config& config) {
        mode_ = config.output;
        silent_ = config.silentDevice;
        dataDirOverride_ = config.dataDir ? config.dataDir : L"";
        positionSaveMs_ = config.positionSaveMs ? config.positionSaveMs : kPositionSaveIntervalMs;
    }

    void Start() {
        if (running_.load()) return;
        wchar_t flag[8];
        const bool testHost = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_HOST", flag, 8) == 1 && flag[0] == L'1';
        if (testHost && mode_ == OutputMode::Wasapi) {
            // The automated run must never open a sound device.
            Log("music: test-host mode refuses the real audio device; using the null device");
            mode_ = OutputMode::Null;
        }
        ResolvePaths();
        ResetState();
        LoadState();
        PublishList();
        PublishView();
        shutdown_.store(false);
        if (!commandEvent_) commandEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ioEvent_) ioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!commandEvent_ || !ioEvent_) {
            Log("music: the engine could not create its events (error %lu); music is off", GetLastError());
            return;
        }
        running_.store(true);
        ioThread_ = CreateThread(nullptr, 0, IoThreadEntry, this, 0, nullptr);
        playerThread_ = CreateThread(nullptr, 0, PlayerThreadEntry, this, 0, nullptr);
        if (!ioThread_ || !playerThread_) {
            Log("music: the engine could not start its threads (error %lu); music is off", GetLastError());
            running_.store(false);
            shutdown_.store(true);
            PostIo(IoJob{});  // Quit, in case the I/O thread did start
            if (ioThread_) {
                WaitForSingleObject(ioThread_, 5000);
                CloseHandle(ioThread_);
                ioThread_ = nullptr;
            }
            return;
        }
        SetThreadPriority(playerThread_, THREAD_PRIORITY_ABOVE_NORMAL);
        SetThreadPriority(ioThread_, THREAD_PRIORITY_BELOW_NORMAL);
    }

    void Shutdown() {
        if (!running_.load()) return;
        shutdown_.store(true);
        Command command;
        command.kind = CommandKind::Shutdown;
        Post(std::move(command));
        if (playerThread_) {
            WaitForSingleObject(playerThread_, 15000);  // the player thread waits for the I/O thread itself
            CloseHandle(playerThread_);
            playerThread_ = nullptr;
        }
        if (ioThread_) {
            WaitForSingleObject(ioThread_, 5000);
            CloseHandle(ioThread_);
            ioThread_ = nullptr;
        }
        running_.store(false);
    }

    // ---- posting (any thread) ----
    void Post(Command command) {
        AcquireSRWLockExclusive(&commandLock_);
        commands_.push_back(std::move(command));
        postedSerial_.fetch_add(1);
        ReleaseSRWLockExclusive(&commandLock_);
        if (commandEvent_) SetEvent(commandEvent_);
    }

    void Simple(CommandKind kind) {
        Command command;
        command.kind = kind;
        Post(std::move(command));
    }

    void InstantHold() { output_.SetUserHold(true); }
    void InstantVolume(float v) { output_.SetVolume(v * v); }

    // ---- snapshot (any thread) ----
    void Read(Snapshot& out, bool withTracks) const {
        std::shared_ptr<const std::vector<TrackInfo>> list;
        AcquireSRWLockShared(&viewLock_);
        out.state = view_.state;
        out.currentIndex = view_.currentIndex;
        out.positionSeconds = view_.positionSeconds;
        out.durationSeconds = view_.durationSeconds;
        out.loop = view_.loop;
        out.volume = view_.volume;
        out.pauseWhenGameUnfocused = view_.pauseWhenGameUnfocused;
        out.outputFailed = view_.outputFailed;
        out.scanning = view_.scanning;
        if (out.messageSerial != view_.messageSerial) {
            out.message = view_.message;
            out.messageSerial = view_.messageSerial;
        }
        const bool listChanged = withTracks || out.listVersion != view_.listVersion;
        if (listChanged) list = view_.list;
        const uint32_t version = view_.listVersion;
        ReleaseSRWLockShared(&viewLock_);
        // The copy of the list is made outside the lock; the list itself is never edited, only replaced.
        if (listChanged) {
            out.tracks = list ? *list : std::vector<TrackInfo>();
            out.listVersion = version;
        }
        out.dialogOpen = IsDialogOpen();
    }

    std::wstring MusicFolder() const {
        AcquireSRWLockShared(&viewLock_);
        std::wstring folder = musicDir_;
        ReleaseSRWLockShared(&viewLock_);
        return folder;
    }

    // ---- test hooks ----
    bool Render(float* out, uint32_t frames) {
        if (mode_ != OutputMode::Manual) {
            memset(out, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
            return false;
        }
        output_.Render(out, frames);
        if (commandEvent_) SetEvent(commandEvent_);  // let the player thread refill straight away
        return true;
    }
    bool Sync(unsigned timeoutMs) {
        if (!running_.load()) return false;
        Command nop;
        nop.kind = CommandKind::Nop;
        Post(std::move(nop));
        const uint32_t target = postedSerial_.load();
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        while (static_cast<int32_t>(cycleSerial_.load() - target) < 0) {
            if (!running_.load() || GetTickCount64() >= until) return false;
            Sleep(0);
        }
        return true;
    }
    bool WaitForIdleIo(unsigned timeoutMs) {
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        for (;;) {
            if (ioPending_.load() == 0) {
                if (!Sync(timeoutMs)) return false;
                if (ioPending_.load() == 0) return true;  // handling the results posted no new I/O work
            }
            if (!running_.load() || GetTickCount64() >= until) return false;
            Sleep(2);
        }
    }
    void SimulateDeviceLoss() { output_.SimulateDeviceLoss(); }
    testing::Counters GetCounters() const {
        return {output_.Consumed(), output_.Produced(), output_.Callbacks(), output_.Underruns(), output_.SampleRate(),
                output_.DeviceStarted(), opens_.load(), deviceRestarts_.load(), settingsSaves_.load()};
    }

private:
    // ================================================================================================
    // Player thread
    // ================================================================================================
    static DWORD WINAPI PlayerThreadEntry(LPVOID self) {
        static_cast<Engine*>(self)->PlayerThread();
        return 0;
    }

    void PlayerThread() {
        NameThread(L"rmm-player");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Tags for rows whose file changed since the cache, then the drop-in folder.
        QueueTagReads(0);
        if (!musicDir_.empty()) {
            CreateDirectoryW(ToExtendedPath(musicDir_).c_str(), nullptr);
            IoJob scan;
            scan.kind = IoJob::Kind::ScanFolder;
            scan.path = musicDir_;
            scan.fromDropIn = true;
            PostIo(std::move(scan));
        }

        for (;;) {
            const bool busy = !tracks_.empty() || endKind_ != EndKind::None || logic_.status == State::Playing;
            WaitForSingleObject(commandEvent_, busy ? 10 : 100);
            const uint32_t target = postedSerial_.load();  // every command up to here is in the queue
            if (DrainCommands()) break;
            Pump();
            cycleSerial_.store(target);
        }

        // Shutdown: stop the sound, let the I/O thread finish its queued saves, then save the
        // final state from here (one writer per file at a time), and release the device.
        if (output_.IsInitialised()) {
            output_.SetUserHold(true);
            output_.CloseGateAndFlush(kGateTimeoutMs);
            output_.StopDevice();
        }
        RememberPosition();
        PostIo(IoJob{});  // Quit
        if (ioThread_) WaitForSingleObject(ioThread_, 10000);
        SaveNow();
        ClearChain();
        output_.Uninit();
        if (SUCCEEDED(com)) CoUninitialize();
        cycleSerial_.store(postedSerial_.load());
    }

    // Returns true when the thread must exit.
    bool DrainCommands() {
        std::vector<Command> batch;
        AcquireSRWLockExclusive(&commandLock_);
        batch.swap(commands_);
        ReleaseSRWLockExclusive(&commandLock_);
        bool quit = false;
        for (Command& command : batch) {
            if (command.kind == CommandKind::Shutdown) quit = true;
            else if (!quit) Handle(command);
        }
        return quit;
    }

    void Handle(Command& c) {
        switch (c.kind) {
            case CommandKind::Play: pausedByFocus_ = false; Apply(logic_.Play()); break;
            case CommandKind::Pause: pausedByFocus_ = false; Apply(logic_.Pause()); break;
            case CommandKind::TogglePlayPause:
                pausedByFocus_ = false;
                Apply(logic_.status == State::Playing ? logic_.Pause() : logic_.Play());
                break;
            case CommandKind::Stop: pausedByFocus_ = false; Apply(logic_.Stop()); break;
            case CommandKind::Next: pausedByFocus_ = false; Apply(logic_.Next()); break;
            case CommandKind::Previous:
                pausedByFocus_ = false;
                Apply(logic_.Previous(PositionSeconds() > kRestartPointSeconds));
                break;
            case CommandKind::PlayIndex: pausedByFocus_ = false; Apply(logic_.PlayIndex(c.index)); break;
            case CommandKind::Seek: HandleSeek(c.seconds); break;
            case CommandKind::SetVolume: HandleVolume(c.value); break;
            case CommandKind::SetLoop: HandleLoop(static_cast<LoopMode>(c.index)); break;
            case CommandKind::SetPauseWhenUnfocused:
                pauseWhenUnfocused_ = c.flag;
                MarkSettingsDirty();
                break;
            case CommandKind::FocusChanged: HandleFocus(c.flag); break;
            case CommandKind::AddFiles: AddPaths(c.paths, false, L""); break;
            case CommandKind::AddFolder: {
                IoJob scan;
                scan.kind = IoJob::Kind::ScanFolder;
                scan.path = NormalisePath(c.text);
                PostIo(std::move(scan));
                break;
            }
            case CommandKind::Remove: HandleRemove(c.index); break;
            case CommandKind::Clear: HandleClear(); break;
            case CommandKind::Rescan:
                if (!musicDir_.empty()) {
                    IoJob scan;
                    scan.kind = IoJob::Kind::ScanFolder;
                    scan.path = musicDir_;
                    scan.fromDropIn = true;
                    PostIo(std::move(scan));
                }
                break;
            case CommandKind::FilesFound: AddPaths(c.paths, c.flag, c.text); break;
            case CommandKind::TagsReady: ApplyTags(c.tags); break;
            case CommandKind::IoMessage: SetMessage(c.text); break;
            case CommandKind::SaveNow: HandleSaveNow(); break;
            case CommandKind::Nop: break;
            case CommandKind::Shutdown: break;
        }
    }

    // ---- actions the logic asks for ----
    void Apply(const Step& step) {
        switch (step.action) {
            case Action::None: break;
            case Action::Start: {
                // The first Play after Start continues the track that was cued, where it was left.
                const int64_t at = step.index == cuedIndex_ ? cuedPositionMs_ : 0;
                cuedIndex_ = -1;
                cuedPositionMs_ = 0;
                StartTrack(step.index, at);
                break;
            }
            case Action::Resume: Resume(); break;
            case Action::Pause:
                output_.SetUserHold(true);
                deviceIdleSince_ = GetTickCount64();
                RememberPosition();
                MarkSettingsDirty();
                break;
            case Action::Stop: StopPlayback(false); break;
            case Action::StopAllFailed: StopPlayback(true); break;
            case Action::Seek: break;  // handled by HandleSeek
        }
    }

    // Opens entries_[index] (or, if it cannot be opened, the tracks the logic moves on to) and
    // makes it the audible track. Never recurses: failures are walked in this loop.
    void StartTrack(int index, int64_t startMs) {
        ClearPlayChain();
        stoppedPositionMs_ = 0;
        if (!EnsureOutput()) {
            // No device: keep the state (Playing); Pump retries every few seconds from here.
            resumeMs_ = startMs;
            return;
        }
        resumeMs_ = 0;
        for (;;) {
            if (index < 0 || index >= static_cast<int>(entries_.size())) return;
            Entry& entry = entries_[static_cast<size_t>(index)];
            OpenError error = OpenError::None;
            opens_.fetch_add(1);
            std::unique_ptr<Source> source = OpenSource(entry.path, &error);
            if (source) {
                OpenTrack track;
                track.source = std::move(source);
                track.info = track.source->Info();
                track.id = entry.id;
                track.index = index;
                track.path = entry.path;
                AdoptOpenedTrack(entry, track.info);
                track.startFrame = output_.Produced();
                if (startMs > 0 && track.info.sampleRate > 0) {
                    const int64_t frame = startMs * static_cast<int64_t>(track.info.sampleRate) / 1000;
                    bool ok = false;
                    if ((track.info.lengthFrames < 0 || frame < track.info.lengthFrames) &&
                        GuardedSeek(track.source.get(), frame, &ok) && ok) {
                        track.offsetFrames = static_cast<uint64_t>(frame) * output_.SampleRate() / track.info.sampleRate;
                        track.framesRead = static_cast<uint64_t>(frame);
                    }
                }
                tracks_.push_back(std::move(track));
                logic_.TrackStarted();
                Log("music: started %s", Utf8(FileName(entry.path, false)).c_str());
                output_.SetUserHold(logic_.status == State::Paused);
                pendingGate_ = true;  // FillRing opens the gate once some audio is queued
                deviceIdleSince_ = 0;
                lastConsumed_ = output_.Consumed();
                lastConsumedChange_ = GetTickCount64();
                RememberPosition();
                MarkSettingsDirty();
                return;
            }
            ReportSkipped(index, error);
            const Step next = logic_.TrackFailed();
            if (next.action == Action::Start) {
                index = next.index;
                continue;
            }
            Apply(next);  // Stop or StopAllFailed; neither starts a track
            return;
        }
    }

    // Exact length from the decoder replaces the estimate from the tag scan; a file that opens
    // loses an earlier problem flag.
    void AdoptOpenedTrack(Entry& entry, const SourceInfo& info) {
        bool changed = entry.problem != OpenError::None;
        entry.problem = OpenError::None;
        if (info.lengthFrames > 0 && info.sampleRate > 0) {
            const int64_t ms = info.lengthFrames * 1000 / info.sampleRate;
            if (ms != entry.durationMs) {
                entry.durationMs = ms;
                changed = true;
                MarkListDirty();
            }
        }
        if (changed) PublishList();
    }

    void Resume() {
        if (tracks_.empty()) {
            // Nothing open (the device was lost while paused): start again where it was.
            StartTrack(logic_.current, rememberedPositionMs_);
            return;
        }
        if (!EnsureOutput()) return;
        output_.SetUserHold(false);
        if (!pendingGate_) output_.OpenGate();
        if (!output_.StartDevice()) ReportOutputFailed();
        deviceIdleSince_ = 0;
        lastConsumed_ = output_.Consumed();
        lastConsumedChange_ = GetTickCount64();
    }

    void StopPlayback(bool allFailed) {
        if (output_.IsInitialised()) {
            output_.SetUserHold(true);
            output_.CloseGateAndFlush(kGateTimeoutMs);
            output_.SetUserHold(false);
        }
        if (!tracks_.empty()) Log("music: stopped %s", Utf8(FileName(tracks_[0].path, false)).c_str());
        ClearPlayChain();
        stoppedPositionMs_ = 0;
        resumeMs_ = 0;
        deviceIdleSince_ = GetTickCount64();
        if (allFailed) {
            SetMessage(L"Nothing in the list could be played");
            Log("music: nothing in the list could be played");
        }
        RememberPosition();
        MarkSettingsDirty();
    }

    // Drops every open track, the converter and any pending end, and closes the gate with the
    // handshake so no stale audio can follow.
    void ClearPlayChain() {
        if (output_.IsInitialised()) output_.CloseGateAndFlush(kGateTimeoutMs);
        tracks_.clear();
        endKind_ = EndKind::None;
        pendingGate_ = false;
        ClearChain();
    }

    void HandleSeek(double seconds) {
        if (logic_.Seek().action != Action::Seek || tracks_.empty() || !tracks_[0].source) return;
        OpenTrack& track = tracks_[0];
        const uint32_t rate = track.info.sampleRate;
        if (!(seconds > 0)) seconds = 0;  // also catches NaN
        int64_t frame = static_cast<int64_t>(seconds * rate);
        if (track.info.lengthFrames > 0 && frame >= track.info.lengthFrames) frame = track.info.lengthFrames - 1;
        output_.CloseGateAndFlush(kGateTimeoutMs);
        tracks_.resize(1);  // a queued successor is dropped; it is opened again at the new end
        endKind_ = EndKind::None;
        ClearChain();
        bool ok = false;
        if (!GuardedSeek(track.source.get(), frame, &ok)) {
            // The decoder faulted inside the seek: the track is damaged from here on, and nothing
            // of it is in the ring any more, so it counts as a track that failed to play.
            track.faulted = true;
            track.framesRead = 0;
            HandleFault();
            return;
        }
        if (!ok) {
            frame = 0;
            GuardedSeek(track.source.get(), 0, &ok);
        }
        track.eof = false;
        track.readFailed = false;
        track.endChecked = false;
        track.framesRead = static_cast<uint64_t>(frame);
        track.startFrame = output_.Produced();
        track.offsetFrames = static_cast<uint64_t>(frame) * output_.SampleRate() / rate;
        pendingGate_ = true;
        lastConsumed_ = output_.Consumed();
        lastConsumedChange_ = GetTickCount64();
        if (logic_.status == State::Paused) RememberPosition();
        MarkSettingsDirty();
    }

    void HandleVolume(float v) {
        if (!(v >= 0)) v = 0;
        if (v > 1) v = 1;
        volume_ = v;
        output_.SetVolume(v * v);  // the lower half of the slider stays usable
        MarkSettingsDirty();
    }

    void HandleLoop(LoopMode mode) {
        if (mode != LoopMode::Off && mode != LoopMode::One && mode != LoopMode::All) return;
        logic_.SetLoop(mode);
        MarkSettingsDirty();
        InvalidateSuccessorIfChanged();
    }

    void HandleFocus(bool focused) {
        if (!focused) {
            if (pauseWhenUnfocused_ && logic_.status == State::Playing) {
                Apply(logic_.Pause());
                pausedByFocus_ = true;
            }
        } else if (pausedByFocus_) {
            pausedByFocus_ = false;
            if (logic_.status == State::Paused) Apply(logic_.Play());
        }
    }

    void HandleRemove(int index) {
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        const uint64_t removedId = entries_[static_cast<size_t>(index)].id;
        const bool wasAudible = !tracks_.empty() && tracks_[0].id == removedId;
        const Step step = logic_.Removed(index);
        entries_.erase(entries_.begin() + index);
        keyIndexDirty_ = true;
        // Indices held by the open tracks move with the list.
        for (OpenTrack& track : tracks_) {
            if (track.id == removedId) track.index = -1;
            else if (track.index > index) --track.index;
        }
        MarkListDirty();
        if (!wasAudible) InvalidateSuccessorIfChanged();
        Apply(step);
        PublishList();
    }

    void HandleClear() {
        const Step step = logic_.Cleared();
        entries_.clear();
        keyIndex_.clear();
        keyIndexDirty_ = false;
        cuedIndex_ = -1;
        stoppedPositionMs_ = 0;
        MarkListDirty();
        Apply(step);
        ClearPlayChain();
        PublishList();
        MarkSettingsDirty();
    }

    // The id of the entry that should follow the audible track when it ends by itself; 0 = none.
    uint64_t ExpectedSuccessorId() const {
        PlaylistState copy = logic_;
        copy.status = State::Playing;  // asked the same way whether playing or paused
        const int next = copy.PeekNextOnEnd();
        return next >= 0 && next < static_cast<int>(entries_.size()) ? entries_[static_cast<size_t>(next)].id : 0;
    }

    // After a change to the loop mode or the list: if the decision taken when the successor was
    // queued no longer holds, let the audible track play to its exact last sample, then decide
    // afresh. A change that leaves the successor the same costs nothing.
    void InvalidateSuccessorIfChanged() {
        if (tracks_.empty()) return;
        const uint64_t expected = ExpectedSuccessorId();
        if (tracks_.size() >= 2) {
            if (tracks_[1].index >= 0 && expected == queuedDecisionId_) return;
            endFrame_ = tracks_[1].startFrame;
            output_.SetPlayLimit(endFrame_);
            tracks_.resize(1);
            ClearChain();  // the converter now holds the dropped track's samples
            endKind_ = EndKind::Redecide;
            Log("music: the list or loop mode changed near the end of %s; the next track is chosen again",
                Utf8(FileName(tracks_[0].path, false)).c_str());
        } else if (endKind_ == EndKind::Final && expected != queuedDecisionId_) {
            endKind_ = EndKind::Redecide;
        }
    }

    // ---- adding ----
    void RebuildKeyIndex() {
        if (!keyIndexDirty_) return;
        keyIndex_.clear();
        keyIndex_.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i) keyIndex_.emplace(entries_[i].key, i);
        keyIndexDirty_ = false;
    }

    void AddPaths(const std::vector<std::wstring>& paths, bool fromDropIn, const std::wstring& folder) {
        RebuildKeyIndex();
        std::vector<std::wstring> folders;
        int added = 0;
        int duplicates = 0;
        int refused = 0;
        const size_t at = entries_.size();
        for (const std::wstring& raw : paths) {
            if (raw.empty()) continue;
            const std::wstring path = NormalisePath(raw);
            const DWORD attributes = GetFileAttributesW(ToExtendedPath(path).c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                folders.push_back(path);
                continue;
            }
            bool ok = true;
            ToUtf8(path, &ok);
            if (!ok) {
                ++refused;  // the name cannot be written to playlist.tsv without loss
                continue;
            }
            std::wstring key = PathKey(path);
            if (keyIndex_.count(key)) {
                ++duplicates;
                continue;
            }
            Entry entry;
            entry.id = nextId_++;
            entry.path = path;
            entry.key = key;
            keyIndex_.emplace(std::move(key), entries_.size());
            entries_.push_back(std::move(entry));
            ++added;
        }
        if (added > 0) {
            logic_.Inserted(static_cast<int>(at), added);
            QueueTagReads(at);
            MarkListDirty();
            InvalidateSuccessorIfChanged();
            PublishList();
        }
        for (const std::wstring& f : folders) {
            IoJob scan;
            scan.kind = IoJob::Kind::ScanFolder;
            scan.path = f;
            PostIo(std::move(scan));
        }
        if (refused > 0) {
            SetMessage(std::to_wstring(refused) +
                       L" file(s) were not added: their names contain characters that cannot be saved in the list");
        } else if (!folder.empty() && !fromDropIn) {
            if (added > 0) SetMessage(L"Added " + std::to_wstring(added) + L" file(s) from " + folder);
            else if (duplicates > 0) SetMessage(L"Every music file in " + folder + L" is already in the list");
            else SetMessage(L"No music files found in " + folder);
        } else if (fromDropIn && added > 0) {
            SetMessage(L"Added " + std::to_wstring(added) + L" file(s) from the music folder");
        } else if (folder.empty() && added > 0) {
            SetMessage(added == 1 ? L"Added " + FileName(entries_.back().path, false) : L"Added " + std::to_wstring(added) + L" files");
        } else if (folder.empty() && added == 0 && duplicates > 0) {
            SetMessage(duplicates == 1 ? L"That file is already in the list" : L"Those files are already in the list");
        }
        if (added > 0) Log("music: added %d file(s)%s", added, fromDropIn ? " from the music folder" : "");
    }

    // Asks the I/O thread for title, artist and duration of entries_[from..]. Rows whose file
    // still has the size and time stored with their tags are only checked for existence.
    void QueueTagReads(size_t from) {
        IoJob job;
        job.kind = IoJob::Kind::ReadTags;
        for (size_t i = from; i < entries_.size(); ++i) {
            TagJobItem item;
            item.path = entries_[i].path;
            item.fileSize = entries_[i].fileSize;
            item.lastWrite = entries_[i].lastWrite;
            item.cached = entries_[i].tagsKnown;
            job.items.push_back(std::move(item));
        }
        if (!job.items.empty()) PostIo(std::move(job));
    }

    void ApplyTags(std::vector<TagResult>& results) {
        RebuildKeyIndex();
        bool changed = false;
        for (TagResult& result : results) {
            const auto found = keyIndex_.find(result.key);
            if (found == keyIndex_.end()) continue;  // removed meanwhile
            Entry& entry = entries_[found->second];
            if (result.problem != OpenError::None) {
                if (entry.problem != result.problem) changed = true;
                entry.problem = result.problem;
                continue;
            }
            // The file is there and readable: lift a flag that said otherwise (not "damaged",
            // which only playback can decide).
            if (entry.problem != OpenError::None && entry.problem != OpenError::Damaged) {
                entry.problem = OpenError::None;
                changed = true;
            }
            if (result.unchanged) continue;
            entry.fileSize = result.fileSize;
            entry.lastWrite = result.lastWrite;
            entry.tagsKnown = true;
            if (result.tagsRead) {
                entry.title = result.tags.title;
                entry.artist = result.tags.artist;
                // A decoder's exact length (from playing it) is kept over a tag-scan estimate.
                if (result.tags.durationSeconds > 0 && entry.durationMs <= 0) {
                    entry.durationMs = static_cast<int64_t>(result.tags.durationSeconds * 1000.0 + 0.5);
                }
            }
            changed = true;
        }
        if (changed) {
            MarkListDirty();
            PublishList();
        }
    }

    // ---- per-iteration work ----
    void Pump() {
        const ULONGLONG now = GetTickCount64();
        if (!tracks_.empty() || endKind_ != EndKind::None) {
            CrossBoundaries();
            FillRing();
        }
        if (logic_.status == State::Playing && tracks_.empty() && endKind_ == EndKind::None && outputFailed_ &&
            now - lastDeviceAttempt_ >= kDeviceRetryMs) {
            StartTrack(logic_.current, resumeMs_);  // retry the device
        }
        Watchdog(now);
        IdleDevice(now);
        Saves(now);
        PublishView();
    }

    // Decodes into the ring until it is full or nothing is left to decode, queueing the next
    // track behind the last one when it ends.
    void FillRing() {
        if (!output_.IsInitialised()) return;
        for (int guard = 0; guard < 512; ++guard) {
            if (tracks_.empty()) break;
            OpenTrack& track = tracks_.back();
            if (track.eof && pendingFrames_ == 0) {
                CheckEarlyEnd(track);
                if (!QueueSuccessor()) break;
                continue;
            }
            if (!track.source) break;
            uint32_t space = kDecodeChunk;
            float* out = nullptr;
            if (!output_.AcquireWrite(&space, &out) || space == 0) {
                output_.CommitWrite(0);
                break;
            }
            const uint32_t written = Decode(track, out, space);
            output_.CommitWrite(written);
            if (track.faulted) {
                HandleFault();  // may replace tracks_; the next pass fetches the back again
                continue;
            }
            if (written == 0 && !track.eof) break;
        }
        OpenGateWhenReady();
    }

    void OpenGateWhenReady() {
        if (!pendingGate_ || tracks_.empty()) return;
        const uint32_t queued = output_.FramesQueued();
        const bool enough = queued >= output_.SampleRate() / 20 || output_.FreeFrames() < kDecodeChunk ||
                            (tracks_.back().eof && pendingFrames_ == 0);
        if (!enough) return;
        pendingGate_ = false;
        output_.OpenGate();
        if (!output_.StartDevice()) ReportOutputFailed();
        lastConsumed_ = output_.Consumed();
        lastConsumedChange_ = GetTickCount64();
    }

    // Reads from `track` (through the converter when its format differs from the pipeline's)
    // into `out`; returns pipeline frames written. Never changes tracks_: a fault only sets
    // track.faulted, a failure or the end only sets track.eof.
    uint32_t Decode(OpenTrack& track, float* out, uint32_t space) {
        const uint32_t pipelineRate = output_.SampleRate();
        const bool needsChain = track.info.sampleRate != pipelineRate || track.info.channels != 2;
        if (needsChain) {
            if (!ChainEnsure(track.info.sampleRate, track.info.channels)) {
                track.eof = true;
                track.readFailed = true;
                return 0;
            }
        } else if (chainActive_) {
            ChainFlush();  // the previous track needed conversion; this one does not
        }

        if (!chainActive_) {
            uint32_t got = 0;
            bool failed = false;
            if (!GuardedRead(track.source.get(), out, space, &got, &failed)) {
                track.faulted = true;
                return 0;
            }
            track.framesRead += got;
            if (got < space || failed) {
                track.eof = true;
                track.readFailed = track.readFailed || failed;
            }
            return got;
        }

        // Through the converter. Source frames that the converter did not take this time wait
        // in pending_ for the next call, so nothing is dropped.
        const uint32_t channels = track.info.channels;
        uint32_t written = 0;
        for (int spins = 0; written < space && spins < 16; ++spins) {
            if (pendingFrames_ == 0) {
                if (track.eof) break;
                ma_uint64 needed = 0;
                ma_data_converter_get_required_input_frame_count(&chain_, space - written, &needed);
                if (needed == 0) needed = 1;
                if (needed > kDecodeChunk) needed = kDecodeChunk;
                if (pending_.size() < static_cast<size_t>(needed) * channels) pending_.resize(static_cast<size_t>(needed) * channels);
                uint32_t got = 0;
                bool failed = false;
                if (!GuardedRead(track.source.get(), pending_.data(), static_cast<uint32_t>(needed), &got, &failed)) {
                    track.faulted = true;
                    return written;
                }
                track.framesRead += got;
                if (got < needed || failed) {
                    track.eof = true;
                    track.readFailed = track.readFailed || failed;
                }
                pendingFrames_ = got;
                pendingOffset_ = 0;
                if (got == 0) break;
            }
            ma_uint64 inCount = pendingFrames_;
            ma_uint64 outCount = space - written;
            ma_data_converter_process_pcm_frames(&chain_, pending_.data() + static_cast<size_t>(pendingOffset_) * channels, &inCount,
                                                 out + static_cast<size_t>(written) * 2, &outCount);
            pendingOffset_ += static_cast<uint32_t>(inCount);
            pendingFrames_ -= static_cast<uint32_t>(inCount);
            written += static_cast<uint32_t>(outCount);
            if (inCount == 0 && outCount == 0) break;
        }
        return written;
    }

    // A decoder faulted inside tracks_.back(). Its object is abandoned (leaked on purpose: its
    // state cannot be trusted to run a clean-up), the entry is flagged, and play moves on.
    void HandleFault() {
        OpenTrack& track = tracks_.back();
        AbandonSource(track.source);
        Log("music: the decoder failed inside %s; the file is treated as damaged", Utf8(FileName(track.path, false)).c_str());
        if (track.index >= 0) ReportSkipped(track.index, OpenError::Damaged);
        pendingFrames_ = 0;
        if (tracks_.size() >= 2) {
            // The queued successor broke before anyone heard it: cut the output where it would
            // have started and decide again there.
            endFrame_ = track.startFrame;
            output_.SetPlayLimit(endFrame_);
            tracks_.pop_back();
            ClearChain();
            endKind_ = EndKind::Redecide;
            return;
        }
        if (track.framesRead > 0) {
            // Part of it was heard: it ends here, like a file that stops early.
            track.eof = true;
            track.endChecked = true;
            return;
        }
        ClearPlayChain();
        const Step next = logic_.TrackFailed();
        if (next.action == Action::Start) StartTrack(next.index, 0);
        else Apply(next);
    }

    // A file whose decoder stopped with an error, or well before its stated length, is flagged
    // as damaged; what was decoded still plays.
    void CheckEarlyEnd(OpenTrack& track) {
        if (track.endChecked) return;
        track.endChecked = true;
        const int64_t length = track.info.lengthFrames;
        const int64_t read = static_cast<int64_t>(track.framesRead);
        const int64_t tolerance = std::max<int64_t>(static_cast<int64_t>(track.info.sampleRate) / 4, length / 20);
        const bool short_ = length > 0 && read + tolerance < length;
        if (!track.readFailed && !short_) return;
        if (track.index < 0) return;
        Entry& entry = entries_[static_cast<size_t>(track.index)];
        entry.problem = OpenError::Damaged;
        SetMessage(FileName(entry.path, false) + L" stopped early: the file is damaged");
        Log("music: %s ended early (damaged)", Utf8(FileName(entry.path, false)).c_str());
        PublishList();
    }

    // The decoding track ended: open what follows on a copy of the logic and write it behind
    // the current audio. Returns false when nothing more can be decoded now.
    bool QueueSuccessor() {
        if (tracks_.size() >= 2 || endKind_ != EndKind::None) return false;  // already decided
        if (logic_.status != State::Playing) return false;  // paused: decided once it plays again
        if (output_.FreeFrames() < kSuccessorHeadroom) return false;  // room for the converter tail first
        PlaylistState copy = logic_;
        queuedDecisionId_ = ExpectedSuccessorId();
        Step step = copy.TrackEnded();
        while (step.action == Action::Start) {
            Entry& entry = entries_[static_cast<size_t>(step.index)];
            OpenError error = OpenError::None;
            opens_.fetch_add(1);
            std::unique_ptr<Source> source = OpenSource(entry.path, &error);
            if (source) {
                OpenTrack next;
                next.source = std::move(source);
                next.info = next.source->Info();
                next.id = entry.id;
                next.index = step.index;
                next.path = entry.path;
                AdoptOpenedTrack(entry, next.info);
                // With the same format the converter keeps running across the boundary (no
                // tick); with another format its tail is flushed first, so the boundary sits
                // after the last sample of the old track.
                if (next.info.sampleRate != output_.SampleRate() || next.info.channels != 2) {
                    ChainEnsure(next.info.sampleRate, next.info.channels);
                } else if (chainActive_) {
                    ChainFlush();
                }
                next.startFrame = output_.Produced();
                tracks_.push_back(std::move(next));
                return true;
            }
            ReportSkipped(step.index, error);  // reported now; the audible track is untouched
            step = copy.TrackFailed();
        }
        // Stop or StopAllFailed: the list ends behind what is in the ring.
        if (chainActive_) ChainFlush();
        endKind_ = EndKind::Final;
        endFrame_ = output_.Produced();
        endState_ = copy;
        endAllFailed_ = step.action == Action::StopAllFailed;
        output_.SetPlayLimit(endFrame_);
        return false;
    }

    void CrossBoundaries() {
        const uint64_t consumed = output_.Consumed();
        while (tracks_.size() >= 2 && consumed >= tracks_[1].startFrame) {
            Log("music: ended %s", Utf8(FileName(tracks_[0].path, false)).c_str());
            // Only the position in the list moves; the status stays what the user made it (a
            // pause in the last few milliseconds of a track must stay a pause).
            logic_.current = tracks_[1].index;
            logic_.failures = 0;
            tracks_.erase(tracks_.begin());
            Log("music: started %s", Utf8(FileName(tracks_[0].path, false)).c_str());
            RememberPosition();
            MarkSettingsDirty();
        }
        if (endKind_ == EndKind::None || consumed < endFrame_ || logic_.status != State::Playing) return;
        if (!tracks_.empty()) Log("music: ended %s", Utf8(FileName(tracks_[0].path, false)).c_str());
        const EndKind kind = endKind_;
        endKind_ = EndKind::None;
        output_.SetPlayLimit(UINT64_MAX);
        if (kind == EndKind::Final) {
            // The decision was taken on a copy; a row removed since then can leave its index past
            // the end of the list.
            const int cued = endState_.current;
            logic_.current = logic_.count == 0 ? -1 : (cued >= 0 && cued < logic_.count ? cued : 0);
            logic_.failures = 0;
            logic_.status = State::Stopped;
            Log("music: list finished");
            StopPlayback(endAllFailed_);
            return;
        }
        // Redecide on the real logic: it may have changed since the end was queued.
        const Step step = logic_.TrackEnded();
        if (step.action == Action::Start) {
            StartTrack(step.index, 0);
        } else {
            Log("music: list finished");
            Apply(step);
        }
    }

    void Watchdog(ULONGLONG now) {
        if (!output_.IsInitialised() || mode_ == OutputMode::Manual) return;
        if (logic_.status != State::Playing || !output_.GateOpen() || output_.UserHold() || !output_.DeviceStarted()) {
            lastConsumedChange_ = now;
            return;
        }
        const uint64_t consumed = output_.Consumed();
        if (consumed != lastConsumed_) {
            lastConsumed_ = consumed;
            lastConsumedChange_ = now;
            return;
        }
        if (output_.FramesQueued() == 0 || consumed >= output_.PlayLimit()) {
            lastConsumedChange_ = now;  // nothing to play yet: not a stall
            return;
        }
        if (now - lastConsumedChange_ < kWatchdogMs) return;
        deviceRestarts_.fetch_add(1);
        Log("music: no audio was taken for %u s; restarting the output device", kWatchdogMs / 1000);
        lastConsumedChange_ = now;
        output_.StopDevice();
        if (output_.StartDevice()) return;  // the ring is untouched: music continues where it was
        // The device would not start again: re-create it and continue the track where it was.
        const int64_t at = static_cast<int64_t>(PositionSeconds() * 1000.0);
        const int index = logic_.current;
        ClearPlayChain();
        output_.Uninit();
        StartTrack(index, at);
        if (output_.IsInitialised()) SetMessage(L"The audio device was opened again");
    }

    void IdleDevice(ULONGLONG now) {
        if (!output_.IsInitialised() || !output_.DeviceStarted() || mode_ == OutputMode::Manual) return;
        if (logic_.status == State::Playing) return;
        if (deviceIdleSince_ == 0) deviceIdleSince_ = now;
        const unsigned limit = logic_.status == State::Paused ? kPausedDeviceIdleMs : kStoppedDeviceIdleMs;
        if (now - deviceIdleSince_ >= limit) {
            output_.StopDevice();
            deviceIdleSince_ = 0;
        }
    }

    void Saves(ULONGLONG now) {
        if (listDirty_ && now - listDirtyAt_ >= kListSaveDelayMs && !playlistPath_.empty()) {
            listDirty_ = false;
            IoJob job;
            job.kind = IoJob::Kind::SaveFile;
            job.path = playlistPath_;
            job.content = FormatPlaylist(Stored());
            PostIo(std::move(job));
        }
        if (logic_.status == State::Playing && now - lastPositionSave_ >= positionSaveMs_) {
            lastPositionSave_ = now;
            RememberPosition();
            MarkSettingsDirty();
        }
        if (settingsDirty_ && now - settingsDirtyAt_ >= kSettingsSaveDelayMs && !settingsPath_.empty()) {
            settingsDirty_ = false;
            QueueSettingsSave();
        }
    }

    // The game was asked to close: the list (if it changed) and the song with its place go to the
    // I/O thread at once, without waiting for the usual delays. The music keeps playing; the close
    // may still be cancelled, and the final save at Shutdown follows when the window goes.
    void HandleSaveNow() {
        if (listDirty_ && !playlistPath_.empty()) {
            listDirty_ = false;
            IoJob job;
            job.kind = IoJob::Kind::SaveFile;
            job.path = playlistPath_;
            job.content = FormatPlaylist(Stored());
            PostIo(std::move(job));
        }
        if (settingsPath_.empty()) return;
        RememberPosition();
        lastPositionSave_ = GetTickCount64();
        settingsDirty_ = false;
        QueueSettingsSave();
    }

    // Hands the [music] values to the I/O thread, unless they are what was saved last (a song
    // that stands still, or a change that was undone, is not written again).
    void QueueSettingsSave() {
        MusicSettings settings = CurrentSettings();
        if (haveSaved_ && SameSettings(settings, lastSaved_)) return;
        lastSaved_ = settings;
        haveSaved_ = true;
        settingsSaves_.fetch_add(1);
        IoJob job;
        job.kind = IoJob::Kind::SaveSettings;
        job.path = settingsPath_;
        job.settings = std::move(settings);
        PostIo(std::move(job));
    }

    void MarkListDirty() {
        listDirty_ = true;
        listDirtyAt_ = GetTickCount64();
    }

    void MarkSettingsDirty() {
        if (!settingsDirty_) settingsDirtyAt_ = GetTickCount64();
        settingsDirty_ = true;
    }

    // ---- output device ----
    bool EnsureOutput() {
        if (output_.IsInitialised()) return true;
        lastDeviceAttempt_ = GetTickCount64();
        if (!output_.Init(mode_, silent_)) {
            ReportOutputFailed();
            return false;
        }
        if (outputFailed_) SetMessage(L"An audio device was found; music can play again");
        outputFailed_ = false;
        output_.SetVolume(volume_ * volume_);
        Log("music: output opened (%s, %u Hz)",
            mode_ == OutputMode::Wasapi ? "shared mode" : mode_ == OutputMode::Null ? "null device" : "manual", output_.SampleRate());
        return true;
    }

    void ReportOutputFailed() {
        if (!outputFailed_) {
            outputFailed_ = true;
            SetMessage(L"No audio device is available; music starts as soon as one is found");
            Log("music: the audio device could not be opened; retrying every %u s", kDeviceRetryMs / 1000);
        }
    }

    // ---- converter chain (kept across a track boundary when the format matches) ----
    bool ChainEnsure(uint32_t rate, uint32_t channels) {
        if (chainActive_ && chainRate_ == rate && chainChannels_ == channels) return true;
        if (chainActive_) ChainFlush();
        ma_data_converter_config config =
            ma_data_converter_config_init(ma_format_f32, ma_format_f32, channels, 2, rate, output_.SampleRate());
        config.resampling.algorithm = ma_resample_algorithm_linear;
        config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
        if (ma_data_converter_init(&config, nullptr, &chain_) != MA_SUCCESS) return false;
        chainActive_ = true;
        chainRate_ = rate;
        chainChannels_ = channels;
        pendingFrames_ = 0;
        pendingOffset_ = 0;
        return true;
    }

    // Pushes the converter's tail (the last few samples it still holds) into the ring, then
    // drops the converter.
    void ChainFlush() {
        if (!chainActive_) return;
        const ma_uint64 latency = ma_data_converter_get_input_latency(&chain_);
        if (latency > 0 && latency <= 256) {
            float zeros[256 * 8] = {};
            float tail[1024 * 2];
            const ma_uint64 channels = chainChannels_ <= 8 ? chainChannels_ : 0;
            if (channels > 0) {
                ma_uint64 inCount = latency;
                ma_uint64 outCount = 1024;
                ma_data_converter_process_pcm_frames(&chain_, zeros, &inCount, tail, &outCount);
                output_.Write(tail, static_cast<uint32_t>(outCount));
            }
        }
        ClearChain();
    }

    void ClearChain() {
        if (chainActive_) {
            ma_data_converter_uninit(&chain_, nullptr);
            chainActive_ = false;
        }
        pendingFrames_ = 0;
        pendingOffset_ = 0;
    }

    // ---- reporting ----
    void ReportSkipped(int index, OpenError error) {
        if (index < 0 || index >= static_cast<int>(entries_.size())) return;
        Entry& entry = entries_[static_cast<size_t>(index)];
        entry.problem = error;
        const std::wstring name = FileName(entry.path, false);
        SetMessage(L"Skipped " + name + L": " + DescribeProblem(error));
        Log("music: skipped %s (%s)", Utf8(name).c_str(), Utf8(DescribeProblem(error)).c_str());
        PublishList();
    }

    void SetMessage(const std::wstring& text) {
        AcquireSRWLockExclusive(&viewLock_);
        view_.message = text;
        ++view_.messageSerial;
        ReleaseSRWLockExclusive(&viewLock_);
    }

    double PositionSeconds() const {
        if (tracks_.empty()) return static_cast<double>(stoppedPositionMs_) / 1000.0;
        const OpenTrack& track = tracks_[0];
        const uint64_t consumed = output_.Consumed();
        const uint64_t played = consumed > track.startFrame ? consumed - track.startFrame : 0;
        return static_cast<double>(played + track.offsetFrames) / static_cast<double>(output_.SampleRate());
    }

    double DurationSeconds() const {
        if (!tracks_.empty()) {
            const OpenTrack& track = tracks_[0];
            if (track.info.lengthFrames > 0 && track.info.sampleRate > 0) {
                return static_cast<double>(track.info.lengthFrames) / track.info.sampleRate;
            }
        }
        if (logic_.current >= 0 && logic_.current < static_cast<int>(entries_.size())) {
            const int64_t ms = entries_[static_cast<size_t>(logic_.current)].durationMs;
            if (ms > 0) return static_cast<double>(ms) / 1000.0;
        }
        return -1;
    }

    void PublishList() {
        auto list = std::make_shared<std::vector<TrackInfo>>();
        list->reserve(entries_.size());
        for (const Entry& entry : entries_) {
            TrackInfo info;
            info.path = entry.path;
            info.title = entry.title.empty() ? FileName(entry.path, true) : entry.title;
            info.artist = entry.artist;
            info.durationSeconds = entry.durationMs > 0 ? static_cast<double>(entry.durationMs) / 1000.0 : -1;
            info.supported = entry.problem == OpenError::None;
            info.problem = DescribeProblem(entry.problem);
            list->push_back(std::move(info));
        }
        std::shared_ptr<const std::vector<TrackInfo>> old = std::move(list);
        AcquireSRWLockExclusive(&viewLock_);
        view_.list.swap(old);
        ++view_.listVersion;
        ReleaseSRWLockExclusive(&viewLock_);
        // `old` (the previous list) is released here, outside the lock.
    }

    void PublishView() {
        const double position = PositionSeconds();
        const double duration = DurationSeconds();
        AcquireSRWLockExclusive(&viewLock_);
        view_.state = logic_.status;
        view_.currentIndex = logic_.current;
        view_.positionSeconds = position;
        view_.durationSeconds = duration;
        view_.loop = logic_.loop;
        view_.volume = volume_;
        view_.pauseWhenGameUnfocused = pauseWhenUnfocused_;
        view_.outputFailed = outputFailed_;
        view_.scanning = ioPending_.load() > 0;
        ReleaseSRWLockExclusive(&viewLock_);
    }

    // ---- persistence ----
    void ResolvePaths() {
        std::wstring dir = dataDirOverride_;
        if (dir.empty()) {
            wchar_t buffer[MAX_PATH] = L"";
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
            if (n > 0 && n < MAX_PATH) dir = std::wstring(buffer) + L"\\RacingMasterMod";
        }
        dir = NormalisePath(dir);
        AcquireSRWLockExclusive(&viewLock_);
        dataDir_ = dir;
        musicDir_ = dir.empty() ? L"" : dir + L"\\" + kMusicFolderName;
        playlistPath_ = dir.empty() ? L"" : dir + L"\\" + kPlaylistName;
        settingsPath_ = dir.empty() ? L"" : dir + L"\\" + kSettingsName;
        ReleaseSRWLockExclusive(&viewLock_);
        if (!dir.empty()) CreateDirectoryW(ToExtendedPath(dir).c_str(), nullptr);
    }

    // Everything the player thread owns goes back to "just started" (Start after Shutdown).
    void ResetState() {
        entries_.clear();
        keyIndex_.clear();
        keyIndexDirty_ = false;
        logic_ = PlaylistState{};
        tracks_.clear();
        endKind_ = EndKind::None;
        pendingGate_ = false;
        ClearChain();
        volume_ = 0.6f;
        pauseWhenUnfocused_ = false;
        pausedByFocus_ = false;
        outputFailed_ = false;
        cuedIndex_ = -1;
        cuedPositionMs_ = 0;
        stoppedPositionMs_ = 0;
        rememberedPositionMs_ = 0;
        resumeMs_ = 0;
        listDirty_ = false;
        settingsDirty_ = false;
        lastPositionSave_ = GetTickCount64();
        haveSaved_ = false;
        lastDeviceAttempt_ = 0;
        deviceIdleSince_ = 0;
        opens_.store(0);
        deviceRestarts_.store(0);
        settingsSaves_.store(0);
        AcquireSRWLockExclusive(&viewLock_);
        view_.message.clear();
        ++view_.messageSerial;
        ReleaseSRWLockExclusive(&viewLock_);
    }

    void LoadState() {
        if (dataDir_.empty()) {
            Log("music: LOCALAPPDATA is not set; the playlist and settings will not be saved");
            return;
        }
        DeleteLeftoverTempFiles(dataDir_, kPlaylistName);
        DeleteLeftoverTempFiles(dataDir_, kSettingsName);

        std::vector<StoredTrack> stored;
        std::string text;
        bool loaded = false;
        const bool mainExists = ReadWholeFile(playlistPath_, &text, 64u << 20);
        if (mainExists) {
            loaded = ParsePlaylist(text, &stored);
            if (!loaded) Log("music: playlist.tsv is not a complete playlist file; trying the backup");
        }
        if (!loaded && ReadWholeFile(playlistPath_ + L".bak", &text, 64u << 20)) {
            loaded = ParsePlaylist(text, &stored);
            if (loaded) Log("music: the playlist was restored from its backup");
        }
        if (!loaded && mainExists) {
            // Keep the unreadable file for a person to repair; the next save must not destroy it.
            const std::wstring broken = playlistPath_ + L".broken";
            MoveFileExW(ToExtendedPath(playlistPath_).c_str(), ToExtendedPath(broken).c_str(), MOVEFILE_REPLACE_EXISTING);
            Log("music: the unreadable playlist was moved aside as playlist.tsv.broken");
            SetMessage(L"The saved playlist could not be read; it was kept as playlist.tsv.broken");
        }
        for (StoredTrack& track : stored) {
            Entry entry;
            entry.id = nextId_++;
            entry.path = std::move(track.path);
            entry.key = PathKey(entry.path);
            if (keyIndex_.count(entry.key)) continue;  // a hand-edited file may repeat a path
            entry.title = std::move(track.title);
            entry.artist = std::move(track.artist);
            entry.durationMs = track.durationMs;
            entry.fileSize = track.fileSize;
            entry.lastWrite = track.lastWrite;
            entry.tagsKnown = track.lastWrite != 0;
            keyIndex_.emplace(entry.key, entries_.size());
            entries_.push_back(std::move(entry));
        }

        MusicSettings settings;
        if (ReadWholeFile(settingsPath_, &text, 1u << 20)) ReadMusicSection(text, &settings);
        volume_ = static_cast<float>(settings.volumePercent) / 100.0f;
        output_.SetVolume(volume_ * volume_);
        pauseWhenUnfocused_ = settings.pauseWhenGameUnfocused;
        logic_.loop = settings.loop == 0 ? LoopMode::Off : settings.loop == 1 ? LoopMode::One : LoopMode::All;
        logic_.Inserted(0, static_cast<int>(entries_.size()));
        if (!settings.currentPath.empty()) {
            const auto found = keyIndex_.find(PathKey(settings.currentPath));
            if (found != keyIndex_.end()) {
                logic_.current = static_cast<int>(found->second);
                cuedIndex_ = logic_.current;
                cuedPositionMs_ = settings.positionMs;
                stoppedPositionMs_ = settings.positionMs;
                rememberedPositionMs_ = settings.positionMs;
            }
        }
        Log("music: %d track(s) in the list, loop %s, volume %d%%", logic_.count,
            logic_.loop == LoopMode::Off ? "off" : logic_.loop == LoopMode::One ? "one" : "all", settings.volumePercent);
    }

    std::vector<StoredTrack> Stored() const {
        std::vector<StoredTrack> stored;
        stored.reserve(entries_.size());
        for (const Entry& entry : entries_) {
            StoredTrack track;
            track.path = entry.path;
            track.title = entry.title;
            track.artist = entry.artist;
            track.durationMs = entry.durationMs;
            track.fileSize = entry.tagsKnown ? entry.fileSize : 0;
            track.lastWrite = entry.tagsKnown ? entry.lastWrite : 0;
            stored.push_back(std::move(track));
        }
        return stored;
    }

    void RememberPosition() {
        if (tracks_.empty()) {
            rememberedPositionMs_ = logic_.status == State::Stopped ? stoppedPositionMs_ : rememberedPositionMs_;
            return;
        }
        rememberedPositionMs_ = static_cast<int64_t>(PositionSeconds() * 1000.0);
    }

    MusicSettings CurrentSettings() const {
        MusicSettings settings;
        settings.volumePercent = static_cast<int>(volume_ * 100.0f + 0.5f);
        settings.loop = logic_.loop == LoopMode::Off ? 0 : logic_.loop == LoopMode::One ? 1 : 2;
        settings.pauseWhenGameUnfocused = pauseWhenUnfocused_;
        if (logic_.current >= 0 && logic_.current < static_cast<int>(entries_.size())) {
            settings.currentPath = entries_[static_cast<size_t>(logic_.current)].path;
        }
        settings.positionMs = rememberedPositionMs_ > 0 ? rememberedPositionMs_ : 0;
        return settings;
    }

    // Final save at shutdown, on the player thread after the I/O thread has stopped.
    void SaveNow() {
        if (dataDir_.empty()) return;
        unsigned long error = 0;
        if (!SaveFileAtomically(playlistPath_, FormatPlaylist(Stored()), &error)) {
            Log("music: the playlist could not be saved (Windows error %lu)", error);
        }
        bool leftAsIs = false;
        lastSaved_ = CurrentSettings();
        haveSaved_ = true;
        settingsSaves_.fetch_add(1);
        if (!SaveSettings(settingsPath_, lastSaved_, &error, &leftAsIs)) {
            if (leftAsIs) {
                LogSettingsLeftAsIs(error);
            } else {
                Log("music: the settings could not be saved (Windows error %lu)", error);
            }
        }
        listDirty_ = false;
        settingsDirty_ = false;
    }

    // Rewrites only the [music] section of settings.ini; the loader's [mod] lines and anything
    // else in the file stay as they are. A file that is there but cannot be read at this moment
    // (another program holds it, or it is over 1 MB) is left alone: writing only [music] would
    // drop every other section. *leftAsIs then says so, and the caller logs it after the lock.
    static bool SaveSettings(const std::wstring& path, const MusicSettings& settings, unsigned long* error,
                             bool* leftAsIs) {
        *leftAsIs = false;
        const SettingsFileLock lock;  // the panel rewrites its [panel] lines in the same file
        constexpr size_t kMaxBytes = 1u << 20;
        std::string existing;
        if (!ReadWholeFile(path, &existing, kMaxBytes)) {
            const DWORD readError = GetLastError();
            WIN32_FILE_ATTRIBUTE_DATA data = {};
            if (GetFileAttributesExW(ToExtendedPath(path).c_str(), GetFileExInfoStandard, &data)) {
                const bool tooLarge = data.nFileSizeHigh != 0 || data.nFileSizeLow > kMaxBytes;
                *error = tooLarge ? ERROR_FILE_TOO_LARGE : (readError != 0 ? readError : ERROR_READ_FAULT);
                *leftAsIs = true;
                return false;
            }
        }
        return SaveFileReplacing(path, WriteMusicSection(existing, settings), error);   // never leaves it missing
    }

    static void LogSettingsLeftAsIs(unsigned long error) {
        Log("music: settings.ini is there but could not be read just now (Windows error %lu%s), so it was left as it "
            "is and its other sections are kept; the music settings are saved again at the next change",
            error, error == ERROR_FILE_TOO_LARGE ? ": larger than 1 MB" : "");
    }

    // ================================================================================================
    // I/O thread
    // ================================================================================================
    void PostIo(IoJob job) {
        ioPending_.fetch_add(1);
        AcquireSRWLockExclusive(&ioLock_);
        ioJobs_.push_back(std::move(job));
        ReleaseSRWLockExclusive(&ioLock_);
        if (ioEvent_) SetEvent(ioEvent_);
    }

    static DWORD WINAPI IoThreadEntry(LPVOID self) {
        static_cast<Engine*>(self)->IoThread();
        return 0;
    }

    void IoThread() {
        NameThread(L"rmm-io");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool quit = false;
        while (!quit) {
            WaitForSingleObject(ioEvent_, 250);
            std::vector<IoJob> batch;
            AcquireSRWLockExclusive(&ioLock_);
            batch.swap(ioJobs_);
            ReleaseSRWLockExclusive(&ioLock_);
            for (IoJob& job : batch) {
                switch (job.kind) {
                    case IoJob::Kind::Quit: quit = true; break;
                    case IoJob::Kind::ScanFolder: if (!quit) ScanFolder(job.path, job.fromDropIn); break;
                    case IoJob::Kind::ReadTags: if (!quit) ReadTagsFor(job.items); break;
                    case IoJob::Kind::SaveFile:
                    case IoJob::Kind::SaveSettings: {
                        // Saves still run after Quit was seen in the same batch: losing them would
                        // lose the user's last change.
                        unsigned long error = 0;
                        bool leftAsIs = false;
                        const bool ok = job.kind == IoJob::Kind::SaveFile
                                            ? SaveFileAtomically(job.path, job.content, &error)
                                            : SaveSettings(job.path, job.settings, &error, &leftAsIs);
                        if (leftAsIs) {
                            LogSettingsLeftAsIs(error);
                            Command failed;
                            failed.kind = CommandKind::IoMessage;
                            failed.text = FileName(job.path, false) +
                                          L" could not be read just now, so the music settings were not saved into it; "
                                          L"they are saved again at the next change";
                            Post(std::move(failed));
                        } else if (!ok) {
                            Log("music: could not save %s (Windows error %lu)", Utf8(FileName(job.path, false)).c_str(), error);
                            Command failed;
                            failed.kind = CommandKind::IoMessage;
                            failed.text = FileName(job.path, false) + L" could not be saved (Windows error " +
                                          std::to_wstring(error) + L"); it is saved again at the next change";
                            Post(std::move(failed));
                        }
                        break;
                    }
                }
                ioPending_.fetch_sub(1);
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    void ScanFolder(const std::wstring& folder, bool fromDropIn) {
        const DWORD attributes = GetFileAttributesW(ToExtendedPath(folder).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!fromDropIn) {
                Command missing;
                missing.kind = CommandKind::IoMessage;
                missing.text = L"The folder " + folder + L" could not be opened";
                Post(std::move(missing));
            }
            return;
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        std::vector<std::pair<std::wstring, std::wstring>> found;  // relative path (for sorting), full path
        bool truncated = false;
        ScanInto(folder, L"", 0, &found, &truncated);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        std::sort(found.begin(), found.end(),
                  [](const std::pair<std::wstring, std::wstring>& a, const std::pair<std::wstring, std::wstring>& b) {
                      return NaturalLess(a.first, b.first);
                  });
        Command command;
        command.kind = CommandKind::FilesFound;
        command.flag = fromDropIn;
        command.text = folder;
        command.paths.reserve(found.size());
        for (auto& pair : found) command.paths.push_back(std::move(pair.second));
        Post(std::move(command));
        if (truncated) {
            Log("music: the scan of %s stopped at %zu files", Utf8(folder).c_str(), kMaxScanFiles);
            Command notice;
            notice.kind = CommandKind::IoMessage;
            notice.text = L"Only the first " + std::to_wstring(kMaxScanFiles) + L" files in " + folder + L" were added";
            Post(std::move(notice));
        }
    }

    void ScanInto(const std::wstring& folder, const std::wstring& relative, int depth,
                  std::vector<std::pair<std::wstring, std::wstring>>* found, bool* truncated) {
        if (depth > kMaxScanDepth || shutdown_.load() || *truncated) return;
        const std::wstring pattern = ToExtendedPath(folder) + L"\\*";
        WIN32_FIND_DATAW data = {};
        const HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                               FIND_FIRST_EX_LARGE_FETCH);
        if (search == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> subfolders;
        do {
            const wchar_t* name = data.cFileName;
            if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'))) continue;
            if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // no junction loops, no shortcuts followed
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                subfolders.push_back(name);
                continue;
            }
            const std::wstring full = folder + L"\\" + name;
            if (!HasMusicExtension(full)) continue;
            if (found->size() >= kMaxScanFiles) {
                *truncated = true;
                break;
            }
            found->emplace_back(relative.empty() ? std::wstring(name) : relative + L"\\" + name, full);
        } while (FindNextFileW(search, &data));
        FindClose(search);
        for (const std::wstring& sub : subfolders) {
            ScanInto(folder + L"\\" + sub, relative.empty() ? sub : relative + L"\\" + sub, depth + 1, found, truncated);
        }
    }

    void ReadTagsFor(const std::vector<TagJobItem>& items) {
        std::vector<TagResult> batch;
        ULONGLONG lastPost = GetTickCount64();
        auto flush = [&]() {
            if (batch.empty()) return;
            Command command;
            command.kind = CommandKind::TagsReady;
            command.tags = std::move(batch);
            batch.clear();
            Post(std::move(command));
            lastPost = GetTickCount64();
        };
        for (const TagJobItem& item : items) {
            if (shutdown_.load()) break;
            TagResult result;
            result.path = item.path;
            result.key = PathKey(item.path);
            WIN32_FILE_ATTRIBUTE_DATA attributes = {};
            if (!GetFileAttributesExW(ToExtendedPath(item.path).c_str(), GetFileExInfoStandard, &attributes)) {
                const DWORD code = GetLastError();
                result.problem = (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) ? OpenError::AccessDenied
                                                                                                 : OpenError::Missing;
            } else {
                result.fileSize = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
                result.lastWrite = FileTimeValue(attributes.ftLastWriteTime);
                if (attributes.dwFileAttributes &
                    (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE)) {
                    result.problem = OpenError::OnlineOnly;  // never opened: that would start a download
                } else if (item.cached && item.fileSize == result.fileSize && item.lastWrite == result.lastWrite) {
                    result.unchanged = true;
                } else {
                    OpenError sniff = OpenError::None;
                    const Kind kind = SniffFile(item.path, &sniff);
                    if (kind == Kind::Unknown) {
                        result.problem = sniff;
                    } else {
                        bool faulted = false;
                        result.tagsRead = ReadTags(item.path, kind, &result.tags, &faulted);
                        if (faulted) {
                            result.problem = OpenError::Damaged;
                            Log("music: reading the tags of %s failed inside the parser; the file is treated as damaged",
                                Utf8(FileName(item.path, false)).c_str());
                        }
                    }
                }
            }
            batch.push_back(std::move(result));
            if (batch.size() >= 50 || GetTickCount64() - lastPost >= 250) flush();
        }
        flush();
    }

    // ================================================================================================
    // State
    // ================================================================================================
    // Configuration (set before Start).
    OutputMode mode_ = OutputMode::Wasapi;
    bool silent_ = false;
    std::wstring dataDirOverride_;

    // Queues.
    SRWLOCK commandLock_;
    std::vector<Command> commands_;
    HANDLE commandEvent_ = nullptr;
    std::atomic<uint32_t> postedSerial_{0};
    std::atomic<uint32_t> cycleSerial_{0};
    SRWLOCK ioLock_;
    std::vector<IoJob> ioJobs_;
    HANDLE ioEvent_ = nullptr;
    std::atomic<int> ioPending_{0};

    // Threads.
    HANDLE playerThread_ = nullptr;
    HANDLE ioThread_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};

    // Published view.
    mutable SRWLOCK viewLock_;
    View view_;
    std::wstring dataDir_;
    std::wstring musicDir_;
    std::wstring playlistPath_;
    std::wstring settingsPath_;

    // Player-thread state.
    std::vector<Entry> entries_;
    std::unordered_map<std::wstring, size_t> keyIndex_;  // PathKey -> position in entries_
    bool keyIndexDirty_ = false;
    uint64_t nextId_ = 1;
    PlaylistState logic_;
    AudioOutput output_;
    std::vector<OpenTrack> tracks_;
    bool pendingGate_ = false;
    EndKind endKind_ = EndKind::None;
    uint64_t endFrame_ = 0;
    PlaylistState endState_;
    bool endAllFailed_ = false;
    uint64_t queuedDecisionId_ = 0;
    ma_data_converter chain_ = {};
    bool chainActive_ = false;
    uint32_t chainRate_ = 0;
    uint32_t chainChannels_ = 0;
    std::vector<float> pending_;
    uint32_t pendingFrames_ = 0;
    uint32_t pendingOffset_ = 0;
    float volume_ = 0.6f;
    bool pauseWhenUnfocused_ = false;
    bool pausedByFocus_ = false;
    bool outputFailed_ = false;
    int cuedIndex_ = -1;
    int64_t cuedPositionMs_ = 0;
    int64_t stoppedPositionMs_ = 0;
    int64_t rememberedPositionMs_ = 0;
    int64_t resumeMs_ = 0;
    uint64_t lastConsumed_ = 0;
    ULONGLONG lastConsumedChange_ = 0;
    ULONGLONG deviceIdleSince_ = 0;
    ULONGLONG lastDeviceAttempt_ = 0;
    bool listDirty_ = false;
    ULONGLONG listDirtyAt_ = 0;
    bool settingsDirty_ = false;
    ULONGLONG settingsDirtyAt_ = 0;
    ULONGLONG lastPositionSave_ = 0;
    unsigned positionSaveMs_ = kPositionSaveIntervalMs;
    MusicSettings lastSaved_;   // the [music] values last handed to a save (valid once haveSaved_)
    bool haveSaved_ = false;
    std::atomic<uint32_t> opens_{0};
    std::atomic<uint32_t> deviceRestarts_{0};
    std::atomic<uint32_t> settingsSaves_{0};
};

// Created on first use and never destroyed: nothing may run at process exit (the game can end
// at any moment, and destructors under the loader lock must not join threads or close devices).
Engine& TheEngine() {
    static Engine* const engine = new Engine;
    return *engine;
}

}  // namespace

// ---- Player --------------------------------------------------------------------------------------

Player& Player::Get() {
    static Player player;
    return player;
}

void Player::Start() { TheEngine().Start(); }
void Player::Shutdown() { TheEngine().Shutdown(); }

void Player::Play() { TheEngine().Simple(CommandKind::Play); }
void Player::Pause() {
    TheEngine().InstantHold();
    TheEngine().Simple(CommandKind::Pause);
}
void Player::TogglePlayPause() { TheEngine().Simple(CommandKind::TogglePlayPause); }
void Player::Stop() {
    TheEngine().InstantHold();
    TheEngine().Simple(CommandKind::Stop);
}
void Player::Next() { TheEngine().Simple(CommandKind::Next); }
void Player::Previous() { TheEngine().Simple(CommandKind::Previous); }

void Player::PlayIndex(int index) {
    Command command;
    command.kind = CommandKind::PlayIndex;
    command.index = index;
    TheEngine().Post(std::move(command));
}

void Player::Seek(double seconds) {
    Command command;
    command.kind = CommandKind::Seek;
    command.seconds = seconds;
    TheEngine().Post(std::move(command));
}

void Player::SetVolume(float v) {
    if (!(v >= 0)) v = 0;
    if (v > 1) v = 1;
    TheEngine().InstantVolume(v);
    Command command;
    command.kind = CommandKind::SetVolume;
    command.value = v;
    TheEngine().Post(std::move(command));
}

void Player::SetLoop(LoopMode m) {
    Command command;
    command.kind = CommandKind::SetLoop;
    command.index = static_cast<int>(m);
    TheEngine().Post(std::move(command));
}

void Player::SetPauseWhenGameUnfocused(bool on) {
    Command command;
    command.kind = CommandKind::SetPauseWhenUnfocused;
    command.flag = on;
    TheEngine().Post(std::move(command));
}

void Player::OnGameFocusChanged(bool focused) {
    Command command;
    command.kind = CommandKind::FocusChanged;
    command.flag = focused;
    TheEngine().Post(std::move(command));
}

void Player::AddFiles(const std::vector<std::wstring>& paths) {
    Command command;
    command.kind = CommandKind::AddFiles;
    command.paths = paths;
    TheEngine().Post(std::move(command));
}

void Player::AddFolder(const std::wstring& folder) {
    if (folder.empty()) return;  // a cancelled folder dialog
    Command command;
    command.kind = CommandKind::AddFolder;
    command.text = folder;
    TheEngine().Post(std::move(command));
}

void Player::Remove(int index) {
    Command command;
    command.kind = CommandKind::Remove;
    command.index = index;
    TheEngine().Post(std::move(command));
}

void Player::Clear() { TheEngine().Simple(CommandKind::Clear); }
void Player::SaveNow() { TheEngine().Simple(CommandKind::SaveNow); }
void Player::RescanMusicFolder() { TheEngine().Simple(CommandKind::Rescan); }

Snapshot Player::GetSnapshot() const {
    Snapshot snapshot;
    TheEngine().Read(snapshot, true);
    return snapshot;
}

void Player::GetSnapshotInto(Snapshot& out) const { TheEngine().Read(out, false); }

std::wstring Player::MusicFolder() const { return TheEngine().MusicFolder(); }

// ---- testing hooks -------------------------------------------------------------------------------

namespace testing {

void Configure(const Config& config) { TheEngine().Configure(config); }
bool Render(float* out, uint32_t frames) { return TheEngine().Render(out, frames); }
bool Sync(unsigned timeoutMs) { return TheEngine().Sync(timeoutMs); }
bool WaitForIdleIo(unsigned timeoutMs) { return TheEngine().WaitForIdleIo(timeoutMs); }
void SimulateDeviceLoss() { TheEngine().SimulateDeviceLoss(); }
Counters GetCounters() { return TheEngine().GetCounters(); }

}  // namespace testing

}  // namespace music

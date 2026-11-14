#pragma once

// Remote control of whatever other program is playing music (Spotify, a browser, any player)
// through Windows media sessions. Plain types only: no Windows or WinRT header is needed to use it.
//
// Threads:
//   Start()        any thread; creates the media thread the first time (cheap afterwards). Best
//                  called from the mod's worker thread or on the player's action, not every frame.
//   GetSnapshot()  any thread, every frame if wanted: copies a few strings under a lock that the
//                  media thread holds only for a pointer swap.
//   Play() ...     any thread: queues the command and returns at once. The outcome lands in a
//                  later snapshot (lastResult, resultSequence and a plain-English message).
//   Shutdown()     in the game: the media thread lets go of every session and sleeps until
//                  Start() is called again (it keeps its COM apartment, see remote.cpp). In the
//                  developer test host: the thread finishes; Shutdown waits up to 2 s for it.
//
// Nothing here waits for Windows or for another program. The media thread does that, with a
// time limit on every call, and the panel shows what it reports.

#include <string>
#include <vector>

namespace remote {

enum class Status { NoSession, Playing, Paused, Stopped, Unknown };
enum class Repeat { Unknown, Off, One, All };

// Which buttons the app currently offers. When a button that is off is pressed anyway, the
// snapshot's message says why.
struct Controls {
    bool play, pause, stop, next, previous, repeat;
};

// Where the media side stands as a whole.
enum class State {
    Off,            // Start() not called yet, or Shutdown() finished
    Starting,       // asking Windows for the session list
    Unsupported,    // this Windows has no media session API (older than Windows 10 1809)
    NotAnswering,   // Windows did not answer in time; the thread keeps retrying
    NoSession,      // no music app has registered a session
    WaitingForApp,  // the picked app is not running (or has closed); waiting for it
    Ready,          // a session is being controlled
    Faulted         // the media thread hit a fault and stopped for this run
};

// Outcome of the last command (the plain-English text is in Snapshot::message).
enum class Result {
    None,         // no command yet
    Pending,      // sent, waiting for the app
    Done,         // the app did it (or accepted a skip, which has no visible confirmation)
    NotOffered,   // the app does not offer that control right now
    Refused,      // Windows or the app said no
    NoReply,      // no answer within 1.5 s
    NoEffect,     // accepted, but the expected change did not show within 2.5 s
    SessionGone,  // the app closed while the command was in flight
    NoSession     // nothing to send to
};

// Which line Snapshot::message holds, so the panel can write it in any language. {app} is
// Snapshot::messageApp. The English text is in Snapshot::message.
enum class Message {
    None,              // nothing to say (a title is showing)
    LookingForApps,    // Looking for music apps...
    Unsupported,       // Controlling other apps needs Windows 10 (version 1809) or newer. Local files still work.
    NotAnswering,      // Windows is not answering about music apps right now. The panel keeps trying. Local files still work.
    Faulted,           // Controlling other apps stopped working in this session. Local files still work.
    NoApp,             // No music app found. Start playing something in Spotify, a browser or another player and it will show up here.
    WaitingForApp,     // Waiting for {app}. Start it and it will show up here.
    PickedNotRunning,  // {app} is not running. Start it, or pick another app below.
    PickedClosed,      // {app} has closed. Start it again, or pick another app below.
    AppClosed,         // {app} has closed.
    IsPlaying,         // {app} is playing.   (shown when the app shares no title)
    IsPaused,          // {app} is paused.
    IsStopped,         // {app} is stopped.
    AlreadyPlaying,    // {app} is already playing.
    AlreadyPaused,     // {app} is already paused.
    Sending,           // Sending to {app}...
    DonePlaying,       // {app} is playing.
    DonePaused,        // {app} is paused.
    DoneStopped,       // {app} has stopped.
    DonePlayPause,     // {app} accepted play/pause.
    DoneNext,          // Asked {app} for the next track.
    DonePrevious,      // Asked {app} for the previous track.
    DoneLoopOff,       // Looping is off in {app}.
    DoneLoopOne,       // Loop one is on in {app}.
    DoneLoopAll,       // Loop all is on in {app}.
    NoPlay,            // {app} does not offer "play" right now.
    NoPause,           // {app} does not offer "pause" right now.
    NoPlayPause,       // {app} does not offer "play/pause" right now.
    NoStop,            // {app} has no stop. Use pause.
    NoNext,            // {app} does not offer "next track" right now.
    NoPrevious,        // {app} does not offer "previous track" right now.
    LoopNotAllowed,    // {app} does not let other programs change looping. Change it in {app} itself.
    LoopIgnored,       // {app} did not change looping. Change it in {app} itself.
    Refused,           // {app} did not accept that. Try again, or use {app} itself.
    NoReply,           // {app} did not answer. Try again, or use {app} itself.
    NoEffect,          // {app} did not respond. Try again, or use {app} itself.
    TestGuard          // The test host may not send commands to {app}.  (developer test host only)
};

// One entry of the app picker.
struct App {
    std::wstring id;        // Windows' app id, the key for SelectApp(); never shown to the player
    std::wstring name;      // friendly name for the screen ("Google Chrome")
    Status status = Status::Unknown;
    bool isWindowsChoice = false;   // the session Windows calls current
};

struct Snapshot {
    bool available = false;          // a session is being controlled; the fields below are live
    Status status = Status::NoSession;
    std::wstring app;                // friendly name of the controlled (or picked) app
    std::wstring title;              // may be empty: many apps share no title
    std::wstring artist;
    double positionSeconds = 0.0;    // worked out for this moment while playing; 0 without a timeline
    double durationSeconds = 0.0;    // 0 when the app publishes no timeline
    Repeat repeat = Repeat::Unknown; // what the app reports, never what was last requested
    Controls controls = {};
    std::wstring message;            // plain-English line for the panel ("Spotify is paused.",
                                     // "Google Chrome does not offer "next track" right now.");
                                     // empty when there is nothing to say

    // Additions beyond the agreed minimum.
    State state = State::Off;
    bool hasTimeline = false;        // the app publishes a position and a length
    std::wstring appId;              // id of the controlled or picked app (matches App::id)
    std::vector<App> apps;           // every session Windows lists, for the picker
    bool followingWindows = true;    // true: control whichever session Windows calls current
    Result lastResult = Result::None;
    unsigned resultSequence = 0;     // grows by one each time a command outcome lands
    Message messageId = Message::None;   // which line message holds, for translation
    std::wstring messageApp;             // the {app} in that line (may differ from app: a closed one)
};

class Remote {
public:
    static Remote& Get();

    void Start();
    void Shutdown();

    void Play();
    void Pause();
    void TogglePlayPause();
    void Stop();
    void Next();
    void Previous();
    void SetRepeat(Repeat r);   // Repeat::Unknown is ignored

    // Control this app (an id from Snapshot::apps) even when Windows' choice changes; if it is
    // not running, wait for it. FollowWindows() returns to the default.
    void SelectApp(const std::wstring& appId);
    void FollowWindows();

    Snapshot GetSnapshot() const;   // cheap, callable from the frame thread every frame

private:
    Remote() = default;
};

// Developer test host only (RACING_MASTER_MOD_TEST_HOST=1 in testhost.exe). Called once by the
// mod's worker thread. When RACING_MASTER_MOD_TEST_MEDIA_SCRIPT is set, starts a thread that runs
// its steps against the host's silent fake player and logs each outcome; otherwise does nothing.
// In the game it does nothing.
void StartTestScriptIfAsked();

}  // namespace remote

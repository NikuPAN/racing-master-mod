// The engine end to end (src\music\player.cpp, audio_output.cpp): the real player thread, ring,
// converter and audio callback. Manual output: the test pulls audio through the callback as fast
// as it likes and checks the samples. One case runs miniaudio's null device in real time.
// Nothing opens a sound device.
#include "musictest.h"

#include "music/player.h"
#include "music/playlist_store.h"
#include "music/testing.h"

#include <windows.h>

#include <cmath>
#include <cstdio>

using namespace music;

namespace {

const std::vector<std::pair<char, double>> kKnown = {{'A', 440.0}, {'B', 660.0}, {'C', 880.0}, {'G', 500.0}};
constexpr double kPi = 3.141592653589793;

// The largest sample-to-sample step a clean sine of this frequency has at 48 kHz.
double NaturalStep(double frequency) {
    return 2.0 * mt::kAmplitude * std::sin(kPi * frequency / 48000.0);
}

std::wstring EngineFixture(const wchar_t* name) {
    static std::wstring dir;
    if (dir.empty()) {
        dir = mt::FixtureDir() + L"\\engine \u5F15\u64CE";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir + L"\\" + name;
}

struct Fixtures {
    std::wstring a, b, c, a10, g1, g2, g1x, g2x;
};

const Fixtures& Files() {
    static Fixtures f;
    if (f.a.empty()) {
        f.a = EngineFixture(L"A \u6E2C\u8A66 440.wav");
        f.b = EngineFixture(L"B \u6E2C\u8A66 660.wav");
        f.c = EngineFixture(L"C \u6E2C\u8A66 880.wav");
        f.a10 = EngineFixture(L"A ten seconds.wav");
        mt::WriteWav(f.a, 48000, 2, 48000, 440.0);
        mt::WriteWav(f.b, 48000, 2, 48000, 660.0);
        mt::WriteWav(f.c, 48000, 2, 48000, 880.0);
        mt::WriteWav(f.a10, 48000, 2, 480000, 440.0);
        // One continuous 500 Hz sine cut in two at a sample that is not a zero crossing, at the
        // pipeline rate and at 44.1 kHz (which goes through the resampler across the cut).
        f.g1 = EngineFixture(L"gapless 1.wav");
        f.g2 = EngineFixture(L"gapless 2.wav");
        mt::WriteWav(f.g1, 48000, 2, 28813, 500.0, 0);
        mt::WriteWav(f.g2, 48000, 2, 28800, 500.0, 28813);
        f.g1x = EngineFixture(L"gapless 1 44k.wav");
        f.g2x = EngineFixture(L"gapless 2 44k.wav");
        mt::WriteWav(f.g1x, 44100, 2, 26471, 500.0, 0);
        mt::WriteWav(f.g2x, 44100, 2, 26460, 500.0, 26471);
    }
    return f;
}

// One engine from Start to Shutdown, with its own data folder.
struct Session {
    explicit Session(const std::wstring& dataDir, OutputMode mode = OutputMode::Manual, bool fullVolume = true) : dir(dataDir) {
        testing::Config config;
        config.output = mode;
        config.dataDir = dir.c_str();
        testing::Configure(config);
        Player::Get().Start();
        if (fullVolume) Player::Get().SetVolume(1.0f);
        testing::Sync(5000);
    }
    Session() : Session(mt::NewDir(L"engine")) {}
    ~Session() { Player::Get().Shutdown(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    std::wstring dir;
};

Player& P() { return Player::Get(); }

Snapshot Snap() {
    testing::Sync(5000);
    return P().GetSnapshot();
}

// Pulls `seconds` of audio through the callback in 10 ms pieces, like a device would, and keeps
// the player thread at least half a second ahead.
void Render(double seconds, mt::Signal* capture) {
    constexpr uint32_t kChunk = 480;
    float buffer[kChunk * 2];
    const size_t total = static_cast<size_t>(seconds * 48000);
    for (size_t done = 0; done < total; done += kChunk) {
        const uint64_t consumedBefore = testing::GetCounters().consumed;
        testing::Render(buffer, kChunk);
        if (capture) capture->samples.insert(capture->samples.end(), buffer, buffer + kChunk * 2);
        const testing::Counters c = testing::GetCounters();
        // Render pulls far faster than real time. Wait for the player thread when the ring runs
        // low, and also when nothing was taken: the output is holding at a point where the player
        // must decide (a late loop change, the end of the list, a pause). The ring can still be
        // full of a dropped successor there, so the first test alone would read the whole window
        // as silence before the player thread has had a turn; a real device leaves it time.
        if (c.produced - c.consumed < 24000 || c.consumed == consumedBefore) testing::Sync(2000);
    }
}

bool Silent(const mt::Signal& s, size_t from, size_t to) {
    for (size_t i = from * 2; i < to * 2 && i < s.samples.size(); ++i) {
        if (std::fabs(s.samples[i]) > 1e-6f) return false;
    }
    return true;
}

}  // namespace

MT_TEST(500, EnginePosition, "engine: position, pause, volume") {
    const Fixtures& f = Files();
    Session session;
    P().AddFiles({f.a10});
    P().Play();
    testing::Sync(5000);
    bool monotonic = true;
    double worst = 0;
    double last = -1;
    for (int i = 1; i <= 10; ++i) {
        Render(0.1, nullptr);
        const double position = Snap().positionSeconds;
        worst = std::max(worst, std::fabs(position - 0.1 * i));
        if (position < last) monotonic = false;
        last = position;
    }
    mt::Check(monotonic, "position only moves forward while playing");
    mt::Check(worst <= 0.020, "position equals the audio rendered, within 20 ms", "worst difference %.4f s", worst);

    P().Pause();
    const double pausedAt = Snap().positionSeconds;
    mt::Signal paused;
    Render(0.3, &paused);
    mt::Check(Silent(paused, 480, paused.Frames()), "paused: silence after a 10 ms fade");
    const double stillAt = Snap().positionSeconds;
    mt::Check(Snap().state == State::Paused && std::fabs(stillAt - pausedAt) <= 0.011, "paused: the position holds",
              "%.4f s then %.4f s", pausedAt, stillAt);
    P().Play();
    testing::Sync(5000);
    mt::Signal resumed;
    Render(0.3, &resumed);
    mt::Check(mt::Sequence(resumed, kKnown) == "A" && Snap().positionSeconds > stillAt + 0.25, "resume continues the same track",
              "sequence %s, position %.3f", mt::Sequence(resumed, kKnown).c_str(), Snap().positionSeconds);

    // A jump from full volume to 10 % gain: ramped, so no sample-to-sample step above the sine's own.
    mt::Signal ramp;
    Render(0.1, &ramp);
    P().SetVolume(0.316f);  // the gain is the square: 0.1
    Render(0.1, &ramp);
    const double step = mt::MaxStep(ramp, 0, ramp.Frames());
    mt::Check(step <= NaturalStep(440.0) * 1.02, "volume change is ramped (no click)", "largest step %.5f, sine's own %.5f", step,
              NaturalStep(440.0));
    const double quiet = mt::Rms(ramp, ramp.Frames() - 2400, ramp.Frames());
    mt::Check(std::fabs(quiet - 0.1 * mt::kExpectedRms) < 0.01 * mt::kExpectedRms + 0.002, "volume 0.316 plays at a tenth of the level",
              "RMS %.4f", quiet);
    P().SetVolume(0.0f);
    mt::Signal mute;
    Render(0.1, &mute);
    mt::Check(Silent(mute, 480, mute.Frames()), "volume 0 is silence");
}

MT_TEST(510, EngineEnd, "engine: end of track, loop all, loop one") {
    const Fixtures& f = Files();
    {
        Session session;
        P().SetLoop(LoopMode::Off);
        P().AddFiles({f.a, f.b});
        P().Play();
        testing::Sync(5000);
        mt::Signal out;
        Render(2.5, &out);
        const Snapshot s = Snap();
        mt::Check(mt::Sequence(out, kKnown) == "AB.", "loop off: A, then B, then silence", "heard %s", mt::Sequence(out, kKnown).c_str());
        mt::Check(s.state == State::Stopped && s.currentIndex == 0, "loop off: stopped at the end with the first track cued",
                  "state %d, current %d", static_cast<int>(s.state), s.currentIndex);
        mt::Check(testing::GetCounters().underruns == 0, "no underrun while playing");
    }
    {
        Session session;
        P().SetLoop(LoopMode::All);
        P().AddFiles({f.a, f.b});
        P().Play();
        testing::Sync(5000);
        mt::Signal out;
        Render(4.9, &out);
        mt::Check(mt::Sequence(out, kKnown) == "ABABA", "loop all: A, B, A, B, A in five seconds", "heard %s",
                  mt::Sequence(out, kKnown).c_str());
    }
    {
        Session session;
        P().SetLoop(LoopMode::One);
        P().AddFiles({f.a, f.b});
        P().Play();
        testing::Sync(5000);
        mt::Signal out;
        Render(2.9, &out);
        mt::Check(mt::Sequence(out, kKnown) == "A" && testing::GetCounters().opens >= 3, "loop one: A three times, nothing else",
                  "heard %s, %u opens", mt::Sequence(out, kKnown).c_str(), testing::GetCounters().opens);
        P().Next();
        testing::Sync(5000);
        mt::Signal next;
        Render(0.5, &next);
        mt::Check(mt::Sequence(next, kKnown) == "B" && Snap().currentIndex == 1, "loop one: Next still moves on", "heard %s",
                  mt::Sequence(next, kKnown).c_str());
    }
}

MT_TEST(520, EngineGapless, "engine: gapless change-over") {
    const Fixtures& f = Files();
    struct Case {
        const char* name;
        std::wstring first, second;
        size_t boundary;  // output frame of the first sample of the second file
    } cases[] = {
        {"gapless at 48 kHz", f.g1, f.g2, 28813},
        {"gapless at 44.1 kHz through the resampler", f.g1x, f.g2x, static_cast<size_t>(26471.0 * 48000.0 / 44100.0)},
    };
    for (const Case& c : cases) {
        Session session;
        P().SetLoop(LoopMode::Off);
        P().AddFiles({c.first, c.second});
        P().Play();
        testing::Sync(5000);
        mt::Signal out;
        Render(1.4, &out);
        const size_t from = c.boundary - 2400;
        const size_t to = c.boundary + 2400;
        const double step = mt::MaxStep(out, from, to);
        const size_t zeros = mt::LongestZeroRun(out, from, to);
        char name[160];
        snprintf(name, sizeof(name), "%s: no step above the sine's own around the boundary", c.name);
        mt::Check(step <= NaturalStep(500.0) * 1.02, name, "largest step %.5f, sine's own %.5f", step, NaturalStep(500.0));
        snprintf(name, sizeof(name), "%s: no gap (no run of zeros) at the boundary", c.name);
        mt::Check(zeros < 3 && mt::Rms(out, from, to) > mt::kExpectedRms * 0.95, name, "longest zero run %zu samples", zeros);
        snprintf(name, sizeof(name), "%s: the second file is reached and the list ends", c.name);
        mt::Check(Snap().state == State::Stopped && testing::GetCounters().underruns == 0, name);
    }
}

MT_TEST(530, EngineSeek, "engine: seeking") {
    const Fixtures& f = Files();
    {
        Session session;
        P().SetLoop(LoopMode::Off);
        P().AddFiles({f.a, f.b});
        P().Play();
        testing::Sync(5000);
        P().Seek(0.5);
        const double position = Snap().positionSeconds;
        mt::Signal out;
        Render(1.0, &out);
        const double b = mt::FirstWindow(out, kKnown, 'B');
        mt::Check(std::fabs(position - 0.5) < 0.02, "seek to 0.5 s: the position says 0.5 s", "%.3f s", position);
        mt::Check(b >= 0.45 && b <= 0.56, "seek to 0.5 s in a 1 s track: the next track comes after about 0.5 s more", "after %.3f s", b);
    }
    const std::wstring mp3 = mt::FixtureDir() + L"\\\u6E2C\u8A66 mp3 #1 100%.mp3";
    if (!mt::Exists(mp3)) {
        mt::Skip("MP3 seek twice through the engine", "no MP3 fixture on this PC (see the MP3 decoding case)");
        return;
    }
    Session session;
    P().AddFiles({mp3});
    P().Play();
    testing::Sync(5000);
    P().Seek(0.6);
    const double first = Snap().positionSeconds;
    Render(0.1, nullptr);
    P().Seek(0.2);
    testing::Sync(5000);  // let the player thread seek before audio is pulled
    Render(0.1, nullptr);
    const double second = Snap().positionSeconds;
    mt::Check(std::fabs(first - 0.6) < 0.02 && std::fabs(second - 0.3) < 0.02, "MP3: seek forward, then back, positions follow",
              "%.3f s, then %.3f s", first, second);
}

MT_TEST(540, EngineSkip, "engine: missing and damaged files are skipped and flagged") {
    const Fixtures& f = Files();
    const std::wstring dir = mt::FixtureDir();
    const std::wstring missing = dir + L"\\\u4E0D\u5B58\u5728 gone.flac";
    const std::wstring garbage = dir + L"\\garbage \u6E2C\u8A66.mp3";
    const std::wstring text = dir + L"\\text \u6E2C\u8A66.m4a";
    // A FLAC cut in half: its header still states the full length, so the early end is noticed.
    // (A WAV cut in half is not: miniaudio trims its stated length to the bytes present, and it
    // simply plays as a shorter file.)
    const std::wstring flac = EngineFixture(L"C 880.flac");
    const std::wstring half = EngineFixture(L"C cut in half.flac");
    if (mt::EncodeWithMediaFoundation(flac, L"flac", 48000, 1.0, 880.0) < 0) {
        mt::Skip("skipping missing and damaged files", "this PC's Media Foundation has no FLAC encoder for the cut file");
        return;
    }
    const std::string whole = mt::ReadBytes(flac);
    mt::WriteBytes(half, whole.substr(0, whole.size() / 2));
    if (!mt::Exists(garbage) || !mt::Exists(text)) {
        mt::Fail("skip test fixtures", "the decoding cases did not leave their damaged files");
        return;
    }
    {
        Session session;
        P().SetLoop(LoopMode::Off);
        P().AddFiles({f.a, missing, garbage, half, text, f.b});
        P().Play();
        testing::Sync(5000);
        const ULONGLONG started = GetTickCount64();
        mt::Signal out;
        Render(3.3, &out);
        testing::WaitForIdleIo(5000);
        const Snapshot s = Snap();
        mt::Check(mt::Sequence(out, kKnown) == "ACB.", "the good parts play in order: A, the intact half of the cut FLAC (C), B", "heard %s",
                  mt::Sequence(out, kKnown).c_str());
        bool flags = s.tracks.size() == 6 && s.tracks[0].supported && s.tracks[5].supported;
        for (int i = 1; flags && i <= 4; ++i) flags = !s.tracks[static_cast<size_t>(i)].supported && !s.tracks[static_cast<size_t>(i)].problem.empty();
        std::string seen;
        for (const TrackInfo& t : s.tracks) seen += t.supported ? "ok " : "flagged ";
        mt::Check(flags, "the four bad rows are flagged with a reason, the good ones are not", "rows: %s", seen.c_str());
        mt::Check(s.tracks.size() == 6 && s.tracks[1].problem == L"file not found", "the missing file says \"file not found\"");
        mt::Check(GetTickCount64() - started < 15000, "skipping does not stall", "%llu ms",
                  static_cast<unsigned long long>(GetTickCount64() - started));
    }
    {
        Session session;
        P().SetLoop(LoopMode::All);
        P().AddFiles({missing, garbage, text});
        P().Play();
        testing::Sync(5000);
        Render(0.5, nullptr);
        const Snapshot s = Snap();
        mt::Check(s.state == State::Stopped && s.message == L"Nothing in the list could be played" && testing::GetCounters().opens <= 3,
                  "a list of only bad files, loop all: stops after one pass with one message", "state %d, %u opens",
                  static_cast<int>(s.state), testing::GetCounters().opens);
    }
}

MT_TEST(550, EngineRemove, "engine: removing tracks while playing") {
    const Fixtures& f = Files();
    {
        Session session;
        P().AddFiles({f.a, f.b, f.c});
        P().Play();
        testing::Sync(5000);
        Render(0.2, nullptr);
        P().Remove(0);
        testing::Sync(5000);
        mt::Signal out;
        Render(0.3, &out);
        const Snapshot s = Snap();
        mt::Check(mt::Sequence(out, kKnown) == "B" && s.currentIndex == 0 && s.tracks.size() == 2 && s.state == State::Playing,
                  "removing the playing track starts the one that followed", "heard %s", mt::Sequence(out, kKnown).c_str());
    }
    {
        Session session;
        P().AddFiles({f.a});
        P().Play();
        testing::Sync(5000);
        Render(0.1, nullptr);
        P().Remove(0);
        testing::Sync(5000);  // without it, audio pulled before the player thread acts is not silent
        mt::Signal out;
        Render(0.2, &out);
        const Snapshot s = Snap();
        mt::Check(s.state == State::Stopped && s.tracks.empty() && s.currentIndex == -1 && Silent(out, 480 * 4, out.Frames()),
                  "removing the only track stops and empties the list");
    }
}

MT_TEST(560, EngineLateChange, "engine: the next track changes after it was queued") {
    const Fixtures& f = Files();
    Session session;
    P().SetLoop(LoopMode::Off);
    P().AddFiles({f.a, f.b});
    P().Play();
    testing::Sync(5000);
    mt::Signal out;
    Render(0.7, &out);  // B is already decoded behind A in the ring by now
    const uint32_t opensBefore = testing::GetCounters().opens;
    P().SetLoop(LoopMode::One);
    testing::Sync(5000);
    Render(1.0, &out);
    mt::Check(opensBefore >= 2, "the next track was queued early (gapless preparation)", "%u opens", opensBefore);
    // Only A and B can play here. A 50 ms window that straddles the short silence and A's restart
    // holds few cycles and was sometimes read as the 500 Hz G ("A.GA"); against A and B only it is
    // unknown and skipped, while B would still be heard.
    const std::vector<std::pair<char, double>> onlyAB = {{'A', 440.0}, {'B', 660.0}};
    const std::string heard = mt::Sequence(out, onlyAB);
    mt::Check(heard == "A" || heard == "A.A", "loop one set late: A plays to its end, then A again, never B", "heard %s",
              heard.c_str());
}

MT_TEST(570, EngineNullDevice, "engine: real-time null device, device loss") {
    const Fixtures& f = Files();
    Session session(mt::NewDir(L"null device"), OutputMode::Null);
    P().AddFiles({f.a10});
    P().Play();
    Sleep(2000);
    testing::Counters c = testing::GetCounters();
    mt::Check(c.deviceStarted && c.consumed > 96000 * 8 / 10 && c.consumed < 96000 * 12 / 10 && c.callbacks > 50,
              "null device: about 48000 frames a second taken in real time", "%llu frames, %llu callbacks in 2 s",
              static_cast<unsigned long long>(c.consumed), static_cast<unsigned long long>(c.callbacks));
    P().Pause();
    Sleep(200);
    const uint64_t held = testing::GetCounters().consumed;
    Sleep(300);
    mt::Check(testing::GetCounters().consumed == held, "null device: nothing is taken while paused");
    P().Play();
    Sleep(300);
    mt::Check(testing::GetCounters().consumed > held, "null device: play continues");

    const double before = Snap().positionSeconds;
    testing::SimulateDeviceLoss();
    Sleep(4500);
    const Snapshot s = Snap();
    mt::Check(testing::GetCounters().deviceRestarts >= 1, "device loss: the watchdog restarts the output");
    mt::Check(s.state == State::Playing && s.positionSeconds > before + 0.5 && s.positionSeconds < before + 3.0,
              "device loss: music continues from where it was, not from the start", "%.2f s before, %.2f s after", before,
              s.positionSeconds);
    P().Stop();
    mt::Check(Snap().state == State::Stopped, "null device: stop");
}

MT_TEST(580, EnginePersistence, "engine: list, cued track and position survive a restart") {
    const Fixtures& f = Files();
    const std::wstring dir = mt::NewDir(L"persist \u6E2C\u8A66");
    {
        Session session(dir, OutputMode::Manual, false);
        P().SetLoop(LoopMode::Off);
        P().SetVolume(0.5f);
        P().AddFiles({f.a10, f.b});
        P().Play();
        testing::Sync(5000);
        Render(0.3, nullptr);
    }
    const std::string settings = mt::ReadBytes(dir + L"\\settings.ini");
    mt::Check(mt::Exists(dir + L"\\playlist.tsv") && settings.find("[music]") != std::string::npos &&
                  settings.find("volume=50") != std::string::npos && settings.find("loop=off") != std::string::npos,
              "playlist.tsv and the [music] section are written");
    Session session(dir, OutputMode::Manual, false);
    const Snapshot s = Snap();
    mt::Check(s.tracks.size() == 2 && s.tracks[0].path == f.a10 && s.tracks[1].path == f.b, "the list comes back");
    mt::Check(s.state == State::Stopped && s.currentIndex == 0 && std::fabs(s.positionSeconds - 0.3) < 0.1,
              "the cued track and position come back, stopped (nothing plays by itself)", "state %d, position %.3f",
              static_cast<int>(s.state), s.positionSeconds);
    mt::Check(s.loop == LoopMode::Off && std::fabs(s.volume - 0.5f) < 0.01f, "loop mode and volume come back");
    P().Play();
    const double resumed = Snap().positionSeconds;
    mt::Check(std::fabs(resumed - 0.3) < 0.1, "Play continues where it was left", "%.3f s", resumed);
}

// The game can end without the mod seeing it close (a forced exit, a crash). While a song plays,
// its place is saved every 10 s, and only when it moved; here every 300 ms, so the case is quick.
MT_TEST(585, EnginePositionSaves, "engine: the playing position is saved regularly, only when it changed") {
    const Fixtures& f = Files();
    const std::wstring dir = mt::NewDir(L"position saves");
    testing::Config config;
    config.output = OutputMode::Manual;
    config.dataDir = dir.c_str();
    config.positionSaveMs = 300;
    testing::Configure(config);
    P().Start();
    P().AddFiles({f.a10});
    testing::WaitForIdleIo(5000);
    P().Play();
    testing::Sync(5000);
    Render(1.0, nullptr);
    // The interval runs on the clock; the position comes from the audio rendered so far.
    auto waitAndSettle = [](unsigned ms) {
        const ULONGLONG until = GetTickCount64() + ms;
        while (GetTickCount64() < until) {
            testing::Sync(2000);
            Sleep(20);
        }
        testing::WaitForIdleIo(5000);
    };
    waitAndSettle(1200);
    MusicSettings saved;
    ReadMusicSection(mt::ReadBytes(dir + L"\\settings.ini"), &saved);
    mt::Check(saved.currentPath == f.a10 && saved.positionMs >= 900 && saved.positionMs <= 1100,
              "while a song plays, the song and its place reach settings.ini without a stop or a close",
              "position_ms=%lld", static_cast<long long>(saved.positionMs));

    // Playing, but no audio pulled: the place does not move, so nothing is written again.
    const uint32_t before = testing::GetCounters().settingsSaves;
    waitAndSettle(1200);
    mt::Check(testing::GetCounters().settingsSaves == before, "a place that did not move is not saved again",
              "%u saves before, %u after", before, testing::GetCounters().settingsSaves);

    Render(0.5, nullptr);
    waitAndSettle(1200);
    ReadMusicSection(mt::ReadBytes(dir + L"\\settings.ini"), &saved);
    mt::Check(testing::GetCounters().settingsSaves == before + 1 && saved.positionMs >= 1400 && saved.positionMs <= 1600,
              "once it moves, the next interval saves it, once", "%u saves after, position_ms=%lld",
              testing::GetCounters().settingsSaves, static_cast<long long>(saved.positionMs));
    P().Shutdown();
}

// When the game is asked to close, the worker asks for an immediate save and the music plays on
// (the close may be cancelled). Here with the engine's own 10 s interval, so only SaveNow can
// have written the place.
MT_TEST(586, EngineSaveNow, "engine: SaveNow writes the song and its place at once and keeps playing") {
    const Fixtures& f = Files();
    Session session(mt::NewDir(L"save now"));
    P().AddFiles({f.a10, f.b});
    testing::WaitForIdleIo(5000);
    P().PlayIndex(0);
    testing::Sync(5000);
    // Let the save that starting the song asks for (place 0) go through first.
    const ULONGLONG until = GetTickCount64() + 800;
    while (GetTickCount64() < until) {
        testing::Sync(2000);
        Sleep(20);
    }
    testing::WaitForIdleIo(5000);
    const uint32_t before = testing::GetCounters().settingsSaves;
    Render(0.7, nullptr);
    P().SaveNow();
    testing::Sync(5000);
    testing::WaitForIdleIo(5000);
    MusicSettings saved;
    ReadMusicSection(mt::ReadBytes(session.dir + L"\\settings.ini"), &saved);
    mt::Check(testing::GetCounters().settingsSaves == before + 1 && saved.currentPath == f.a10 && saved.positionMs >= 650 &&
                  saved.positionMs <= 750,
              "SaveNow writes the song and its place straight away", "%u saves before, %u after, position_ms=%lld", before,
              testing::GetCounters().settingsSaves, static_cast<long long>(saved.positionMs));
    std::vector<StoredTrack> list;
    ParsePlaylist(mt::ReadBytes(session.dir + L"\\playlist.tsv"), &list);
    mt::Check(list.size() == 2, "the list is in playlist.tsv as well", "%zu tracks in playlist.tsv", list.size());
    mt::Check(Snap().state == State::Playing, "the music keeps playing after SaveNow");
    P().SaveNow();
    testing::Sync(5000);
    testing::WaitForIdleIo(5000);
    mt::Check(testing::GetCounters().settingsSaves == before + 1, "a second SaveNow with nothing changed writes nothing");
}

// A [music] save that did not reach the file (another program held settings.ini, or the write
// failed) must not count as saved: the next SaveNow or periodic save writes the same values again,
// even though nothing changed since. Otherwise the place could stay unsaved until the final save,
// which a game that ends from its own menu or crashes never makes.
MT_TEST(587, EngineSaveRetried, "engine: a settings save that could not be written is made again by the next SaveNow") {
    const Fixtures& f = Files();
    Session session(mt::NewDir(L"save retried"));
    const std::wstring path = session.dir + L"\\settings.ini";
    P().AddFiles({f.a10});
    testing::WaitForIdleIo(5000);
    P().PlayIndex(0);
    testing::Sync(5000);
    const ULONGLONG until = GetTickCount64() + 800;
    while (GetTickCount64() < until) {
        testing::Sync(2000);
        Sleep(20);
    }
    testing::WaitForIdleIo(5000);
    Render(0.6, nullptr);   // no more audio is pulled after this, so the place stands still
    // Another program holds settings.ini (no sharing): the engine cannot read it and leaves it as it is.
    const HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    P().SaveNow();
    testing::Sync(5000);
    testing::WaitForIdleIo(5000);
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    MusicSettings saved;
    ReadMusicSection(mt::ReadBytes(path), &saved);
    mt::Check(held != INVALID_HANDLE_VALUE && saved.positionMs < 500,
              "while another program holds settings.ini, the place is not written into it", "position_ms=%lld",
              static_cast<long long>(saved.positionMs));
    testing::Sync(5000);   // the I/O thread's report of the failed save reaches the player thread
    P().SaveNow();
    testing::Sync(5000);
    testing::WaitForIdleIo(5000);
    ReadMusicSection(mt::ReadBytes(path), &saved);
    mt::Check(saved.currentPath == f.a10 && saved.positionMs >= 550 && saved.positionMs <= 650,
              "once it is free, the next SaveNow writes the place, although nothing changed since", "position_ms=%lld",
              static_cast<long long>(saved.positionMs));
}

MT_TEST(590, EngineFolders, "engine: drop-in folder, Add folder, natural order, duplicates") {
    const std::wstring dir = mt::NewDir(L"folders");
    CreateDirectoryW((dir + L"\\music").c_str(), nullptr);
    mt::WriteWav(dir + L"\\music\\dropped \u6E2C\u8A66.wav", 48000, 2, 4800, 440.0);
    Session session(dir);
    testing::WaitForIdleIo(10000);
    Snapshot s = Snap();
    mt::Check(s.tracks.size() == 1 && s.tracks[0].title == L"dropped \u6E2C\u8A66" && P().MusicFolder() == dir + L"\\music",
              "a file in the drop-in folder is in the list at start, titled by its name", "%zu tracks", s.tracks.size());
    mt::WriteWav(dir + L"\\music\\dropped later.wav", 48000, 2, 4800, 440.0);
    P().RescanMusicFolder();
    testing::WaitForIdleIo(10000);
    s = Snap();
    mt::Check(s.tracks.size() == 2 && s.tracks[1].title == L"dropped later", "a rescan appends a new file and skips known ones");

    const std::wstring album = mt::NewDir(L"album \u5C08\u8F2F");
    for (const wchar_t* name : {L"\\10.wav", L"\\2.wav", L"\\1.wav", L"\\notes.txt"}) mt::WriteWav(album + name, 48000, 2, 4800, 660.0);
    CreateDirectoryW((album + L"\\disc 2").c_str(), nullptr);
    mt::WriteWav(album + L"\\disc 2\\3.wav", 48000, 2, 4800, 660.0);
    P().AddFolder(album);
    testing::WaitForIdleIo(10000);
    s = Snap();
    const bool order = s.tracks.size() == 6 && s.tracks[2].title == L"1" && s.tracks[3].title == L"2" && s.tracks[4].title == L"10" &&
                       s.tracks[5].title == L"3";
    mt::Check(order, "Add folder: recursive, music files only, natural order (1, 2, 10, then the sub-folder)", "%zu tracks",
              s.tracks.size());
    mt::Check(s.tracks.size() == 6 && std::fabs(s.tracks[2].durationSeconds - 0.1) < 0.01, "durations are read for the list");
    P().AddFolder(album);
    testing::WaitForIdleIo(10000);
    s = Snap();
    mt::Check(s.tracks.size() == 6 && s.message.find(L"already in the list") != std::wstring::npos,
              "adding the same folder again adds nothing and says so");
    Snapshot kept = P().GetSnapshot();
    kept.tracks.clear();
    P().GetSnapshotInto(kept);
    mt::Check(kept.tracks.empty(), "GetSnapshotInto skips the list copy when the list did not change");
}

MT_TEST(600, EngineSettingsFiles, "engine: settings.ini shared with the loader, damaged playlist") {
    {
        const std::wstring dir = mt::NewDir(L"settings");
        const std::wstring text = L"[mod]\r\nstage=1\r\n";
        std::string bytes = "\xFF\xFE";
        for (wchar_t c : text) {
            bytes.push_back(static_cast<char>(c & 0xFF));
            bytes.push_back(static_cast<char>(c >> 8));
        }
        mt::WriteBytes(dir + L"\\settings.ini", bytes);
        {
            Session session(dir, OutputMode::Manual, false);
            P().SetVolume(0.25f);
            testing::Sync(5000);
        }
        const std::string after = mt::ReadBytes(dir + L"\\settings.ini");
        mt::Check(after.find("[mod]\r\nstage=1\r\n") == 0 && after.find("volume=25") != std::string::npos,
                  "a UTF-16 settings.ini keeps the loader's [mod] lines when the music settings are saved");
    }
    {
        // A settings.ini that is there but cannot be read when the engine saves (here: over the
        // engine's 1 MB limit; in the game also an editor or scanner holding it) is left as it
        // is. Writing only [music] would drop the [mod], [panel] and [keys] lines.
        const std::wstring dir = mt::NewDir(L"settings unreadable");
        std::string bytes = "[mod]\r\nstage=1\r\n[panel]\r\nmode=on\r\n;";
        bytes.append(1100000, 'x');
        bytes += "\r\n";
        mt::WriteBytes(dir + L"\\settings.ini", bytes);
        {
            Session session(dir, OutputMode::Manual, false);
            P().SetVolume(0.25f);
            testing::Sync(5000);
            testing::WaitForIdleIo(5000);
        }
        mt::Check(mt::ReadBytes(dir + L"\\settings.ini") == bytes,
                  "a settings.ini the engine cannot read is left as it is, not replaced by one with only [music]");
    }
    {
        const std::wstring dir = mt::NewDir(L"damaged playlist");
        mt::WriteBytes(dir + L"\\playlist.tsv", "RMM-PLAYLIST\t1\r\nT\t0\t0\t-1\t\t\tC:\\one.mp3\r\n");  // cut before END
        mt::WriteBytes(dir + L"\\playlist.tsv.bak", "RMM-PLAYLIST\t1\r\nT\t0\t0\t-1\t\t\tC:\\backup.mp3\r\nEND\t1\r\n");
        {
            Session session(dir);
            const Snapshot s = Snap();
            mt::Check(s.tracks.size() == 1 && s.tracks[0].path == L"C:\\backup.mp3", "a playlist cut short falls back to the backup");
        }
        mt::WriteBytes(dir + L"\\playlist.tsv", "garbage");
        mt::WriteBytes(dir + L"\\playlist.tsv.bak", "garbage too");
        Session session(dir);
        const Snapshot s = Snap();
        mt::Check(s.tracks.empty() && mt::Exists(dir + L"\\playlist.tsv.broken") && !s.message.empty(),
                  "when both are unreadable: empty list, the file kept as playlist.tsv.broken, and a message");
    }
}

MT_TEST(610, EngineFormats, "engine: every format through the converter to 48 kHz") {
    struct Item {
        const char* name;
        std::wstring path;
        double frequency;
    };
    const Item items[] = {
        {"engine WAV 44.1 kHz", mt::FixtureDir() + L"\\\u6E2C\u8A66 wav #1 100%.wav", 440.0},
        {"engine FLAC", mt::FixtureDir() + L"\\\u6E2C\u8A66 flac #1 100%.flac", 660.0},
        {"engine MP3", mt::FixtureDir() + L"\\\u6E2C\u8A66 mp3 #1 100%.mp3", 880.0},
        {"engine M4A / AAC", mt::FixtureDir() + L"\\\u6E2C\u8A66 aac #1 100%.m4a", 550.0},
        {"engine Opus", mt::FixtureDir() + L"\\\u6E2C\u8A66 opus #1 100%.opus", 770.0},
        {"engine OGG Vorbis", mt::FixtureDir() + L"\\\u6E2C\u8A66 vorbis #1 100%.ogg", 440.0},
    };
    for (const Item& item : items) {
        if (!mt::Exists(item.path)) {
            mt::Skip(item.name, "no fixture of this format on this PC (see its decoding case)");
            continue;
        }
        Session session;
        P().SetLoop(LoopMode::Off);
        P().AddFiles({item.path});
        P().Play();
        testing::Sync(5000);
        mt::Signal out;
        Render(0.9, &out);
        mt::CheckSine(item.name, out, item.frequency);
    }
}

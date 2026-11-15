// Even volume (0.3.2): the loudness meter (src\music\loudness.cpp) against the reference cases of
// EBU Tech 3341, the gain rule, the L lines in playlist.tsv, and the engine playing a loud and a
// quiet song at the same level. Manual output: nothing makes a sound.
#include "musictest.h"

#include "music/loudness.h"
#include "music/player.h"
#include "music/playlist_store.h"
#include "music/testing.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace music;

namespace {

constexpr double kPi = 3.141592653589793;

// Feeds `seconds` of a stereo (or mono) sine at `dbfs` peak level into the meter, in 1000-frame
// pieces like a decoder would.
void FeedSine(LoudnessMeter* meter, uint32_t rate, uint32_t channels, double frequency, double dbfs, double seconds,
              uint64_t* phase) {
    const double amplitude = std::pow(10.0, dbfs / 20.0);
    const uint64_t total = static_cast<uint64_t>(seconds * rate + 0.5);
    std::vector<float> chunk(1000 * channels);
    for (uint64_t done = 0; done < total;) {
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(1000, total - done));
        for (uint32_t i = 0; i < n; ++i) {
            const float v = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(*phase + i) / rate));
            for (uint32_t c = 0; c < channels; ++c) chunk[static_cast<size_t>(i) * channels + c] = v;
        }
        meter->Add(chunk.data(), n);
        *phase += n;
        done += n;
    }
}

struct Segment {
    double dbfs;
    double seconds;
};

double Measure(uint32_t rate, uint32_t channels, std::initializer_list<Segment> segments) {
    LoudnessMeter meter;
    if (!meter.Init(rate, channels)) return 999;
    uint64_t phase = 0;
    for (const Segment& s : segments) FeedSine(&meter, rate, channels, 1000.0, s.dbfs, s.seconds, &phase);
    double lufs = 0;
    return meter.Integrated(&lufs) ? lufs : 999;
}

void CheckLufs(const char* name, double got, double expected) {
    mt::Check(std::fabs(got - expected) <= 0.1, name, "%.3f LUFS, expected %.1f +-0.1", got, expected);
}

double Db(double ratio) { return 20.0 * std::log10(ratio); }

}  // namespace

MT_TEST(380, LoudnessMeterCases, "even volume: the loudness meter (EBU Tech 3341 cases) and the gain rule") {
    // EBU Tech 3341 (2016) table 1, cases 1-5: stereo 1 kHz sines.
    CheckLufs("meter: EBU 3341 case 1 (-23 dBFS, 20 s) reads -23.0", Measure(48000, 2, {{-23, 20}}), -23.0);
    CheckLufs("meter: EBU 3341 case 2 (-33 dBFS, 20 s) reads -33.0", Measure(48000, 2, {{-33, 20}}), -33.0);
    CheckLufs("meter: EBU 3341 case 3 (relative gate: -36/-23/-36 dBFS) reads -23.0",
              Measure(48000, 2, {{-36, 10}, {-23, 60}, {-36, 10}}), -23.0);
    CheckLufs("meter: EBU 3341 case 4 (both gates: -72/-36/-23/-36/-72 dBFS) reads -23.0",
              Measure(48000, 2, {{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}}), -23.0);
    CheckLufs("meter: EBU 3341 case 5 (-26/-20/-26 dBFS) reads -23.0", Measure(48000, 2, {{-26, 20}, {-20, 20.1}, {-26, 20}}),
              -23.0);
    CheckLufs("meter: 44.1 kHz reads the same as 48 kHz (filters designed per rate)", Measure(44100, 2, {{-23, 20}}), -23.0);
    CheckLufs("meter: 96 kHz reads the same", Measure(96000, 2, {{-23, 20}}), -23.0);
    CheckLufs("meter: a mono file reads like the same sine on both speakers", Measure(48000, 1, {{-23, 20}}), -23.0);
    mt::Check(Measure(48000, 2, {{-80, 10}}) == 999, "meter: silence below the -70 LUFS gate gives no reading");
    mt::Check(Measure(48000, 2, {{-23, 0.3}}) == 999, "meter: shorter than one 400 ms block gives no reading");

    LoudnessMeter peakMeter;
    peakMeter.Init(48000, 2);
    uint64_t phase = 0;
    FeedSine(&peakMeter, 48000, 2, 1000.0, -6.0, 1.0, &phase);
    mt::Check(std::fabs(Db(peakMeter.Peak()) + 6.0) < 0.05, "meter: the sample peak is kept", "%.3f dBFS", Db(peakMeter.Peak()));

    // The gain rule: to -14 LUFS, never more than +12 dB, never a sample past 0.95.
    const double down = Db(LevelGain(-8.0, 1.0f));
    mt::Check(std::fabs(down + 6.0) < 0.01, "gain: a song at -8 LUFS is turned down 6 dB", "%.3f dB", down);
    const double up = Db(LevelGain(-20.0, 0.25f));
    mt::Check(std::fabs(up - 6.0) < 0.01, "gain: a quiet song at -20 LUFS with room to spare is raised 6 dB", "%.3f dB", up);
    const float capped = LevelGain(-20.0, 0.9f);
    mt::Check(std::fabs(capped * 0.9f - 0.95f) < 0.001f, "gain: raising stops where the loudest sample reaches 0.95",
              "gain %.4f", capped);
    mt::Check(LevelGain(-20.0, 1.0f) == 1.0f, "gain: a song already at full scale is never raised");
    const double most = Db(LevelGain(-60.0, 0.001f));
    mt::Check(std::fabs(most - 12.0) < 0.01, "gain: never raised more than 12 dB", "%.3f dB", most);
}

MT_TEST(405, PlaylistLoudness, "playlist.tsv: the measured loudness (L lines)") {
    std::vector<StoredTrack> tracks(3);
    tracks[0].path = L"C:\\Music\\loud.flac";
    tracks[0].fileSize = 100;
    tracks[0].lastWrite = 200;
    tracks[0].loudnessState = 1;
    tracks[0].lufs = -7.53;
    tracks[0].peak = 0.987654f;
    tracks[1].path = L"C:\\Music\\silent.wav";
    tracks[1].fileSize = 5;
    tracks[1].lastWrite = 6;
    tracks[1].loudnessState = 2;
    tracks[2].path = L"C:\\Music\\new.mp3";   // tags not read yet: its loudness is not written
    tracks[2].loudnessState = 1;
    tracks[2].lufs = -10;
    const std::string text = FormatPlaylist(tracks);
    std::vector<StoredTrack> back;
    const bool ok = ParsePlaylist(text, &back);
    mt::Check(ok && back.size() == 3 && back[0].loudnessState == 1 && std::fabs(back[0].lufs + 7.53) < 0.006 &&
                  std::fabs(back[0].peak - 0.987654f) < 2e-6f,
              "L line: loudness and peak survive a save", "%s", text.c_str());
    mt::Check(ok && back.size() == 3 && back[1].loudnessState == 2, "L line: 'measured, no result' survives a save");
    mt::Check(ok && back.size() == 3 && back[2].loudnessState == 0, "L line: not written for a row whose file is not known yet");
    mt::Check(text.find("END\t3") != std::string::npos, "L line: the END count still counts songs only");

    const std::string damaged = "RMM-PLAYLIST\t1\r\nT\t1\t2\t-1\t\t\tC:\\a.mp3\r\nL\tloud\tvery\r\nL\t-500\r\nEND\t1\r\n";
    const bool damagedOk = ParsePlaylist(damaged, &back);
    mt::Check(damagedOk && back.size() == 1 && back[0].loudnessState == 0,
              "L line: an unreadable L line is ignored (the song is measured again), the list still loads");
    const std::string old = "RMM-PLAYLIST\t1\r\nT\t1\t2\t-1\t\t\tC:\\a.mp3\r\nEND\t1\r\n";
    mt::Check(ParsePlaylist(old, &back) && back.size() == 1 && back[0].loudnessState == 0,
              "L line: a list saved before 0.3.2 loads unchanged");

    MusicSettings settings;
    ReadMusicSection("[music]\r\neven_volume=0\r\n", &settings);
    const bool off = !settings.evenVolume;
    MusicSettings defaults;
    mt::Check(off && defaults.evenVolume && WriteMusicSection("", settings).find("even_volume=0") != std::string::npos,
              "settings.ini: even_volume is read and written, on by default");
}

namespace {

std::wstring LoudnessFixture(const wchar_t* name) {
    static std::wstring dir;
    if (dir.empty()) {
        dir = mt::FixtureDir() + L"\\even volume \u97F3\u91CF";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir + L"\\" + name;
}

void RenderSeconds(double seconds, mt::Signal* capture) {
    constexpr uint32_t kChunk = 480;
    float buffer[kChunk * 2];
    const size_t total = static_cast<size_t>(seconds * 48000);
    for (size_t done = 0; done < total; done += kChunk) {
        const uint64_t consumedBefore = testing::GetCounters().consumed;
        testing::Render(buffer, kChunk);
        if (capture) capture->samples.insert(capture->samples.end(), buffer, buffer + kChunk * 2);
        const testing::Counters c = testing::GetCounters();
        if (c.produced - c.consumed < 24000 || c.consumed == consumedBefore) testing::Sync(2000);
    }
}

bool WaitForMeasured(uint32_t count, unsigned timeoutMs) {
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    while (testing::GetCounters().measured < count) {
        if (GetTickCount64() >= until) return false;
        testing::WaitForIdleIo(1000);
        Sleep(10);
    }
    return testing::Sync(5000);
}

void StartEngine(const std::wstring& dir, bool evenByDefault) {
    testing::Config config;
    config.output = OutputMode::Manual;
    config.dataDir = dir.c_str();
    config.evenVolume = evenByDefault;
    testing::Configure(config);
    Player::Get().Start();
    Player::Get().SetVolume(1.0f);
    testing::Sync(5000);
}

}  // namespace

MT_TEST(620, EngineEvenVolume, "engine: even volume plays a loud and a quiet song at the same level") {
    // 18 dB apart: amplitude 0.5 and 0.0625. Two seconds of the loud one, then twelve of the quiet.
    const std::wstring loud = LoudnessFixture(L"loud 440.wav");
    const std::wstring quiet = LoudnessFixture(L"quiet 660.wav");
    mt::WriteWav(loud, 48000, 2, 96000, 440.0, 0, 0.5);
    mt::WriteWav(quiet, 48000, 2, 576000, 660.0, 0, 0.0625);
    const std::wstring dir = mt::NewDir(L"even");

    StartEngine(dir, true);
    Player& p = Player::Get();
    p.SetLoop(LoopMode::Off);
    p.AddFiles({loud, quiet});
    const bool measured = WaitForMeasured(2, 20000);
    mt::Check(measured, "both songs are measured in the background after they are added", "measured %u",
              testing::GetCounters().measured);
    p.Play();
    testing::Sync(5000);
    mt::Signal out;
    RenderSeconds(4.0, &out);
    const double loudRms = mt::Rms(out, 24000, 90000);     // 0.5-1.9 s: the loud song after its fade-in
    const double quietRms = mt::Rms(out, 110000, 190000);  // 2.3-4.0 s: the quiet song
    const double apart = Db(loudRms / quietRms);
    mt::Check(std::fabs(apart) < 0.5, "the loud and the quiet song come out within 0.5 dB of each other (18 dB apart in the files)",
              "loud RMS %.4f, quiet RMS %.4f, %.2f dB apart", loudRms, quietRms, apart);
    mt::Check(loudRms < 0.5 / std::sqrt(2.0) * 0.6, "the loud song is turned down, not the quiet one pushed into clipping",
              "loud RMS %.4f", loudRms);
    float worst = 0;
    for (const float s : out.samples) worst = std::max(worst, std::fabs(s));
    mt::Check(worst <= 0.951f, "no sample passes 0.95", "peak %.4f", worst);
    mt::Signal settled;   // from 0.3 s, after the fade-in from silence
    settled.samples.assign(out.samples.begin() + 2 * 14400, out.samples.end());
    const std::string order = mt::Sequence(settled, {{'L', 440.0}, {'Q', 660.0}});
    mt::Check(mt::LongestZeroRun(out, 90000, 110000) < 3 && order == "LQ", "the change-over between them stays gapless",
              "sequence %s", order.c_str());

    // Off: the quiet song goes back to its own level, smoothly (ring of up to 2 s, then a ramp).
    p.SetEvenVolume(false);
    mt::Check(!p.EvenVolume(), "the setting shows off at once");
    mt::Signal after;
    RenderSeconds(5.0, &after);
    const double ownLevel = 0.0625 / std::sqrt(2.0);
    const double offRms = mt::Rms(after, 216000, 240000);
    mt::Check(std::fabs(Db(offRms / ownLevel)) < 0.3, "switched off, the quiet song plays at its own level again",
              "RMS %.4f, its own %.4f", offRms, ownLevel);
    double biggestStep = 0;
    for (size_t i = 2; i < after.samples.size(); i += 2) {
        biggestStep = std::max(biggestStep, static_cast<double>(std::fabs(after.samples[i] - after.samples[i - 2])));
    }
    mt::Check(biggestStep < 2.0 * 0.0625 * 3.2 * std::sin(kPi * 660.0 / 48000.0) * 1.1, "switching it off makes no jump in level",
              "largest step %.5f", biggestStep);
    p.Shutdown();

    const std::string list = mt::ReadBytes(dir + L"\\playlist.tsv");
    size_t lines = 0;
    for (size_t at = list.find("\nL\t"); at != std::string::npos; at = list.find("\nL\t", at + 1)) ++lines;
    mt::Check(lines == 2, "playlist.tsv keeps both measurements", "%zu L line(s)", lines);
    const std::string ini = mt::ReadBytes(dir + L"\\settings.ini");
    mt::Check(ini.find("even_volume=0") != std::string::npos, "settings.ini keeps the setting off");

    // Next start: nothing is measured again; switched back on, the saved measurement is used at once.
    StartEngine(dir, true);
    p.SetEvenVolume(true);
    p.PlayIndex(1);
    testing::Sync(5000);
    mt::Signal again;
    RenderSeconds(1.0, &again);
    const double againRms = mt::Rms(again, 24000, 48000);
    mt::Check(testing::GetCounters().measured == 0 && std::fabs(Db(againRms / quietRms)) < 0.5,
              "after a restart the saved measurement is used, nothing is measured again",
              "measured %u, RMS %.4f (was %.4f)", testing::GetCounters().measured, againRms, quietRms);
    p.Shutdown();
}

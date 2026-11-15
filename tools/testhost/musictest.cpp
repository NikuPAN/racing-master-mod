// musictest.exe: silent tests for the music engine (src\music), run by tools\run-host-test.ps1.
//
// Usage:  musictest.exe <empty work folder> <repository folder>
//
// It links the same engine objects that go into version.dll. Nothing here makes a sound or shows a
// window: engine cases use the Manual output (the test pulls audio through the real callback) or
// miniaudio's null backend; the file dialog is configured but never shown. It also sets
// RACING_MASTER_MOD_TEST_HOST=1 for itself, under which the engine refuses a real audio device
// and the picker never calls Show, so a mistake in a test cannot become audible or visible.
#include "musictest.h"

#include "core/log.h"

#include <windows.h>
#include <objbase.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace mt {

namespace {

struct Case {
    int order;
    const char* title;
    TestFn fn;
};

std::vector<Case>& Cases() {
    static std::vector<Case> cases;
    return cases;
}

int g_passed = 0;
int g_failed = 0;
int g_skipped = 0;
std::wstring g_work;
std::wstring g_repo;
std::wstring g_fixtures;

void PrintLine(const char* status, const char* name, const char* detail) {
    if (detail && detail[0]) printf("%s %s -- %s\n", status, name, detail);
    else printf("%s %s\n", status, name);
    fflush(stdout);
}

}  // namespace

Registrar::Registrar(int order, const char* title, TestFn fn) {
    Cases().push_back({order, title, fn});
}

void Pass(const char* name) {
    ++g_passed;
    PrintLine("PASS", name, nullptr);
}

void Fail(const char* name, const char* format, ...) {
    char detail[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    ++g_failed;
    PrintLine("FAIL", name, detail);
}

void Skip(const char* name, const char* reason) {
    ++g_skipped;
    PrintLine("SKIPPED", name, reason);
}

bool Check(bool ok, const char* name, const char* format, ...) {
    if (ok) {
        Pass(name);
        return true;
    }
    char detail[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    ++g_failed;
    PrintLine("FAIL", name, detail);
    return false;
}

const std::wstring& WorkDir() { return g_work; }
const std::wstring& RepoDir() { return g_repo; }
const std::wstring& FixtureDir() { return g_fixtures; }

std::wstring NewDir(const std::wstring& name) {
    static int serial = 0;
    const std::wstring dir = g_work + L"\\" + name + L"-" + std::to_wstring(++serial);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

std::string ReadBytes(const std::wstring& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size = {};
    GetFileSizeEx(file, &size);
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!bytes.empty()) ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    bytes.resize(read);
    return bytes;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---- signal checks ----

double Rms(const Signal& s, size_t from, size_t to) {
    to = std::min(to, s.Frames());
    if (from >= to) return 0;
    double sum = 0;
    for (size_t i = from; i < to; ++i) {
        const double v = s.samples[i * s.channels];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

double Frequency(const Signal& s, size_t from, size_t to) {
    to = std::min(to, s.Frames());
    // Upward zero crossings, interpolated to a fraction of a sample; frequency from the first to
    // the last one, so the window edges do not matter.
    double first = -1;
    double last = -1;
    int count = 0;
    for (size_t i = from + 1; i < to; ++i) {
        const float a = s.samples[(i - 1) * s.channels];
        const float b = s.samples[i * s.channels];
        if (a < 0 && b >= 0) {
            const double at = static_cast<double>(i - 1) + static_cast<double>(-a) / static_cast<double>(b - a);
            if (first < 0) first = at;
            last = at;
            ++count;
        }
    }
    if (count < 2 || last <= first) return 0;
    return static_cast<double>(count - 1) * s.rate / (last - first);
}

double MaxStep(const Signal& s, size_t from, size_t to) {
    to = std::min(to, s.Frames());
    double worst = 0;
    for (size_t i = from + 1; i < to; ++i) {
        for (uint32_t c = 0; c < s.channels; ++c) {
            const double d = std::fabs(static_cast<double>(s.samples[i * s.channels + c]) - s.samples[(i - 1) * s.channels + c]);
            worst = std::max(worst, d);
        }
    }
    return worst;
}

size_t LongestZeroRun(const Signal& s, size_t from, size_t to) {
    to = std::min(to, s.Frames());
    size_t longest = 0;
    size_t run = 0;
    for (size_t i = from; i < to; ++i) {
        if (std::fabs(s.samples[i * s.channels]) < 1e-7f) longest = std::max(longest, ++run);
        else run = 0;
    }
    return longest;
}

void CheckSine(const std::string& name, const Signal& s, double frequency) {
    const size_t from = s.Frames() * 15 / 100;
    const size_t to = s.Frames() * 85 / 100;
    const double rms = Rms(s, from, to);
    const double f = Frequency(s, from, to);
    Check(std::fabs(rms - kExpectedRms) <= kExpectedRms * 0.05, (name + ": level is right (RMS within 5 %)").c_str(),
          "RMS %.4f, expected %.4f over %zu frames", rms, kExpectedRms, to - from);
    Check(std::fabs(f - frequency) <= frequency * 0.01, (name + ": pitch is right (within 1 %)").c_str(), "measured %.2f Hz, expected %.2f Hz",
          f, frequency);
}

namespace {

char Label(const Signal& s, size_t from, size_t to, const std::vector<std::pair<char, double>>& known) {
    if (Rms(s, from, to) < 0.02) return '.';
    const double f = Frequency(s, from, to);
    for (const auto& k : known) {
        if (std::fabs(f - k.second) <= k.second * 0.04) return k.first;
    }
    return '?';
}

}  // namespace

std::string Sequence(const Signal& s, const std::vector<std::pair<char, double>>& known) {
    std::string out;
    const size_t window = s.rate / 20;
    for (size_t from = 0; from + window <= s.Frames(); from += window) {
        const char label = Label(s, from, from + window, known);
        if (label == '?') continue;  // a window that straddles a change
        if (out.empty() || out.back() != label) out += label;
    }
    return out;
}

double FirstWindow(const Signal& s, const std::vector<std::pair<char, double>>& known, char letter) {
    const size_t window = s.rate / 20;
    for (size_t from = 0; from + window <= s.Frames(); from += window) {
        if (Label(s, from, from + window, known) == letter) return static_cast<double>(from) / s.rate;
    }
    return -1;
}

}  // namespace mt

namespace {

// musictest.exe --fixtures <folder>: the songs for the end-to-end panel run in the test host
// (tools\run-host-test.ps1), made with the same writers as the engine's own fixtures. Four 20 s
// songs in <folder>\music (FLAC and MP3 through Media Foundation's encoders, Opus and OGG Vorbis
// with title and artist tags), one WAV in <folder>\add for "Add files". A PC without a Media
// Foundation encoder gets a WAV under the same name instead, and the line says so.
int MakePanelFixtures(const std::wstring& folder) {
    SetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_HOST", L"1");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        printf("fixtures: COM or Media Foundation could not be started\n");
        return 1;
    }
    const std::wstring music = folder + L"\\music";
    const std::wstring add = folder + L"\\add";
    CreateDirectoryW(folder.c_str(), nullptr);
    CreateDirectoryW(music.c_str(), nullptr);
    CreateDirectoryW(add.c_str(), nullptr);
    constexpr double kSeconds = 20.0;
    bool ok = true;
    auto encoded = [&](const char* label, const std::wstring& base, const wchar_t* format, const wchar_t* extension,
                       double frequency) {
        const long hr = mt::EncodeWithMediaFoundation(base + extension, format, 44100, kSeconds, frequency);
        if (SUCCEEDED(hr)) {
            printf("fixture %s: written\n", label);
            return;
        }
        const bool wav = mt::WriteWav(base + L".wav", 44100, 2, static_cast<uint32_t>(kSeconds * 44100), frequency);
        printf("fixture %s: this PC has no Media Foundation encoder for it (0x%08lx); %s\n", label,
               static_cast<unsigned long>(hr), wav ? "a WAV was written instead" : "the WAV fallback FAILED");
        ok = ok && wav;
    };
    auto written = [&](const char* label, bool done) {
        printf("fixture %s: %s\n", label, done ? "written" : "FAILED");
        ok = ok && done;
    };
    encoded("01 FLAC", music + L"\\01 Alpha 測試", L"flac", L".flac", 440.0);
    encoded("02 MP3", music + L"\\02 Beta 測試", L"mp3", L".mp3", 660.0);
    written("03 Opus", mt::WriteOpus(music + L"\\03 gamma.opus", 880.0, kSeconds, "Gamma 測試曲", "測試樂團"));
    written("04 OGG Vorbis", mt::WriteVorbis(music + L"\\04 delta.ogg", 44100, 500.0, kSeconds, "Delta 測試曲", "Test artist"));
    written("05 WAV for Add files",
            mt::WriteWav(add + L"\\05 Epsilon 測試.wav", 48000, 2, static_cast<uint32_t>(kSeconds * 48000), 770.0));
    printf("fixtures: %s\n", ok ? "done" : "FAILED");
    return ok ? 0 : 1;
}

}  // namespace

int SaveLoopChild(const wchar_t* path, const wchar_t* eventName);   // musictest_text.cpp

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && lstrcmpW(argv[1], L"--fixtures") == 0) {
        return MakePanelFixtures(argv[2]);
    }
    if (argc == 4 && lstrcmpW(argv[1], L"--save-loop") == 0) {
        return SaveLoopChild(argv[2], argv[3]);   // musictest_text.cpp, case 115
    }
    if (argc < 3) {
        printf("usage: musictest.exe <empty work folder> <repository folder>\n"
               "       musictest.exe --fixtures <folder>   (songs for the end-to-end panel run)\n");
        return 2;
    }
    // Belt and braces: under this flag the engine refuses WASAPI and the picker never shows.
    SetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_HOST", L"1");
    mt::g_work = argv[1];
    mt::g_repo = argv[2];
    mt::g_fixtures = mt::g_work + L"\\fixtures 測試音樂 简体 #1 100%";
    CreateDirectoryW(mt::g_work.c_str(), nullptr);
    CreateDirectoryW(mt::g_fixtures.c_str(), nullptr);

    // The engine logs through the mod's log, at %LOCALAPPDATA%\RacingMasterMod\mod.log; the test
    // script points LOCALAPPDATA at the work folder.
    LogOpen();
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        printf("FAIL setup -- COM or Media Foundation could not be started\n");
        return 1;
    }

    std::vector<mt::Case> cases = mt::Cases();
    std::stable_sort(cases.begin(), cases.end(), [](const mt::Case& a, const mt::Case& b) { return a.order < b.order; });
    for (const mt::Case& c : cases) {
        printf("--- %s\n", c.title);
        fflush(stdout);
        const ULONGLONG started = GetTickCount64();
        c.fn();
        printf("    (%llu ms)\n", static_cast<unsigned long long>(GetTickCount64() - started));
    }
    printf("\nmusictest: %d passed, %d failed, %d skipped\n", mt::g_passed, mt::g_failed, mt::g_skipped);
    fflush(stdout);
    // No MFShutdown/CoUninitialize: the engine's own threads have finished, and the process ends.
    return mt::g_failed == 0 ? 0 : 1;
}

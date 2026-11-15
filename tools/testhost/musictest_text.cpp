// Text helpers (src\music\text.*) and crash-safe saves (src\music\atomic_file.*).
#include "musictest.h"

#include "music/atomic_file.h"
#include "music/text.h"

#include <windows.h>

#include <cstdio>

using namespace music;

MT_TEST(100, TextHelpers, "text helpers: UTF-8, paths, natural order") {
    const std::wstring chinese = L"\u6E2C\u8A66\u6B4C\u66F2 \u7B80\u4F53 #1.mp3";
    bool ok = false;
    const std::string utf8 = ToUtf8(chinese, &ok);
    mt::Check(ok && FromUtf8(utf8) == chinese, "UTF-8: Chinese text survives the round trip");
    const wchar_t lone[] = {L'a', static_cast<wchar_t>(0xD800), L'b', 0};
    ToUtf8(lone, &ok);
    mt::Check(!ok, "UTF-8: a name with an unpaired surrogate is refused, not saved with a loss");
    mt::Check(FromUtf8("\xC3\x28").empty(), "UTF-8: invalid bytes are refused");

    const std::string utf16le = std::string("\xFF\xFE[\0m\0u\0s\0i\0c\0]\0", 16);
    mt::Check(TextFileToUtf8(utf16le) == "[music]", "settings text: UTF-16 little endian is read");
    const std::string utf16be = std::string("\xFE\xFF\0[\0m\0o\0d\0]", 12);
    mt::Check(TextFileToUtf8(utf16be) == "[mod]", "settings text: UTF-16 big endian is read");
    mt::Check(TextFileToUtf8("\xEF\xBB\xBFstage=1") == "stage=1", "settings text: a UTF-8 byte order mark is dropped");

    mt::Check(NormalisePath(L"C:/Music/./a/../b.mp3") == L"C:\\Music\\b.mp3", "paths: slashes, . and .. are resolved");
    mt::Check(NormalisePath(L"\\\\?\\C:\\Music\\b.mp3") == L"C:\\Music\\b.mp3", "paths: a stored path never carries \\\\?\\");
    mt::Check(ToExtendedPath(L"C:/Music/b.mp3") == L"\\\\?\\C:\\Music\\b.mp3", "paths: \\\\?\\ is added after normalising");
    mt::Check(ToExtendedPath(L"\\\\server\\share\\b.mp3") == L"\\\\?\\UNC\\server\\share\\b.mp3",
              "paths: a network path gets \\\\?\\UNC\\");
    mt::Check(SamePath(L"C:\\Music\\\u6E2C\u8A66.MP3", L"c:\\music\\\u6E2C\u8A66.mp3") &&
                  PathKey(L"C:\\Music\\\u6E2C\u8A66.MP3") == PathKey(L"c:\\music\\\u6E2C\u8A66.mp3"),
              "paths: the duplicate check ignores case, also with Chinese names");
    mt::Check(NaturalLess(L"2 - b.mp3", L"10 - a.mp3") && !NaturalLess(L"10 - a.mp3", L"2 - b.mp3"),
              "natural order: 2 comes before 10");
    mt::Check(FileName(L"C:\\a\\b.c\\name.flac", true) == L"name" && Extension(L"C:\\a\\X.FLAC") == L".flac",
              "file name and extension");
    mt::Check(SingleLine(L"a\tb\r\nc") == L"a b  c", "titles: tabs and line breaks become spaces");
}

MT_TEST(110, AtomicFile, "crash-safe save: temporary file, swap, backup") {
    const std::wstring dir = mt::NewDir(L"atomic \u6E2C\u8A66");
    const std::wstring path = dir + L"\\playlist.tsv";
    unsigned long error = 0;
    mt::Check(SaveFileAtomically(path, "first", &error), "first save creates the file", "Windows error %lu", error);
    mt::Check(SaveFileAtomically(path, "second", &error), "second save replaces it", "Windows error %lu", error);
    std::string text;
    mt::Check(ReadWholeFile(path, &text, 1024) && text == "second", "the file holds the newest content");
    mt::Check(ReadWholeFile(path + L".bak", &text, 1024) && text == "first", "the previous version is kept as .bak");
    mt::Check(!ReadWholeFile(path, &text, 3), "a file over the size limit is not read");

    mt::WriteBytes(dir + L"\\playlist.tsv.1234.tmp", "left over from a crash");
    DeleteLeftoverTempFiles(dir, L"playlist.tsv");
    mt::Check(!mt::Exists(dir + L"\\playlist.tsv.1234.tmp") && mt::Exists(path), "leftover .tmp files are deleted, the list is kept");

    // A path beyond 260 characters, with Chinese in it, works through the \\?\ prefix.
    std::wstring deep = dir;
    while (deep.size() < 300) {
        deep += L"\\\u9577\u8DEF\u5F91 folder name";
        CreateDirectoryW((L"\\\\?\\" + deep).c_str(), nullptr);
    }
    mt::Check(SaveFileAtomically(deep + L"\\settings.ini", "x", &error) && ReadWholeFile(deep + L"\\settings.ini", &text, 16),
              "a save under a path longer than 260 characters works", "Windows error %lu", error);
    mt::Check(SaveFileReplacing(deep + L"\\settings.ini", "y", &error) && ReadWholeFile(deep + L"\\settings.ini", &text, 16) &&
                  text == "y",
              "the single-rename save (settings.ini) works under a long path too", "Windows error %lu", error);
}

// The game can end its process at any moment, also in the middle of a save of settings.ini (0.3.1
// saves it every 10 s while a song plays, and at the very moment the X is clicked). Nothing reads a
// backup of settings.ini, so the file must never be missing, whenever the process ends. A child
// copy of this program saves settings.ini in a loop (musictest.exe --save-loop) and is ended with
// TerminateProcess at a random moment, 100 times. ReplaceFileW (rename the old file to .bak, then
// the new one into place) left the file missing after about 5-8 % of such kills.
MT_TEST(115, AtomicFileKilled, "crash-safe save: settings.ini is there after the process is ended mid-save") {
    const std::wstring dir = mt::NewDir(L"killed mid-save \u6E2C\u8A66");
    const std::wstring path = dir + L"\\settings.ini";
    unsigned long error = 0;
    mt::Check(SaveFileReplacing(path, "[music]\r\nposition_ms=0\r\n", &error), "the first settings.ini is written",
              "Windows error %lu", error);
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    int kills = 0, missing = 0, notStarted = 0;
    unsigned seed = GetTickCount();
    for (int i = 0; i < 100; ++i) {
        wchar_t eventName[96];
        swprintf_s(eventName, L"Local\\rmm-musictest-save-loop-%lu-%d", GetCurrentProcessId(), i);
        const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --save-loop \"" + path + L"\" " + eventName;
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child = {};
        if (!ready || !CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                      &startup, &child)) {
            ++notStarted;
            if (ready) CloseHandle(ready);
            continue;
        }
        // After its first complete save, a random moment within the next 12 ms.
        if (WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0) ++notStarted;
        seed = seed * 1103515245u + 12345u;
        const ULONGLONG spinUntil = GetTickCount64();
        LARGE_INTEGER frequency = {}, start = {}, now = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        const LONGLONG wait = frequency.QuadPart * static_cast<LONGLONG>((seed >> 8) % 12000) / 1000000;
        do {
            QueryPerformanceCounter(&now);
        } while (now.QuadPart - start.QuadPart < wait && GetTickCount64() - spinUntil < 100);
        TerminateProcess(child.hProcess, 1);
        WaitForSingleObject(child.hProcess, 10000);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        CloseHandle(ready);
        ++kills;
        if (!mt::Exists(path)) {
            ++missing;
            SaveFileReplacing(path, "[music]\r\nposition_ms=0\r\n", &error);   // put it back for the next round
        }
    }
    mt::Check(notStarted == 0 && kills == 100, "the saving copy of the program was started and ended 100 times",
              "%d ended, %d not started", kills, notStarted);
    mt::Check(missing == 0, "settings.ini was there after every one of the 100 kills mid-save",
              "missing after %d of %d kills", missing, kills);
    DeleteLeftoverTempFiles(dir, L"settings.ini");
}

// musictest.exe --save-loop <path> <event name>: saves `path` over and over, the way the engine saves
// settings.ini, and sets the event after the first save. Ends only when the test ends it.
int SaveLoopChild(const wchar_t* path, const wchar_t* eventName) {
    const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    unsigned long error = 0;
    for (unsigned n = 1;; ++n) {
        const std::string content = "[mod]\r\nstage=2\r\n\r\n[music]\r\ncurrent=C:\\\\music\\\\a.mp3\r\nposition_ms=" +
                                    std::to_string(n * 10) + "\r\n";
        if (!SaveFileReplacing(path, content, &error)) {
            Sleep(1);
        } else if (n == 1 && ready) {
            SetEvent(ready);
        }
    }
}

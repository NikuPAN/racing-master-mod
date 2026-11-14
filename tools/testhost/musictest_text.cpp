// Text helpers (src\music\text.*) and crash-safe saves (src\music\atomic_file.*).
#include "musictest.h"

#include "music/atomic_file.h"
#include "music/text.h"

#include <windows.h>

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
}

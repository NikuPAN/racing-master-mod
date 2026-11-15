#include "atomic_file.h"

#include "text.h"

#include <windows.h>

namespace music {

namespace {

// Writes `content` to <path>.<pid>.tmp and flushes it to disk. On failure the temporary file is gone.
bool WriteTemporary(const std::wstring& tempX, const std::string& content, unsigned long* error) {
    const HANDLE file = CreateFileW(tempX.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (ok && done < content.size()) {
        const DWORD chunk = static_cast<DWORD>(content.size() - done > 1u << 20 ? 1u << 20 : content.size() - done);
        DWORD written = 0;
        ok = WriteFile(file, content.data() + done, chunk, &written, nullptr) && written == chunk;
        done += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!ok) *error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(tempX.c_str());
        return false;
    }
    return true;
}

std::wstring TemporaryName(const std::wstring& path) {
    return path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
}

}  // namespace

bool SaveFileAtomically(const std::wstring& path, const std::string& content, unsigned long* error) {
    *error = 0;
    const std::wstring tempX = ToExtendedPath(TemporaryName(path));
    const std::wstring pathX = ToExtendedPath(path);
    const std::wstring backupX = ToExtendedPath(path + L".bak");
    if (!WriteTemporary(tempX, content, error)) {
        return false;
    }

    if (ReplaceFileW(pathX.c_str(), tempX.c_str(), backupX.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        return true;
    }
    const DWORD replaceError = GetLastError();
    // ERROR_FILE_NOT_FOUND: first save, there is nothing to replace yet.
    // ERROR_UNABLE_TO_MOVE_REPLACEMENT_2: Windows already moved the old file to the backup name but
    // could not move the new one into place; the main name is free, so finish the move by hand.
    if (replaceError == ERROR_FILE_NOT_FOUND || replaceError == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        if (MoveFileExW(tempX.c_str(), pathX.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        *error = GetLastError();
    } else {
        *error = replaceError;
    }
    DeleteFileW(tempX.c_str());
    return false;
}

bool SaveFileReplacing(const std::wstring& path, const std::string& content, unsigned long* error) {
    *error = 0;
    const std::wstring tempX = ToExtendedPath(TemporaryName(path));
    const std::wstring pathX = ToExtendedPath(path);
    if (!WriteTemporary(tempX, content, error)) {
        return false;
    }
    // One rename: at every moment the name holds either the old file or the new one.
    if (MoveFileExW(tempX.c_str(), pathX.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    *error = GetLastError();
    DeleteFileW(tempX.c_str());
    return false;
}

bool ReadWholeFile(const std::wstring& path, std::string* out, size_t maxBytes) {
    out->clear();
    const std::wstring pathX = ToExtendedPath(path);
    const HANDLE file = CreateFileW(pathX.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
        CloseHandle(file);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    bool ok = true;
    while (ok && done < out->size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(out->size() - done > 1u << 20 ? 1u << 20 : out->size() - done);
        ok = ReadFile(file, out->data() + done, chunk, &read, nullptr) && read != 0;
        done += read;
    }
    CloseHandle(file);
    if (!ok) out->clear();
    return ok;
}

void DeleteLeftoverTempFiles(const std::wstring& folder, const std::wstring& prefix) {
    const std::wstring pattern = ToExtendedPath(folder + L"\\" + prefix + L"*.tmp");
    WIN32_FIND_DATAW found = {};
    const HANDLE search = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::wstring full = ToExtendedPath(folder + L"\\" + found.cFileName);
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

}  // namespace music

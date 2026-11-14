#include "text.h"

#include <windows.h>

namespace music {

std::string ToUtf8(std::wstring_view text, bool* ok) {
    if (ok) *ok = true;
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        if (ok) *ok = false;
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(std::string_view text) {
    if (text.empty()) return {};
    const int length = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, out.data(), needed);
    return out;
}

std::string TextFileToUtf8(const std::string& bytes) {
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t n = bytes.size();
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return bytes.substr(3);
    const bool little = n >= 2 && b[0] == 0xFF && b[1] == 0xFE;
    const bool big = n >= 2 && b[0] == 0xFE && b[1] == 0xFF;
    if (!little && !big) return bytes;
    std::wstring wide((n - 2) / 2, L'\0');
    for (size_t i = 0; i < wide.size(); ++i) {
        const unsigned char first = b[2 + i * 2];
        const unsigned char second = b[3 + i * 2];
        wide[i] = little ? static_cast<wchar_t>(first | (second << 8)) : static_cast<wchar_t>((first << 8) | second);
    }
    if (wide.empty()) return {};
    // Plain conversion (unpaired surrogates become U+FFFD): this is somebody's settings file, and
    // losing one odd character is better than losing the file.
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring NormalisePath(const std::wstring& path) {
    if (path.empty()) return {};
    std::wstring in = path;
    if (in.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        in = L"\\\\" + in.substr(8);
    } else if (in.compare(0, 4, L"\\\\?\\") == 0) {
        in = in.substr(4);
    }
    // GetFullPathNameW only rewrites text (slashes, ".", ".."); it does not touch the disk. The
    // wide version is not limited to 260 characters.
    const DWORD needed = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (needed == 0) return in;
    std::wstring out(needed, L'\0');
    const DWORD written = GetFullPathNameW(in.c_str(), needed, out.data(), nullptr);
    if (written == 0 || written >= needed) return in;
    out.resize(written);
    // A trailing backslash only belongs to a drive root ("C:\").
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.compare(0, 4, L"\\\\?\\") == 0) return path;
    const std::wstring full = NormalisePath(path);
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') return L"\\\\?\\UNC\\" + full.substr(2);
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\') return L"\\\\?\\" + full;
    return full;  // unusual (a device path): handed over unchanged
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

std::wstring PathKey(const std::wstring& path) {
    if (path.empty()) return {};
    std::wstring key(path.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.c_str(), static_cast<int>(path.size()), key.data(),
                                static_cast<int>(key.size()), nullptr, nullptr, 0);
    if (n <= 0) return path;
    key.resize(static_cast<size_t>(n));
    return key;
}

bool NaturalLess(const std::wstring& a, const std::wstring& b) {
    const int result = CompareStringEx(LOCALE_NAME_USER_DEFAULT, SORT_DIGITSASNUMBERS | NORM_IGNORECASE, a.c_str(),
                                       static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr,
                                       nullptr, 0);
    if (result == 0) {  // the comparison itself failed (invalid text): fall back to ordinal order
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
               CSTR_LESS_THAN;
    }
    return result == CSTR_LESS_THAN;
}

std::wstring FileName(const std::wstring& path, bool withoutExtension) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (withoutExtension) {
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name.erase(dot);
    }
    return name;
}

std::wstring Extension(const std::wstring& path) {
    const std::wstring name = FileName(path, false);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    std::wstring extension = name.substr(dot);
    for (wchar_t& c : extension) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return extension;
}

std::wstring SingleLine(std::wstring text) {
    for (wchar_t& c : text) {
        if (c < 0x20 || c == 0x7F) c = L' ';
    }
    return text;
}

}  // namespace music

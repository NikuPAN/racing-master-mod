#include "fonts.h"

#include "ui_common.h"

#include "../core/log.h"

#include <windows.h>

#include <cstdio>

namespace {

struct FontSource {
    const wchar_t* file;      // in %WINDIR%\Fonts
    unsigned faceIndex;       // index inside a .ttc collection
    const char* covers;       // for the log
};

// The first file that loads is the base font; the rest fill in glyphs it lacks, in this order.
// Every file is in Microsoft's base Windows 11 font list. YuGothM.ttc face 1 is the regular
// weight of Yu Gothic UI (YuGothR.ttc face 1 is the semilight weight).
const FontSource kChain[] = {
    {L"msjh.ttc", 1, "Traditional Chinese and Latin (Microsoft JhengHei UI)"},
    {L"msyh.ttc", 1, "Simplified Chinese (Microsoft YaHei UI)"},
    {L"YuGothM.ttc", 1, "Japanese (Yu Gothic UI)"},
    {L"malgun.ttf", 0, "Korean (Malgun Gothic)"},
    {L"mingliub.ttc", 2, "Hong Kong characters and CJK Extension B (MingLiU_HKSCS-ExtB)"},
    {L"seguisym.ttf", 0, "symbols (Segoe UI Symbol)"},
    {L"segoeui.ttf", 0, "Latin (Segoe UI)"},
};
constexpr int kChainCount = static_cast<int>(sizeof(kChain) / sizeof(kChain[0]));

struct MappedFont {
    const void* data = nullptr;
    int size = 0;
    bool tried = false;
};
MappedFont g_mapped[kChainCount];   // views stay mapped for the life of the process

void MapFont(int index) {
    MappedFont& font = g_mapped[index];
    if (font.tried) {
        return;
    }
    font.tried = true;
    wchar_t path[MAX_PATH];
    const UINT len = GetWindowsDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 40) {
        return;
    }
    lstrcatW(path, L"\\Fonts\\");
    lstrcatW(path, kChain[index].file);
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        char path8[MAX_PATH * 3];
        Log("panel: font file %s is not installed (error %lu); text that needs it falls back to the next font",
            LogUtf8(path, path8, sizeof(path8)), GetLastError());
        return;
    }
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 0x7FFFFFFF) {
        const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping) {
            font.data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            if (font.data) {
                font.size = static_cast<int>(size.QuadPart);
            }
            CloseHandle(mapping);   // the view keeps the mapping alive
        }
    }
    if (!font.data) {
        char path8[MAX_PATH * 3];
        Log("panel: font file %s could not be mapped into memory (error %lu); skipped",
            LogUtf8(path, path8, sizeof(path8)), GetLastError());
    }
    CloseHandle(file);
}

bool g_loggedChain = false;

}  // namespace

int LoadFontChain() {
    ImGuiIO& io = ImGui::GetIO();
    int loaded = 0;
    for (int i = 0; i < kChainCount; ++i) {
        MapFont(i);
        if (!g_mapped[i].data) {
            continue;
        }
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;   // the mapped view outlives the atlas
        config.FontNo = static_cast<int>(kChain[i].faceIndex);
        config.MergeMode = loaded > 0;
        snprintf(config.Name, sizeof(config.Name), "font %d", i);
        // Size 0: the font is sized by the style (FontSizeBase and the scale factors), see scale.cpp.
        if (io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(g_mapped[i].data), g_mapped[i].size, 0.0f, &config)) {
            ++loaded;
        } else if (!g_loggedChain) {
            Log("panel: Dear ImGui refused the font for %s; skipped", kChain[i].covers);
        }
    }
    if (loaded == 0) {
        io.Fonts->AddFontDefaultVector();
        if (!g_loggedChain) {
            Log("panel: none of the Windows fonts could be used; the panel shows Latin text only");
        }
    } else if (!g_loggedChain) {
        Log("panel: %d of %d Windows fonts in use", loaded, kChainCount);
    }
    g_loggedChain = true;
    return loaded;
}

void FontAtlasValve() {
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    bool tooLarge = false;
    for (ImTextureData* texture : platform.Textures) {
        if (texture && (texture->Width > 4096 || texture->Height > 4096)) {
            tooLarge = true;
        }
    }
    if (!tooLarge) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->ClearFonts();
    LoadFontChain();
    io.Fonts->CompactCache();   // ClearFonts plus a reload alone leave the big texture in place
    Log("panel: the glyph texture had grown past 4096 pixels and was rebuilt small");
}

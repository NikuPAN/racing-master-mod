// The music panel's widgets: a compact bar in a corner, the song list on demand, a settings row
// on demand, and short notices. Frame thread only (between ImGui::NewFrame and ImGui::Render),
// apart from PanelConfigure on the worker before the hook exists.
//
//   Row 1: source ("My files" / "Other app"), what is playing, Settings, Hide
//   Row 2: previous, play or pause, stop, next, what repeats, volume
//   Row 3: elapsed, seek line, length, Show list
//   beside the bar, in a window of its own (away from the screen edge): the status lines, the
//          song list (Add files, Add folder, one row per song with its length and a remove button)
//          and the settings row (language, text size, place, pause when not in focus, keys)
//
// Sizes are written in units (pixels at 1080p with medium text) times Units(); no pixel constant.
// Every control that cannot be used is greyed out and says why when the pointer rests on it.

#include "panel.h"

#include "input_queue.h"
#include "media_api.h"
#include "other_app.h"
#include "scale.h"
#include "ui_common.h"
#include "ui_settings.h"

#include "../core/log.h"

#include "imgui_internal.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static_assert(static_cast<int>(remote::Message::TestGuard) + 1 == kRemoteMessageCount,
              "strings.h: the remote message table must list every remote::Message in order");

namespace {

enum Source : unsigned { SourceMyFiles = 0, SourceOtherApp = 1 };

// Configured by the worker before the hook; frame thread only afterwards.
UiSettings g_settings;
HotkeyTable g_keys;
bool g_testHost = false;
bool g_testTextField = false;
Language g_language = Language::English;

bool g_listOpen = false;
bool g_settingsOpen = false;
bool g_seekActive = false;
float g_seekValue = 0.0f;
char g_toast[256] = "";
ULONGLONG g_toastUntil = 0;
ULONGLONG g_noticeUntil = 0;
// The engine's status line ("Added 1984 file(s) ...", a skipped song) is a notice, not a fixture:
// it shows for kEngineMessageMs from the first frame the panel draws it, then goes (the owner,
// 2026-11-15: "this text doesn't need to be persistent"). Keyed on the engine's messageSerial, so
// the same text set again shows again.
constexpr ULONGLONG kEngineMessageMs = 6000;
uint32_t g_engineMessageSerial = 0;
ULONGLONG g_engineMessageUntil = 0;
int g_clearConfirmCount = -1;   // Clear list asked "take all N songs off?" for this N; -1 = not asking
char g_testText[64] = "";
char g_musicFolder[MAX_PATH * 3] = "";   // the engine's music folder, UTF-8 (set on the first frame)

PanelTestItem g_items[64];
int g_itemCount = 0;

// ---- small helpers -------------------------------------------------------------------------------

Language ResolveLanguage(unsigned setting) {
    if (setting == 1) return Language::English;
    if (setting == 2) return Language::ChineseTraditional;
    const LANGID id = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(id) == LANG_CHINESE) {
        const WORD sub = SUBLANGID(id);
        if (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU) {
            return Language::ChineseTraditional;
        }
    }
    return Language::English;
}

const Strings& S() {
    return StringsFor(g_language);
}

float U() {
    return Units();
}

void Mark(const char* name) {
    if (!g_testHost || g_itemCount >= static_cast<int>(sizeof(g_items) / sizeof(g_items[0]))) {
        return;
    }
    PanelTestItem& item = g_items[g_itemCount++];
    strcpy_s(item.name, name);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    item.x = (min.x + max.x) * 0.5f;
    item.y = (min.y + max.y) * 0.5f;
    item.text = 0.0f;
    item.box = 0.0f;
}

// Mark for a text drawn in a fixed-width box (TextFit): also the width the text needs and the
// box's, so the test host can tell whether it was cut off.
void MarkFit(const char* name, const char* text, float box) {
    const int before = g_itemCount;
    Mark(name);
    if (g_itemCount > before) {
        g_items[before].text = ImGui::CalcTextSize(text).x;
        g_items[before].box = box;
    }
}

void MarkIndexed(const char* prefix, int index) {
    char name[24];
    snprintf(name, sizeof(name), "%s%d", prefix, index);
    Mark(name);
}

// Hover text for the item just drawn, also when it is greyed out. `second` is an optional
// second paragraph (the reason a control cannot be used).
void Tip(const char* first, const char* second = nullptr) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    if (first && first[0]) {
        ImGui::TextUnformatted(first);
    }
    if (second && second[0]) {
        ImGui::TextUnformatted(second);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

const char* Utf8(const std::wstring& text, char* buffer, int size) {
    buffer[0] = '\0';
    if (!text.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), buffer, size - 1,
                                          nullptr, nullptr);
        buffer[n > 0 ? n : 0] = '\0';
    }
    return buffer;
}

// "Title - Artist", or the file name without its folder and extension when there is no title.
void TrackLabel(const music::TrackInfo& track, char* buffer, int size) {
    char title[512], artist[256];
    if (!track.title.empty()) {
        Utf8(track.title, title, sizeof(title));
    } else {
        std::wstring name = track.path;
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
        Utf8(name, title, sizeof(title));
    }
    if (!title[0]) {
        strcpy_s(title, S().unknownTitle);
    }
    Utf8(track.artist, artist, sizeof(artist));
    if (artist[0]) {
        snprintf(buffer, static_cast<size_t>(size), "%s - %s", title, artist);
    } else {
        snprintf(buffer, static_cast<size_t>(size), "%s", title);
    }
}

void FormatTime(double seconds, char* buffer, int size) {
    if (seconds < 0.0 || seconds != seconds) {
        snprintf(buffer, static_cast<size_t>(size), "--:--");
        return;
    }
    const long long total = static_cast<long long>(seconds);
    if (total >= 3600) {
        snprintf(buffer, static_cast<size_t>(size), "%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        snprintf(buffer, static_cast<size_t>(size), "%lld:%02lld", total / 60, total % 60);
    }
}

// Text that ends in "..." when it does not fit `width`.
// Like ImGui::Text after AlignTextToFramePadding, it sits on the same baseline as the buttons
// beside it: the text is drawn at the line's text offset, not at the top of the line. It is an
// item (its rectangle is the text's own line box), so a Mark after it gives its centre.
void TextFit(const char* text, float width) {
    const float offset = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;   // set by AlignTextToFramePadding
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.y += offset;
    const float height = ImGui::GetTextLineHeight();
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::ItemSize(ImVec2(width, height), offset);
    const ImRect box(pos, ImVec2(pos.x + width, pos.y + height));
    ImGui::ItemAdd(box, 0);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), box.Min, box.Max, box.Max.x, text, nullptr, &size);
}

// The widest a time can be drawn in the shape of `text` ("1:12:30" -> any of "0:00:00" ...
// "9:99:99"): the font's digits need not all be the same width.
float ShapeWidth(const char* text) {
    float widest = 0.0f;
    char shape[24];
    for (char digit = '0'; digit <= '9'; ++digit) {
        size_t i = 0;
        for (; text[i] && i + 1 < sizeof(shape); ++i) {
            shape[i] = text[i] >= '0' && text[i] <= '9' ? digit : text[i];
        }
        shape[i] = '\0';
        widest = ImMax(widest, ImGui::CalcTextSize(shape).x);
    }
    return widest;
}

// The width both times of a row get: enough for the longest time this song can show (h:mm:ss
// once it is an hour or longer), never less than "00:00", so a time is never cut off and the bar
// beside it does not change size while the song plays. The owner saw "1:1..." for a 1:12:30
// video in 0.3.0, which reserved "00:00" only.
float TimeWidth(const char* elapsed, const char* total) {
    return ImCeil(ImMax(ImMax(ShapeWidth(elapsed), ShapeWidth(total)), ImGui::CalcTextSize("00:00").x)) + 1.0f;
}

// A time in its reserved box, reported to the test host with its fit.
void TimeText(const char* name, const char* text, float width) {
    TextFit(text, width);
    MarkFit(name, text, width);
}

// A progress line that cannot be dragged (another app's timeline): as tall as a button row for
// layout, with the line itself thinner and centred on it, so it sits on the same centre line as
// the times beside it (0.3.0 drew a half-height bar at the top of the row).
void TimeLine(float fraction, float width) {
    const float rowHeight = ImGui::GetFrameHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImRect row(pos, ImVec2(pos.x + width, pos.y + rowHeight));
    ImGui::ItemSize(row, ImGui::GetStyle().FramePadding.y);
    if (!ImGui::ItemAdd(row, 0)) {
        return;
    }
    const float thickness = ImMax(2.0f, ImFloor(rowHeight * 0.32f));
    const float top = ImFloor(row.GetCenter().y - thickness * 0.5f);
    const ImVec2 min(row.Min.x, top);
    const ImVec2 max(row.Max.x, top + thickness);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = thickness * 0.5f;
    draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    if (fraction > 0.0f) {
        draw->AddRectFilled(min, ImVec2(ImMax(min.x + thickness, min.x + (max.x - min.x) * fraction), max.y),
                            ImGui::GetColorU32(ImGuiCol_SliderGrab), rounding);
    }
}

void ShowToast(const char* text, ULONGLONG milliseconds = 2500) {
    strcpy_s(g_toast, text);
    g_toastUntil = GetTickCount64() + milliseconds;
}

// The music engine's state for this moment. One Snapshot is kept across frames and refreshed with
// GetSnapshotInto, which copies the song list only when it changed (the list can hold thousands
// of songs; copying it every frame would cost the game frame time for nothing). Frame thread
// only. Each caller refreshes it once and must not call this again while still using the
// reference from an earlier call (a refresh can replace the list).
const music::Snapshot& CurrentSnapshot() {
    static music::Snapshot* const snapshot = new music::Snapshot();   // created on first use, never freed
    music::Player::Get().GetSnapshotInto(*snapshot);
    return *snapshot;
}

void SaveSettings() {
    UiSettingsRequestSave(g_settings);
}

void FilesChosen(const std::vector<std::wstring>& files) {
    music::Player::Get().AddFiles(files);
}

void FolderChosen(const std::wstring& folder) {
    music::Player::Get().AddFolder(folder);
}

// ---- icons -------------------------------------------------------------------------------------------

// Expand: arrows pointing out to two corners (the compact panel's way back to the full one).
// Compact: arrows pointing in from them (the full panel's way to one row).
enum class Icon { Previous, Play, Pause, Stop, Next, Expand, Compact };

void DrawIcon(Icon icon, ImVec2 min, ImVec2 max, ImU32 colour) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float cx = ImFloor((min.x + max.x) * 0.5f);
    const float cy = ImFloor((min.y + max.y) * 0.5f);
    const float s = ImFloor((max.y - min.y) * 0.28f);
    const float bar = ImMax(2.0f, ImFloor(s * 0.45f));
    switch (icon) {
        case Icon::Play:
            draw->AddTriangleFilled(ImVec2(cx - s * 0.8f, cy - s), ImVec2(cx - s * 0.8f, cy + s), ImVec2(cx + s, cy),
                                    colour);
            break;
        case Icon::Pause:
            draw->AddRectFilled(ImVec2(cx - s * 0.8f, cy - s), ImVec2(cx - s * 0.8f + bar, cy + s), colour);
            draw->AddRectFilled(ImVec2(cx + s * 0.8f - bar, cy - s), ImVec2(cx + s * 0.8f, cy + s), colour);
            break;
        case Icon::Stop:
            draw->AddRectFilled(ImVec2(cx - s * 0.8f, cy - s * 0.8f), ImVec2(cx + s * 0.8f, cy + s * 0.8f), colour);
            break;
        case Icon::Previous:
            draw->AddRectFilled(ImVec2(cx - s, cy - s), ImVec2(cx - s + bar, cy + s), colour);
            draw->AddTriangleFilled(ImVec2(cx + s, cy - s), ImVec2(cx + s, cy + s), ImVec2(cx - s + bar, cy), colour);
            break;
        case Icon::Next:
            draw->AddRectFilled(ImVec2(cx + s - bar, cy - s), ImVec2(cx + s, cy + s), colour);
            draw->AddTriangleFilled(ImVec2(cx - s, cy - s), ImVec2(cx - s, cy + s), ImVec2(cx + s - bar, cy), colour);
            break;
        case Icon::Expand:
        case Icon::Compact: {
            const float t = ImMax(1.5f, s * 0.28f);
            const bool out = icon == Icon::Expand;
            const float g = out ? s : ImFloor(s * 0.2f);   // where the arrow heads sit, from the centre
            const float a = ImFloor(s * (out ? 0.8f : 0.65f));
            if (out) {
                draw->AddLine(ImVec2(cx - s, cy + s), ImVec2(cx + s, cy - s), colour, t);   // corner to corner
            } else {
                draw->AddLine(ImVec2(cx - s, cy + s), ImVec2(cx - g, cy + g), colour, t);
                draw->AddLine(ImVec2(cx + s, cy - s), ImVec2(cx + g, cy - g), colour, t);
            }
            const float d = out ? -a : a;   // heads open towards the centre (out) or away from it (in)
            const ImVec2 upper[3] = {ImVec2(cx + g + (out ? -a : a), cy - g), ImVec2(cx + g, cy - g), ImVec2(cx + g, cy - g + (out ? a : -a))};
            const ImVec2 lower[3] = {ImVec2(cx - g - d, cy + g), ImVec2(cx - g, cy + g), ImVec2(cx - g, cy + g + d)};
            for (const ImVec2* head : {upper, lower}) {
                draw->AddLine(head[0], head[1], colour, t);
                draw->AddLine(head[1], head[2], colour, t);
            }
            break;
        }
    }
}

float IconButtonWidth();

// A square-ish button with a drawn icon. Returns true when clicked.
bool IconButton(const char* id, Icon icon, bool enabled, const char* name, const char* reason) {
    const float h = ImGui::GetFrameHeight();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(id, ImVec2(IconButtonWidth(), h));
    DrawIcon(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::EndDisabled();
    Tip(name, enabled ? nullptr : reason);
    return pressed && enabled;
}

float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float IconButtonWidth() {
    return ImFloor(ImGui::GetFrameHeight() * 1.3f);
}

// The compact panel (one row) or the full one; remembered in settings.ini.
void SetCompact(bool compact) {
    if (g_settings.compact == compact) {
        return;
    }
    g_settings.compact = compact;
    g_seekActive = false;
    SaveSettings();
}

// ---- what each source can do ------------------------------------------------------------------------

const char* LoopLabel(music::LoopMode mode) {
    switch (mode) {
        case music::LoopMode::One: return S().loopOne;
        case music::LoopMode::All: return S().loopAll;
        default: return S().loopOff;
    }
}

const char* RepeatLabel(remote::Repeat repeat) {
    switch (repeat) {
        case remote::Repeat::Off: return S().loopOff;
        case remote::Repeat::One: return S().loopOne;
        case remote::Repeat::All: return S().loopAll;
        default: return S().loopUnknown;
    }
}

music::LoopMode NextLoop(music::LoopMode mode) {
    switch (mode) {
        case music::LoopMode::Off: return music::LoopMode::One;
        case music::LoopMode::One: return music::LoopMode::All;
        default: return music::LoopMode::Off;
    }
}

remote::Repeat NextRepeat(remote::Repeat repeat) {
    switch (repeat) {
        case remote::Repeat::Off: return remote::Repeat::One;
        case remote::Repeat::One: return remote::Repeat::All;
        case remote::Repeat::All: return remote::Repeat::Off;
        default: return remote::Repeat::One;   // unknown: start the cycle
    }
}

// What the panel last asked for, until the engine or the other app shows it. Two presses of F9
// (or of the repeat button) before the new state is published would otherwise both start from
// the old one, and the second press would be lost; the same for F7 with "Other app", which
// decides between play and pause from what the app last reported. A request the app never
// carries out is dropped after 1.5 s, and the panel follows what is shown again. Frame thread only.
struct Pending {
    int target = -1;   // the requested value; -1 = nothing pending
    ULONGLONG until = 0;

    int Effective(int shown) {
        if (target >= 0 && (shown == target || GetTickCount64() >= until)) {
            target = -1;
        }
        return target >= 0 ? target : shown;
    }
    void Set(int value) {
        target = value;
        until = GetTickCount64() + 1500;
    }
};
Pending g_pendingLoop;      // My files: music::LoopMode
Pending g_pendingRepeat;    // Other app: remote::Repeat
Pending g_pendingPlaying;   // Other app: 1 playing, 0 not

music::LoopMode EffectiveLoop(const music::Snapshot& snapshot) {
    return static_cast<music::LoopMode>(g_pendingLoop.Effective(static_cast<int>(snapshot.loop)));
}

remote::Repeat EffectiveRepeat(const remote::Snapshot& snapshot) {
    return static_cast<remote::Repeat>(g_pendingRepeat.Effective(static_cast<int>(snapshot.repeat)));
}

// My files: the next repeat mode after what was last asked for, asked for now.
music::LoopMode StepLoop(const music::Snapshot& snapshot) {
    const music::LoopMode next = NextLoop(EffectiveLoop(snapshot));
    g_pendingLoop.Set(static_cast<int>(next));
    music::Player::Get().SetLoop(next);
    return next;
}

// The calls the panel makes on "Other app", all through other_app (the real remote in the game).
struct OtherAppCalls {
    void Play() { other_app::Play(); }
    void Pause() { other_app::Pause(); }
    void Stop() { other_app::Stop(); }
    void Next() { other_app::Next(); }
    void Previous() { other_app::Previous(); }
    // Play or pause, remembered until the app shows it (see Pending).
    void PlayOrPause(bool pause) {
        g_pendingPlaying.Set(pause ? 0 : 1);
        if (pause) other_app::Pause(); else other_app::Play();
    }
    // The next repeat mode after what was last asked for. Returns it.
    remote::Repeat StepRepeat(const remote::Snapshot& snapshot) {
        const remote::Repeat next = NextRepeat(EffectiveRepeat(snapshot));
        g_pendingRepeat.Set(static_cast<int>(next));
        other_app::SetRepeat(next);
        return next;
    }
};

// The remote's status line in the panel's language, with the app's name put in; empty when the
// remote has nothing to say. The English wording is the remote's own (remote.h).
const char* RemoteMessageLine(const remote::Snapshot& snapshot, char* buffer, int size) {
    buffer[0] = '\0';
    const int id = static_cast<int>(snapshot.messageId);
    if (id <= 0 || id >= kRemoteMessageCount) {
        return buffer;
    }
    const char* text = RemoteMessageText(g_language, id);
    char app[128];
    Utf8(snapshot.messageApp, app, sizeof(app));
    if (!app[0]) {
        strcpy_s(app, S().sourceOtherApp);   // an app with no readable name (remote/app_names.h)
    }
    int used = 0;
    for (const char* p = text; *p && used < size - 1;) {
        if (strncmp(p, "{app}", 5) == 0) {
            for (const char* a = app; *a && used < size - 1; ++a) buffer[used++] = *a;
            p += 5;
        } else {
            buffer[used++] = *p++;
        }
    }
    buffer[used] = '\0';
    return buffer;
}

struct RemoteView {
    bool session;
    bool playing;       // as the app reports
    bool wantPlaying;   // as last asked for (play or pause) until the app shows it, else `playing`
    char reason[256];   // why a control the app refuses is greyed out
};

RemoteView ViewOf(const remote::Snapshot& snapshot) {
    RemoteView view = {};
    view.session = snapshot.available && snapshot.status != remote::Status::NoSession;
    view.playing = snapshot.status == remote::Status::Playing;
    view.wantPlaying = g_pendingPlaying.Effective(view.playing ? 1 : 0) == 1;
    if (!view.session) {
        strcpy_s(view.reason, S().reasonNoApp);
    } else if (snapshot.app.empty()) {
        strcpy_s(view.reason, S().reasonAppNoControlNoName);
    } else {
        char app[128];
        snprintf(view.reason, sizeof(view.reason), S().reasonAppNoControl, Utf8(snapshot.app, app, sizeof(app)));
    }
    return view;
}

// ---- the bar ---------------------------------------------------------------------------------------------

void SourceRow(const char* nowPlaying, float width, bool displayOnly) {
    const Strings& s = S();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    for (unsigned i = 0; i < 2; ++i) {
        if (i == 1) {
            // A segmented pair: a thin gap of window background between the two halves.
            ImGui::SameLine(0.0f, ImMax(2.0f, ImFloor(U() * 3.0f)));
        }
        const bool selected = g_settings.source == i;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        char label[64];
        snprintf(label, sizeof(label), "%s##source%u", i == 0 ? s.sourceMyFiles : s.sourceOtherApp, i);
        ImGui::BeginDisabled(displayOnly);
        if (ImGui::Button(label) && !selected) {
            g_settings.source = i;
            if (i == SourceOtherApp) {
                other_app::Start();   // the player's action; cheap when already started
            }
            Shared().localSourceActive.store(i == SourceMyFiles, std::memory_order_relaxed);
            g_seekActive = false;
            SaveSettings();
        }
        ImGui::EndDisabled();
        if (selected) {
            ImGui::PopStyleColor();
        }
        Tip(i == 0 ? s.sourceMyFilesHint : s.sourceOtherAppHint, displayOnly ? s.reasonDisplayOnly : nullptr);
        Mark(i == 0 ? "source_my_files" : "source_other_app");
    }
    char hideLabel[64];
    snprintf(hideLabel, sizeof(hideLabel), "%s##hide", s.hide);
    char settingsLabel[64];
    snprintf(settingsLabel, sizeof(settingsLabel), "%s##settings", s.settings);
    const float right = ButtonWidth(settingsLabel) + spacing + IconButtonWidth() + spacing + ButtonWidth(hideLabel);
    ImGui::SameLine();
    const float titleWidth = ImMax(width - (ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x) - right - spacing,
                                   U() * 20.0f);
    ImGui::AlignTextToFramePadding();
    TextFit(nowPlaying, titleWidth);
    ImGui::SameLine();
    ImGui::BeginDisabled(displayOnly);
    if (ImGui::Button(settingsLabel)) {
        g_settingsOpen = !g_settingsOpen;
    }
    ImGui::EndDisabled();
    Tip(s.settingsHint, displayOnly ? s.reasonDisplayOnly : nullptr);
    Mark("settings");
    ImGui::SameLine();
    if (IconButton("##compact", Icon::Compact, !displayOnly, s.makeCompact, s.reasonDisplayOnly)) {
        SetCompact(true);
    }
    Mark("compact");
    ImGui::SameLine();
    ImGui::BeginDisabled(displayOnly);
    if (ImGui::Button(hideLabel)) {
        Shared().panelOpen.store(false, std::memory_order_relaxed);
    }
    ImGui::EndDisabled();
    char key[32], hint[160];
    HotkeyDisplayName(g_keys.keys[static_cast<int>(Command::TogglePanel)], s.keyNone, key, sizeof(key));
    snprintf(hint, sizeof(hint), s.hideHint, key);
    Tip(hint, displayOnly ? s.reasonDisplayOnly : nullptr);
    Mark("hide");
}

// The button that shows what repeats and changes it, the same width whatever it says.
bool LoopButton(const char* label, bool enabled, const char* hint, const char* reason) {
    const Strings& s = S();
    float width = 0.0f;
    for (const char* text : {s.loopOff, s.loopOne, s.loopAll, s.loopUnknown}) {
        width = ImMax(width, ButtonWidth(text));
    }
    char id[96];
    snprintf(id, sizeof(id), "%s##loop", label);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(id, ImVec2(width, 0.0f));
    ImGui::EndDisabled();
    Tip(hint, enabled ? nullptr : reason);
    Mark("loop");
    return pressed && enabled;
}

// The two times of "My files" and how wide each is drawn (TimeWidth).
struct MyFilesTimes {
    bool loaded;
    double duration;
    bool canSeek;
    float value;   // the place shown: the engine's, or where a drag on the seek line is
    char elapsed[16];
    char total[16];
    float width;
};

MyFilesTimes TimesOf(const music::Snapshot& snapshot, bool displayOnly) {
    MyFilesTimes t = {};
    t.loaded = snapshot.currentIndex >= 0 && snapshot.currentIndex < static_cast<int>(snapshot.tracks.size());
    t.duration = t.loaded ? snapshot.durationSeconds : 0.0;
    t.canSeek = t.loaded && t.duration > 0.0 && !displayOnly;
    t.value = g_seekActive ? g_seekValue : static_cast<float>(t.loaded ? snapshot.positionSeconds : 0.0);
    FormatTime(t.loaded ? t.value : -1.0, t.elapsed, sizeof(t.elapsed));
    FormatTime(t.loaded && t.duration > 0.0 ? t.duration : -1.0, t.total, sizeof(t.total));
    t.width = TimeWidth(t.elapsed, t.total);
    return t;
}

// Elapsed, the seek line (`lineWidth` wide) and the length, on one line. Full and compact panel.
void SeekLine(const music::Snapshot& snapshot, MyFilesTimes t, float lineWidth, bool displayOnly) {
    const Strings& s = S();
    ImGui::AlignTextToFramePadding();
    TimeText("elapsed", t.elapsed, t.width);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(lineWidth);
    ImGui::BeginDisabled(!t.canSeek);
    const float maximum = t.duration > 0.0 ? static_cast<float>(t.duration) : 1.0f;
    ImGui::SliderFloat("##seek", &t.value, 0.0f, maximum, "", ImGuiSliderFlags_NoInput);
    if (ImGui::IsItemActive()) {
        g_seekActive = true;
        g_seekValue = t.value;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && t.canSeek) {
        music::Player::Get().Seek(static_cast<double>(g_seekValue));
    }
    if (!ImGui::IsItemActive()) {
        g_seekActive = false;
    }
    ImGui::EndDisabled();
    const char* reason = displayOnly ? s.reasonDisplayOnly
                         : snapshot.tracks.empty() ? s.reasonNoSongs
                         : !t.loaded ? s.reasonNothingLoaded
                                     : s.reasonNoLength;
    Tip(s.seekHint, t.canSeek ? nullptr : reason);
    Mark("seek");
    ImGui::SameLine();
    TimeText("total", t.total, t.width);
}

void TimeRowMyFiles(const music::Snapshot& snapshot, float width, bool displayOnly) {
    const Strings& s = S();
    const MyFilesTimes times = TimesOf(snapshot, displayOnly);
    char listLabel[64];
    snprintf(listLabel, sizeof(listLabel), "%s##list", g_listOpen ? s.hideList : s.showList);
    const float listWidth = ImMax(ButtonWidth(s.showList), ButtonWidth(s.hideList));
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    SeekLine(snapshot, times, ImMax(width - times.width * 2.0f - listWidth - spacing * 3.0f, U() * 40.0f), displayOnly);
    ImGui::SameLine();
    ImGui::BeginDisabled(displayOnly);
    if (ImGui::Button(listLabel, ImVec2(listWidth, 0.0f))) {
        g_listOpen = !g_listOpen;
    }
    ImGui::EndDisabled();
    Tip(s.listHint, displayOnly ? s.reasonDisplayOnly : nullptr);
    Mark("list");
}

// Another app's elapsed time, timeline and length on one line ("--:--" and an empty line when it
// reports none). `lineWidth(timeWidth)` gives the line's width once the times' width is known.
template <typename Width>
void ProgressLine(const remote::Snapshot& snapshot, Width lineWidth) {
    const bool timeline = snapshot.durationSeconds > 0.0;
    char elapsed[16], total[16];
    FormatTime(timeline ? snapshot.positionSeconds : -1.0, elapsed, sizeof(elapsed));
    FormatTime(timeline ? snapshot.durationSeconds : -1.0, total, sizeof(total));
    const float timeWidth = TimeWidth(elapsed, total);
    ImGui::AlignTextToFramePadding();
    TimeText("elapsed", elapsed, timeWidth);
    ImGui::SameLine();
    TimeLine(timeline ? static_cast<float>(snapshot.positionSeconds / snapshot.durationSeconds) : 0.0f, lineWidth(timeWidth));
    Mark("progress");
    if (!timeline) {
        Tip(S().noTimeline);
    }
    ImGui::SameLine();
    TimeText("total", total, timeWidth);
}

// Another app has no list here (the list holds the player's own songs) and no seek: only its
// timeline, when it reports one, or a line saying it does not; an empty row when no app is playing.
void TimeRowOtherApp(const remote::Snapshot& snapshot, const RemoteView& view, float width) {
    const Strings& s = S();
    if (!view.session) {
        // An empty row: the bar keeps its three rows whatever the source shows, so it never
        // changes height (and never moves) when an app starts or stops playing.
        ImGui::Dummy(ImVec2(width, ImGui::GetFrameHeight()));
        return;
    }
    if (snapshot.durationSeconds > 0.0) {
        // The app reports a timeline: shown, but another app cannot be moved through from here.
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ProgressLine(snapshot, [&](float timeWidth) { return ImMax(width - timeWidth * 2.0f - spacing * 2.0f, U() * 40.0f); });
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        TextFit(s.noTimeline, width);
        ImGui::PopStyleColor();
    }
}

// Previous, play or pause, (stop,) next for "My files". The compact panel leaves out stop.
void TransportMyFiles(const music::Snapshot& snapshot, bool displayOnly, bool withStop) {
    const Strings& s = S();
    music::Player& player = music::Player::Get();
    const bool any = !snapshot.tracks.empty() && !displayOnly;
    const char* reason = displayOnly ? s.reasonDisplayOnly : s.reasonNoSongs;
    if (IconButton("##previous", Icon::Previous, any, s.previous, reason)) player.Previous();
    Mark("previous");
    ImGui::SameLine();
    const bool playing = snapshot.state == music::State::Playing;
    if (IconButton("##play_pause", playing ? Icon::Pause : Icon::Play, any, playing ? s.pause : s.play, reason)) {
        player.TogglePlayPause();
    }
    Mark("play_pause");
    ImGui::SameLine();
    if (withStop) {
        const bool canStop = any && snapshot.state != music::State::Stopped;
        if (IconButton("##stop", Icon::Stop, canStop, s.stop, any ? s.reasonAlreadyStopped : reason)) player.Stop();
        Mark("stop");
        ImGui::SameLine();
    }
    if (IconButton("##next", Icon::Next, any, s.next, reason)) player.Next();
    Mark("next");
}

// The volume of "My files" as a slider with the percentage inside. The full panel puts the word
// "Volume" in front of it; the compact panel has no room for the word, and its hover text says it.
void VolumeSlider(const music::Snapshot& snapshot, float width, bool displayOnly, const char* id, const char* mark) {
    const Strings& s = S();
    ImGui::SetNextItemWidth(width);
    int volume = static_cast<int>(snapshot.volume * 100.0f + 0.5f);
    ImGui::BeginDisabled(displayOnly);
    if (ImGui::SliderInt(id, &volume, 0, 100, "%d%%", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)) {
        music::Player::Get().SetVolume(static_cast<float>(volume) / 100.0f);
    }
    ImGui::EndDisabled();
    Tip(s.volumeHint, displayOnly ? s.reasonDisplayOnly : nullptr);
    Mark(mark);
}

void ControlsMyFiles(const music::Snapshot& snapshot, float width, bool displayOnly) {
    const Strings& s = S();
    TransportMyFiles(snapshot, displayOnly, true);
    ImGui::SameLine();
    if (LoopButton(LoopLabel(EffectiveLoop(snapshot)), !displayOnly, s.loopHint, s.reasonDisplayOnly)) {
        StepLoop(snapshot);
    }
    ImGui::SameLine();
    // Up to 150 units (about 150 px at 1080p with medium text), at the right end of the row, so it
    // reads as a slider rather than a label with a handle squeezed against the edge. The word stands
    // in front; only the percentage is inside, so the handle never covers the word. When the buttons
    // before it leave less room (longer labels, larger text), the slider shrinks to fit rather than
    // being pushed off the panel's edge.
    const float labelWidth = ImGui::CalcTextSize(s.volumeWord).x + ImGui::GetStyle().ItemSpacing.x;
    const float left = width - (ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x);
    float sliderWidth = ImFloor(U() * 150.0f);
    if (left > sliderWidth + labelWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left - sliderWidth - labelWidth);
    } else {
        sliderWidth = ImMax(ImFloor(left - labelWidth), ImFloor(U() * 40.0f));
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.volumeWord);
    ImGui::SameLine();
    VolumeSlider(snapshot, sliderWidth, displayOnly, "##volume", "volume");
}

// Previous, play or pause, (stop,) next for "Other app". The compact panel leaves out stop.
void TransportOtherApp(const remote::Snapshot& snapshot, const RemoteView& view, bool displayOnly, bool withStop) {
    const Strings& s = S();
    OtherAppCalls app;
    const char* reason = displayOnly ? s.reasonDisplayOnly : view.reason;
    const bool live = view.session && !displayOnly;
    const remote::Controls& c = snapshot.controls;
    if (IconButton("##previous", Icon::Previous, live && c.previous, s.previous, reason)) app.Previous();
    Mark("previous");
    ImGui::SameLine();
    const bool canToggle = live && (view.wantPlaying ? c.pause : c.play);
    if (IconButton("##play_pause", view.wantPlaying ? Icon::Pause : Icon::Play, canToggle,
                   view.wantPlaying ? s.pause : s.play, reason)) {
        app.PlayOrPause(view.wantPlaying);
    }
    Mark("play_pause");
    ImGui::SameLine();
    if (withStop) {
        if (IconButton("##stop", Icon::Stop, live && c.stop, s.stop, reason)) app.Stop();
        Mark("stop");
        ImGui::SameLine();
    }
    if (IconButton("##next", Icon::Next, live && c.next, s.next, reason)) app.Next();
    Mark("next");
}

void ControlsOtherApp(const remote::Snapshot& snapshot, const RemoteView& view, float width, bool displayOnly) {
    const Strings& s = S();
    OtherAppCalls app;
    const char* reason = displayOnly ? s.reasonDisplayOnly : view.reason;
    const bool live = view.session && !displayOnly;
    const remote::Controls& c = snapshot.controls;
    TransportOtherApp(snapshot, view, displayOnly, true);
    ImGui::SameLine();
    const remote::Repeat repeat = EffectiveRepeat(snapshot);
    const bool unknown = repeat == remote::Repeat::Unknown;
    const bool canRepeat = live && c.repeat;
    // Greyed: what the app does not allow, and (when it does not say) that it does not say; never
    // "click to change".
    const char* repeatHint = canRepeat ? (unknown ? s.loopUnknownHint : s.loopHint) : (unknown ? s.loopUnknownInfo : nullptr);
    if (LoopButton(RepeatLabel(repeat), canRepeat, repeatHint, reason)) {
        app.StepRepeat(snapshot);
    }
    // No volume here: another app's volume is set in that app.
    (void)width;
}

// True when the panel sits at the bottom of the screen: the song list and the settings row then
// open above the bar, so the bar stays where it is.
bool AtBottom() {
    return g_settings.corner == UiCornerBottomLeft || g_settings.corner == UiCornerBottomRight ||
           g_settings.corner == UiCornerBottomCentre;
}

// How tall the song list was on the last frame it was drawn (0: not drawn then), so the next
// frame can tell how much of the panel's height the rest needs.
float g_lastListHeight = 0.0f;
bool g_listDrawnThisFrame = false;

// The bar's height on this frame (its window is drawn before the list's).
float g_barHeight = 0.0f;

// The number of song rows to show: up to 8, fewer when the whole panel would not fit on the
// screen otherwise (a small game window, large text, the settings row open as well). Measured from
// the previous frame: the list window's height then, less the list's own height, is what the rest
// of that window needs; the bar is its own window, measured on this frame.
int ListRowsThatFit(int count, float rowStep, const PanelFrameInfo& info) {
    int rows = count < 8 ? count : 8;
    if (g_lastListHeight <= 0.0f) {
        return rows;
    }
    // The content's own height, not the window's: ImGui clamps an auto-sized window to the screen,
    // which would hide by how much the content is too tall.
    const ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float rest = window->ContentSizeIdeal.y + window->WindowPadding.y * 2.0f - g_lastListHeight;
    const float available = info.displayHeight - g_barHeight - rest - ImGui::GetStyle().WindowPadding.y;
    const int fit = static_cast<int>(available / rowStep);
    const int least = count < 2 ? count : 2;
    if (fit < rows) {
        rows = fit > least ? fit : least;
    }
    return rows;
}

// The song list, in the window beside the bar (DrawExtras puts the dividers).
void SongList(const music::Snapshot& snapshot, const PanelFrameInfo& info) {
    const Strings& s = S();
    // The file window belongs to the game's top-level window, so it stays above the game and
    // gives the focus back when it closes; Windows keeps the game from taking input meanwhile
    // (the hints say so). In exclusive full screen it would minimise the game, so it is not
    // offered there, and only one can be open at a time.
    const HWND owner = info.window ? GetAncestor(info.window, GA_ROOT) : nullptr;
    char fullscreenReason[MAX_PATH * 3 + 512];
    snprintf(fullscreenReason, sizeof(fullscreenReason), s.reasonFullscreen,
             g_musicFolder[0] ? g_musicFolder : "%LOCALAPPDATA%\\RacingMasterMod\\music");
    const bool canPick = !info.exclusiveFullscreen && !snapshot.dialogOpen;
    const char* pickReason = info.exclusiveFullscreen ? fullscreenReason : s.reasonDialogOpen;
    ImGui::BeginDisabled(!canPick);
    if (ImGui::Button((std::string(s.addFiles) + "##add_files").c_str()) && canPick) {
        music::OpenAddFilesDialog(owner, &FilesChosen);
    }
    ImGui::EndDisabled();
    Tip(s.addFilesHint, canPick ? nullptr : pickReason);
    Mark("add_files");
    ImGui::SameLine();
    ImGui::BeginDisabled(!canPick);
    if (ImGui::Button((std::string(s.addFolder) + "##add_folder").c_str()) && canPick) {
        music::OpenAddFolderDialog(owner, &FolderChosen);
    }
    ImGui::EndDisabled();
    Tip(s.addFolderHint, canPick ? nullptr : pickReason);
    Mark("add_folder");
    const int count = static_cast<int>(snapshot.tracks.size());
    // Clear list (0.3.2, the owner: "in case I have 1000+ tracks, I don't want to click 1000 times").
    // The first click asks, naming the count; the question goes away if the list changes meanwhile.
    if (count == 0 || count != g_clearConfirmCount) {
        g_clearConfirmCount = -1;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(count == 0 || g_clearConfirmCount >= 0);
    if (ImGui::Button((std::string(s.clearList) + "##clear_list").c_str()) && count > 0) {
        g_clearConfirmCount = count;
    }
    ImGui::EndDisabled();
    Tip(s.clearListHint, count == 0 ? s.reasonNoSongs : nullptr);
    Mark("clear_list");
    char countText[64];
    if (count == 1) {
        snprintf(countText, sizeof(countText), "%s", s.songCountOne);
    } else {
        snprintf(countText, sizeof(countText), s.songCountMany, count);
    }
    ImGui::SameLine();
    const float countWidth = ImGui::CalcTextSize(countText).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImMax(0.0f, ImGui::GetContentRegionAvail().x - countWidth));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(countText);

    if (g_clearConfirmCount >= 0) {
        char question[160];
        snprintf(question, sizeof(question), s.clearListConfirm, count);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(question);
        ImGui::SameLine();
        if (ImGui::Button((std::string(s.clearListYes) + "##clear_yes").c_str())) {
            music::Player::Get().Clear();
            g_clearConfirmCount = -1;
        }
        Mark("clear_yes");
        ImGui::SameLine();
        if (ImGui::Button((std::string(s.clearListNo) + "##clear_no").c_str())) {
            g_clearConfirmCount = -1;
        }
        Mark("clear_no");
    }

    if (count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", s.listEmpty);
        ImGui::PopStyleColor();
        return;
    }
    const float rowHeight = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    const int visibleRows = ListRowsThatFit(count, rowHeight + style.ItemSpacing.y, info);
    const float childHeight = visibleRows * (rowHeight + style.ItemSpacing.y) + style.WindowPadding.y;
    g_lastListHeight = childHeight;
    g_listDrawnThisFrame = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * 0.5f, style.WindowPadding.y * 0.5f));
    ImGui::BeginChild("##songs", ImVec2(0.0f, childHeight), ImGuiChildFlags_Borders);
    const float removeWidth = rowHeight;
    const float listTimeWidth = ImGui::CalcTextSize("00:00").x;
    const float markWidth = ImFloor(rowHeight * 0.7f);
    ImGuiListClipper clipper;
    clipper.Begin(count, rowHeight + style.ItemSpacing.y);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const music::TrackInfo& track = snapshot.tracks[static_cast<size_t>(i)];
            ImGui::PushID(i);
            const float rowWidth = ImGui::GetContentRegionAvail().x - removeWidth - style.ItemSpacing.x;
            const bool current = i == snapshot.currentIndex;
            if (ImGui::Selectable("##row", current, 0, ImVec2(rowWidth, rowHeight)) &&
                track.supported) {
                music::Player::Get().PlayIndex(i);
            }
            char problem[300] = "";
            if (!track.supported) {
                char reasonText[256];
                Utf8(track.problem, reasonText, sizeof(reasonText));
                if (reasonText[0]) {
                    snprintf(problem, sizeof(problem), s.cannotPlay, reasonText);
                } else {
                    snprintf(problem, sizeof(problem), "%s", s.cannotPlayUnknown);
                }
            }
            Tip(track.supported ? s.playThisHint : problem);
            if (i < 8) MarkIndexed("row", i);
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImU32 textColour = ImGui::GetColorU32(track.supported ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            if (current) {
                // The playing (or loaded) song: a small triangle in front of it.
                const float cy = ImFloor((min.y + max.y) * 0.5f);
                const float h = ImFloor(rowHeight * 0.22f);
                const float x = ImFloor(min.x + markWidth * 0.3f);
                draw->AddTriangleFilled(ImVec2(x, cy - h), ImVec2(x, cy + h), ImVec2(x + h * 1.6f, cy),
                                        ImGui::GetColorU32(ImGuiCol_CheckMark));
            }
            char label[768];
            TrackLabel(track, label, sizeof(label));
            const float textTop = ImFloor((min.y + max.y - ImGui::GetTextLineHeight()) * 0.5f);
            const ImVec2 labelSize = ImGui::CalcTextSize(label);
            char length[16];
            FormatTime(track.durationSeconds > 0.0 ? track.durationSeconds : -1.0, length, sizeof(length));
            const float lengthWidth = ImGui::CalcTextSize(length).x;
            // The title stops before the length, also when the length is an hour or more.
            const float titleEnd = max.x - ImMax(listTimeWidth, lengthWidth) - style.ItemSpacing.x;
            ImGui::PushStyleColor(ImGuiCol_Text, textColour);
            ImGui::RenderTextEllipsis(draw, ImVec2(min.x + markWidth, textTop), ImVec2(titleEnd, max.y), titleEnd, label,
                                      nullptr, &labelSize);
            ImGui::PopStyleColor();
            draw->AddText(ImVec2(max.x - lengthWidth - style.FramePadding.x * 0.5f, textTop), textColour, length);

            ImGui::SameLine();
            if (ImGui::Button("##remove", ImVec2(removeWidth, rowHeight))) {
                music::Player::Get().Remove(i);
            }
            {
                // A drawn cross, so it does not depend on a font having the character.
                const ImVec2 bmin = ImGui::GetItemRectMin();
                const ImVec2 bmax = ImGui::GetItemRectMax();
                const float c = ImFloor(rowHeight * 0.2f);
                const ImVec2 mid(ImFloor((bmin.x + bmax.x) * 0.5f), ImFloor((bmin.y + bmax.y) * 0.5f));
                const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
                draw->AddLine(ImVec2(mid.x - c, mid.y - c), ImVec2(mid.x + c, mid.y + c), colour, ImMax(1.0f, U() * 1.5f));
                draw->AddLine(ImVec2(mid.x - c, mid.y + c), ImVec2(mid.x + c, mid.y - c), colour, ImMax(1.0f, U() * 1.5f));
            }
            Tip(s.remove, s.removeHint);
            if (i < 8) MarkIndexed("remove", i);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool ChoiceCombo(const char* id, const char* const* items, int count, unsigned* value, const char* testPrefix) {
    bool changed = false;
    const char* preview = items[*value < static_cast<unsigned>(count) ? *value : 0];
    float width = 0.0f;
    for (int i = 0; i < count; ++i) {
        width = ImMax(width, ImGui::CalcTextSize(items[i]).x);
    }
    ImGui::SetNextItemWidth(width + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i], *value == static_cast<unsigned>(i))) {
                changed = *value != static_cast<unsigned>(i);
                *value = static_cast<unsigned>(i);
            }
            MarkIndexed(testPrefix, i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Where the panel sits: the six places in screen order (top row, then bottom row); the stored
// value is a UiCorner.
void PositionChoice() {
    const Strings& s = S();
    static const unsigned kShownOrder[UiCornerCount] = {UiCornerTopLeft,    UiCornerTopCentre,    UiCornerTopRight,
                                                        UiCornerBottomLeft, UiCornerBottomCentre, UiCornerBottomRight};
    const char* names[UiCornerCount] = {s.positionTopLeft,    s.positionTopCentre,    s.positionTopRight,
                                        s.positionBottomLeft, s.positionBottomCentre, s.positionBottomRight};
    unsigned shown = 0;
    for (unsigned i = 0; i < UiCornerCount; ++i) {
        if (kShownOrder[i] == g_settings.corner) shown = i;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.position);
    ImGui::SameLine();
    if (ChoiceCombo("##position", names, UiCornerCount, &shown, "position_item")) {
        g_settings.corner = kShownOrder[shown];
        SaveSettings();
    }
    Mark("position");
}

// The settings row, in the window beside the bar (DrawExtras puts the dividers).
void SettingsRow() {
    const Strings& s = S();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.language);
    ImGui::SameLine();
    const char* languages[3] = {s.languageFollowWindows, LanguageName(Language::English),
                                LanguageName(Language::ChineseTraditional)};
    if (ChoiceCombo("##language", languages, 3, &g_settings.language, "language_item")) {
        g_language = ResolveLanguage(g_settings.language);
        SaveSettings();
    }
    Mark("language");
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 3.0f);
    ImGui::TextUnformatted(s.textSize);
    ImGui::SameLine();
    const char* sizes[3] = {s.textSmall, s.textMedium, s.textLarge};
    if (ChoiceCombo("##text_size", sizes, 3, &g_settings.textSize, "text_size_item")) {
        SaveSettings();
    }
    Mark("text_size");
    PositionChoice();
    char label[160];
    snprintf(label, sizeof(label), "%s##pause", s.pauseWhenUnfocused);
    if (ImGui::Checkbox(label, &g_settings.pauseWhenUnfocused)) {
        music::Player::Get().SetPauseWhenGameUnfocused(g_settings.pauseWhenUnfocused);
        SaveSettings();
    }
    Tip(s.pauseWhenUnfocusedHint);
    Mark("pause");
    char evenLabel[160];
    snprintf(evenLabel, sizeof(evenLabel), "%s##even_volume", s.evenVolume);
    bool even = music::Player::Get().EvenVolume();
    if (ImGui::Checkbox(evenLabel, &even)) {
        music::Player::Get().SetEvenVolume(even);   // the engine saves it with the other [music] values
    }
    Tip(s.evenVolumeHint);
    Mark("even_volume");
    char compactLabel[96];
    snprintf(compactLabel, sizeof(compactLabel), "%s##compact_setting", s.compactPanel);
    bool compact = g_settings.compact;
    if (ImGui::Checkbox(compactLabel, &compact)) {
        SetCompact(compact);
    }
    Tip(s.compactPanelHint);
    Mark("compact_setting");

    char keys[kCommandCount][32];
    for (int i = 0; i < kCommandCount; ++i) {
        HotkeyDisplayName(g_keys.keys[i], s.keyNone, keys[i], sizeof(keys[i]));
    }
    char line[512];
    snprintf(line, sizeof(line), s.keysLine, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5]);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", line);
    ImGui::PopStyleColor();

    if (g_testTextField) {
        // Test host only: a text field, to check that keys reach the panel only while it has focus.
        ImGui::SetNextItemWidth(U() * 200.0f);
        ImGui::InputText("##test_field", g_testText, sizeof(g_testText));
        Mark("test_field");
    }
}

void MutedWrapped(const char* text) {
    if (!text || !text[0]) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// Window position for the chosen place (UiCorner), flush with the screen edges it touches (the
// owner's decision for 0.3.2: no gap, so the panel covers as little of the game as it can; 0.3.1
// kept 16 units away). `pivot` makes the window grow away from those edges; the centre places
// grow to both sides equally.
void PlaceInCorner(const PanelFrameInfo& info, ImVec2* pos, ImVec2* pivot) {
    const bool right = g_settings.corner == UiCornerTopRight || g_settings.corner == UiCornerBottomRight;
    const bool centre = g_settings.corner == UiCornerBottomCentre || g_settings.corner == UiCornerTopCentre;
    const bool bottom = AtBottom();
    pos->x = centre ? ImFloor(info.displayWidth * 0.5f) : (right ? info.displayWidth : 0.0f);
    pos->y = bottom ? info.displayHeight : 0.0f;
    pivot->x = centre ? 0.5f : (right ? 1.0f : 0.0f);
    pivot->y = bottom ? 1.0f : 0.0f;
}

// What row 1 says for "My files": the song, or why there is none.
void NowPlayingMyFiles(const music::Snapshot& snapshot, char* buffer, int size) {
    const Strings& s = S();
    if (snapshot.tracks.empty()) {
        snprintf(buffer, static_cast<size_t>(size), "%s", s.noSongsYet);
    } else if (snapshot.currentIndex >= 0 && snapshot.currentIndex < static_cast<int>(snapshot.tracks.size())) {
        TrackLabel(snapshot.tracks[static_cast<size_t>(snapshot.currentIndex)], buffer, size);
    } else {
        snprintf(buffer, static_cast<size_t>(size), "%s", s.nothingPlaying);
    }
}

// What row 1 says for "Other app": "App: title - artist", or that nothing is playing. An app with
// no readable name (remote/app_names.h) shows its song alone.
void NowPlayingOtherApp(const remote::Snapshot& snapshot, const RemoteView& view, char* buffer, int size) {
    const Strings& s = S();
    if (!view.session) {
        snprintf(buffer, static_cast<size_t>(size), "%s", s.noAppPlaying);
        return;
    }
    char app[128], title[384], artist[256];
    Utf8(snapshot.app, app, sizeof(app));
    Utf8(snapshot.title, title, sizeof(title));
    Utf8(snapshot.artist, artist, sizeof(artist));
    const char* shownTitle = title[0] ? title : s.unknownTitle;
    const size_t n = static_cast<size_t>(size);
    if (app[0] && artist[0]) snprintf(buffer, n, "%s: %s - %s", app, shownTitle, artist);
    else if (app[0]) snprintf(buffer, n, "%s: %s", app, shownTitle);
    else if (artist[0]) snprintf(buffer, n, "%s - %s", shownTitle, artist);
    else snprintf(buffer, n, "%s", shownTitle);
}

constexpr ImGuiWindowFlags kPanelWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                               ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;

// The panel is two windows (0.3.2, the owner's decision 6): the bar alone, anchored at its place,
// and beside it, on the side away from the screen edge, the window with the settings row, the song
// list and the status lines. ImGui sizes an auto-sized window from the previous frame's content,
// so while the panel was one window the bar was drawn one row off for one frame whenever the list,
// the settings row or a status line appeared at a bottom place. The bar's content never changes
// height now (three rows for both sources), so it never moves; only the other window grows, away
// from the bar.
struct BarPlace {
    ImVec2 pos;    // the bar window's top-left corner on this frame
    ImVec2 size;   // and its size
};

void DrawExtras(const PanelFrameInfo& info, const BarPlace& bar, const char* message, const char* help,
                const music::Snapshot* songs) {
    const bool list = songs && g_listOpen && !info.displayOnly;
    const bool status = (message && message[0]) || (help && help[0]);
    if (!g_settingsOpen && !list && !status) {
        return;
    }
    const bool bottom = AtBottom();
    ImVec2 pos, pivot;
    PlaceInCorner(info, &pos, &pivot);
    pos.x = bar.pos.x + bar.size.x * pivot.x;   // the same side (or centre) as the bar
    pos.y = bottom ? bar.pos.y : bar.pos.y + bar.size.y;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(bar.size.x, 0.0f), ImGuiCond_Always);
    if (!ImGui::Begin("Music panel extras###rmm_extras", nullptr, kPanelWindowFlags)) {
        ImGui::End();
        return;
    }
    // From the bar outward: the song list, the settings row, the status lines. The status line is
    // outermost because it goes by itself a few seconds after it appears (0.3.2): at the far edge
    // its going only shrinks that edge, while nearer the bar it moved the list and the settings row
    // under the pointer. The window is drawn top-down, so at the bottom of the screen the order is
    // reversed.
    bool drewSection = false;
    for (int i = 0; i < 3; ++i) {
        const int which = bottom ? 2 - i : i;
        if (which == 2 && status) {
            if (drewSection) ImGui::Separator();
            MutedWrapped(help);
            MutedWrapped(message);
            drewSection = true;
        } else if (which == 0 && list) {
            if (drewSection) ImGui::Separator();
            SongList(*songs, info);
            drewSection = true;
        } else if (which == 1 && g_settingsOpen) {
            if (drewSection) ImGui::Separator();
            SettingsRow();
            drewSection = true;
        }
    }
    ImGui::End();
}

void DrawBar(const PanelFrameInfo& info) {
    ImVec2 pos, pivot;
    PlaceInCorner(info, &pos, &pivot);
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = ImFloor(U() * 520.0f);
    if (width > info.displayWidth) width = info.displayWidth;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    g_listDrawnThisFrame = false;
    const bool shown = ImGui::Begin("Music panel###rmm_panel", nullptr, kPanelWindowFlags);
    BarPlace bar;
    bar.pos = ImGui::GetWindowPos();
    bar.size = ImGui::GetWindowSize();
    g_barHeight = bar.size.y;
    const float inner = width - style.WindowPadding.x * 2.0f;
    char nowPlaying[768];
    char message[512] = "";
    const char* help = nullptr;
    const music::Snapshot* songs = nullptr;
    if (g_settings.source == SourceMyFiles) {
        const music::Snapshot& snapshot = CurrentSnapshot();
        songs = &snapshot;
        // The engine's status line ("Added 6 file(s)", an error) comes and goes: it goes in the
        // window beside the bar, never in the bar, and only for a few seconds after it changes.
        if (snapshot.messageSerial != g_engineMessageSerial) {
            g_engineMessageSerial = snapshot.messageSerial;
            g_engineMessageUntil = snapshot.message.empty() ? 0 : GetTickCount64() + kEngineMessageMs;
        }
        if (GetTickCount64() < g_engineMessageUntil) {
            Utf8(snapshot.message, message, sizeof(message));
        }
        if (shown) {
            NowPlayingMyFiles(snapshot, nowPlaying, sizeof(nowPlaying));
            SourceRow(nowPlaying, inner, info.displayOnly);
            ControlsMyFiles(snapshot, inner, info.displayOnly);
            TimeRowMyFiles(snapshot, inner, info.displayOnly);
        }
    } else {
        const remote::Snapshot other = other_app::GetSnapshot();
        const RemoteView view = ViewOf(other);
        RemoteMessageLine(other, message, sizeof(message));
        help = !view.session && !message[0] ? S().noAppHelp : nullptr;
        if (shown) {
            NowPlayingOtherApp(other, view, nowPlaying, sizeof(nowPlaying));
            SourceRow(nowPlaying, inner, info.displayOnly);
            ControlsOtherApp(other, view, inner, info.displayOnly);
            TimeRowOtherApp(other, view, inner);
        }
    }
    ImGui::End();
    if (shown) {
        DrawExtras(info, bar, message, help, songs);
    }
    if (!g_listDrawnThisFrame) {
        g_lastListHeight = 0.0f;   // the next list starts from 8 rows and is measured again
    }
}

// The compact panel (0.3.2, the owner's decision 2): one row for tight spaces, such as the strip
// under the game's bottom tabs (about 970 x 70 px at 2558x1360). Left to right: previous, play or
// pause, next, the song (cut off with "..." when long; the whole of it on hover), the elapsed time,
// a short time line (draggable for "My files"; another app cannot be moved through from here), the
// length, for "My files" a small volume slider (the owner, 2026-11-15: "so I don't need to expand
// it back just to adjust volume"; another app's volume is not the mod's to set), and the button
// back to the full panel. Repeat, the source, the list and the settings are in the full panel
// only. One window, at the same six places as the full panel; 700 units wide: at 2558x1360 (scale
// 1.25) 766 px with small text and 875 px with medium, inside the 970 px strip (large is wider).
void DrawCompact(const PanelFrameInfo& info) {
    const Strings& s = S();
    ImVec2 pos, pivot;
    PlaceInCorner(info, &pos, &pivot);
    float width = ImFloor(U() * 700.0f);
    if (width > info.displayWidth) width = info.displayWidth;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    g_listDrawnThisFrame = false;
    if (!ImGui::Begin("Music panel###rmm_panel", nullptr, kPanelWindowFlags)) {
        ImGui::End();
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float inner = width - style.WindowPadding.x * 2.0f;
    const float lineWidth = ImFloor(U() * 110.0f);
    const bool myFiles = g_settings.source == SourceMyFiles;
    const float volumeWidth = myFiles ? ImFloor(U() * 64.0f) : 0.0f;
    char nowPlaying[768];
    // The song gets what is left once the buttons, the two times (`timeWidth` each), the line, the
    // volume (My files), the expand button and the gaps between them have their room.
    auto title = [&](float timeWidth) {
        ImGui::SameLine();
        const float used = ImGui::GetCursorPosX() - style.WindowPadding.x;
        const float volume = myFiles ? volumeWidth + spacing : 0.0f;
        const float w = ImMax(ImFloor(inner - used - timeWidth * 2.0f - lineWidth - volume - IconButtonWidth() - spacing * 4.0f) - 1.0f,
                              U() * 40.0f);
        ImGui::AlignTextToFramePadding();
        TextFit(nowPlaying, w);
        if (ImGui::CalcTextSize(nowPlaying).x > w) {
            Tip(nowPlaying);
        }
        Mark("title");
        ImGui::SameLine();
    };
    if (myFiles) {
        const music::Snapshot& snapshot = CurrentSnapshot();
        NowPlayingMyFiles(snapshot, nowPlaying, sizeof(nowPlaying));
        TransportMyFiles(snapshot, info.displayOnly, false);
        const MyFilesTimes times = TimesOf(snapshot, info.displayOnly);
        title(times.width);
        SeekLine(snapshot, times, lineWidth, info.displayOnly);
        ImGui::SameLine();
        VolumeSlider(snapshot, volumeWidth, info.displayOnly, "##compact_volume", "compact_volume");
    } else {
        const remote::Snapshot other = other_app::GetSnapshot();
        const RemoteView view = ViewOf(other);
        NowPlayingOtherApp(other, view, nowPlaying, sizeof(nowPlaying));
        TransportOtherApp(other, view, info.displayOnly, false);
        char elapsed[16], total[16];
        const bool timeline = other.durationSeconds > 0.0;
        FormatTime(timeline ? other.positionSeconds : -1.0, elapsed, sizeof(elapsed));
        FormatTime(timeline ? other.durationSeconds : -1.0, total, sizeof(total));
        title(TimeWidth(elapsed, total));
        ProgressLine(other, [&](float) { return lineWidth; });
    }
    ImGui::SameLine();
    if (IconButton("##expand", Icon::Expand, !info.displayOnly, s.showFullPanel, s.reasonDisplayOnly)) {
        SetCompact(false);
    }
    Mark("expand");
    ImGui::End();
    g_lastListHeight = 0.0f;
}

void DrawNotice(const PanelFrameInfo& info, const char* text) {
    ImVec2 pos, pivot;
    PlaceInCorner(info, &pos, &pivot);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("Music notice###rmm_notice", nullptr, flags)) {
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
}

}  // namespace

void PanelConfigure(const UiSettings& settings, const HotkeyTable& keys, bool testHost) {
    g_settings = settings;
    g_keys = keys;
    g_testHost = testHost;
    g_language = ResolveLanguage(settings.language);
    Shared().localSourceActive.store(settings.source == SourceMyFiles, std::memory_order_relaxed);
    if (testHost) {
        wchar_t value[16];
        const DWORD n = GetEnvironmentVariableW(L"RACING_MASTER_MOD_TEST_TEXT_FIELD", value, 16);
        g_testTextField = n == 1 && value[0] == L'1';
        if (GetEnvironmentVariableW(L"RACING_MASTER_MOD_SOURCE", value, 16) > 0) {
            g_settings.source = lstrcmpiW(value, L"other_app") == 0 ? SourceOtherApp : SourceMyFiles;
            Shared().localSourceActive.store(g_settings.source == SourceMyFiles, std::memory_order_relaxed);
        }
        if (GetEnvironmentVariableW(L"RACING_MASTER_MOD_LIST", value, 16) > 0) {
            g_listOpen = lstrcmpiW(value, L"open") == 0;
        }
        if (GetEnvironmentVariableW(L"RACING_MASTER_MOD_SETTINGS_ROW", value, 16) > 0) {
            g_settingsOpen = lstrcmpiW(value, L"open") == 0;
        }
    }
}

void PanelFirstFrame() {
    music::Player::Get().SetPauseWhenGameUnfocused(g_settings.pauseWhenUnfocused);
    Utf8(music::Player::Get().MusicFolder(), g_musicFolder, sizeof(g_musicFolder));
    const Hotkey& toggle = g_keys.keys[static_cast<int>(Command::TogglePanel)];
    if (!g_testHost && toggle.vk != 0 && !Shared().panelOpen.load()) {
        // A first-time player learns the key without reading anything.
        char key[32];
        HotkeyDisplayName(toggle, S().keyNone, key, sizeof(key));
        snprintf(g_toast, sizeof(g_toast), S().showPanelNotice, key);
        g_noticeUntil = GetTickCount64() + 10000;
    }
}

bool PanelWantsFrame(bool panelOpen, ULONGLONG now) {
    return panelOpen || now < g_toastUntil || now < g_noticeUntil;
}

void BuildPanel(const PanelFrameInfo& info, bool panelOpen) {
    g_itemCount = 0;
    if (panelOpen) {
        if (g_settings.compact) {
            DrawCompact(info);
        } else {
            DrawBar(info);
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_toastUntil || now < g_noticeUntil) {
        DrawNotice(info, g_toast);
    }
}

void RunMusicCommand(Command command) {
    const Strings& s = S();
    const bool open = Shared().panelOpen.load(std::memory_order_relaxed);
    if (g_settings.source == SourceMyFiles) {
        music::Player& player = music::Player::Get();
        const music::Snapshot& snapshot = CurrentSnapshot();
        if (snapshot.tracks.empty() && command != Command::LoopMode) {
            if (!open) ShowToast(s.reasonNoSongs);
            return;
        }
        switch (command) {
            case Command::Stop: player.Stop(); break;
            case Command::Previous: player.Previous(); break;
            case Command::PlayPause: player.TogglePlayPause(); break;
            case Command::Next: player.Next(); break;
            case Command::LoopMode: {
                const music::LoopMode next = StepLoop(snapshot);
                if (!open) ShowToast(LoopLabel(next));
                break;
            }
            default: break;
        }
        return;
    }
    OtherAppCalls app;
    const remote::Snapshot snapshot = other_app::GetSnapshot();
    const RemoteView view = ViewOf(snapshot);
    const remote::Controls& c = snapshot.controls;
    bool allowed = false;
    switch (command) {
        case Command::Stop: allowed = c.stop; break;
        case Command::Previous: allowed = c.previous; break;
        case Command::PlayPause: allowed = view.wantPlaying ? c.pause : c.play; break;
        case Command::Next: allowed = c.next; break;
        case Command::LoopMode: allowed = c.repeat; break;
        default: break;
    }
    if (!view.session || !allowed) {
        if (!open) ShowToast(view.reason);
        return;
    }
    switch (command) {
        case Command::Stop: app.Stop(); break;
        case Command::Previous: app.Previous(); break;
        case Command::PlayPause: app.PlayOrPause(view.wantPlaying); break;
        case Command::Next: app.Next(); break;
        case Command::LoopMode: {
            const remote::Repeat next = app.StepRepeat(snapshot);
            if (!open) ShowToast(RepeatLabel(next));
            break;
        }
        default: break;
    }
}

void PanelClickedOutside() {
    ImGui::ClosePopupsExceptModals();
    if (ImGui::GetIO().WantTextInput) {
        // The keyboard goes back to the game as soon as the player clicks into it.
        ImGui::ClearActiveID();
        ImGui::FocusWindow(nullptr);
    }
}

unsigned PanelTextSize() {
    return g_settings.textSize;
}

int PanelTestItems(const PanelTestItem** items) {
    *items = g_items;
    return g_itemCount;
}

int PanelAppendTestState(char* buffer, int size) {
    const music::Snapshot& snapshot = CurrentSnapshot();
    const int n = snprintf(buffer, static_cast<size_t>(size),
                           "list %d\nsettings %d\nsource %u\nlanguage %u\ntextsize %u\npause %d\nstate %d\nindex %d\n"
                           "tracks %d\nloop %d\nvolume %d\ntextfield %s\ntoast %d\nwanttext %d\nscanning %d\n",
                           g_listOpen ? 1 : 0, g_settingsOpen ? 1 : 0, g_settings.source, g_settings.language,
                           g_settings.textSize, g_settings.pauseWhenUnfocused ? 1 : 0, static_cast<int>(snapshot.state),
                           snapshot.currentIndex, static_cast<int>(snapshot.tracks.size()),
                           static_cast<int>(snapshot.loop), static_cast<int>(snapshot.volume * 100.0f + 0.5f),
                           g_testText[0] ? g_testText : "-", GetTickCount64() < g_toastUntil ? 1 : 0,
                           Shared().wantTextInput.load() ? 1 : 0, snapshot.scanning ? 1 : 0);
    if (n <= 0 || n >= size) {
        return 0;
    }
    // Whole seconds only, so the file changes about once a second while a song plays; the number
    // of songs the engine marked as unplayable. The other app's state only while "Other app" is
    // the source.
    int unplayable = 0;
    for (const music::TrackInfo& track : snapshot.tracks) {
        unplayable += track.supported ? 0 : 1;
    }
    // "message": whether the engine's status line is showing (it goes a few seconds after it
    // changed); "clearask": whether Clear list is asking.
    int extra = snprintf(buffer + n, static_cast<size_t>(size - n),
                         "position %d\nunplayable %d\ncorner %u\nmessage %d\ncompact %d\nclearask %d\n",
                         static_cast<int>(snapshot.positionSeconds), unplayable, g_settings.corner,
                         !snapshot.message.empty() && GetTickCount64() < g_engineMessageUntil ? 1 : 0,
                         g_settings.compact ? 1 : 0, g_clearConfirmCount >= 0 ? 1 : 0);
    if (g_settings.source == SourceOtherApp && extra > 0 && n + extra < size) {
        const remote::Snapshot other = other_app::GetSnapshot();
        const RemoteView view = ViewOf(other);
        const int more = snprintf(buffer + n + extra, static_cast<size_t>(size - n - extra),
                                  "other_session %d\nother_playing %d\nother_repeat %d\n", view.session ? 1 : 0,
                                  view.playing ? 1 : 0, static_cast<int>(other.repeat));
        extra = more > 0 ? extra + more : extra;
    }
    return extra > 0 && n + extra < size ? n + extra : n;
}

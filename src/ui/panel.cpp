// The music panel's widgets: a compact bar in a corner, the song list on demand, a settings row
// on demand, and short notices. Frame thread only (between ImGui::NewFrame and ImGui::Render),
// apart from PanelConfigure on the worker before the hook exists.
//
//   Row 1: source ("My files" / "Other app"), what is playing, Settings, Hide
//   Row 2: previous, play or pause, stop, next, what repeats, volume
//   Row 3: elapsed, seek line, length, Show list
//   then:  the song list (Add files, Add folder, one row per song with its length and a remove
//          button) and/or the settings row (language, text size, pause when not in focus, keys)
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
// beside it: the text is drawn at the line's text offset, not at the top of the line.
void TextFit(const char* text, float width) {
    const float offset = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;   // set by AlignTextToFramePadding
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.y += offset;
    const float height = ImGui::GetTextLineHeight();
    const ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::ItemSize(ImVec2(width, height), offset);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + width, pos.y + height), pos.x + width,
                              text, nullptr, &size);
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

enum class Icon { Previous, Play, Pause, Stop, Next };

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
    }
}

// A square-ish button with a drawn icon. Returns true when clicked.
bool IconButton(const char* id, Icon icon, bool enabled, const char* name, const char* reason) {
    const float h = ImGui::GetFrameHeight();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(id, ImVec2(ImFloor(h * 1.3f), h));
    DrawIcon(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Text));
    ImGui::EndDisabled();
    Tip(name, enabled ? nullptr : reason);
    return pressed && enabled;
}

float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
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
    const float right = ButtonWidth(settingsLabel) + spacing + ButtonWidth(hideLabel);
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

void TimeRowMyFiles(const music::Snapshot& snapshot, float width, bool displayOnly) {
    const Strings& s = S();
    const bool loaded = snapshot.currentIndex >= 0 && snapshot.currentIndex < static_cast<int>(snapshot.tracks.size());
    const double duration = loaded ? snapshot.durationSeconds : 0.0;
    const bool canSeek = loaded && duration > 0.0 && !displayOnly;
    float value = g_seekActive ? g_seekValue : static_cast<float>(loaded ? snapshot.positionSeconds : 0.0);
    char elapsed[16], total[16];
    FormatTime(loaded ? value : -1.0, elapsed, sizeof(elapsed));
    FormatTime(loaded && duration > 0.0 ? duration : -1.0, total, sizeof(total));

    const float timeWidth = ImGui::CalcTextSize("00:00").x;
    char listLabel[64];
    snprintf(listLabel, sizeof(listLabel), "%s##list", g_listOpen ? s.hideList : s.showList);
    const float listWidth = ImMax(ButtonWidth(s.showList), ButtonWidth(s.hideList));
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    TextFit(elapsed, timeWidth);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImMax(width - timeWidth * 2.0f - listWidth - spacing * 3.0f, U() * 40.0f));
    ImGui::BeginDisabled(!canSeek);
    const float maximum = duration > 0.0 ? static_cast<float>(duration) : 1.0f;
    ImGui::SliderFloat("##seek", &value, 0.0f, maximum, "", ImGuiSliderFlags_NoInput);
    if (ImGui::IsItemActive()) {
        g_seekActive = true;
        g_seekValue = value;
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && canSeek) {
        music::Player::Get().Seek(static_cast<double>(g_seekValue));
    }
    if (!ImGui::IsItemActive()) {
        g_seekActive = false;
    }
    ImGui::EndDisabled();
    const char* reason = displayOnly ? s.reasonDisplayOnly
                         : snapshot.tracks.empty() ? s.reasonNoSongs
                         : !loaded ? s.reasonNothingLoaded
                                   : s.reasonNoLength;
    Tip(s.seekHint, canSeek ? nullptr : reason);
    Mark("seek");
    ImGui::SameLine();
    TextFit(total, timeWidth);
    ImGui::SameLine();
    ImGui::BeginDisabled(displayOnly);
    if (ImGui::Button(listLabel, ImVec2(listWidth, 0.0f))) {
        g_listOpen = !g_listOpen;
    }
    ImGui::EndDisabled();
    Tip(s.listHint, displayOnly ? s.reasonDisplayOnly : nullptr);
    Mark("list");
}

// Another app has no list here (the list holds the player's own songs) and no seek: only its
// timeline, when it reports one, or a line saying it does not. Nothing when no app is playing.
void TimeRowOtherApp(const remote::Snapshot& snapshot, const RemoteView& view, float width) {
    const Strings& s = S();
    if (!view.session) {
        return;
    }
    if (snapshot.durationSeconds > 0.0) {
        // The app reports a timeline: shown, but another app cannot be moved through from here.
        const float timeWidth = ImGui::CalcTextSize("00:00").x;
        char elapsed[16], total[16];
        FormatTime(snapshot.positionSeconds, elapsed, sizeof(elapsed));
        FormatTime(snapshot.durationSeconds, total, sizeof(total));
        ImGui::AlignTextToFramePadding();
        TextFit(elapsed, timeWidth);
        ImGui::SameLine();
        float fraction = static_cast<float>(snapshot.positionSeconds / snapshot.durationSeconds);
        fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab));
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        ImGui::ProgressBar(fraction, ImVec2(ImMax(width - timeWidth * 2.0f - spacing * 2.0f, U() * 40.0f),
                                            ImGui::GetFrameHeight() * 0.5f),
                           "");
        ImGui::PopStyleColor();
        Mark("progress");
        ImGui::SameLine();
        TextFit(total, timeWidth);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        TextFit(s.noTimeline, width);
        ImGui::PopStyleColor();
    }
}

void ControlsMyFiles(const music::Snapshot& snapshot, float width, bool displayOnly) {
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
    const bool canStop = any && snapshot.state != music::State::Stopped;
    if (IconButton("##stop", Icon::Stop, canStop, s.stop, any ? s.reasonAlreadyStopped : reason)) player.Stop();
    Mark("stop");
    ImGui::SameLine();
    if (IconButton("##next", Icon::Next, any, s.next, reason)) player.Next();
    Mark("next");
    ImGui::SameLine();
    if (LoopButton(LoopLabel(EffectiveLoop(snapshot)), !displayOnly, s.loopHint, s.reasonDisplayOnly)) {
        StepLoop(snapshot);
    }
    ImGui::SameLine();
    // A fixed width (150 units, about 150 px at 1080p with medium text), at the right end of the
    // row, so it reads as a slider rather than a label with a handle squeezed against the edge.
    // The word stands in front; only the percentage is inside, so the handle never covers the word.
    const float sliderWidth = ImFloor(U() * 150.0f);
    const float labelWidth = ImGui::CalcTextSize(s.volumeWord).x + ImGui::GetStyle().ItemSpacing.x;
    const float left = width - (ImGui::GetCursorPosX() - ImGui::GetStyle().WindowPadding.x);
    if (left > sliderWidth + labelWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left - sliderWidth - labelWidth);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(s.volumeWord);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sliderWidth);
    int volume = static_cast<int>(snapshot.volume * 100.0f + 0.5f);
    ImGui::BeginDisabled(displayOnly);
    if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%", ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)) {
        player.SetVolume(static_cast<float>(volume) / 100.0f);
    }
    ImGui::EndDisabled();
    Tip(s.volumeHint, displayOnly ? s.reasonDisplayOnly : nullptr);
    Mark("volume");
}

void ControlsOtherApp(const remote::Snapshot& snapshot, const RemoteView& view, float width, bool displayOnly) {
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
    if (IconButton("##stop", Icon::Stop, live && c.stop, s.stop, reason)) app.Stop();
    Mark("stop");
    ImGui::SameLine();
    if (IconButton("##next", Icon::Next, live && c.next, s.next, reason)) app.Next();
    Mark("next");
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

void SongList(const music::Snapshot& snapshot, const PanelFrameInfo& info) {
    const Strings& s = S();
    ImGui::Separator();
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

    if (count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", s.listEmpty);
        ImGui::PopStyleColor();
        return;
    }
    const float rowHeight = ImGui::GetFrameHeight();
    const int visibleRows = count < 8 ? count : 8;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float childHeight = visibleRows * (rowHeight + style.ItemSpacing.y) + style.WindowPadding.y;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x * 0.5f, style.WindowPadding.y * 0.5f));
    ImGui::BeginChild("##songs", ImVec2(0.0f, childHeight), ImGuiChildFlags_Borders);
    const float removeWidth = rowHeight;
    const float timeWidth = ImGui::CalcTextSize("00:00").x;
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
            ImGui::PushStyleColor(ImGuiCol_Text, textColour);
            ImGui::RenderTextEllipsis(draw, ImVec2(min.x + markWidth, textTop),
                                      ImVec2(max.x - timeWidth - style.ItemSpacing.x, max.y),
                                      max.x - timeWidth - style.ItemSpacing.x, label, nullptr, &labelSize);
            ImGui::PopStyleColor();
            char length[16];
            FormatTime(track.durationSeconds > 0.0 ? track.durationSeconds : -1.0, length, sizeof(length));
            const float lengthWidth = ImGui::CalcTextSize(length).x;
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

void SettingsRow() {
    const Strings& s = S();
    ImGui::Separator();
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
    char label[160];
    snprintf(label, sizeof(label), "%s##pause", s.pauseWhenUnfocused);
    if (ImGui::Checkbox(label, &g_settings.pauseWhenUnfocused)) {
        music::Player::Get().SetPauseWhenGameUnfocused(g_settings.pauseWhenUnfocused);
        SaveSettings();
    }
    Tip(s.pauseWhenUnfocusedHint);
    Mark("pause");

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

// Window position for the chosen corner. `pivot` makes the window grow away from the corner.
void PlaceInCorner(const PanelFrameInfo& info, ImVec2* pos, ImVec2* pivot) {
    const float margin = ImFloor(U() * 16.0f);
    const bool right = g_settings.corner == 1 || g_settings.corner == 3;
    const bool bottom = g_settings.corner >= 2;
    pos->x = right ? info.displayWidth - margin : margin;
    pos->y = bottom ? info.displayHeight - margin : margin;
    pivot->x = right ? 1.0f : 0.0f;
    pivot->y = bottom ? 1.0f : 0.0f;
}

void DrawBar(const PanelFrameInfo& info) {
    const Strings& s = S();
    ImVec2 pos, pivot;
    PlaceInCorner(info, &pos, &pivot);
    const ImGuiStyle& style = ImGui::GetStyle();
    float width = ImFloor(U() * 520.0f);
    const float maxWidth = info.displayWidth - U() * 32.0f;
    if (width > maxWidth) width = ImMax(maxWidth, U() * 200.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    if (!ImGui::Begin("Music panel###rmm_panel", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const float inner = width - style.WindowPadding.x * 2.0f;
    const bool myFiles = g_settings.source == SourceMyFiles;
    char nowPlaying[768];
    if (myFiles) {
        const music::Snapshot& snapshot = CurrentSnapshot();
        if (snapshot.tracks.empty()) {
            strcpy_s(nowPlaying, s.noSongsYet);
        } else if (snapshot.currentIndex >= 0 && snapshot.currentIndex < static_cast<int>(snapshot.tracks.size())) {
            TrackLabel(snapshot.tracks[static_cast<size_t>(snapshot.currentIndex)], nowPlaying, sizeof(nowPlaying));
        } else {
            strcpy_s(nowPlaying, s.nothingPlaying);
        }
        SourceRow(nowPlaying, inner, info.displayOnly);
        ControlsMyFiles(snapshot, inner, info.displayOnly);
        TimeRowMyFiles(snapshot, inner, info.displayOnly);
        char message[512];
        MutedWrapped(Utf8(snapshot.message, message, sizeof(message)));
        if (g_listOpen && !info.displayOnly) {
            SongList(snapshot, info);
        }
    } else {
        const remote::Snapshot snapshot = other_app::GetSnapshot();
        const RemoteView view = ViewOf(snapshot);
        if (!view.session) {
            strcpy_s(nowPlaying, s.noAppPlaying);
        } else {
            char app[128], title[384], artist[256];
            Utf8(snapshot.app, app, sizeof(app));
            Utf8(snapshot.title, title, sizeof(title));
            Utf8(snapshot.artist, artist, sizeof(artist));
            const char* shownTitle = title[0] ? title : s.unknownTitle;
            if (app[0] && artist[0]) snprintf(nowPlaying, sizeof(nowPlaying), "%s: %s - %s", app, shownTitle, artist);
            else if (app[0]) snprintf(nowPlaying, sizeof(nowPlaying), "%s: %s", app, shownTitle);
            else if (artist[0]) snprintf(nowPlaying, sizeof(nowPlaying), "%s - %s", shownTitle, artist);
            else snprintf(nowPlaying, sizeof(nowPlaying), "%s", shownTitle);
        }
        SourceRow(nowPlaying, inner, info.displayOnly);
        ControlsOtherApp(snapshot, view, inner, info.displayOnly);
        TimeRowOtherApp(snapshot, view, inner);
        char message[512];
        RemoteMessageLine(snapshot, message, sizeof(message));
        if (!view.session && !message[0]) {
            MutedWrapped(s.noAppHelp);
        }
        MutedWrapped(message);
    }
    if (g_settingsOpen) {
        SettingsRow();
    }
    ImGui::End();
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
        DrawBar(info);
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
    int extra = snprintf(buffer + n, static_cast<size_t>(size - n), "position %d\nunplayable %d\n",
                         static_cast<int>(snapshot.positionSeconds), unplayable);
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

#include "scale.h"

#include "ui_common.h"

namespace {

ImGuiStyle g_baseStyle;
float g_appliedScale = 0.0f;
unsigned g_appliedTextSize = 99;
float g_units = 1.0f;

float UiScaleForHeight(unsigned height) {
    float scale = static_cast<float>(height) / 1080.0f;
    scale = static_cast<float>(static_cast<int>(scale * 4.0f + 0.5f)) / 4.0f;   // quarter steps
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 3.0f) scale = 3.0f;
    return scale;
}

float TextChoice(unsigned textSize) {
    switch (textSize) {
        case 0: return 0.875f;
        case 2: return 1.25f;
        default: return 1.0f;
    }
}

}  // namespace

void ScaleCaptureBaseStyle() {
    g_baseStyle = ImGui::GetStyle();
    g_appliedScale = 0.0f;
    g_appliedTextSize = 99;
}

float ApplyScale(unsigned backBufferHeight, unsigned textSize) {
    const float scale = UiScaleForHeight(backBufferHeight);
    const float choice = TextChoice(textSize);
    if (scale == g_appliedScale && textSize == g_appliedTextSize) {
        return g_units;
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style = g_baseStyle;                      // ScaleAllSizes is cumulative: always start from the base
    style.ScaleAllSizes(scale * choice);      // paddings, spacing, rounding, scrollbar
    style.FontSizeBase = 16.0f;               // design size: medium at 1080p
    style.FontScaleDpi = scale;               // resolution
    style.FontScaleMain = choice;             // small | medium | large
    style.MouseCursorScale = scale;           // must come after ScaleAllSizes, which truncates it to whole numbers
    g_appliedScale = scale;
    g_appliedTextSize = textSize;
    g_units = scale * choice;
    return g_units;
}

float Units() {
    return g_units;
}

float CurrentUiScale() {
    return g_appliedScale;
}

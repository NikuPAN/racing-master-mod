#pragma once

// Scaling. The panel ignores DPI; like the game's own interface it scales with the height of the
// frame. Text size is the player's three-way choice, stored as a word, never as pixels.
//
//   uiScale    = height / 1080 rounded to quarters, between 1 and 3 (1080 -> 1, 1360 -> 1.25, 2160 -> 2)
//   textChoice = small 0.875, medium 1.0, large 1.25
//   font size  = 16 px * uiScale * textChoice   (medium: 16 px at 1080p, 20 px at 1360p, 32 px at 4K)
//
// Everything laid out in panel code is written in "units" (pixels at 1080p, medium) and
// multiplied by Units(); no pixel constant is written anywhere.

// Remembers the current style as the base that ApplyScale starts from. Call once after styling.
void ScaleCaptureBaseStyle();

// Call before ImGui::NewFrame(). No-op when nothing changed. Returns uiScale * textChoice.
float ApplyScale(unsigned backBufferHeight, unsigned textSize);

// The multiplier of the frame being built (what ApplyScale last returned).
float Units();

// Resolution factor only (for the log).
float CurrentUiScale();

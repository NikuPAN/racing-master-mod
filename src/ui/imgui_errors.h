#pragma once

// Dear ImGui's mistakes are logged, never shown; see imgui_errors.cpp.
// ModImGuiAssertFail itself is declared in imconfig_mod.h (ImGui's IM_ASSERT calls it).

// Call once after ImGui::CreateContext().
void InstallImGuiErrorPolicy(bool testHost);

// True once an ImGui assertion failed; the panel then switches itself off after the frame.
bool ImGuiAssertTripped();

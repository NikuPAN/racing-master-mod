// What happens when Dear ImGui finds a mistake. Players must never see a dialog or lose the game
// because of the panel:
//
//   IM_ASSERT (imconfig_mod.h)  logged once; the panel switches itself off after the current
//                               frame. Code after a failed assertion may fault; the frame guard
//                               in present_hook.cpp then catches that.
//   recoverable mistakes        (a missing End or Pop...) ImGui recovers by itself; the first few
//                               are logged through the error callback so they can be fixed.
//
// The error callback is required, not optional: with ImGui's three error-recovery outputs off
// and no callback, ImGui::NewFrame() asserts on every frame (design brief, checker note 1).

#include "imgui_errors.h"

#include "ui_common.h"

#include "../core/log.h"

#include "imgui_internal.h"

#include <cstring>

namespace {

volatile bool g_assertTripped = false;
int g_errorMessages = 0;

void ModImGuiError(ImGuiContext*, void*, const char* message) {
    if (g_errorMessages < 5) {
        Log("panel: Dear ImGui reported a mistake in the panel code and recovered: %s", message ? message : "");
    }
    ++g_errorMessages;
}

}  // namespace

void ModImGuiAssertFail(const char* expression, const char* file, int line) {
    if (!g_assertTripped) {
        g_assertTripped = true;
        const char* name = file ? strrchr(file, '\\') : nullptr;
        Log("panel: IMGUI ASSERT \"%s\" (%s:%d); the panel switches itself off for this session",
            expression ? expression : "", name ? name + 1 : (file ? file : ""), line);
    }
}

void InstallImGuiErrorPolicy(bool testHost) {
    ImGuiIO& io = ImGui::GetIO();
    // In the test host recoverable mistakes assert, so a test catches them; players never see
    // ImGui's programmer-error tools.
    io.ConfigErrorRecoveryEnableAssert = testHost;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    // Internal field ("may be exposed in public API eventually"): if a later ImGui renames it the
    // build fails, which is the safe direction. Do not "fix" that by dropping the callback.
    ImGui::GetCurrentContext()->ErrorCallback = ModImGuiError;
}

bool ImGuiAssertTripped() {
    return g_assertTripped;
}

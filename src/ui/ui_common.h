#pragma once

// Included by every panel file that uses Dear ImGui. Checks that the mod's configuration header
// was applied: IMGUI_CHECKVERSION() cannot tell, because the structure sizes it compares are the
// same with and without IMGUI_USE_WCHAR32.

#include "imgui.h"

static_assert(sizeof(ImWchar) == 4, "imconfig_mod.h was not applied: build with /DIMGUI_USER_CONFIG=<imconfig_mod.h>");

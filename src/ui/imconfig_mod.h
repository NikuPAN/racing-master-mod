#pragma once

// Dear ImGui configuration for the mod. Selected with /DIMGUI_USER_CONFIG=<imconfig_mod.h> for
// every file that includes imgui.h (the build script adds it to the panel's flag list).
//
// An ImGui assertion must never open a dialog or abort the game: it is logged once and the panel
// switches itself off at the end of the current frame. The panel is built with /DNDEBUG, so
// without this override IM_ASSERT would be the C runtime's assert compiled to nothing, which
// hides panel mistakes; keep the override (imgui_errors.cpp has the policy).
void ModImGuiAssertFail(const char* expression, const char* file, int line);
#define IM_ASSERT(_EXPR) ((void)((!!(_EXPR)) || (ModImGuiAssertFail(#_EXPR, __FILE__, __LINE__), 0)))

#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_WIN32_FUNCTIONS          // no clipboard or IME calls from the frame thread, no imm32
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS  // no ShellExecute, no shell32
#define IMGUI_DISABLE_FILE_FUNCTIONS           // no fopen: fonts are mapped by the mod, no imgui.ini is written
#define IMGUI_DISABLE_DEFAULT_FONT_BITMAP      // keep only the embedded vector font as the last resort
#define IMGUI_USE_WCHAR32                      // code points above U+FFFF (CJK Extension B, Hong Kong characters)

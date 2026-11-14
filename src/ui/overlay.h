#pragma once

// The in-game music panel as the core sees it. Dear ImGui drawn from the Present hook:
//
//   worker thread   PanelStart: settings, hotkeys, loads d3dcompiler_47.dll, resolves the
//                   delay-loaded functions the other threads will call, starts the settings writer
//   frame thread    PanelOnFrame, inside GuardedOnFrame's fault guard: first eligible frame
//                   initialises (ImGui context, fonts, shaders, our own pipeline state, window
//                   subclass); every frame runs hotkey commands; while the panel or a notice is
//                   visible it draws inside a swapped device context state, holding nothing that
//                   refers to the back buffer once the frame is done
//   fault handler   PanelOnFault: input handed back to the game, panel off for the session
//
// No header here includes ImGui or the media interfaces, so core files stay free of both.

#include <windows.h>

struct FrameRefs;
struct IDXGISwapChain;
struct UiSettings;

// Worker thread, after the settings are read and before the frame hook is installed. Does
// nothing when [panel] mode=off.
void PanelStart(const UiSettings& settings, bool testHost);

// Frame thread, from OnFrame for the bound swap chain. Everything it acquires from Direct3D goes
// into `refs`, so the fault handler can give it back.
void PanelOnFrame(IDXGISwapChain* swapChain, UINT presentFlags, FrameRefs& refs);

// From the fault handler, after the game's pipeline state was put back, and from PanelOnFrame
// after an ImGui assertion or a fault in the window procedure. Sets atomics only: no ImGui, no
// Direct3D, no allocation.
void PanelOnFault();

// Any thread: the window the panel draws into (the swap chain's output window), or null before
// the panel's first frame. The worker watches it to stop the music when the game closes.
HWND PanelGameWindow();

// Any thread: true once the panel has switched itself off for the session (a fault in the frame
// code, an ImGui assertion, or a fault in the window procedure). From then on nothing in the
// game can reach the music; the worker pauses it.
bool PanelSwitchedOff();

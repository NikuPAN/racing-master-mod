#pragma once

// Hooks IDXGISwapChain::Present / Present1 so the mod gets a callback once per frame.
//
// The hook replaces two entries in DXGI's swap chain function table (the same place overlays
// such as Steam, Discord and OBS attach to). No game code or game file is modified.
//
// markerSeconds: how long to draw the small "mod is alive" square after the first frame.
// Returns false if the hook could not be installed; the reason is written to the log.
bool InstallPresentHook(unsigned markerSeconds);

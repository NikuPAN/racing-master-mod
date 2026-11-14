#pragma once

// Finds the function table DXGI uses for every swap chain in this process by creating a
// throwaway swap chain on a hidden helper window. Everything created is released again before
// returning. Worker thread only (it loads d3d11.dll and uses user32).
//
// hasPresent1 is set when the table also carries IDXGISwapChain1::Present1.
// Returns nullptr if no helper device could be created; the reason is written to the log.
void** FindSwapChainTable(bool* hasPresent1);

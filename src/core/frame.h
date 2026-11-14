#pragma once

#include <d3d11_1.h>

// Everything the per-frame code holds while it runs. A plain struct without a destructor,
// declared OUTSIDE the __try block in GuardedOnFrame, so that the fault handler can still
// release it: with structured exception handling, destructors and manual Release() calls
// below the fault are skipped, and one leaked back-buffer reference makes the game's next
// ResizeBuffers fail.
//
// Rules for later per-frame work (music panel, DLSS):
// - Every Direct3D object acquired during a frame goes into this struct; never into a local
//   smart pointer below the guard. Extend the struct and ReleaseFrameRefs together.
// - Nothing is kept from one frame to the next.
struct FrameRefs {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11DeviceContext1* context1;
    ID3D11Texture2D* backBuffer;
    ID3D11RenderTargetView* target;
    // The game's whole pipeline state while the panel draws inside its own (set by the first
    // SwapDeviceContextState, handed back by the second). Non-null between the two swaps: the
    // fault handler then clears our state and swaps the game's back in before releasing.
    ID3DDeviceContextState* gameState;
};

// Releases every member in reverse order of acquisition and zeroes the struct.
void ReleaseFrameRefs(FrameRefs& refs);

// Fault handler only: if a fault came between the panel's two state swaps, clears the panel's
// state (which holds the back buffer) and puts the game's state back on the context. Each step
// has its own guard. Call before ReleaseFrameRefs.
void RestoreGameStateAfterFault(FrameRefs& refs);

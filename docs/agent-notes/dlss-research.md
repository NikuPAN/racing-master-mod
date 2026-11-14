---
name: dlss-research
description: Research brief of 2026-11-14 on adding DLSS to a UE 4.26 Direct3D 11 game through the official SDK - prior art, SDK facts, engine internals, risks
type: reference
---

Collected by a research pass on 2026-11-14 from public sources. **Nothing here has been run
against the game**, and the items marked UNVERIFIED were not confirmed from a primary source.
Re-read the linked sources before relying on a detail.

## It has been done before

- **Luma Framework** (github.com/Filoppi/Luma-Framework, custom MIT) ships a generic Unreal
  Engine 4 Direct3D 11 mod that replaces the stock TAA pass with DLSS (DLAA) through the
  official NGX D3D11 API and offers presets L and M. Read first:
  `Source/Games/Unreal Engine/main.cpp`, `includes/shader_detect.hpp`,
  `Shaders/Unreal Engine/Luma_MotionVec_UE4_Decode.hlsl`, `Source/Core/dlss/DLSS.cpp`.
  - Finds TAA by scanning shader bytecode for the velocity-decode constants, then checks the
    bound textures (two colour, one depth, one 16-bit velocity).
  - Reads jitter from the View uniform buffer (found by size on Map/Unmap), projection m20/m21.
  - Builds motion vectors with its own pass from depth and `ClipToPrevClip`.
  - Its wiki lists several UE titles as blocked by Easy Anti-Cheat.
- days-gone-dlss (MIT, D3D11) downloads the DLSS SDK at build time instead of committing it.
- The Quarry DLSS (MIT, UE 4.26.2) has a `docs/FINDINGS.md` that documents the TAA pass bindings.

## DLSS SDK (github.com/NVIDIA/DLSS)

- Release 310.9.16 (2026-10-16). Direct3D 11 is still supported: `NVSDK_NGX_D3D11_Init_with_ProjectID`,
  `NGX_D3D11_CREATE_DLSS_EXT`, `NGX_D3D11_EVALUATE_DLSS_EXT`.
- "DLSS 4.5" = second-generation transformer = render presets **L (12)** and **M (13)**, added
  in 310.5.0. Preset K (11) is the first transformer model. Set with
  `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA` and its per-mode siblings.
- L/M on Direct3D 11: UNVERIFIED by NVIDIA; two shipping D3D11 integrations select them.
  Confirm on the game with the DLSS on-screen debug display.
- No NVIDIA application id is needed: use `Init_with_ProjectID`, engine type CUSTOM, a GUID.
- Inputs: colour before tonemapping (set `IsHDR`), UAV-capable output, depth (`DepthInverted`
  for UE's reversed-Z), motion vectors in render-resolution pixels pointing to the previous
  frame, jitter in pixels within -0.5..+0.5, `AutoExposure` (presets L and M ignore an
  exposure texture), `InReset` on camera cuts. NGX preserves the D3D11 immediate context state.

## Licence points that affect this repo

- `nvngx_dlss.dll` may only be distributed as part of an application, not stand-alone; do not
  strip its signature; never ship the development DLL.
- The SDK must not become subject to an open-source licence: keep NVIDIA headers and libraries
  out of the MIT tree and fetch them at build time.
- NVIDIA GPUs only; attribution and NVIDIA marks required in an about box or credits.
- Whether a fan mod counts as an "application" is our reading, not NVIDIA's. The licence gives
  a contact address for questions.

## Unreal Engine 4.26 TAA, as documented from other games (verify on this one)

- Main TAA is a compute shader (`TAAStandalone.usf`, `MainCS`), also reused for SSR, light
  shafts and depth of field, so the main pass must be told apart.
- Bindings seen on a UE 4.26.2 D3D11 game: t0 eye adaptation, t1 scene colour (RGBA16F),
  t2 depth (reversed-Z), t3 velocity (RGBA16 UNORM), t4 stencil, t5 history, u0 output,
  cb0 pass constants, cb1 View uniform buffer.
- Velocity is written only by movable objects; a zero texel means "reproject with
  `ClipToPrevClip`". The cvar that widens this is read-only in a shipped build.
- With `r.TemporalAA.Upsampling=0` (this game) jitter is 8 Gaussian-windowed Halton samples.

## Risks, most serious first

1. **Account risk is real for NetEase games.** In October 2026 NetEase suspended Marvel Rivals
   players who ran DLL-loader mods, then lifted the suspensions while saying such mods stay
   prohibited (press report, primary post not read). No report either way was found for
   Racing Master.
2. **DLAA only.** At the TAA slot input and output are the same size, so this gives better
   anti-aliasing at native resolution, not upscaling. True upscaling needs engine values
   changed in memory, which is more invasive.
3. Motion-vector gaps (wind-animated foliage and similar may ghost).
4. Constant-buffer offsets come from other games; NetEase's engine fork may differ, so they
   must be discovered at run time.
5. NVIDIA can remotely disable a custom project id, and the NVIDIA app override can swap the
   DLL or preset.

**Why:** this turns "weeks of blind reverse engineering" into "adapt a known, public approach",
and puts a concrete precedent behind the ban warning.

**How to apply:** start the DLSS work by reading Luma's generic UE mod end to end. Do not copy
its code without checking its licence terms against this repo's MIT licence.

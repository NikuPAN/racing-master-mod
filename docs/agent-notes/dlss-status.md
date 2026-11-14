---
name: dlss-status
description: What DLSS 4.5 and DLSS 5 require officially as of November 2026 and why neither can simply be switched on for this game
type: reference
---

Checked by web search on 2026-11-14. Sources were mostly tech press, not NVIDIA's developer
documentation, so treat version numbers as **unverified** until read from NVIDIA directly.

- **DLSS 4.5** (second-generation transformer model): the NVIDIA app override only works in
  games that already ship DLSS. This game ships none.
- **DLSS 5** (neural rendering): launched September 2026. Officially GeForce RTX 50 series
  only; RTX 40 support is announced but had no date or driver. Press reports say the game must
  use DirectX 12 and already support DLSS Super Resolution and Frame Generation. This game is
  DirectX 11 with neither.
- **Community "DLSS 5 one-click" tools** inject ReShade plus a leaked, closed-source NVIDIA
  build, and their own installer refuses games with anti-cheat. Not to be used here.
- **OptiScaler** needs a game that already feeds DLSS 2+, FSR 2+ or XeSS. FSR 1 is not an input.

What this means for the mod: DLSS has to be driven by our own code. The hook must find, inside
the engine's Direct3D 11 frame, the jittered scene colour, depth, motion vectors and the
per-frame jitter, and hand them to NVIDIA's official DLSS SDK in place of the engine's TAA pass.

**Why:** The owner expected DLSS 5 to be reachable on their card soon; the gap between "announced" and
"usable in a DirectX 11 game with no DLSS" is the whole project.

**How to apply:** before starting the DLSS work, read NVIDIA's DLSS programming guide for the
Direct3D 11 path and the licence terms for redistributing `nvngx_dlss.dll`.

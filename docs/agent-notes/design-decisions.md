---
name: design-decisions
description: What the owner decided on 2026-11-14 about the shape of the mod, with the alternatives they turned down
type: decision
---

The owner's choices, made with the trade-offs in front of them:

1. **One mod, loaded into the game.** The owner first asked for two separate GitHub projects (music,
   DLSS). After seeing that the game has no plugin slot, they chose "in-game panel only": the
   music player lives inside the same injected DLL as DLSS, as one project. A separate
   desktop overlay app was proposed and turned down ("why always a .NET desktop app if we can
   build a plugin").
2. **Real DLSS 4.5 through a DLL hook**, not a no-injection graphics preset and not parked.
   They accepted: weeks of reverse engineering, breakage on game patches, and ban risk.
3. **DLSS 5 comes after 4.5 works**, as a follow-up.
4. **Music sources:** local files and folders, plus remote control of whatever is playing in
   Spotify, a browser or another player (Windows media session controls).
5. **The game's own music is silenced with the game's music volume slider**, which exists.
6. **Repository:** private under the owner's GitHub account first, public later; MIT; README in
   English and Traditional Chinese.

7. **Panel key: Insert.** Free in the game's bindings (Tab, F11, BackSpace and Delete are
   taken). Must stay changeable in the settings file.
8. **Panel layout: a compact bar in a corner, playlist on demand.** One row for source and
   current track, one for previous / play-pause / stop / next, loop mode and volume, one for
   the seek line and a button that drops the playlist down with "Add files" and "Add folder".
   The larger always-open window was turned down because it covers more of the game.

Conditions I attached and the owner accepted by choosing option 2:

- First test on a throwaway account.
- If the anti-cheat blocks the DLL, the work stops. No bypass will be written.

**Why:** these are taste and risk decisions that belong to the owner; a later agent should not
re-open them or quietly switch back to an external app.

**How to apply:** the repository name `racing-master-mod` is a working name the owner has not
confirmed. Ask before the repo goes public.

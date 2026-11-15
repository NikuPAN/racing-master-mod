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

9. **Hotkeys while racing: F5 stop, F6 previous, F7 play/pause, F8 next, F9 loop mode.**
   Single keys only (Shift, Ctrl and Alt are game controls); all changeable in settings.
10. **Clicks outside the panel go to the game.** The owner turned down "panel owns the mouse
    while open" (the design's recommendation). Clicks on the panel stay on the panel.
11. **Playback when the game is minimised or unfocused is a setting** (keep playing or pause),
    not a fixed behaviour.
12. **Opus is in version 1** (files saved from YouTube are often Opus), alongside MP3, FLAC,
    WAV, OGG Vorbis and M4A/AAC.
13. **The panel's default place is the bottom centre** (2026-11-15, after the first game run of
    0.3.0: top-right covered the game's diamond counter). The four corners stay selectable.
14. **Six fixed places, flush with the screen edges** (2026-11-15, for 0.3.2): top left, top
    centre, top right, bottom left, bottom centre (default), bottom right. The bar's outer edge
    touches the window's edge (no 16-50 px margin); at the centre places only the top or bottom
    edge is flush and the bar is centred. No dragging.
15. **A compact panel** (2026-11-15, for 0.3.2): one row for tight spaces (the strip under the
    game's bottom tabs at bottom centre, about 970 x 70 px at 2558x1360, small and medium text):
    previous, play/pause, next, the song (cut off with "...", never wrapped), elapsed, a short time
    line (seekable when the source allows), length, and a button back to the full panel. Volume,
    repeat, source, list and settings live only in the full panel, which gets a matching button.
    The choice is remembered (`[panel] compact=`) and is also in Settings. Insert shows and hides
    whichever is chosen; F5-F9 unchanged. One window.
16. **The bar never moves** (2026-11-15, for 0.3.2): the panel is two windows, the bar alone and
    the list/settings/status lines beside it, so nothing appearing next to the bar moves it.

Conditions I attached and the owner accepted by choosing option 2:

- First test on a throwaway account.
- If the anti-cheat blocks the DLL, the work stops. No bypass will be written.

**Why:** these are taste and risk decisions that belong to the owner; a later agent should not
re-open them or quietly switch back to an external app.

**How to apply:** the repository name `racing-master-mod` is a working name the owner has not
confirmed. Ask before the repo goes public.

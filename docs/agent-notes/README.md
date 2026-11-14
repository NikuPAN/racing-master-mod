# Agent notes

One fact per file. Machine-specific notes live in `local/` (gitignored).

- [game-facts.md](game-facts.md) — what the game is built on and how it is protected (verified on disk, 2026-11-14)
- [design-decisions.md](design-decisions.md) — decisions the owner made and why
- [dlss-status.md](dlss-status.md) — what DLSS 4.5 and 5 officially require, and what that means here
- [dlss-research.md](dlss-research.md) — prior art (Luma Framework), SDK and licence facts, UE 4.26 TAA internals, risks
- [loader-hardening.md](loader-hardening.md) — what the loader review found in 0.1.0, what 0.2.0 changed, and what was deliberately left alone
- [test-host.md](test-host.md) — how the hidden test host checks the loader and the panel, and the conditions that make a flip-model frame readable
- [music-panel.md](music-panel.md) — the in-game music panel: departures from the design brief, choices for the owner, what is verified and what is not
- [media-remote-design.md](media-remote-design.md) — how src/remote controls other music apps, and the choices that must not be undone
- [media-remote-tests.md](media-remote-tests.md) — the silent fake player, the test guard, the media scenarios and their side effect on the PC
- [media-remote-elevated-game.md](media-remote-elevated-game.md) — open question: media sessions from the elevated game, and the log lines that answer it

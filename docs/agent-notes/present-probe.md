---
name: present-probe
description: The [debug] present_probe diagnostic (0.3.1) for the NVIDIA Smooth Motion blink, what each line of its block means, and what each finding would point to
type: reference
---

**Why it exists.** In the owner's game run of 2026-11-15, with NVIDIA Smooth Motion on (driver
frame generation: `NvPresent64.dll` from the driver store, loaded by `nvldumdx.dll`) the hook
saw 120 presents a second against the game's 60, and the panel blinked as a whole while the
game's own HUD stayed steady. Off, steady. Public sources (the RTSS author on guru3d, Dalamud
PRs goatcorp/Dalamud#2849 and #2881, jlrouzies-fr/DLSS5-Feeder#1, NVIDIA's Linux README on
nvpresent) say Smooth Motion may call Present from its own thread(s), possibly concurrently, and
probably wraps the game's swap chain, so the game's own presents may go to a wrapper whose table
the mod did not patch. None of that is verified for this game. 0.3.1 draws only on the thread
that presents the game's swap chain first and passes the same chain's presents from other
threads straight on (`CLAUDE.md`, "The mod draws on one thread only"), logs one plain line when
`NvPresent64.dll` is in the process, and adds this probe to find out what really happens. That
rule stops the mod drawing from a driver thread while the game's thread presents too; it cannot
help if the game's own presents never reach the hook (finding 3 below), because then the driver's
thread is the only one the mod sees.

**Switch:** `[debug]` `present_probe=1` in the per-user `settings.ini` (default off). Off, the
hook does one load of `g_probeOn` per present and nothing else (run 45 checks that nothing is
recorded and nothing is logged). On, the presenting threads record into fixed tables with
interlocked operations only (`src/core/present_probe.cpp`: no allocation, lock, log or file on
those threads); the worker writes the block every 30 s.

**The block** (worker, every 30 s):

1. `present probe, last 30 s: N presents reached the hook (since start: Present a, Present1 b);
   at most K threads inside the hook at once; threads: <id> x<count> (<outcome> n, ...) ... (T in
   all)`: per thread, what the frame work did with its presents, so the line shows which thread
   the mod draws on ("panel drawn") and which it passes on.
2. One `present probe: called from <module>: Present a, Present1 b; <outcome> n ...` line per
   module the call came from (the return address of the hook, resolved to a module by address
   range). Outcomes: panel drawn, game frame without the panel, another swap chain, passed on
   while another thread drew, frame work off, game frame from a thread the mod does not draw on.
   Calls whose return address is in no module (a trampoline or other generated code, a likely way
   for a driver or an overlay to call Present) get their own `called from code in no module` line
   with outcomes; such addresses are remembered by page, so they are not looked up every present.
3. One `present probe: swap chain <pointer>, n presents: its function table is in <module> (the
   table the mod patched | not ...), and its Present entry points into <module>` line per swap
   chain (up to 4).
4. `present probe: flags: none n, DO_NOT_SEQUENCE n, ...; sync interval: 0 xn 1 xn ...`
5. `present probe: the Present the mod passes frames on to is in <module> (Present1 in
   <module>); the patched table's Present entry now points into <module>; game frames n in the
   last 30 s, Windows counted m presents of the game's swap chain` (from
   `IDXGISwapChain::GetLastPresentCount`, read on the frame thread for the game's chain; the
   first block gives the count itself, later blocks the change since the last one).

**What each finding would mean (read with Smooth Motion on, then off):**

1. **K = 2 or more, or two threads with similar counts** (the game's render thread and a
   driver thread): presents from two threads. The mod draws only on the first; the "game frame
   from a thread the mod does not draw on" count shows how many presents went out without the
   panel. If those are the generated frames and the panel is drawn only on every other one, that
   alone may explain the blink. Do **not** fix that by drawing on the other thread's presents:
   the immediate context is not thread-safe, and drawing from a driver thread while the game's
   thread renders changed the game's state in run 45 (before the one-thread rule). The fix would
   have to draw before the frame reaches the driver's layer (finding 3's hook), or the advice
   stays "Smooth Motion off". **If "panel drawn" appears under a caller other than the game's
   executable (for example `NvPresent64.dll`), the mod is drawing from the driver's thread: treat
   that as a crash risk and tell the owner to keep Smooth Motion off until it is solved.**
2. **The original Present (or the patched table's entry) is in NvPresent64.dll**: hook order.
   The driver patched the same table, before or after the mod. "Points into NvPresent64.dll"
   after the mod's patch means the driver re-patched and now calls us (or skips us); the fix is
   about who wraps whom, not about threads.
3. **The game's executable (`g112-Win64-Shipping.exe`) never appears as a caller, or the
   presents the hook saw on the game's swap chain (its "swap chain" line) are about half of
   Windows' present count for it, or a swap chain line says "not the table the mod patched"**:
   wrapper bypass. ("game frames" counts only presents that ran the frame work, so presents
   passed on from another thread are not in it; compare the swap chain line instead.) The game
   presents to a wrapper object whose own table
   the mod never patched; what reaches the hook is the driver presenting the real chain
   underneath (generated and real frames alike). Fix C from the research: hook swap-chain
   creation (`IDXGIFactory::CreateSwapChain*`) and patch the wrapper's table too, so the panel
   is drawn on the game's frame before the driver's layer sees it.
4. **Only one thread, one caller (the game), the mod's table and the original in dxgi.dll,
   and Windows' count equals the hook's**: Smooth Motion inserts its frames below DXGI where
   the mod cannot see them. The panel is drawn on the real frames and the generated ones are
   interpolated from frames with and without it; nothing in the hook can fix that, so the
   advice stays "turn Smooth Motion off for this game" (the log line says so).
5. **Flags other than none / DO_NOT_SEQUENCE, or sync intervals that differ between callers**:
   tells the game's presents from the driver's (for example the driver presenting with
   interval 0 while the game uses 1); useful to pick which presents to draw on.

**Test host:** run 46 turns the probe on with a 2 s block (`RACING_MASTER_MOD_TEST_PROBE_SECONDS`,
test host only) during two-thread presents (the second thread calls Present through a few bytes
of generated code, so its caller is "code in no module") and checks every line above for sane
values, including that the per-thread outcomes show the panel drawn on the first thread and the second
thread's presents passed on;
`RACING_MASTER_MOD_TEST_SMOOTH_MOTION_DLL` points the Smooth Motion check at a DLL the host does
load. The real `NvPresent64.dll` is never copied or loaded here.

**How to apply:** ask the owner for `mod.log` from a run with Smooth Motion on and
`present_probe=1`, read the block against the list above, and decide the fix from the finding.
Keep the probe off by default and its frame-thread side free of allocation, locks and logging.

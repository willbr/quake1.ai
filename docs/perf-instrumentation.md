# Perf instrumentation

_Extracted from CLAUDE.md (reference detail; CLAUDE.md keeps a summary + pointer here)._

## Perf instrumentation

`sdlquake/engine/perf.{c,h}` — scoped per-frame timers feeding both a live
overlay graph and an offline capture format.

- `PERF_SCOPE("name") { ... }` wraps a block; `Perf_PushScope`/`PopScope` for
  free-form pairs. Names must be string literals (stored by pointer).
- Engine side: `_Host_Frame` (root), `input`, `Cbuf_Execute`, `server`,
  `SV_Physics.edicts`, `dll_overlays`, `SCR_UpdateScreen`, `V_RenderView`
  (with `R_SetupFrame` / `R_EdgeDrawing` / `R_RenderWorld` / `R_ScanEdges` /
  `R_DrawBEntities` / `R_DrawEntitiesOnList` / `R_DrawParticles` /
  `R_DrawViewModel_2D` children), `VID_Update` (with `palette_expand` /
  `SDL_present` children), `S_Update`, `ImguiLayer_Render`,
  `CL_ReadFromServer`.
- Game DLL side (across the ABI via `eng->Perf_PushScope` —
  `GAME_API_VERSION` bumped to 26): `game_dll.start_frame` →
  `Sim_Frame` (→ `Sim_Retrofit`, `Sim_AI`, `Wind`, `Sim_Arena`),
  `Spike_GibPathScan`, `Missile_SmokeWake`, `StartFrame`. Use the
  `SIM_PERF("name")` macro inside `sdlquake/game/` to add more.
- **Live overlay during play**: `showperf 1` (cvar) renders the Perf panel
  as a non-interactive HUD over the running game.
- **Live profiling inside F3** (`perf_live` cvar, default on): the F3 dev
  overlay used to freeze the profiler *and* the game sim on open, so the
  graphs never updated. With `perf_live` on, opening F3 keeps the profiler
  ticking and the host keeps running the sim (a `Perf_Live()` bypass added to
  the two `SV_Physics` gates in `host.c`, suppressed inside the F2 editor),
  so the flame graph shows real gameplay load. The Profile panel's
  **Play/Pause** button flips `perf_live` live — Pause freezes both for
  hovering/scrubbing. `perf_live 0` restores the classic freeze-on-open.
  Note: with it on, the world simulates under *all* F3 panels (the passive
  player can take damage while the overlay is up).
- **`devoverlay` console command**: console twin for the F3 toggle (the key
  is otherwise the only path) — `devoverlay` toggles, `devoverlay 0|1` forces
  closed/open (idempotent; MCP/headless/cfg-friendly). Refuses while the F2
  editor owns the overlay. Registered from `ImguiLayer_Init`.
- The panel shows smoothed FPS, last-256-frames frametime sparkline, and a
  flame graph of the most recent frame (hover for ms / start / depth).
- Console: `profile <n>` captures `n` frames to `profiles/perf_<ts>.json`
  (Chrome trace, drop into chrome://tracing or speedscope.app) plus
  `perf_<ts>_summary.json` (per-scope avg/p50/p95/max/calls). The argument
  also accepts a wall-clock duration (`profile 10s` = 10 seconds) or
  `profile level` to capture until the current level ends (map change /
  disconnect / intermission, whichever comes first; auto-stops if the event
  buffer fills on a very long level). `profile level` works from the command
  line — `quake +map e1m1 +profile level` defers until the map finishes
  loading, then captures a clean per-level trace.
- **Record button** (Profile panel): UI twin for `profile` — Record/Stop
  + a duration combo (`1s/10s/30s/60s/120s/level`). Starting forces the profiler live (`Perf_SetLive(1)`)
  and exits replay first, so it captures even from a paused state (a paused
  profiler records nothing — the bare console command stalls there). On
  completion the trace auto-loads into replay for immediate inspection.
  `Perf_StopCapture()` finalizes early.
- `scripts/perf_diff.py old_summary.json new_summary.json` diffs two
  captures for regression tracking.

Captures can be loaded back into the live flame graph: pick a file from
the Profile window's "Source" combo (or `perf_replay <path>` from the
console). The frametime histogram above the flame graph colours each
frame green / yellow / red by ms — click the worst bar to jump straight
to that frame. The FPS sparkline stays live so you can compare against
real-time engine load.

Below the flame graph is the **aggregate table** — per-scope totals over
the whole capture (replay mode) or the live frame ring (last 256 frames):
columns are `scope / calls / total ms / avg ms / max ms / % window`.
Sort combo above the table cycles total / avg / max / calls / name. Use
it to find scopes that are individually cheap but consistently expensive
(e.g. `palette_expand` at 5 ms × 60 fps = 30% of frame time, which
wouldn't stand out in any single frame's flame graph).


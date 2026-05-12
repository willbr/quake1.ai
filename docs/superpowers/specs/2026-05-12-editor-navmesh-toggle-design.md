# Editor navmesh render toggle — design

## Goal

Expose the existing navmesh debug draw as a checkbox in the editor toolbar so
the author doesn't have to drop to the console and type `sim_nav_debug 1`, and
have it work *while the editor is open* — which is the only use case that
matters.

## Background

- The game DLL (`sdlquake/game/sim/sim_nav.c`) registers a cvar
  `sim_nav_debug` in `Sim_Nav_Init`. When set, `Sim_Nav_Frame` submits the
  navmesh edges as `SV_DebugLine` calls each frame.
- `SV_DebugLine` lands in the engine's `DebugLines_Add` ring;
  `DebugLines_Draw` blits all queued lines into `vid.buffer` from
  `VID_Update` and clears the queue.
- `Sim_Nav_Frame` was originally called from `Sim_Frame` (in `sim_main.c`),
  which in turn is invoked via `game_api->start_frame` from `SV_Physics`.
- `SV_Physics` is gated by `if (!sv.paused && !Editor_IsPaused()) ...` in
  `host.c`. Opening the editor pauses the sim, so the entire `Sim_Frame`
  chain stops ticking — including the navmesh debug-line submission. Result:
  the checkbox did nothing while the editor was open (the only place it can
  be clicked), but lines reappeared once the editor was closed and the sim
  resumed. This was the bug.

## Change

Two parts:

1. **UI** — `sdlquake/engine/editor/editor_ui.c`: add a `navmesh` checkbox in
   the toolbar row alongside `angles` / `links`. Reads
   `Cvar_VariableValue("sim_nav_debug")`, writes `sim_nav_debug 0|1` via
   `Cbuf_AddText`. Same pattern as the existing toggles.

2. **Rendering** — bypass the sim-pause gate by giving the game DLL a
   per-render-frame hook:

   - `sdlquake/game/game_api.h`: bump `GAME_API_VERSION` 10 → 11; add
     `void (*debug_draw_overlays)(void);` to `game_api_t`.
   - `sdlquake/game/game_main.c`: add `game_debug_draw_overlays` thunk that
     calls `Sim_Nav_Frame()`; wire it into the `s_api` initializer.
   - `sdlquake/game/sim/sim_main.c`: remove the `Sim_Nav_Frame()` call from
     `Sim_Frame` so lines aren't submitted twice when both paths run.
   - `sdlquake/engine_src/host.c`: invoke
     `g_game_api->debug_draw_overlays()` just before `SCR_UpdateScreen()` in
     `_Host_Frame`. Guarded by a NULL check on `g_game_api` and on the
     callback pointer (older DLLs would still rejected by version mismatch,
     but the check is cheap).

The new hook is general — future overlays (AI cones, stim hits, patrol
arrows) can hang off the same callback rather than each acquiring its own
plumbing.

## What is explicitly NOT in scope

- An editor-side rendering path that walks the navmesh and draws via
  `Editor_DrawLine3D`. The chosen approach keeps the existing
  `Sim_Nav_Frame` body unchanged; only its caller and call-site change.
- Per-overlay toggle (the whole `debug_draw_overlays` is a single fan-out;
  each overlay inside it self-gates on its own cvar, as `Sim_Nav_Frame`
  already does).
- Custom colour, depth-sorted draw, or map-view-only behaviour.

## Verification

- Build succeeds: `zig build`.
- In-editor (sim previously paused): open editor with a map loaded; click
  `navmesh`; sky-blue navmesh edges appear in the 3D viewport immediately
  and persist across editor open/close transitions.
- Console parity: `sim_nav_debug 0` / `1` from the console flips the
  checkbox state on next frame.
- Hot-reload sanity: rebuilding `game.dll` keeps the new ABI version intact;
  loader rejects pre-bump DLLs.

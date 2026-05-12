# Editor navmesh render toggle — design

## Goal

Expose the existing navmesh debug draw as a checkbox in the editor toolbar so
the author doesn't have to drop to the console and type `sim_nav_debug 1`.

## Background

- The game DLL (`sdlquake/game/sim/sim_nav.c`) registers a cvar
  `sim_nav_debug` in `Sim_Nav_Init`. When set, `Sim_Nav_Frame` submits the
  navmesh edges as `SV_DebugLine` calls each frame.
- `SV_DebugLine` lands in the engine's `DebugLines_Add` ring; `DebugLines_Draw`
  blits all queued lines into `vid.buffer` from `VID_Update` once per frame.
- `Sim_Nav_Frame` is called from `Sim_Frame` in `sim_main.c`, which only ticks
  while the sim is running (live view or playing). In editor "map" view, no
  navmesh is drawn — accepted as a caveat for this change.
- The editor toolbar already has the pattern for cvar-backed checkboxes:
  `editor_show_angles` and `editor_show_links` in `editor_ui.c` around lines
  268–293.

## Change

Add a single checkbox to the editor toolbar that mirrors and drives
`sim_nav_debug`.

- File touched: `sdlquake/engine/editor/editor_ui.c`.
- Label: `navmesh`.
- Position: same row/group as `angles` and `links`.
- Read state from `Cvar_VariableValue("sim_nav_debug")`.
- Write state on toggle via `Cbuf_AddText("sim_nav_debug 0|1\n")`.
- No new cvar; no new engine API; no game-DLL change; no ABI bump.

That's the whole change — ≈ 12 lines.

## What is explicitly NOT in scope

- Editor-side navmesh rendering path (would require a game_api accessor and
  ABI bump). The toggle works only when the sim is running.
- Custom colour, depth-sorted draw, or per-node markers.
- Map-view rendering of the navmesh.

If any of those become wanted later, they can be added without revisiting this
change.

## Verification

- Build succeeds: `zig build`.
- In-editor: open the editor with a map loaded; the new `navmesh` checkbox
  appears next to `angles` / `links`; toggling it shows/hides the sky-blue
  navmesh edges in the 3D viewport.
- Console parity: typing `sim_nav_debug 1` / `0` flips the checkbox state on
  next frame (and vice versa).

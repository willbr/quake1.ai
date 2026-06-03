# Actor-editor Viewport Translate Gizmo (MVP) — Design

**Date:** 2026-06-03
**Status:** Design approved. Next: implementation plan (writing-plans).
**Scope:** First slice of direct-manipulation editing in the Actor editor's
Viewport — a 3-axis **translate** gizmo on a **click-selected part**.
Rotate/scale, joints, and snapping are explicitly deferred to later slices.

## Why

The Actor editor authors geometry through numeric `DragFloat3` fields and their
console twins (`actor_edit_move/scale/rot`). That works and is verifiable, but
direct manipulation in the 3D view is the natural authoring gesture. A viewport
gizmo was attempted once (commit `bbf9af7`) and reverted: the 3D overlay lines
didn't render over the **lit** actor view (the world filled the framebuffer +
z-buffer), and the drag was mouse-only / unverifiable.

Two things changed that make a retry tractable:

1. **Hide-world backdrop** (commit `c12ce39`): Actor mode now previews the actor
   against the same cleared backdrop + grid the Particle editor uses. The
   particle grid renders fine there via the z-tested `Editor_DrawLine3D` path —
   i.e. the exact overlay path that was "absent over the lit actor view" now has
   a proven home. The first build step re-tests this hypothesis directly.
2. **Accumulator insight**: part edits flow through per-part accumulators
   `s_voff` / `s_vscale` / `s_vrot`, which `edit_rebuild()` applies every frame.
   The numeric fields *and* the console commands both just write these. A gizmo
   can write the **same** `s_voff` — so it adds only an input + overlay layer,
   and the transform itself stays verified through `actor_edit_move`.

## Locked decisions (brainstorm outcomes)

| Decision | Choice | Why |
|---|---|---|
| Transforms | **Translate only** | Smallest slice that proves the overlay end-to-end; highest-value 3D gesture |
| Targets | **Parts only** (not joints) | Parts use the simple accumulator path; joints (world↔actor delta) are a later slice |
| Selection | **Click part in viewport** (ray-vs-triangle) | Native feel; sets `s_selmesh` |
| Structure | **Bespoke gizmo in `edit_actor.c`** (Approach A) | `gizmo.c` is `edit_entity_t`-coupled; reusing it is a big refactor (YAGNI) |
| Handles | **3 axis arrows** (X red / Y green / Z blue), `Editor_DrawLine3D` | Matches the map-editor idiom; the proven z-tested line path |
| Snapping | **None** in MVP | Free drag; snapping is later polish |
| Overlay fallback | **ImGui draw-list on the panel rect** (Approach C) | Only if the engine-line overlay still won't render (step-1 gate) |

## Architecture

Lives entirely in `sdlquake/engine/editor/edit_actor.c` by populating the two
currently-unset `actor_mode` vtable slots (`render_scene`, `process_event`); no
changes to `gizmo.c`, the engine render order, or any ABI.

**Active-gate:** editor open AND Actor mode AND Edit-geometry on (`s_editmode`)
AND a valid `s_selmesh`. Outside that, both slots no-op (today's behavior is
preserved exactly).

### Components

1. **Handle overlay — `render_scene`** (runs post-entity, on the hide-world
   backdrop).
   - Compute the selected part's centroid in model space (mean over its vertex
     range in `s_edit`), add the entity origin (orbit focus) → world centroid.
     Entity angles are 0, so world = model + focus (no rotation math).
   - Draw three axis arrows from the centroid, length scaled to camera distance
     (readable at any zoom), via the z-tested `Editor_DrawLine3D`: +X red, +Y
     green, +Z blue (reuse `EDIT_COLOR_AXIS_*`).
   - Optional selected-part cue (bbox or tint) — nice-to-have, not required.

2. **Picking — `process_event`, LMB-down over the Viewport panel.**
   - `Editor_ScreenToRay(sx,sy)` → world ray. Subtract focus → actor/model
     space.
   - First test the ray against the three axis handles (distance-to-segment
     within a pixel/world threshold). A hit → begin an axis drag (component 3),
     do **not** re-pick.
   - Otherwise ray-vs-triangle across **all** parts' triangles in `s_edit`;
     nearest hit sets `s_selmesh`. Miss → keep the current selection.

3. **Drag — `process_event`, LMB-move while a handle drag is active.**
   - Project the current mouse ray onto the active axis line (ray-vs-axis-plane,
     the `gizmo.c::ray_vs_axis_plane` pattern) → a point on the axis → world
     delta since the previous move along that axis.
   - Add the delta into `s_voff[s_selmesh]` for the active axis. `edit_rebuild()`
     (already called every frame in the geometry editor) applies it live.
   - LMB-up ends the drag.

4. **Console twin — `actor_edit_pick <sx> <sy>`.**
   - Runs the same ray-pick at the given window coords and prints the hit part
     index/name (or "miss"). Makes the picking logic MCP-verifiable without a
     mouse.

### Input gating & coordinates

- LMB events act only when the press is over the Viewport panel — reuse the
  existing viewport-rect mapping (`window_to_vid` via the panel image rect) and
  the `Editor_MouseOverViewport` / `WantCaptureMouse` exemption already used for
  editor picking. Presses that begin over an ImGui panel are ignored (mirrors the
  orbit camera's `editor_view_captured()` guard).
- RMB orbit is polled in `editor_orbit_camera` and is untouched; LMB is otherwise
  unused, so there is no button conflict.
- `Editor_ScreenToRay` expects super-pixel / vid coords; convert window→vid
  through the same path picking already uses.

## Data flow

```
LMB down (over viewport)
  └─ Editor_ScreenToRay → ray (world) → −focus → model space
       ├─ hits an axis handle? → start drag on that axis
       └─ else ray-vs-tri over all parts → set s_selmesh
LMB move (dragging)
  └─ ray-vs-axis-plane → delta → s_voff[s_selmesh][axis] += delta
       └─ edit_rebuild() (per frame) → s_edit verts → live render
LMB up → end drag
render_scene (every actor-edit frame)
  └─ draw 3 axis arrows at world centroid of s_selmesh
```

## Build order (de-risk first)

1. **Overlay smoke test.** Implement only `render_scene` drawing the axis handles
   for `s_selmesh`. `screenshot_gpu` to confirm the lines render on the backdrop
   over the actor. **Gate:** if absent, switch to the ImGui draw-list fallback
   before proceeding.
2. **Pick.** Add ray-vs-triangle + the `actor_edit_pick` console twin. Verify via
   MCP: `actor_edit_pick` at known coords returns the expected part; selecting
   via `actor_edit_sel` + screenshot shows the handles move to that part.
3. **Drag.** Add axis-handle hit-test + drag → `s_voff`. Verify the apply path
   with `actor_edit_move` (already works) + screenshot; the user confirms the
   live click + drag.

## Verification

| Piece | How | By |
|---|---|---|
| Handles render on backdrop | `screenshot_gpu` | me |
| Pick logic | `actor_edit_pick <sx> <sy>` via MCP | me |
| Drag *effect* (accumulator → mesh) | identical to `actor_edit_move` + screenshot | me |
| Live click + drag gesture | in-game, at the machine | user |

Launch flags per project convention: `-nosound -nofocus`, MCP HTTP transport.

## Out of scope (later slices)

Rotate + scale handles; joint gizmo (`edit_joint_move`, needs world↔actor delta);
plane-drag (2-axis) handles; grid / increment snapping; multi-part selection;
gizmo for Map/Particle modes.

## References

- Reverted attempt: commit `bbf9af7` (groundwork kept: `Editor_ScreenToRay`,
  `Editor_WindowToVid`, `actor_edit_sel`).
- Hide-world backdrop that unblocks the overlay: commit `c12ce39`.
- Map-editor gizmo (structural template, **not** reused): `gizmo.c`
  (`Editor_GizmoDraw/MouseDown/MouseMove/MouseUp`, `ray_vs_axis_plane`).
- Accumulator apply path: `edit_actor.c::edit_rebuild` (consumes `s_voff` /
  `s_vscale` / `s_vrot`).
- Actor editor lineage: `docs/superpowers/specs/2026-06-01-skeletal-actors-design.md`,
  `docs/superpowers/specs/2026-06-02-skeletal-actors-e2-animation-timeline.md`.

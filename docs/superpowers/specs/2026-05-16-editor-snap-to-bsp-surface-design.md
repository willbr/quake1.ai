# Editor Snap-to-BSP-Surface — Design Spec

**Date:** 2026-05-16
**Status:** Shipped

---

## Overview

Add a surface-snap mode to the in-game 3D map editor's translate gizmo. While a translate handle (axis arrow, planar square, or face-resize handle) is being dragged, if the cursor ray hits a world-BSP face or another editor brush face, the selection snaps so that its bbox **face nearest the surface normal** lies flush against that surface — projected back onto the handle's axis/plane constraint so only the constrained coords change.

One new cvar, three new helpers in `gizmo.c`, and a handful of integration points in the existing drag handlers. No new gizmo handle is added; behaviour layers on top of the existing arrows/squares/face handles.

---

## Cvar

| Cvar | Default | Meaning |
|---|---|---|
| `editor_snap_surface` | `"1"` | When non-zero, translate drags that have a BSP/brush face under the cursor snap the selection to that surface. |

Registered in `gizmo.c` alongside `editor_grid_snap` / `editor_grid_size` / `editor_grid_absolute`. UI toggle goes in the toolbar's existing Snap menu next to the grid-snap controls.

---

## Semantics

### Drag-mode behaviour (when `editor_snap_surface != 0` and a surface is hit)

| Drag mode | Snap behaviour |
|---|---|
| **Axis-translate** (X/Y/Z arrow) | Cursor's BSP-hit coord on the dragged axis becomes the new coord. Other two axes unchanged. |
| **Planar-translate** (YZ/XZ/XY square) | Cursor's BSP-hit `u` and `v` coords become the new u/v. The constrained-out axis stays put. |
| **Face-resize** (brush face push) | If `|dot(hit_normal, face_normal)| > 0.95` (surfaces ≈ co-planar in orientation), the face's `plane.dist` is snapped so the dragged face lies in the hit surface's plane (the dragged face itself lands on the hit surface — no bbox offset, because face-resize is moving a single plane, not a whole bbox). Otherwise falls through to the existing grid-snapped offset. |
| **Axis-rotate** (rotation ring) | Unchanged — surface snap doesn't apply to rotation. |

The constraint-projection rule (axes that aren't dragged stay put) preserves the user's "only X moves" mental model. Without it, axis-arrow drags would surprise-jump the other two axes whenever the cursor crosses a wall.

### Bbox offset along normal

The snapped coord is offset along the hit normal so that the selection's bbox face nearest the surface lands flush:

```
offset_along_normal = projection_of_centroid_to_face_nearest_normal(bbox, normal)
target_along_normal = hit_point_along_normal + offset_along_normal
```

Concretely, for a monster bbox `[-16,-16,-24]..[16,16,32]` dropped on a floor (normal `+Z`):
- Bbox centroid Z = `(−24 + 32) / 2 = 4` (relative to entity origin)
- Distance from centroid to `−Z` face = `28`
- Floor Z (hit) + 28 = new entity Z. Monster's feet (`−Z` bbox face) land on the floor.

For a wall (normal `+X`), the bbox's `−X` face touches the wall. The math is the same in any axis direction.

### Surface-snap overrides grid-snap

When a surface hit is active during a drag, grid snap is **skipped** on the constrained axes. Surface contact is the user-visible behaviour; grid alignment would push the bbox a few units off the surface and visibly break that contact. Grid snap still applies on unconstrained axes (in axis-arrow drags) — those coords aren't touched by surface snap and follow the existing path.

### Surface-snap fall-through

When the cursor ray misses every surface, or when `editor_snap_surface == 0`, the drag falls through to today's behaviour (axis-line closest-t-on-ray + grid snap). No visible change.

---

## Components

### New helpers (in `gizmo.c`)

#### `selection_bbox(vec3_t out_mins, vec3_t out_maxs)`

Combined world-space bbox of the current selection:

- **Brush items:** `b->mins / b->maxs` (maintained by `Brush_Translate` / compile).
- **Point entity items:** prefer `e->live_ent->v.absmin/absmax` when the edict exists and is non-free (gives the actual gameplay hull). Else fall back to a per-classname size lookup. The lookup re-uses the wire-bbox sizes already used by `Editor_DrawLightGizmos` / wire-bbox draw — extract that into an `Editor_EntityBBox(e, mins, maxs)` helper in `render_wire.c`.
- **Multi-select:** union of all per-item bboxes.

Returns 1 on success, 0 if the selection has no resolvable bbox (empty `func_group` etc.) — drag falls through to the no-snap path in that case.

#### `surface_offset_along_normal(const vec3_t mins, const vec3_t maxs, const vec3_t normal)`

Returns the signed distance from the bbox centroid to the bbox face whose outward normal is most opposite to `normal`. For axis-aligned `normal = (0,0,1)`, returns `(maxs[2]-mins[2])*0.5` (centroid → −Z face distance). For an oblique `normal`, projects all 8 bbox corners onto `-normal` and returns the max — the corner that sticks out the most against the surface.

#### `surface_hit_for_drag(float sx, float sy, vec3_t out_hit, vec3_t out_normal)`

Wraps `Editor_RaycastForPlacement` but excludes the currently-dragged selection from the editor-brush face pass so a brush being moved doesn't snap to its own faces (or to faces of other brushes in a multi-select group).

Implementation: copy the body of `Editor_RaycastForPlacement` from `render_wire.c` into `gizmo.c` (or factor a `_Ex` variant with a "skip these (e_idx, b_idx) pairs" callback). The world-BSP trace doesn't need exclusion; you can't drag world BSP.

### Integration points (in `gizmo.c`)

#### `Editor_GizmoMouseMove` — axis-translate branch

After computing `t_now` / `raw_offset` the existing way, check surface snap:

```c
if (editor_snap_surface.value != 0.0f) {
    vec3_t hit, n;
    if (surface_hit_for_drag(sx, sy, hit, n)) {
        vec3_t mins, maxs;
        if (selection_bbox(mins, maxs)) {
            float off = surface_offset_along_normal(mins, maxs, n);
            float target = hit[s_drag_axis] + n[s_drag_axis] * off;
            raw_offset = target - s_drag_origin[s_drag_axis];
            // Bypass grid snap: surface contact wins on this axis.
            float step = (raw_offset) - s_drag_applied;
            if (step == 0.0f) return;
            s_drag_applied = raw_offset;
            // ... apply_translate_delta(delta) with delta[s_drag_axis] = step
            return;
        }
    }
}
// existing path: compute_snapped_offset(raw_offset, s_drag_origin[axis]) → step
```

#### `Editor_GizmoMouseMove` — planar-translate branch

Same shape. With `u = (s_drag_planar_axis + 1) % 3`, `v = (s_drag_planar_axis + 2) % 3`:

```c
if (editor_snap_surface.value != 0.0f) {
    vec3_t hit, n;
    if (surface_hit_for_drag(sx, sy, hit, n)) {
        vec3_t mins, maxs;
        if (selection_bbox(mins, maxs)) {
            float off    = surface_offset_along_normal(mins, maxs, n);
            float t_u    = hit[u] + n[u] * off;        // absolute target u
            float t_v    = hit[v] + n[v] * off;        // absolute target v
            float snap_u = t_u - s_drag_origin[u];
            float snap_v = t_v - s_drag_origin[v];
            float step_u = snap_u - s_drag_planar_applied[u];
            float step_v = snap_v - s_drag_planar_applied[v];
            if (step_u != 0 || step_v != 0) {
                s_drag_planar_applied[u] = snap_u;
                s_drag_planar_applied[v] = snap_v;
                vec3_t delta = {0,0,0};
                delta[u] = step_u; delta[v] = step_v;
                apply_translate_delta(delta);
            }
            return;
        }
    }
}
// existing path: ray_vs_axis_plane + compute_snapped_offset on raw_u/raw_v
```

The unconstrained axis (`s_drag_planar_axis`) keeps its value because `delta[n]` stays 0 throughout.

#### `Editor_GizmoMouseMove` — face-resize branch

```c
if (editor_snap_surface.value != 0.0f && s_drag_plane_idx >= 0) {
    vec3_t hit, n;
    if (surface_hit_for_drag(sx, sy, hit, n)) {
        if (fabsf(DotProduct(n, s_drag_dir)) > 0.95f) {
            float target_dist = DotProduct(hit, s_drag_dir);
            float snapped_offset = target_dist - s_drag_start_dist;
            float step = snapped_offset - s_drag_applied;
            if (step != 0.0f) {
                s_drag_applied = snapped_offset;
                Brush_TranslateFace(b, s_drag_plane_idx, step);
            }
            return;
        }
    }
}
// existing path
```

The 0.95 dot-product gate (≈18° tolerance) prevents accidentally trying to snap a face to a perpendicular wall (where "co-planar" doesn't mean anything).

---

## Edge cases

- **Selection is the only thing under the cursor.** Excluded from the brush-face pass by `surface_hit_for_drag`. If the cursor ray misses everything else, surface snap doesn't engage and the drag falls through to grid-snap as today.
- **Multi-select with mixed brushes + point entities.** `selection_bbox` returns the union; one offset applies to the whole group. Brush items move via `Brush_Translate(delta)`, entity items via `Entity_TranslateOrigin(delta)` — both already handled by `apply_translate_delta`.
- **Empty `func_group` or unresolved selection.** `selection_bbox` returns 0; surface-snap branch is skipped, drag falls through.
- **Cursor-ray misses everything but selection has been near a surface.** No memory — the snap is per-frame. Once the cursor leaves the surface, the drag returns to constrained axis math (using `closest_t_line_ray` from the current mouse ray). This can make the selection visibly jump when the cursor crosses the silhouette edge of a wall; acceptable, and the user can disable snap with the cvar to avoid it.
- **`Brush_TranslateFace` collapsing a face.** Existing concern (face-resize today already protects against degenerate windings). Surface snap doesn't introduce new degeneracy paths — it just sets `step` to a different magnitude.
- **Rotated brushes.** Brush bbox stays axial; `selection_bbox` for a rotated brush is the axial bbox of the rotated verts, which is what `b->mins/b->maxs` already holds.
- **`live_ent` bbox isn't yet populated.** New entities placed in the editor get `live_ent` populated on next server frame — but during placement the drag uses the per-classname fallback. After the first sim tick, future drags will use the more accurate `absmin/absmax`. Acceptable lag.

---

## UI

One row in the toolbar Snap menu, alongside the existing grid-snap and rotate-snap rows:

```
[x] Snap to surface          (cvar: editor_snap_surface)
```

A checkbox bound to `editor_snap_surface` via the existing cvar-checkbox helper used by `editor_grid_snap`. No size/offset controls — the offset is derived from the selection bbox.

---

## Out of scope

- **"Drop to surface" command** (one-shot trace-down). Useful future addition but tracked separately — the gizmo-drag flow covers the interactive case.
- **Vertex/edge snap** between brushes (e.g., snap brush vert to nearest BSP vert). Different feature; would need a separate pick path.
- **Surface-aware rotation** (rotate selection to align with hit-surface normal). Possible future but not in this spec.
- **Snap to invisible BSP faces** like clip brushes. Today's `Editor_RaycastForPlacement` traces the world hull (hull 0 — solid surfaces), which is correct: clip brushes don't appear in that hull and won't accidentally snap to.

---

## Test plan

Verification is manual + visual — no test suite in the project.

1. `zig build run -- +map e1m1`, F2 to open editor.
2. Select an `info_player_start`. Drag the Z arrow downward — should drop onto the floor at the cursor, feet flush.
3. Drag the X arrow toward a wall — should hit the wall with the entity's `−X` bbox face flush against it; Y and Z stay put.
4. Drag the YZ planar square across a floor — should slide along the floor; X stays put.
5. Select a brush. Use the face-resize handle to push a face toward a parallel wall — should snap when the face plane reaches the wall plane.
6. `editor_snap_surface 0` — repeat (2)/(3)/(4); all behaviour returns to today's grid-snap-only.
7. Multi-select a monster + a light. Drag onto a ledge — both items move together, the lower one's bbox face lands on the ledge (whichever has the larger normal-projection offset wins by way of the union bbox).
8. Drag a brush whose face is part of the snap candidate set — confirm it doesn't snap to itself (selection-exclusion working).

# Editor Rotation Snap — Design Spec

**Date:** 2026-05-11  
**Status:** Approved

---

## Overview

Add angle snap to the in-game 3D map editor's rotation gizmo, mirroring the existing translation snap system exactly. Three new cvars, a matching toolbar UI block, and snap math applied in the rotation drag path of `gizmo.c`.

---

## Cvars

Three new cvars registered alongside the existing translate-snap cvars in `gizmo.c`:

| Cvar | Default | Meaning |
|---|---|---|
| `editor_rotate_snap` | `"1"` | Toggle rotation snap on/off |
| `editor_rotate_snap_size` | `"45"` | Snap granularity in degrees |
| `editor_rotate_snap_absolute` | `"1"` | Absolute (world-aligned) vs relative (delta) snap |

Registered in `gizmo.c` alongside `editor_grid_snap`, `editor_grid_size`, `editor_grid_absolute`.

---

## Snap Modes

Both modes track a running `s_rotate_total_raw` — the total accumulated raw angle since drag-start (reset to 0 in `Editor_GizmoMouseDown`). This mirrors how translate snap accumulates `raw_offset` rather than snapping per-frame deltas (which would be jerky and imprecise).

**Relative** (`editor_rotate_snap_absolute = 0`):  
Snap the total accumulated drag angle to the nearest multiple of the snap size.  
`snapped_total = snap_to_grid(s_rotate_total_raw, snap_rad)`  
Result: rotation advances in fixed steps from wherever the drag began.

**Absolute** (`editor_rotate_snap_absolute = 1`):  
Snap `entity_start_angle + s_rotate_total_raw` to the nearest world-aligned multiple.  
`snapped_total = snap_to_grid(entity_start_angle_rad + s_rotate_total_raw, snap_rad) - entity_start_angle_rad`  
Result: rotation always lands on 0°, 45°, 90°, etc. in world space regardless of starting angle. `entity_start_angle_rad` is the entity's yaw/pitch/roll at the moment the drag begins, cached per-drag in a new `s_rotate_start_angle` variable. For multi-entity selections, all entities use the same `s_rotate_total_raw` snapped value (the snap is applied to the shared drag angle, not per-entity world angles).

Both modes use `snap_to_grid` already in `gizmo.c` (lines 44–50). A second static `s_rotate_total_applied` tracks how much snapped angle has already been applied so each frame only applies the incremental difference.

---

## Angle Presets

Seven presets in the toolbar dropdown, matching the pattern of `grid_values[]` in `editor_ui.c`:

```c
static const float rotate_snap_values[] = { 5, 10, 15, 22.5f, 30, 45, 90 };
```

Default selection: 45°.

---

## Toolbar UI

Add a rotation snap row to the toolbar in `editor_ui.c`, directly below (or adjacent to) the existing translate snap row (lines 269–307). The layout mirrors translate snap exactly:

```
[rsnap ☑]  [abs ☑]  [45° ▾]
```

- `rsnap` checkbox — toggles `editor_rotate_snap`
- `abs` checkbox — toggles `editor_rotate_snap_absolute` (greyed out when rsnap off)
- Angle dropdown — `editor_rotate_snap_size` preset picker using `rotate_snap_values[]`

Label column width and widget sizing match the existing snap row for visual alignment.

---

## Gizmo Changes (`gizmo.c`)

The rotation drag path lives in `Editor_GizmoMouseMove()` at lines 752–810. Currently the raw per-frame cursor delta is applied directly via `entity_apply_rotation_delta()` and `Brush_Rotate()`.

**New static state** (alongside existing `s_drag_applied` for translate):

```c
static float s_rotate_total_raw;     // accumulated raw angle since drag-start (radians)
static float s_rotate_total_applied; // total snapped angle already applied (radians)
static float s_rotate_start_angle;   // entity's world angle at drag-start (radians, absolute mode only)
```

Reset all three to 0.0f when a rotation drag begins in `Editor_GizmoMouseDown`.

**Per-frame snap logic** (replaces direct application of `raw_delta`):

```c
// raw_delta = per-frame cursor angle change (already computed via wrap_delta)
s_rotate_total_raw += raw_delta;

float apply_delta;
if (editor_rotate_snap.value) {
    float snap_rad = editor_rotate_snap_size.value * (M_PI / 180.0f);
    float snapped_total;
    if (editor_rotate_snap_absolute.value) {
        snapped_total = snap_to_grid(s_rotate_start_angle + s_rotate_total_raw, snap_rad)
                        - s_rotate_start_angle;
    } else {
        snapped_total = snap_to_grid(s_rotate_total_raw, snap_rad);
    }
    apply_delta = snapped_total - s_rotate_total_applied;
    s_rotate_total_applied = snapped_total;
} else {
    apply_delta = raw_delta;
}
// Pass apply_delta to entity_apply_rotation_delta() and Brush_Rotate()
```

`snap_to_grid` is already defined at `gizmo.c:44–50` — no new helper needed. `s_rotate_start_angle` is populated from the primary selected entity's yaw (in radians) when the drag begins.

---

## Affected Files

| File | Change |
|---|---|
| `sdlquake/engine/editor/gizmo.c` | New cvars; snap logic in rotation drag path |
| `sdlquake/engine/editor/editor_ui.c` | New toolbar row for rotate snap controls |

No changes to `edit_scene.c`, `editor.h`, `editor_internal.h`, or `game_api.h`.

---

## Out of Scope

- Keyboard shortcut to toggle rotate snap (can be added later)
- Per-axis snap sizes (one size applies to all three rotation axes)
- Snapping the face-resize or translate gizmos further (already complete)

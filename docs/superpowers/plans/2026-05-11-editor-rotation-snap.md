# Editor Rotation Snap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add angle snap to the rotation gizmo rings, mirroring the existing translate snap system with matching cvars and toolbar UI.

**Architecture:** Three new cvars defined in `editor.c` and registered in `Editor_Init`. `gizmo.c` extern-declares them, adds three new static drag-state floats, resets them in `Editor_GizmoMouseDown`, and applies snap math in the rotation path of `Editor_GizmoMouseMove`. `editor_ui.c` adds a matching toolbar row with rsnap checkbox, abs checkbox, and angle preset dropdown.

**Tech Stack:** C (gnu89), ImGui via the `IG_*` wrapper API, Quake cvar system.

---

### Task 1: Define and register the three rotation snap cvars

**Files:**
- Modify: `sdlquake/engine/editor/editor.c:75-77` (cvar definitions)
- Modify: `sdlquake/engine/editor/editor.c:1274-1276` (cvar registrations in `Editor_Init`)

The translate snap cvars live at `editor.c:75-77`. The rotate snap cvars go directly after them.

- [ ] **Step 1: Add cvar definitions after line 77 of editor.c**

Open `sdlquake/engine/editor/editor.c`. Find the block:
```c
cvar_t      editor_grid_snap     = { "editor_grid_snap", "1" };
cvar_t      editor_grid_size     = { "editor_grid_size", "16" };
cvar_t      editor_grid_absolute = { "editor_grid_absolute", "1" };
```

Add immediately after it:
```c
cvar_t      editor_rotate_snap          = { "editor_rotate_snap",          "1"  };
cvar_t      editor_rotate_snap_size     = { "editor_rotate_snap_size",     "45" };
cvar_t      editor_rotate_snap_absolute = { "editor_rotate_snap_absolute", "1"  };
```

- [ ] **Step 2: Register the new cvars in Editor_Init**

In the same file find (around line 1274):
```c
    Cvar_RegisterVariable(&editor_grid_snap);
    Cvar_RegisterVariable(&editor_grid_size);
    Cvar_RegisterVariable(&editor_grid_absolute);
```

Add immediately after:
```c
    Cvar_RegisterVariable(&editor_rotate_snap);
    Cvar_RegisterVariable(&editor_rotate_snap_size);
    Cvar_RegisterVariable(&editor_rotate_snap_absolute);
```

- [ ] **Step 3: Build to confirm no errors**

```
zig build
```
Expected: compiles without errors. No run needed yet.

- [ ] **Step 4: Commit**

```
git add sdlquake/engine/editor/editor.c
git commit -m "feat(editor): define and register rotation snap cvars"
```

---

### Task 2: Add snap state and logic in gizmo.c

**Files:**
- Modify: `sdlquake/engine/editor/gizmo.c:29-38` (static drag state block)
- Modify: `sdlquake/engine/editor/gizmo.c:40-42` (extern cvar declarations)
- Modify: `sdlquake/engine/editor/gizmo.c:701-711` (`Editor_GizmoMouseDown` rotation drag init)
- Modify: `sdlquake/engine/editor/gizmo.c:752-810` (`Editor_GizmoMouseMove` rotation path)

- [ ] **Step 1: Add extern declarations for the new cvars**

In `gizmo.c`, find:
```c
extern cvar_t   editor_grid_snap;
extern cvar_t   editor_grid_size;
extern cvar_t   editor_grid_absolute;
```

Add immediately after:
```c
extern cvar_t   editor_rotate_snap;
extern cvar_t   editor_rotate_snap_size;
extern cvar_t   editor_rotate_snap_absolute;
```

- [ ] **Step 2: Add new static state variables**

Find:
```c
static vec3_t   s_rotate_pivot;         // selection centroid captured at rotate-drag start
static float    s_rotate_prev_angle;    // last frame's mouse angle in radians, for incremental step rotation
```

Add two lines immediately after:
```c
static float    s_rotate_total_raw;     // accumulated raw angle since drag-start (radians)
static float    s_rotate_total_applied; // total snapped angle already applied (radians)
static float    s_rotate_start_angle;   // primary entity's angle on drag axis at drag-start (absolute snap)
```

- [ ] **Step 3: Reset new state in Editor_GizmoMouseDown when a rotation drag begins**

Find the rotation drag init block in `Editor_GizmoMouseDown` (starts with `if (best_ring_axis >= 0)`):
```c
    if (best_ring_axis >= 0)
    {
        History_Push("rotate");
        s_drag_axis        = -1;
        s_drag_plane_idx   = -1;
        s_drag_rotate_axis = best_ring_axis;
        VectorCopy(centroid, s_rotate_pivot);
        if (!rotate_mouse_angle(best_ring_axis, s_rotate_pivot, r_org, r_dir,
                                &s_rotate_prev_angle))
            s_rotate_prev_angle = 0;
        return 1;
    }
```

Replace with:
```c
    if (best_ring_axis >= 0)
    {
        History_Push("rotate");
        s_drag_axis        = -1;
        s_drag_plane_idx   = -1;
        s_drag_rotate_axis = best_ring_axis;
        VectorCopy(centroid, s_rotate_pivot);
        if (!rotate_mouse_angle(best_ring_axis, s_rotate_pivot, r_org, r_dir,
                                &s_rotate_prev_angle))
            s_rotate_prev_angle = 0;
        s_rotate_total_raw     = 0.0f;
        s_rotate_total_applied = 0.0f;
        s_rotate_start_angle   = 0.0f;
        {
            edit_entity_t *pe = Scene_GetSelectedEntity();
            if (pe)
            {
                int k, ai = -1, angle_idx = -1;
                float pitch = 0, yaw = 0, roll = 0;
                for (k = 0; k < pe->numkv; k++)
                {
                    if      (!strcmp(pe->kv[k].key, "angles")) { ai        = k; break; }
                    else if (!strcmp(pe->kv[k].key, "angle"))    angle_idx = k;
                }
                if (ai >= 0)
                    sscanf(pe->kv[ai].value, "%f %f %f", &pitch, &yaw, &roll);
                else if (angle_idx >= 0)
                    yaw = (float)atof(pe->kv[angle_idx].value);
                if      (best_ring_axis == 2) s_rotate_start_angle = yaw   * (3.14159265f / 180.0f);
                else if (best_ring_axis == 0) s_rotate_start_angle = pitch * (3.14159265f / 180.0f);
                else                          s_rotate_start_angle = roll  * (3.14159265f / 180.0f);
            }
        }
        return 1;
    }
```

- [ ] **Step 4: Apply snap in Editor_GizmoMouseMove rotation path**

Find the rotation section of `Editor_GizmoMouseMove` (inside `if (s_drag_rotate_axis >= 0)`). Look for:
```c
        step = wrap_delta(a_now - s_rotate_prev_angle);
        s_rotate_prev_angle = a_now;
        if (step == 0.0f) return;
```

Replace those three lines with:
```c
        {
            float raw_delta = wrap_delta(a_now - s_rotate_prev_angle);
            s_rotate_prev_angle  = a_now;
            s_rotate_total_raw  += raw_delta;
            if (editor_rotate_snap.value != 0.0f)
            {
                float snap_rad = editor_rotate_snap_size.value * (3.14159265f / 180.0f);
                float snapped_total;
                if (snap_rad <= 0.0f) snap_rad = 3.14159265f / 4.0f;
                if (editor_rotate_snap_absolute.value != 0.0f)
                    snapped_total = floorf((s_rotate_start_angle + s_rotate_total_raw) / snap_rad + 0.5f) * snap_rad
                                    - s_rotate_start_angle;
                else
                    snapped_total = floorf(s_rotate_total_raw / snap_rad + 0.5f) * snap_rad;
                step = snapped_total - s_rotate_total_applied;
                s_rotate_total_applied = snapped_total;
            }
            else
            {
                step = raw_delta;
            }
        }
        if (step == 0.0f) return;
```

Note: `floorf` is already available via `<math.h>` (included at the top of `gizmo.c`).

- [ ] **Step 5: Build and verify**

```
zig build run -- +map e1m1
```

Open the editor (F2), select an entity, grab a rotation ring, and drag. With `editor_rotate_snap 1` (default), rotation should snap to 45° increments. Try `editor_rotate_snap_size 15` in the console to verify finer snapping. Try `editor_rotate_snap_absolute 0` to verify relative mode (snaps relative to drag-start instead of world angles). Try `editor_rotate_snap 0` to verify free rotation still works.

- [ ] **Step 6: Commit**

```
git add sdlquake/engine/editor/gizmo.c
git commit -m "feat(editor): rotation snap — accumulate angle delta and snap per-drag"
```

---

### Task 3: Add rotation snap controls to the toolbar

**Files:**
- Modify: `sdlquake/engine/editor/editor_ui.c:137-141` (extern cvar declarations in `draw_toolbar`)
- Modify: `sdlquake/engine/editor/editor_ui.c:151-156` (static data in `draw_toolbar`)
- Modify: `sdlquake/engine/editor/editor_ui.c:291-307` (after the grid combo block)

- [ ] **Step 1: Declare the new cvars in draw_toolbar**

Find in `draw_toolbar` (around line 137):
```c
    extern cvar_t editor_grid_snap;
    extern cvar_t editor_grid_size;
    extern cvar_t editor_grid_absolute;
```

Add immediately after:
```c
    extern cvar_t editor_rotate_snap;
    extern cvar_t editor_rotate_snap_size;
    extern cvar_t editor_rotate_snap_absolute;
```

- [ ] **Step 2: Add rotate snap preset arrays**

Find (around line 151):
```c
    static const float grid_values[] = { 1, 4, 8, 16, 18, 32, 45, 56, 64, 128 };
    static const char *grid_items[] = {
        "1", "4", "8", "16 (build)", "18 (step)", "32 (door)",
        "45 (jump)", "56 (player)", "64", "128 (room)"
    };
    enum { GRID_N = (int)(sizeof(grid_values) / sizeof(grid_values[0])) };
```

Add immediately after:
```c
    static const float rotate_snap_values[] = { 5, 10, 15, 22.5f, 30, 45, 90 };
    static const char *rotate_snap_items[]  = { "5", "10", "15", "22.5", "30", "45", "90" };
    enum { RSNAP_N = (int)(sizeof(rotate_snap_values) / sizeof(rotate_snap_values[0])) };
```

- [ ] **Step 3: Add the three rotate snap controls to the toolbar**

Find the end of the grid combo block (around line 307):
```c
        if (IG_Combo("grid", &sel, grid_items, GRID_N))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "editor_grid_size %g\n", grid_values[sel]);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        int cam = (int)editor_camera.value;
```

Insert the three new controls between the closing `}` of the grid combo block and the `IG_SameLine` that starts the camera combo — i.e., after the closing `}    }` and before the `IG_SameLine(0, -1);` / `int cam =` block:

```c
    IG_SameLine(0, -1);
    {
        int rsnap = editor_rotate_snap.value != 0.0f;
        if (IG_Checkbox("rsnap", &rsnap))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_rotate_snap %d\n", rsnap ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        int rabs = editor_rotate_snap_absolute.value != 0.0f;
        if (IG_Checkbox("abs##rot", &rabs))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_rotate_snap_absolute %d\n", rabs ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        int sel = 5, k;    /* default index 5 = 45° */
        float cur = editor_rotate_snap_size.value;
        for (k = 0; k < RSNAP_N; k++)
            if (rotate_snap_values[k] == cur) { sel = k; break; }
        IG_SetNextItemWidth(60);
        if (IG_Combo("rangle", &sel, rotate_snap_items, RSNAP_N))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "editor_rotate_snap_size %g\n", rotate_snap_values[sel]);
            Cbuf_AddText(buf);
        }
    }
```

Note: The abs checkbox uses label `"abs##rot"` — ImGui displays it as `"abs"` but the `##rot` suffix makes the widget ID unique so it doesn't conflict with the translate snap `"abs"` checkbox.

- [ ] **Step 4: Build and verify UI**

```
zig build run -- +map e1m1
```

Open the editor (F2). The toolbar should show `rsnap`, `abs`, and `rangle` controls after the existing `grid` dropdown. Clicking `rsnap` toggles rotation snap. The angle dropdown changes the snap granularity and the rotation ring immediately respects it. `abs` toggles absolute vs relative mode.

- [ ] **Step 5: Commit**

```
git add sdlquake/engine/editor/editor_ui.c
git commit -m "feat(editor): rotation snap toolbar controls — rsnap checkbox, abs, angle dropdown"
```

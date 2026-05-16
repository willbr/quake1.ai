# Editor Snap-to-BSP-Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `editor_snap_surface` cvar (default `1`) that makes translate-gizmo drags (axis, planar, face-resize) snap the selection so its bbox face nearest the surface normal lies flush against the BSP/brush face under the cursor.

**Architecture:** One new cvar in `editor.c`. Two existing static helpers in `render_wire.c` (`point_entity_bbox`, `selection_bbox`) get promoted to public `Editor_PointEntityBBox` / `Editor_SelectionBBox`. `Editor_RaycastForPlacement` gets factored to take a "skip these (entity,brush) pairs" exclusion list; a new `Editor_RaycastSurface` thin wrapper passes the current selection as the skip-list. `gizmo.c` adds `surface_offset_along_normal` + `surface_hit_for_drag` helpers and inserts a snap branch at the top of each translate path in `Editor_GizmoMouseMove`. `editor_ui.c` adds one checkbox to the toolbar Snap row.

**Tech Stack:** C (gnu89, no test suite — verify by build + in-game smoke test).

**Spec:** `docs/superpowers/specs/2026-05-16-editor-snap-to-bsp-surface-design.md`

---

### Task 1: Define and register the `editor_snap_surface` cvar + UI checkbox

**Files:**
- Modify: `sdlquake/engine/editor/editor.c:79-84` (cvar definitions block)
- Modify: `sdlquake/engine/editor/editor.c:1782-1787` (cvar registrations in `Editor_Init`)
- Modify: `sdlquake/engine/editor/editor_ui.c:309-314` (toolbar Snap row)

This task adds plumbing only — no drag-time behaviour yet. The cvar exists, the checkbox toggles it, but nothing reads it. Confirms registration + UI plumb without risk.

- [ ] **Step 1: Add cvar definition after the translate-snap block in editor.c**

Open `sdlquake/engine/editor/editor.c`. Find lines 79-84:
```c
cvar_t      editor_grid_snap     = { "editor_grid_snap", "1" };
cvar_t      editor_grid_size     = { "editor_grid_size", "16" };
cvar_t      editor_grid_absolute = { "editor_grid_absolute", "1" };
// Rotate snap state. Mirrors translate snap one-for-one.
cvar_t      editor_rotate_snap          = { "editor_rotate_snap", "1" };
cvar_t      editor_rotate_snap_size     = { "editor_rotate_snap_size", "45" };
cvar_t      editor_rotate_snap_absolute = { "editor_rotate_snap_absolute", "1" };
```

Insert immediately after line 81 (after `editor_grid_absolute`, before the rotate-snap comment):
```c
// Snap-to-BSP-surface. When 1, translate drags whose cursor ray hits a
// BSP/brush face snap the selection so its bbox face nearest the surface
// normal lies flush on that surface. Overrides grid snap on constrained
// axes; falls through to grid snap when the cursor misses everything.
cvar_t      editor_snap_surface   = { "editor_snap_surface", "1" };
```

- [ ] **Step 2: Register the cvar in `Editor_Init`**

In the same file find line 1784:
```c
    Cvar_RegisterVariable(&editor_grid_absolute);
```

Add immediately after:
```c
    Cvar_RegisterVariable(&editor_snap_surface);
```

- [ ] **Step 3: Add toolbar checkbox**

Open `sdlquake/engine/editor/editor_ui.c`. Find lines 309-314:
```c
    // -- Snap (translate) ------------------------------------------------
    ui_cvar_checkbox_same("snap", "editor_grid_snap");
    ui_cvar_checkbox_same("abs",  "editor_grid_absolute");
    IG_SameLine(0, -1);
    ui_cvar_combo_preset("grid", "editor_grid_size",
                         grid_items, grid_values, ARRAY_LEN(grid_items), 140);
```

Add immediately after the `ui_cvar_combo_preset(...)` call:
```c
    ui_cvar_checkbox_same("surface", "editor_snap_surface");
```

- [ ] **Step 4: Build**

```
zig build
```
Expected: compiles without errors.

- [ ] **Step 5: Smoke test**

```
zig build run -- +map e1m1
```
Press F2 to open the editor. Confirm the toolbar Snap row now reads `[x] snap [x] abs grid:16 [x] surface` (the new "surface" checkbox sits after the grid dropdown). Toggle it on/off — no visible behaviour change yet (no drag handler reads it). Close the game.

- [ ] **Step 6: Commit**

```
git add sdlquake/engine/editor/editor.c sdlquake/engine/editor/editor_ui.c
git commit -m "feat(editor): add editor_snap_surface cvar + toolbar checkbox"
```

---

### Task 2: Promote `point_entity_bbox` and `selection_bbox` to public API

**Files:**
- Modify: `sdlquake/engine/editor/render_wire.c:573` (rename `point_entity_bbox` → `Editor_PointEntityBBox`, remove `static`)
- Modify: `sdlquake/engine/editor/render_wire.c:909` (rename `selection_bbox` → `Editor_SelectionBBox`, remove `static`)
- Modify: `sdlquake/engine/editor/render_wire.c` (update all callers within the file)
- Modify: `sdlquake/engine/editor/editor_internal.h` (add prototypes)

These two static helpers in `render_wire.c` already do exactly what the gizmo snap path needs. Promoting (rather than duplicating) keeps a single bbox source of truth — important because `point_entity_bbox` has nuanced behaviour (live model bbox vs absmin/absmax vs class table fallback) that we'd otherwise have to maintain in two places.

- [ ] **Step 1: Find every call site for `point_entity_bbox` in render_wire.c**

```
Grep "point_entity_bbox(" sdlquake/engine/editor/render_wire.c
```

You should see ~3 hits: the definition at line 573 and two callers (currently at lines 933 and 1447). Note the exact line numbers; you'll rename all three.

- [ ] **Step 2: Find every call site for `selection_bbox` in render_wire.c**

```
Grep "selection_bbox(" sdlquake/engine/editor/render_wire.c
```

You should see ~2 hits: the definition at line 909 and one caller at line 1459.

- [ ] **Step 3: Rename `point_entity_bbox` → `Editor_PointEntityBBox` and drop `static`**

In `render_wire.c` line 573, change:
```c
static void point_entity_bbox(const edit_entity_t *e,
                              vec3_t out_mins, vec3_t out_maxs)
```
to:
```c
void Editor_PointEntityBBox(const edit_entity_t *e,
                            vec3_t out_mins, vec3_t out_maxs)
```

Then update both call sites in the same file. Use Edit's `replace_all: true` on the bare identifier `point_entity_bbox(` → `Editor_PointEntityBBox(`.

- [ ] **Step 4: Rename `selection_bbox` → `Editor_SelectionBBox` and drop `static`**

In `render_wire.c` line 909, change:
```c
static int selection_bbox(vec3_t out_mins, vec3_t out_maxs)
```
to:
```c
int Editor_SelectionBBox(vec3_t out_mins, vec3_t out_maxs)
```

Then update the one caller at line 1459. Use Edit with old=`selection_bbox(bmin, bmax)` new=`Editor_SelectionBBox(bmin, bmax)`. (Don't `replace_all` because the spec doc uses that name too, but the spec isn't in render_wire.c so it's safe — still, prefer narrow edits inside this file.)

- [ ] **Step 5: Add prototypes to editor_internal.h**

Open `sdlquake/engine/editor/editor_internal.h`. Find the `render_wire.c` section near line 57. After the existing render_wire prototypes (just before `// edit_scene.c — build 6 axial planes ...` around line 100), add:

```c
// render_wire.c — bbox helpers. PointEntityBBox resolves a single point
// entity's world-space bbox using the same precedence as the wire-bbox
// draw (live model bbox > absmin/absmax > class table fallback). Public
// so gizmo.c can use it for surface-snap offset math.
void Editor_PointEntityBBox(const struct edit_entity_s *e,
                            vec3_t out_mins, vec3_t out_maxs);

// render_wire.c — combined world-space bbox of all selected items
// (brushes via b->mins/maxs, point entities via Editor_PointEntityBBox).
// Returns 1 if at least one item contributed; 0 means empty selection or
// no resolvable bbox (e.g. only empty func_groups).
int  Editor_SelectionBBox(vec3_t out_mins, vec3_t out_maxs);
```

- [ ] **Step 6: Build**

```
zig build
```
Expected: compiles cleanly. If you get unresolved-symbol errors, you missed a `point_entity_bbox(` or `selection_bbox(` call site — re-run the Greps from steps 1-2 and update any that still use the old names.

- [ ] **Step 7: Smoke test**

```
zig build run -- +map e1m1
```
Press F2, click an entity to select it. Confirm the wire bbox still draws correctly around it (this exercises `Editor_PointEntityBBox`). Click another entity while holding Shift to multi-select — the combined union bbox should appear (`Editor_SelectionBBox`). Close the game.

- [ ] **Step 8: Commit**

```
git add sdlquake/engine/editor/render_wire.c sdlquake/engine/editor/editor_internal.h
git commit -m "refactor(editor): promote point_entity_bbox + selection_bbox to public API"
```

---

### Task 3: Factor `Editor_RaycastForPlacement` to take a selection-skip list

**Files:**
- Modify: `sdlquake/engine/editor/render_wire.c:2147-2222` (factor function)
- Modify: `sdlquake/engine/editor/editor_internal.h:82-84` (update prototype, add `_Ex` variant)

`Editor_RaycastForPlacement` already does world-BSP-trace + editor-brush-face-trace and returns the closer hit. The gizmo snap path needs the same thing but with the dragged selection excluded (otherwise a brush snaps onto its own faces). Cleanest approach: add a "skip pairs" param.

- [ ] **Step 1: Add the skip-pair struct and `_Ex` prototype to editor_internal.h**

Open `sdlquake/engine/editor/editor_internal.h`. Find the existing prototype around lines 82-84:
```c
int  Editor_RaycastForPlacement(float sx, float sy,
                                vec3_t out_hit, vec3_t out_normal);
```

Add immediately after it:
```c
// A (entity, brush) pair to exclude from the editor-brush face pass of
// the surface raycast. Used by the gizmo snap path so a brush being
// dragged doesn't snap to its own faces.
typedef struct {
    int e_idx;
    int b_idx;
} editor_skip_pair_t;

// Same trace as Editor_RaycastForPlacement, but skips any editor brush
// matching one of `skip[0..n_skip)`. World-BSP hits aren't filtered
// (you can't drag world BSP).
int  Editor_RaycastForPlacement_Ex(float sx, float sy,
                                   const editor_skip_pair_t *skip, int n_skip,
                                   vec3_t out_hit, vec3_t out_normal);
```

- [ ] **Step 2: Refactor `Editor_RaycastForPlacement` in render_wire.c to forward to `_Ex`**

Open `sdlquake/engine/editor/render_wire.c`. Replace the current `Editor_RaycastForPlacement` body (lines 2147-2222) with two functions: the original signature becomes a thin wrapper, and `_Ex` contains the body with one new check.

Replace the entire function definition starting at line 2147 with:

```c
int Editor_RaycastForPlacement(float sx, float sy,
                               vec3_t out_hit, vec3_t out_normal)
{
    return Editor_RaycastForPlacement_Ex(sx, sy, NULL, 0, out_hit, out_normal);
}

// Same trace, but skips any editor brush listed in `skip`. n_skip == 0
// (or skip == NULL) is identical to Editor_RaycastForPlacement.
int Editor_RaycastForPlacement_Ex(float sx, float sy,
                                  const editor_skip_pair_t *skip, int n_skip,
                                  vec3_t out_hit, vec3_t out_normal)
{
    extern qboolean SV_RecursiveHullCheck (hull_t *, int, float, float,
                                           vec3_t, vec3_t, trace_t *);
    const float TRACE_DIST = 10000.0f;
    vec3_t origin, dir;
    int    have_hit = 0;
    float  best_t = 1e30f;
    vec3_t best_n = { 0, 0, 1 };
    int    i, j, k, s;

    Editor_ScreenToRay(sx, sy, origin, dir);

    // World BSP trace.
    if (cl.worldmodel)
    {
        trace_t trace;
        vec3_t  start, end;
        for (i = 0; i < 3; i++)
        {
            start[i] = origin[i];
            end[i]   = origin[i] + dir[i] * TRACE_DIST;
        }
        memset(&trace, 0, sizeof(trace));
        trace.fraction = 1;
        VectorCopy(end, trace.endpos);
        SV_RecursiveHullCheck(cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);
        if (!trace.startsolid && !trace.allsolid && trace.fraction < 1.0f)
        {
            float t = trace.fraction * TRACE_DIST;
            if (t < best_t)
            {
                best_t = t;
                VectorCopy(trace.plane.normal, best_n);
                have_hit = 1;
            }
        }
    }

    // Editor brushes: find the nearest face hit. Skip hidden categories so
    // the user doesn't drop an entity onto a filtered-out trigger volume.
    // Also skip any pair listed in `skip` (gizmo snap excludes the dragged
    // selection so a brush doesn't snap to itself).
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (Editor_EntityHidden(i)) continue;
        if (Entity_IsPoint(e)) continue;
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            int skipped = 0;
            if (!b->valid) continue;
            for (s = 0; s < n_skip; s++)
            {
                if (skip[s].e_idx == i && skip[s].b_idx == j) { skipped = 1; break; }
            }
            if (skipped) continue;
            for (k = 0; k < b->numfaces; k++)
            {
                edit_face_t *f = &b->faces[k];
                edit_plane_t *pl = &b->planes[f->plane_idx];
                float t;
                if (!ray_vs_face(origin, dir,
                                 (const vec3_t *)f->verts, f->numverts,
                                 pl->normal, pl->dist, &t))
                    continue;
                if (t < best_t)
                {
                    best_t = t;
                    VectorCopy(pl->normal, best_n);
                    have_hit = 1;
                }
            }
        }
    }

    if (!have_hit) return 0;
    for (i = 0; i < 3; i++)
        out_hit[i] = origin[i] + dir[i] * best_t;
    VectorCopy(best_n, out_normal);
    return 1;
}
```

The comment block above the wrapper function (`// Cursor-placement raycast. Combines a BSP trace ...` at lines 2142-2146) stays as-is and now documents both the wrapper and `_Ex`.

- [ ] **Step 3: Build**

```
zig build
```
Expected: compiles cleanly.

- [ ] **Step 4: Smoke test**

```
zig build run -- +map e1m1
```
F2 open editor, click "Add Entity..." in the toolbar, pick `info_player_start`, click the floor. The entity should land at the cursor with a +16 offset along the normal (existing behaviour — this exercises the wrapper `Editor_RaycastForPlacement`). Close the game.

- [ ] **Step 5: Commit**

```
git add sdlquake/engine/editor/render_wire.c sdlquake/engine/editor/editor_internal.h
git commit -m "refactor(editor): factor Editor_RaycastForPlacement to accept a skip list"
```

---

### Task 4: Add `surface_offset_along_normal` + `surface_hit_for_drag` helpers in gizmo.c

**Files:**
- Modify: `sdlquake/engine/editor/gizmo.c:46-51` (extern cvar block)
- Modify: `sdlquake/engine/editor/gizmo.c` (add the two helpers after `snap_to_grid` around line 59)

These helpers don't change any visible behaviour on their own — they're called from Task 5/6/7. Keep this task small so the integration tasks stay focused.

- [ ] **Step 1: Add extern declaration for `editor_snap_surface`**

In `gizmo.c`, find the extern block at lines 46-51:
```c
extern cvar_t   editor_grid_snap;
extern cvar_t   editor_grid_size;
extern cvar_t   editor_grid_absolute;
extern cvar_t   editor_rotate_snap;
extern cvar_t   editor_rotate_snap_size;
extern cvar_t   editor_rotate_snap_absolute;
```

Add at the end of the block:
```c
extern cvar_t   editor_snap_surface;
```

- [ ] **Step 2: Add the two new helpers**

Find `snap_to_grid` at line 53-59. Add immediately after its closing brace:

```c
// Signed distance from the selection bbox centroid to the bbox face whose
// outward normal is most opposite to `n`. For axial n=(0,0,1) and an
// axis-aligned bbox, this is just (maxs[2]-mins[2])*0.5. For oblique n,
// project all 8 corners onto -n and take the max — that's the corner
// sticking out the most against the surface. Used as the offset to add
// along `n` after a surface hit so the bbox face lands flush on the
// surface rather than the centroid landing on it.
static float surface_offset_along_normal(const vec3_t mins, const vec3_t maxs,
                                         const vec3_t n)
{
    vec3_t centroid;
    float best = -1e30f;
    int c, k;
    for (k = 0; k < 3; k++) centroid[k] = (mins[k] + maxs[k]) * 0.5f;
    for (c = 0; c < 8; c++)
    {
        vec3_t corner;
        float  proj;
        corner[0] = (c & 1) ? maxs[0] : mins[0];
        corner[1] = (c & 2) ? maxs[1] : mins[1];
        corner[2] = (c & 4) ? maxs[2] : mins[2];
        // -dot(corner - centroid, n) — how far the corner sticks out
        // OPPOSITE to n (toward the surface that has outward-normal n).
        proj = -((corner[0] - centroid[0]) * n[0]
               + (corner[1] - centroid[1]) * n[1]
               + (corner[2] - centroid[2]) * n[2]);
        if (proj > best) best = proj;
    }
    return best;
}

// Cursor-ray surface trace for the active drag. Same as
// Editor_RaycastForPlacement but excludes every currently-selected brush
// (and the brushes of every selected entity) so a brush being moved
// doesn't snap onto its own faces. Point-entity selections add no skip
// entries — they have no faces to skip in the trace.
static int surface_hit_for_drag(float sx, float sy,
                                vec3_t out_hit, vec3_t out_normal)
{
    enum { MAX_SKIP = 64 };
    editor_skip_pair_t skip[MAX_SKIP];
    int n_skip = 0;
    int n = Scene_NumSelected();
    int i, e_idx, b_idx;
    for (i = 0; i < n && n_skip < MAX_SKIP; i++)
    {
        if (!Scene_GetSelected(i, &e_idx, &b_idx)) continue;
        if (b_idx < 0) continue;            // entity ref — no brush face to skip
        skip[n_skip].e_idx = e_idx;
        skip[n_skip].b_idx = b_idx;
        n_skip++;
    }
    return Editor_RaycastForPlacement_Ex(sx, sy, skip, n_skip,
                                         out_hit, out_normal);
}
```

- [ ] **Step 3: Build**

```
zig build
```
Expected: compiles without warnings. The two new functions are unused at this point — gnu89 doesn't warn on unused statics by default in this codebase, but if you see a warning you can ignore it; the next tasks will use them.

- [ ] **Step 4: Commit**

```
git add sdlquake/engine/editor/gizmo.c
git commit -m "feat(editor): surface_offset_along_normal + surface_hit_for_drag helpers"
```

---

### Task 5: Integrate surface snap into axis-translate drag

**Files:**
- Modify: `sdlquake/engine/editor/gizmo.c:1028-1037` (axis-translate branch of `Editor_GizmoMouseMove`)

The axis-translate branch is at the bottom of `Editor_GizmoMouseMove`, after the rotate / planar / face-resize branches. Today it computes `t_now` / `raw_offset` via `closest_t_line_ray`, snaps to grid, applies the delta. Surface snap inserts before that: if cvar is on AND surface is hit AND selection bbox resolves, override `raw_offset` with the surface-target value and skip grid snap on this axis.

- [ ] **Step 1: Insert the surface-snap branch above the existing axis-translate path**

In `gizmo.c`, find the axis-translate branch around lines 1028-1037:
```c
    else
    {
        snapped_offset = compute_snapped_offset(raw_offset, s_drag_origin[s_drag_axis]);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[s_drag_axis] = step;
        s_drag_applied = snapped_offset;
        apply_translate_delta(delta);
    }
```

Replace the `else` block with:

```c
    else
    {
        // Surface snap: cursor over a BSP/brush face → snap so the
        // selection's bbox face nearest the hit normal lies flush with
        // the surface, projected onto the dragged axis. Other axes
        // untouched. Bypasses grid snap on this axis — surface contact
        // is the user-visible behaviour and grid would lift it off.
        if (editor_snap_surface.value != 0.0f)
        {
            vec3_t hit, n, mins, maxs;
            if (surface_hit_for_drag(sx, sy, hit, n)
             && Editor_SelectionBBox(mins, maxs))
            {
                float off    = surface_offset_along_normal(mins, maxs, n);
                float target = hit[s_drag_axis] + n[s_drag_axis] * off;
                float abs_off = target - s_drag_origin[s_drag_axis];
                step = abs_off - s_drag_applied;
                if (step == 0.0f) return;
                for (i = 0; i < 3; i++) delta[i] = 0;
                delta[s_drag_axis] = step;
                s_drag_applied = abs_off;
                apply_translate_delta(delta);
                return;
            }
        }
        snapped_offset = compute_snapped_offset(raw_offset, s_drag_origin[s_drag_axis]);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[s_drag_axis] = step;
        s_drag_applied = snapped_offset;
        apply_translate_delta(delta);
    }
```

- [ ] **Step 2: Build**

```
zig build
```
Expected: compiles cleanly.

- [ ] **Step 3: Smoke test — axis-arrow snap onto floor**

```
zig build run -- +map e1m1
```
1. F2 open editor.
2. Click `info_player_start` to select it (one is near spawn).
3. Drag the **Z arrow** downward — the entity should land with its feet flush on the floor under the cursor (not 16 units above, not buried). Drop. Look at the gizmo position — bottom of the player bbox should sit on the floor surface.
4. Drag the **X arrow** toward a wall — entity's `-X` bbox face should land on the wall; Y and Z unchanged.
5. Toggle `[ ] surface` off in the toolbar. Drag the Z arrow again — behaviour returns to grid-snap-only (no surface contact, snaps to multiples of 16).
6. Re-enable surface snap. Drag past a wall without releasing — entity should slide along the wall surface as the cursor sweeps.

Close the game.

- [ ] **Step 4: Commit**

```
git add sdlquake/engine/editor/gizmo.c
git commit -m "feat(editor): surface snap in axis-translate gizmo drag"
```

---

### Task 6: Integrate surface snap into planar-translate drag

**Files:**
- Modify: `sdlquake/engine/editor/gizmo.c:987-1009` (planar-translate branch of `Editor_GizmoMouseMove`)

Planar drag is constrained to two axes (u, v) within a world-aligned plane perpendicular to `s_drag_planar_axis`. Surface snap sets the target u/v from the hit point, leaves the constrained-out axis at its drag-start value.

- [ ] **Step 1: Insert the surface-snap branch above the existing planar path**

In `gizmo.c`, find the planar branch around lines 987-1009:
```c
    if (s_drag_planar_axis >= 0)
    {
        vec3_t hit;
        int u = (s_drag_planar_axis + 1) % 3;
        int v = (s_drag_planar_axis + 2) % 3;
        float raw_u, raw_v, snap_u, snap_v, step_u, step_v;
        if (!ray_vs_axis_plane(s_drag_planar_axis, s_drag_origin, r_org, r_dir, hit))
            return;
        raw_u  = hit[u] - s_drag_planar_hit[u];
        raw_v  = hit[v] - s_drag_planar_hit[v];
        snap_u = compute_snapped_offset(raw_u, s_drag_origin[u]);
        snap_v = compute_snapped_offset(raw_v, s_drag_origin[v]);
        step_u = snap_u - s_drag_planar_applied[u];
        step_v = snap_v - s_drag_planar_applied[v];
        if (step_u == 0.0f && step_v == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[u] = step_u;
        delta[v] = step_v;
        s_drag_planar_applied[u] = snap_u;
        s_drag_planar_applied[v] = snap_v;
        apply_translate_delta(delta);
        return;
    }
```

Replace the body with:
```c
    if (s_drag_planar_axis >= 0)
    {
        vec3_t hit;
        int u = (s_drag_planar_axis + 1) % 3;
        int v = (s_drag_planar_axis + 2) % 3;
        float raw_u, raw_v, snap_u, snap_v, step_u, step_v;

        // Surface snap: cursor over a BSP/brush face → snap u/v so the
        // bbox face nearest the hit normal lies flush. Constrained-out
        // axis (s_drag_planar_axis) unchanged.
        if (editor_snap_surface.value != 0.0f)
        {
            vec3_t s_hit, n, mins, maxs;
            if (surface_hit_for_drag(sx, sy, s_hit, n)
             && Editor_SelectionBBox(mins, maxs))
            {
                float off    = surface_offset_along_normal(mins, maxs, n);
                float t_u    = s_hit[u] + n[u] * off;
                float t_v    = s_hit[v] + n[v] * off;
                snap_u = t_u - s_drag_origin[u];
                snap_v = t_v - s_drag_origin[v];
                step_u = snap_u - s_drag_planar_applied[u];
                step_v = snap_v - s_drag_planar_applied[v];
                if (step_u == 0.0f && step_v == 0.0f) return;
                for (i = 0; i < 3; i++) delta[i] = 0;
                delta[u] = step_u;
                delta[v] = step_v;
                s_drag_planar_applied[u] = snap_u;
                s_drag_planar_applied[v] = snap_v;
                apply_translate_delta(delta);
                return;
            }
        }

        if (!ray_vs_axis_plane(s_drag_planar_axis, s_drag_origin, r_org, r_dir, hit))
            return;
        raw_u  = hit[u] - s_drag_planar_hit[u];
        raw_v  = hit[v] - s_drag_planar_hit[v];
        snap_u = compute_snapped_offset(raw_u, s_drag_origin[u]);
        snap_v = compute_snapped_offset(raw_v, s_drag_origin[v]);
        step_u = snap_u - s_drag_planar_applied[u];
        step_v = snap_v - s_drag_planar_applied[v];
        if (step_u == 0.0f && step_v == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[u] = step_u;
        delta[v] = step_v;
        s_drag_planar_applied[u] = snap_u;
        s_drag_planar_applied[v] = snap_v;
        apply_translate_delta(delta);
        return;
    }
```

- [ ] **Step 2: Build**

```
zig build
```
Expected: compiles cleanly.

- [ ] **Step 3: Smoke test — planar handle snap onto floor**

```
zig build run -- +map e1m1
```
1. F2 open editor.
2. Select `info_player_start`.
3. Grab the **XY planar square** (the small square handle in the XY plane — the one whose constrained-out axis is Z). Drag it across the room — the entity should slide along the FLOOR under the cursor (Z value updated only via the surface offset, not via the original planar-plane intersection). Verify the entity stays glued to the floor as you sweep.

   Wait — actually re-read: the XY-planar handle's constrained-out axis is Z (it intends to keep Z fixed). With surface snap on, we override that and set Z from the floor hit. That's the documented behaviour ("snap u/v from hit, snap the bbox face flush"). The constraint that's preserved is the **handle axis** (the constrained-out one in non-snap mode), but surface snap overrides it on this axis when there's a hit. Confirm this matches the spec: yes — the spec says only constrained coords change, but in planar drag the constrained coords ARE u and v, so the constrained-out axis Z is touched only by the bbox-offset along the hit normal. Floor normal is +Z so the +Z component of the offset slides the entity up onto the floor. ✓.

4. Toggle surface snap off, repeat — entity slides in a fixed-Z plane (today's behaviour).
5. Re-enable, drag the XZ or YZ planar handles against a wall — entity should ride the wall surface.

Close the game.

- [ ] **Step 4: Commit**

```
git add sdlquake/engine/editor/gizmo.c
git commit -m "feat(editor): surface snap in planar-translate gizmo drag"
```

---

### Task 7: Integrate surface snap into face-resize drag

**Files:**
- Modify: `sdlquake/engine/editor/gizmo.c:1014-1027` (face-resize branch of `Editor_GizmoMouseMove`)

Face-resize pushes a brush face along its plane normal. Surface snap means: if the cursor ray hits a surface whose normal is roughly co-aligned with the face normal (within ~18°), set the face's `plane.dist` so the face lies on the hit surface. No bbox offset — the moving face IS what touches the surface.

- [ ] **Step 1: Insert the surface-snap branch above the existing face-resize path**

In `gizmo.c`, find the face-resize branch around lines 1014-1027:
```c
    if (s_drag_plane_idx >= 0)
    {
        edit_brush_t *b = Scene_GetSelectedBrush();
        if (!b || !b->valid)
        {
            s_drag_plane_idx = -1;
            return;
        }
        snapped_offset = compute_snapped_offset(raw_offset, s_drag_start_dist);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        s_drag_applied = snapped_offset;
        Brush_TranslateFace(b, s_drag_plane_idx, step);
    }
```

Replace the body with:
```c
    if (s_drag_plane_idx >= 0)
    {
        edit_brush_t *b = Scene_GetSelectedBrush();
        if (!b || !b->valid)
        {
            s_drag_plane_idx = -1;
            return;
        }

        // Surface snap: cursor over a BSP/brush face whose normal is
        // roughly aligned with the dragged face's normal → snap the
        // face's plane.dist so the face lies on the hit surface plane.
        // The 0.95 dot gate (~18°) prevents accidental snaps to
        // perpendicular walls (which don't share a sensible "plane.dist
        // along this face's normal" with the dragged face).
        if (editor_snap_surface.value != 0.0f)
        {
            vec3_t hit, n;
            if (surface_hit_for_drag(sx, sy, hit, n))
            {
                float align = fabsf(DotProduct(n, s_drag_dir));
                if (align > 0.95f)
                {
                    float target_dist  = DotProduct(hit, s_drag_dir);
                    float abs_off      = target_dist - s_drag_start_dist;
                    step = abs_off - s_drag_applied;
                    if (step == 0.0f) return;
                    s_drag_applied = abs_off;
                    Brush_TranslateFace(b, s_drag_plane_idx, step);
                    return;
                }
            }
        }

        snapped_offset = compute_snapped_offset(raw_offset, s_drag_start_dist);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        s_drag_applied = snapped_offset;
        Brush_TranslateFace(b, s_drag_plane_idx, step);
    }
```

- [ ] **Step 2: Build**

```
zig build
```
Expected: compiles cleanly.

- [ ] **Step 3: Smoke test — face-resize snap**

```
zig build run -- +map e1m1
```
1. F2 open editor. Create a test brush: in the console, `editor_brush_add_cube`. A 64-unit cube appears.
2. Select the cube. Grab one of its **face handles** (the small `+` cross on a face). Drag toward a parallel BSP wall — the face should snap when it reaches the wall's plane (the cube's face lands flush against the wall).
3. Try with a perpendicular wall — surface snap should NOT engage (`|dot| < 0.95`); falls through to grid-snapped drag (today's behaviour).
4. Toggle `editor_snap_surface` off, repeat the parallel-wall drag — face should grid-snap only, no surface stick.

Close the game.

- [ ] **Step 4: Commit**

```
git add sdlquake/engine/editor/gizmo.c
git commit -m "feat(editor): surface snap in face-resize gizmo drag"
```

---

### Task 8: Final integration test + spec status update

- [ ] **Step 1: Full editor surface-snap walkthrough**

```
zig build run -- +map e1m1
```
1. F2 open editor.
2. Select `info_player_start`. Drag Z arrow down → feet on floor. ✓
3. Drag X arrow into wall → side on wall. ✓
4. Select a different ent + this one (multi-select). Drag Z arrow down → both translate together, the lower bbox face lands flush. ✓
5. Select a brush. Drag a face onto a parallel BSP wall → face snaps to wall plane. ✓
6. Toggle `[ ] surface` off, repeat (2) → grid-snap-only behaviour returns. ✓
7. `editor_snap_surface 1` in console, confirm cvar reflects the toolbar toggle. ✓

- [ ] **Step 2: Mark spec status as Shipped**

Open `docs/superpowers/specs/2026-05-16-editor-snap-to-bsp-surface-design.md`. Change line 4 from:
```
**Status:** Draft
```
to:
```
**Status:** Shipped
```

- [ ] **Step 3: Commit**

```
git add docs/superpowers/specs/2026-05-16-editor-snap-to-bsp-surface-design.md
git commit -m "docs(editor): mark snap-to-BSP-surface spec as Shipped"
```

- [ ] **Step 4: Push to master**

```
git push
```

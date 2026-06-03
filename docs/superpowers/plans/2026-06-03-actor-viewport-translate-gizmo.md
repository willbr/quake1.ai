# Actor-editor Viewport Translate Gizmo (MVP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a 3-axis translate gizmo to the Actor editor's Viewport — click a part to select it, drag the X/Y/Z handles to move it — driving the existing `s_voff` accumulator.

**Architecture:** All changes live in `sdlquake/engine/editor/edit_actor.c`, by populating the two unused `actor_mode` vtable slots (`render_scene`, `process_event`). The handle overlay draws on the hide-world backdrop via the z-tested `Editor_DrawLine3DOver`; picking is ray-vs-triangle against the rendered `s_edit` meshes; dragging writes `s_voff[mesh][axis]`, which the per-frame `edit_rebuild()` already applies. No `gizmo.c`, engine-render-order, or ABI changes.

**Tech Stack:** C (gnu89 engine module), SDL3 events, Quake mathlib (`DotProduct`/`CrossProduct`/`VectorMA`/`Length`), the editor's `Editor_ScreenToRay`/`Editor_WindowToVid`/`Editor_GetOrbitFocus`/`Editor_DrawLine3DOver` helpers, IQM geometry (`lm_iqm_t`).

---

## How this repo is built & verified

There is **no unit-test suite** (per CLAUDE.md). Verification = build + in-game observation, scripted through the game's MCP server and console twins. Each task below ends with a concrete build + verify + commit.

- **Build:** `zig build` (engine + game.dll). Exit 0 = success.
- **Run with MCP:** `zig build run -- --mcp-http 9876 -nosound -nofocus +map start` (launch detached/background; `-nofocus -nosound` so it doesn't steal focus or play audio).
- **Drive it:** `python3 scripts/mcp_call.py console_exec '{"command":"<cmd>"}'`, `... wait_frames '{"frames":N}'`, `... screenshot_gpu '{"path":"name.png"}'` (writes `screenshots/name.png`). Use `screenshot_gpu`, NOT `screenshot` — the latter misses the GPU view.
- **View a screenshot:** Read `screenshots/name.png`.
- **Editor setup commands (all console-drivable, no mouse needed for setup):**
  - `editor` — open the editor
  - `editor_mode 2` — switch to Actor mode (index 2 in `{map,particle,actor}`)
  - `actor_edit 1` — enter geometry-edit mode (builds `s_edit`/`s_orig`, sets `s_selmesh=0`)
  - `actor_edit_sel <n>` — select part `n`

The dummy actor (`actors/dummy.iqm`, auto-loaded on Actor-mode enter) has 10 cube meshes; part names include `base`, `chest`, `head`, `eye.L`, `eye.R`, `ponytail_01`..`05`.

---

## File structure

- **Modify:** `sdlquake/engine/editor/edit_actor.c`
  - New static gizmo block inserted **immediately after `ActorMode_PushPreview`** (≈ line 445), in dependency order (each task appends below the previous): geometry helpers → pick → axis math → `render_scene`/`process_event` → console twin.
  - `ActorMode_RegisterCmds` (≈ line 792): register `actor_edit_pick`.
  - `const editor_mode_t actor_mode` initializer (end of file, ≈ line 816): add `.render_scene` (Task 1) and `.process_event` (Task 3).

No other files change. (`s_voff`, `s_selmesh`, `s_editmode`, `s_edit`, `MAXEDITMESH`, and the included headers `quakedef.h`/`iqm.h`/`editor_internal.h` already exist in this file.)

---

## Task 1: Handle overlay (render_scene)

Draw three colored axis lines at the selected part's centroid, on the hide-world backdrop. This is the de-risk step: it re-tests whether the engine-line overlay renders over the actor (the thing that failed in the reverted attempt).

**Files:**
- Modify: `sdlquake/engine/editor/edit_actor.c` (insert after `ActorMode_PushPreview`, ≈ line 445; edit the `actor_mode` initializer)

- [ ] **Step 1: Add the drag-axis state + geometry helpers + render_scene**

Insert this block immediately after the closing `}` of `ActorMode_PushPreview`:

```c
// ===========================================================================
// Viewport translate gizmo (MVP). Draws 3 axis handles at the selected part's
// centroid and (Task 3) drags them into s_voff. Active only while the editor is
// open in Actor mode with geometry-edit on and a part selected. See
// docs/superpowers/specs/2026-06-03-actor-viewport-translate-gizmo-design.md.
// ===========================================================================

static int s_drag_axis = -1;   // 0/1/2 while dragging that axis, else -1

// World-space centroid of edited part `mi`: mean of its s_edit verts + the
// orbit focus (preview entity sits at the focus with zero angles, so
// world = model + focus).
static void actor_part_centroid_world (int mi, vec3_t out)
{
    lm_iqm_mesh_t *m = &s_edit->meshes[mi];
    vec3_t focus;
    double sx = 0, sy = 0, sz = 0;
    unsigned k;
    for (k = 0; k < m->num_vertexes; k++) {
        float *p = s_edit->verts[m->first_vertex + k].pos;
        sx += p[0]; sy += p[1]; sz += p[2];
    }
    if (m->num_vertexes) { sx /= m->num_vertexes; sy /= m->num_vertexes; sz /= m->num_vertexes; }
    Editor_GetOrbitFocus (focus);
    out[0] = (float)sx + focus[0];
    out[1] = (float)sy + focus[1];
    out[2] = (float)sz + focus[2];
}

// Camera-relative handle length so it stays readable at any orbit zoom.
static float actor_handle_len (const vec3_t cen_w)
{
    extern vec3_t r_origin;
    vec3_t d;
    float len;
    VectorSubtract (cen_w, r_origin, d);
    len = Length (d) * 0.18f;
    if (len <  8.0f) len =  8.0f;
    if (len > 64.0f) len = 64.0f;
    return len;
}

// actor_mode .render_scene slot: 3 axis arrows at the selected part's centroid.
// Self-gates (runs even when the editor is closed, like the map impl).
static void actor_render_scene (void)
{
    extern int Editor_IsOpen (void);
    vec3_t cen, end;
    float  len;

    if (!Editor_IsOpen () || !s_editmode || !s_edit) return;
    if (s_selmesh < 0 || s_selmesh >= s_edit->nummeshes) return;

    actor_part_centroid_world (s_selmesh, cen);
    len = actor_handle_len (cen);

    VectorCopy (cen, end); end[0] += len;
    Editor_DrawLine3DOver (cen, end, s_drag_axis == 0 ? EDIT_COLOR_AXIS_HOT : EDIT_COLOR_AXIS_X);
    VectorCopy (cen, end); end[1] += len;
    Editor_DrawLine3DOver (cen, end, s_drag_axis == 1 ? EDIT_COLOR_AXIS_HOT : EDIT_COLOR_AXIS_Y);
    VectorCopy (cen, end); end[2] += len;
    Editor_DrawLine3DOver (cen, end, s_drag_axis == 2 ? EDIT_COLOR_AXIS_HOT : EDIT_COLOR_AXIS_Z);
}
```

- [ ] **Step 2: Wire the render_scene slot into the vtable**

In the `const editor_mode_t actor_mode = { ... };` initializer at the end of the file, add the `.render_scene` line:

```c
const editor_mode_t actor_mode = {
    .name         = "Actor",
    .enter        = actor_enter,
    .exit         = actor_exit,
    .draw_ui      = actor_draw_ui,
    .render_scene = actor_render_scene,
};
```

- [ ] **Step 3: Build**

Run: `zig build`
Expected: exits 0, no errors. (Engine module recompiles.)

- [ ] **Step 4: Verify the handles render on the backdrop (the de-risk gate)**

Launch and drive via MCP:

```bash
cd /Users/wjbr/src/quake1.ai
(zig build run -- --mcp-http 9876 -nosound -nofocus +map start >/tmp/gizmo.log 2>&1 &)
# wait for MCP
for i in $(seq 1 30); do python3 scripts/mcp_call.py get_player_state 2>/dev/null | grep -qi result && break; sleep 1; done
python3 scripts/mcp_call.py console_exec '{"command":"editor"}'
python3 scripts/mcp_call.py console_exec '{"command":"editor_mode 2"}'
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit 1"}'
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit_sel 2"}'   # 2 = head
python3 scripts/mcp_call.py wait_frames '{"frames":10}'
python3 scripts/mcp_call.py screenshot_gpu '{"path":"gizmo_t1.png"}'
python3 scripts/mcp_call.py wait_frames '{"frames":10}'
```

Then Read `screenshots/gizmo_t1.png`.
Expected: three short colored lines (red +X, yellow-green +Y, light-blue +Z) emanating from the head cube of the actor, on the grid+backdrop.
**GATE:** if the lines are absent, do NOT proceed — switch to the design's ImGui-draw-list fallback (Approach C) for the overlay. If present, the core hypothesis holds; continue.

- [ ] **Step 5: Commit**

```bash
cd /Users/wjbr/src/quake1.ai
git add sdlquake/engine/editor/edit_actor.c
git commit -m "feat(actors): actor-editor viewport gizmo — axis-handle overlay

render_scene draws 3 camera-scaled axis handles (X/Y/Z) at the selected
part's centroid via the z-tested Editor_DrawLine3DOver, on the hide-world
backdrop. De-risk step for the translate gizmo; picking + drag follow.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Part picking + `actor_edit_pick` console twin

Ray-vs-triangle pick against the rendered meshes, exposed as a console command so the pick logic is MCP-verifiable without a mouse.

**Files:**
- Modify: `sdlquake/engine/editor/edit_actor.c` (append to the gizmo block; add a `Cmd_*` function; register it)

- [ ] **Step 1: Add the ray-vs-triangle pick + a vid-coord wrapper**

Append to the gizmo block (below `actor_render_scene`):

```c
// Möller–Trumbore ray (MODEL space) vs every triangle of s_edit. Returns the
// nearest-hit mesh index, or -1. tris hold global vertex indices into verts.
static int actor_ray_pick_meshes (const vec3_t ro, const vec3_t rd)
{
    int      mi, best = -1;
    float    bestt = 1e30f;
    if (!s_edit) return -1;
    for (mi = 0; mi < s_edit->nummeshes; mi++) {
        lm_iqm_mesh_t *m = &s_edit->meshes[mi];
        unsigned tri;
        for (tri = 0; tri < m->num_triangles; tri++) {
            unsigned *idx = &s_edit->tris[(m->first_triangle + tri) * 3];
            float *v0 = s_edit->verts[idx[0]].pos;
            float *v1 = s_edit->verts[idx[1]].pos;
            float *v2 = s_edit->verts[idx[2]].pos;
            vec3_t e1, e2, p, q, tv;
            float  det, inv, u, vv, t;
            VectorSubtract (v1, v0, e1);
            VectorSubtract (v2, v0, e2);
            CrossProduct (rd, e2, p);
            det = DotProduct (e1, p);
            if (det > -1e-6f && det < 1e-6f) continue;     // ray parallel to tri
            inv = 1.0f / det;
            VectorSubtract (ro, v0, tv);
            u = DotProduct (tv, p) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            CrossProduct (tv, e1, q);
            vv = DotProduct (rd, q) * inv;
            if (vv < 0.0f || u + vv > 1.0f) continue;
            t = DotProduct (e2, q) * inv;
            if (t > 1e-4f && t < bestt) { bestt = t; best = mi; }
        }
    }
    return best;
}

// Pick a part at viewport (vid/super-pixel) coords. World ray from
// Editor_ScreenToRay, shifted to model space by subtracting the orbit focus.
static int actor_pick_at_vid (float vx, float vy)
{
    vec3_t ro, rd, focus, ro_model;
    if (!s_edit) return -1;
    Editor_ScreenToRay (vx, vy, ro, rd);
    Editor_GetOrbitFocus (focus);
    VectorSubtract (ro, focus, ro_model);
    return actor_ray_pick_meshes (ro_model, rd);
}
```

- [ ] **Step 2: Add the `actor_edit_pick` console command**

Add this function just **above** `void ActorMode_RegisterCmds (void)` (≈ line 792), alongside the other `Cmd_ActorEdit*_f` functions:

```c
static void Cmd_ActorEditPick_f (void)
{
    float vx, vy;
    int   hit;
    if (!s_editmode || !s_edit) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    if (Cmd_Argc () >= 3) { vx = Q_atof (Cmd_Argv (1)); vy = Q_atof (Cmd_Argv (2)); }
    else                  { vx = vid.width * 0.5f; vy = vid.height * 0.5f; }   // default: view centre
    hit = actor_pick_at_vid (vx, vy);
    if (hit >= 0) Con_Printf ("actor pick (%.0f %.0f): part %d (%s)\n", vx, vy, hit, s_edit->meshes[hit].name);
    else          Con_Printf ("actor pick (%.0f %.0f): miss\n", vx, vy);
}
```

- [ ] **Step 3: Register the command**

In `ActorMode_RegisterCmds`, add (next to the other `actor_edit_*` registrations):

```c
    Cmd_AddCommand ("actor_edit_pick",  Cmd_ActorEditPick_f);
```

- [ ] **Step 4: Build**

Run: `zig build`
Expected: exits 0, no errors.

- [ ] **Step 5: Verify picking via MCP (center hits a part, corner misses)**

With the game running (relaunch as in Task 1 Step 4 if needed) and the editor set up (`editor` / `editor_mode 2` / `actor_edit 1`):

```bash
cd /Users/wjbr/src/quake1.ai
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit_pick"}'        # no args = view centre
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit_pick 4 4"}'    # top-left corner = empty
python3 scripts/mcp_call.py console_tail '{"lines":6}'
```

Expected (in `console_tail`): the centre pick prints `actor pick (... ...): part N (<name>)` for some on-screen part (the actor is centred in the orbit view); the corner pick prints `... : miss`.

- [ ] **Step 6: Verify handle follows console selection (cross-check projection)**

```bash
cd /Users/wjbr/src/quake1.ai
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit_sel 7"}'   # a ponytail segment
python3 scripts/mcp_call.py wait_frames '{"frames":8}'
python3 scripts/mcp_call.py screenshot_gpu '{"path":"gizmo_t2.png"}'
python3 scripts/mcp_call.py wait_frames '{"frames":8}'
```

Read `screenshots/gizmo_t2.png`. Expected: the axis handles now sit on the ponytail segment (part 7), confirming centroid placement tracks selection.

- [ ] **Step 7: Commit**

```bash
cd /Users/wjbr/src/quake1.ai
git add sdlquake/engine/editor/edit_actor.c
git commit -m "feat(actors): actor-editor gizmo — ray-vs-triangle part picking

Möller-Trumbore pick against s_edit meshes; actor_edit_pick <sx> <sy>
console twin (defaults to view centre) makes it MCP-verifiable. Selection
sets s_selmesh, so the axis handles follow the picked part.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Drag (handle grab + axis drag → s_voff) + process_event

Wire LMB: grab an axis handle and drag to translate; click empty space (or a part) to (re)select.

**Files:**
- Modify: `sdlquake/engine/editor/edit_actor.c` (add an SDL include; append to the gizmo block; edit the `actor_mode` initializer)

- [ ] **Step 1: Add the SDL include**

`edit_actor.c` does not currently include SDL (the `process_event` vtable type is `void*`), but `actor_process_event` uses `SDL_Event` / `SDL_BUTTON_LEFT` / `SDL_EVENT_MOUSE_*`. Add the same include `editor.c` uses, after the existing includes near the top of the file:

```c
#include <SDL3/SDL.h>
```

(Place it alongside `#include "editor_internal.h"` / the other `#include`s at the top, ≈ line 14.)

- [ ] **Step 2: Add the remaining drag state + closest-point + axis hit-test**

Append to the gizmo block (below `actor_pick_at_vid`):

```c
// Drag state (s_drag_axis declared in Task 1).
static int    s_drag_mesh = -1;     // part being dragged
static vec3_t s_drag_cen0;          // axis-line anchor, fixed at grab time
static float  s_drag_s0;            // axis param at grab
static float  s_drag_voff0;         // s_voff[mesh][axis] at grab

// Closest approach between the world axis line (cen + s*ax, ax unit) and the
// ray (ro + t*rd). Outputs axis param s and the gap distance; ok=0 if the ray
// is ~parallel to the axis.
static void actor_axis_closest (const vec3_t cen, const vec3_t ax,
                                const vec3_t ro, const vec3_t rd,
                                float *out_s, float *out_dist, int *ok)
{
    vec3_t r, ap, rp, diff;
    float  b, cc, d, e, denom, s, t;
    VectorSubtract (cen, ro, r);
    b  = DotProduct (ax, rd);
    cc = DotProduct (rd, rd);
    d  = DotProduct (ax, r);
    e  = DotProduct (rd, r);
    denom = cc - b * b;                 // a = ax·ax = 1
    if (denom < 1e-6f) { *ok = 0; *out_s = 0.0f; *out_dist = 1e9f; return; }
    s = (b * e - cc * d) / denom;
    t = (e - b * d) / denom;
    VectorMA (cen, s, ax, ap);
    VectorMA (ro,  t, rd, rp);
    VectorSubtract (ap, rp, diff);
    *out_s    = s;
    *out_dist = Length (diff);
    *ok       = 1;
}

// Which axis handle (0/1/2) the ray grabs, or -1. Handles run [0,len] from cen.
static int actor_axis_hit (const vec3_t cen, float len,
                           const vec3_t ro, const vec3_t rd, float *out_s)
{
    static const vec3_t AXIS[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    int   i, best = -1, ok;
    float bestd = 1e9f, thresh = len * 0.22f;   // grab radius, camera-scaled
    for (i = 0; i < 3; i++) {
        float s, dist;
        actor_axis_closest (cen, AXIS[i], ro, rd, &s, &dist, &ok);
        if (!ok || s < 0.0f || s > len) continue;
        if (dist < thresh && dist < bestd) { bestd = dist; best = i; if (out_s) *out_s = s; }
    }
    return best;
}
```

- [ ] **Step 3: Add the process_event slot**

Append to the gizmo block (below `actor_axis_hit`):

```c
// actor_mode .process_event slot. LMB over the viewport: grab an axis handle
// and drag (writes s_voff, applied by edit_rebuild), else pick a part. RMB
// orbit is polled elsewhere and untouched.
static int actor_process_event (void *evp)
{
    extern int Editor_IsOpen (void);
    extern int Editor_MouseOverViewport (void);
    extern int Editor_LookmodeActive (void);
    static const vec3_t AXIS[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    SDL_Event *ev = (SDL_Event *)evp;
    vec3_t ro, rd, cen, focus, ro_model;
    float  vx, vy, len, s;
    int    axis;

    if (!Editor_IsOpen () || !s_editmode || !s_edit) return 0;

    switch (ev->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev->button.button != SDL_BUTTON_LEFT) return 0;
        if (!Editor_MouseOverViewport () || Editor_LookmodeActive ()) return 0;
        Editor_WindowToVid (ev->button.x, ev->button.y, &vx, &vy);
        Editor_ScreenToRay (vx, vy, ro, rd);
        // Axis-handle grab first (only when a part is selected).
        if (s_selmesh >= 0 && s_selmesh < s_edit->nummeshes) {
            actor_part_centroid_world (s_selmesh, cen);
            len  = actor_handle_len (cen);
            axis = actor_axis_hit (cen, len, ro, rd, &s);
            if (axis >= 0) {
                s_drag_axis  = axis;
                s_drag_mesh  = s_selmesh;
                VectorCopy (cen, s_drag_cen0);
                s_drag_s0    = s;
                s_drag_voff0 = s_voff[s_selmesh][axis];
                return 1;
            }
        }
        // Otherwise pick a part (model-space ray = world ray minus focus).
        Editor_GetOrbitFocus (focus);
        VectorSubtract (ro, focus, ro_model);
        {
            int hit = actor_ray_pick_meshes (ro_model, rd);
            if (hit >= 0) s_selmesh = hit;
        }
        return 1;

    case SDL_EVENT_MOUSE_MOTION:
        if (s_drag_axis < 0) return 0;
        Editor_WindowToVid (ev->motion.x, ev->motion.y, &vx, &vy);
        Editor_ScreenToRay (vx, vy, ro, rd);
        {
            int   ok;
            float s_now, dist;
            actor_axis_closest (s_drag_cen0, AXIS[s_drag_axis], ro, rd, &s_now, &dist, &ok);
            if (ok && s_drag_mesh >= 0 && s_drag_mesh < MAXEDITMESH)
                s_voff[s_drag_mesh][s_drag_axis] = s_drag_voff0 + (s_now - s_drag_s0);
        }
        return 1;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button == SDL_BUTTON_LEFT && s_drag_axis >= 0) {
            s_drag_axis = -1;
            return 1;
        }
        return 0;
    }
    return 0;
}
```

- [ ] **Step 4: Wire the process_event slot into the vtable**

Update the `actor_mode` initializer to its final form:

```c
const editor_mode_t actor_mode = {
    .name          = "Actor",
    .enter         = actor_enter,
    .exit          = actor_exit,
    .draw_ui       = actor_draw_ui,
    .render_scene  = actor_render_scene,
    .process_event = actor_process_event,
};
```

- [ ] **Step 5: Build**

Run: `zig build`
Expected: exits 0, no errors.

- [ ] **Step 6: Verify the apply path (drag effect == actor_edit_move) via MCP**

The drag writes `s_voff`, identical to the already-working `actor_edit_move`. Confirm that path moves the part and the handle tracks it:

```bash
cd /Users/wjbr/src/quake1.ai
# (relaunch + editor/editor_mode 2/actor_edit 1 if not already running)
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit_sel 2"}'        # head
python3 scripts/mcp_call.py console_exec '{"command":"actor_edit_move 2 30 0 0"}' # +X 30
python3 scripts/mcp_call.py wait_frames '{"frames":8}'
python3 scripts/mcp_call.py screenshot_gpu '{"path":"gizmo_t3.png"}'
python3 scripts/mcp_call.py wait_frames '{"frames":8}'
```

Read `screenshots/gizmo_t3.png`. Expected: the head cube is shifted along +X and the axis handles have moved with it (centroid recomputed from the edited mesh). This confirms the shared accumulator → `edit_rebuild` → render path the drag uses.

- [ ] **Step 7: Live drag confirmation (user, at the machine)**

Hand off to the user: in the running editor (Actor mode, Edit geometry on), left-click a part to select it, then drag a colored axis handle. Expected: the part slides along that axis under the cursor; the grabbed axis draws white while dragging; releasing ends the drag. (This is the only mouse-gesture step; the underlying math is verified above.)

- [ ] **Step 8: Commit**

```bash
cd /Users/wjbr/src/quake1.ai
git add sdlquake/engine/editor/edit_actor.c
git commit -m "feat(actors): actor-editor gizmo — axis drag to translate parts

process_event: LMB grabs an axis handle (ray-vs-axis-line) and drags it,
writing s_voff[mesh][axis] (applied by edit_rebuild); LMB on empty space
picks a part. Grabbed axis highlights white. Translate-gizmo MVP complete;
rotate/scale/joints are later slices.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Done when

- `zig build` is clean.
- In Actor mode with Edit-geometry on: the selected part shows X/Y/Z handles on the backdrop (Task 1); `actor_edit_pick` reports the part under given coords / view centre and misses on empty space (Task 2); dragging a handle translates the part and `actor_edit_move` parity holds (Task 3).
- Three commits landed on `master`.

## Out of scope (later slices)

Rotate + scale handles; joint gizmo (`edit_joint_move`); plane-drag (2-axis) handles; grid snapping; multi-part selection. Tracked in the design doc.

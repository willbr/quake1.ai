# r_drawpaths Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a software-renderer debug overlay (`r_drawpaths` + `r_drawpaths_what`) that visualises Quake's `path_corner` patrol network and each live monster's current movement goal.

**Architecture:** Mirror the existing `r_drawbboxes` overlay in `sdlquake/engine/r_bbox.c`. First refactor shared rasterisation helpers (Bayer dither, Bresenham, viewport clip, projection) out of `r_bbox.c` into a new `sdlquake/engine/r_debugdraw.{c,h}` module; then add `sdlquake/engine/r_paths.{c,h}` that walks `sv.edicts` once per frame and draws sky-blue corner-to-corner edges plus lime-green live-monster→goal links into `vid.buffer` before `VID_Update` blits.

**Tech Stack:** Plain C (engine module, gnu89), Zig 0.16 build system, vendored SDL3. No test framework — verification per task is `zig build` success plus a visual check in-game (per CLAUDE.md).

**Spec:** `docs/superpowers/specs/2026-05-07-r-drawpaths-design.md`.

**Verification convention.** This codebase has no automated tests. Each task ends with: (a) `zig build` succeeds with no warnings, (b) a specific in-game visual check passes, (c) `r_drawbboxes 1` still works exactly as before (regression). The build+launch command throughout is `zig build run -- +map e1m1`.

---

## Task 1: Extract shared debug-draw helpers into `r_debugdraw.{c,h}`

**Files:**
- Create: `sdlquake/engine/r_debugdraw.h`
- Create: `sdlquake/engine/r_debugdraw.c`
- Modify: `sdlquake/engine/r_bbox.c` (replace private helpers with `RDD_*` calls)
- Modify: `build.zig` line 78–82 (add `r_debugdraw.c` to `platform_files`)

**What this is:** A pure refactor — no behaviour change. After this task, `r_drawbboxes 1` must look identical to before. The helpers are then ready to reuse from `r_paths.c` in Task 2+.

- [ ] **Step 1: Create the header `sdlquake/engine/r_debugdraw.h`**

```c
// r_debugdraw.h -- shared software-renderer primitives for engine debug overlays.
//
// Used by r_bbox.c (entity bbox overlay) and r_paths.c (patrol path overlay).
// Each consumer calls RDD_BeginFrame(density) once per frame; if RDD_Visible()
// returns 0 the consumer should early-out. After that, the projection and
// drawing helpers honour the cached Bayer threshold.

#ifndef SDLQUAKE_R_DEBUGDRAW_H
#define SDLQUAKE_R_DEBUGDRAW_H

#include "quakedef.h"

#define RDD_NEAR_CLIP 0.01f

// Per-frame state. Call once before any other RDD_ function.
// density is clamped to [0,1]; rounded threshold is stored internally.
void RDD_BeginFrame(float density_0_1);

// Returns non-zero if the rounded Bayer threshold is > 0. When zero, skip drawing.
int  RDD_Visible(void);

// World -> view space (right, up, forward).
void RDD_ToView(const vec3_t world, vec3_t out_view);

// View-space -> screen pixel coords. Returns 0 if view[2] < RDD_NEAR_CLIP
// (caller must clip first), 1 otherwise.
int  RDD_Project(const vec3_t view, float *out_sx, float *out_sy);

// Near-plane clip + project + 2D draw for a line whose endpoints are already
// in view space. r_bbox.c passes pre-cached view[a]/view[b]; r_paths.c
// transforms world->view first via RDD_ToView. Returns nothing — fully clipped
// lines (both endpoints behind near plane) are silently dropped.
void RDD_DrawLine3D_View(const vec3_t view_a, const vec3_t view_b, int color);

// Draw a Bayer-dithered line (clipped to r_refdef.vrect) between two
// already-projected screen-space points.
void RDD_DrawLine2D(int x0, int y0, int x1, int y1, int color);

// Plot a single solid (non-dithered) pixel at the given screen coords,
// clipped to r_refdef.vrect. Use for vertex/corner markers so they read
// clearly even at low density.
void RDD_DrawSolidPixel(int x, int y, int color);

#endif
```

- [ ] **Step 2: Create `sdlquake/engine/r_debugdraw.c`**

This is verbatim-extracted from `r_bbox.c` lines 15, 41, 45–51, 59–75, 98–135 with renames. No logic changes.

```c
// r_debugdraw.c -- shared rasterisation primitives for engine debug overlays.
// Extracted from r_bbox.c (no behavioural change). See r_debugdraw.h for API.

#include <stdlib.h>
#include "quakedef.h"
#include "r_debugdraw.h"

// Software-renderer view-projection state lives in r_shared.h, but that header
// drags in lots of internal driver state. Forward-declare just what we need.
extern float xscale, yscale, xcenter, ycenter;

// 4x4 Bayer ordered-dither matrix, values 0..15. Pixel is written when
// bayer[x%4][y%4] < threshold. Gives a clean stipple at any density.
static const unsigned char bayer4x4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};
static int bayer_threshold;  // density * 16, rounded; 0..16. Set per-frame.

void RDD_BeginFrame(float density_0_1)
{
    if (density_0_1 < 0.0f) density_0_1 = 0.0f;
    if (density_0_1 > 1.0f) density_0_1 = 1.0f;
    bayer_threshold = (int)(density_0_1 * 16.0f + 0.5f);
    if (bayer_threshold > 16) bayer_threshold = 16;
}

int RDD_Visible(void)
{
    return bayer_threshold > 0;
}

void RDD_ToView(const vec3_t world, vec3_t out_view)
{
    vec3_t local;
    VectorSubtract(world, r_origin, local);
    out_view[0] = DotProduct(local, vright);
    out_view[1] = DotProduct(local, vup);
    out_view[2] = DotProduct(local, vpn);
}

int RDD_Project(const vec3_t view, float *out_sx, float *out_sy)
{
    if (view[2] < RDD_NEAR_CLIP) return 0;
    float inv_z = 1.0f / view[2];
    *out_sx = xcenter + xscale * view[0] * inv_z;
    *out_sy = ycenter - yscale * view[1] * inv_z;
    return 1;
}

void RDD_DrawLine3D_View(const vec3_t va, const vec3_t vb, int color)
{
    int   a_in = va[2] >= RDD_NEAR_CLIP;
    int   b_in = vb[2] >= RDD_NEAR_CLIP;
    vec3_t ca, cb;
    float sax, say, sbx, sby;

    if (!a_in && !b_in) return;

    if (a_in) { VectorCopy(va, ca); }
    else {
        float t = (RDD_NEAR_CLIP - va[2]) / (vb[2] - va[2]);
        ca[0] = va[0] + t * (vb[0] - va[0]);
        ca[1] = va[1] + t * (vb[1] - va[1]);
        ca[2] = RDD_NEAR_CLIP;
    }
    if (b_in) { VectorCopy(vb, cb); }
    else {
        float t = (RDD_NEAR_CLIP - vb[2]) / (va[2] - vb[2]);
        cb[0] = vb[0] + t * (va[0] - vb[0]);
        cb[1] = vb[1] + t * (va[1] - vb[1]);
        cb[2] = RDD_NEAR_CLIP;
    }

    RDD_Project(ca, &sax, &say);
    RDD_Project(cb, &sbx, &sby);
    RDD_DrawLine2D((int)sax, (int)say, (int)sbx, (int)sby, color);
}

void RDD_DrawLine2D(int x0, int y0, int x1, int y1, int color)
{
    const int xmin = r_refdef.vrect.x;
    const int ymin = r_refdef.vrect.y;
    const int xmax = r_refdef.vrect.x + r_refdef.vrect.width;   // exclusive
    const int ymax = r_refdef.vrect.y + r_refdef.vrect.height;  // exclusive
    int dx, dy, sx, sy, err, e2;

    if ((x0 < xmin && x1 < xmin) || (x0 >= xmax && x1 >= xmax) ||
        (y0 < ymin && y1 < ymin) || (y0 >= ymax && y1 >= ymax))
        return;

    dx =  abs(x1 - x0); sx = x0 < x1 ? 1 : -1;
    dy = -abs(y1 - y0); sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;)
    {
        if (x0 >= xmin && x0 < xmax && y0 >= ymin && y0 < ymax &&
            bayer4x4[y0 & 3][x0 & 3] < bayer_threshold)
            vid.buffer[y0 * vid.rowbytes + x0] = (byte)color;
        if (x0 == x1 && y0 == y1) break;
        e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void RDD_DrawSolidPixel(int x, int y, int color)
{
    if (x >= r_refdef.vrect.x && x < r_refdef.vrect.x + r_refdef.vrect.width &&
        y >= r_refdef.vrect.y && y < r_refdef.vrect.y + r_refdef.vrect.height)
        vid.buffer[y * vid.rowbytes + x] = (byte)color;
}
```

- [ ] **Step 3: Refactor `sdlquake/engine/r_bbox.c` to use the shared module**

Open `sdlquake/engine/r_bbox.c` and apply these edits in order.

(a) Replace the include / extern block at top (lines 8–16) — keep the `scr_con_current` extern, remove the now-shared `xscale`/etc externs (they live in `r_debugdraw.c` now), add the new include:

```c
#include <stdlib.h>

#include "quakedef.h"
#include "r_bbox.h"
#include "r_debugdraw.h"

extern float scr_con_current;  // pixel height the dropped-down console covers
```

(b) Delete every duplicated symbol — they all live in `r_debugdraw.c` now:

- `#define BBOX_NEAR_CLIP 0.01f`
- The `bayer4x4` static const array and the `static int bayer_threshold;` declaration that follows it
- `static void to_view_space(...)` and its body
- `static void project_view(...)` and its body
- `static void draw_line(...)` and its body
- `static void plot_vertex(...)` and its body

Keep: the colour `#define`s, `cvar_t r_drawbboxes`, `bbox_edges[12][2]`, `color_for_edict`, `draw_bbox`, `RBBox_Init`, `RBBox_Draw`, the `chase_active` extern.

(c) Replace the per-call sites inside `draw_bbox` and `RBBox_Draw`:

In `draw_bbox`, change the `to_view_space(corner, view[c])` calls to `RDD_ToView(corner, view[c])`.

In `draw_bbox`, the per-vertex plot block becomes:

```c
for (c = 0; c < 8; c++)
{
    if (view[c][2] >= RDD_NEAR_CLIP)
    {
        float vx, vy;
        RDD_Project(view[c], &vx, &vy);
        RDD_DrawSolidPixel((int)vx, (int)vy, color);
    }
}
```

In `draw_bbox`, replace the entire 12-edge per-edge clip+project+draw block (lines 173–202 in the current file) with one call per edge — the clipping now lives in the shared module:

```c
for (e = 0; e < 12; e++)
{
    int a = bbox_edges[e][0], b = bbox_edges[e][1];
    RDD_DrawLine3D_View(view[a], view[b], color);
}
```

The local `ca`, `cb`, `sax`, `say`, `sbx`, `sby`, `a_in`, `b_in` variables and their setup go away.

In `RBBox_Draw`, replace the manual `bayer_threshold` setup block (lines 219–225 in the current file) with:

```c
RDD_BeginFrame(r_drawbboxes.value);
if (!RDD_Visible()) return;
```

(d) Sanity check after edits: `r_bbox.c` no longer references `bayer_threshold`, `bayer4x4`, `to_view_space`, `project_view`, `draw_line`, `plot_vertex`, `xscale`, `yscale`, `xcenter`, `ycenter`, or `BBOX_NEAR_CLIP`. The file should be ~50 lines shorter.

- [ ] **Step 4: Wire `r_debugdraw.c` into the build**

Edit `build.zig` lines 78–82, inserting `r_debugdraw.c` so it compiles before `r_bbox.c`:

```zig
        "sdlquake/engine/hotreload.c",
        "sdlquake/engine/sv_bridge.c",
        "sdlquake/engine/imgui_layer.c",
        "sdlquake/engine/r_debugdraw.c",
        "sdlquake/engine/r_bbox.c",
    };
```

- [ ] **Step 5: Build**

Run: `zig build`
Expected: build succeeds with no errors and no new warnings.

If it fails: usually a missing extern or a stray reference to a deleted symbol. Re-check that step 3(b) deletions cover every duplicate in `r_bbox.c`.

- [ ] **Step 6: Visual regression check**

Run: `zig build run -- +map e1m1`

In the in-game console (`~`), type:
```
r_drawbboxes 1
```

Expected: bboxes render exactly as before — sky-blue triggers, red monsters, white player, yellow brushes, etc. Try `r_drawbboxes 0.5` and confirm Bayer stipple still works. Try `r_drawbboxes 0` to turn off.

If it differs from pre-refactor behaviour, the bug is in step 3(c) — re-check the `RDD_*` substitutions match the original argument order.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/engine/r_debugdraw.h sdlquake/engine/r_debugdraw.c sdlquake/engine/r_bbox.c build.zig
git commit -m "refactor(engine): extract r_debugdraw shared raster primitives

Pull the Bayer-dither matrix, viewport-clipped Bresenham, world->view
projection, and per-frame density setup out of r_bbox.c into a new
r_debugdraw module so the upcoming r_drawpaths overlay can share them."
```

---

## Task 2: Add `r_paths.{c,h}` skeleton — register cvars, no-op draw

**Files:**
- Create: `sdlquake/engine/r_paths.h`
- Create: `sdlquake/engine/r_paths.c`
- Modify: `sdlquake/platform/vid_sdl.c` line 180 (add `RPaths_Init()`) and line 214 (add `RPaths_Draw()` before `RBBox_Draw()`)
- Modify: `build.zig` (add `r_paths.c`)

After this task, `r_drawpaths 1` and `r_drawpaths_what 3` are valid console commands but draw nothing yet. Engine still builds and `r_drawbboxes` still works.

- [ ] **Step 1: Create `sdlquake/engine/r_paths.h`**

```c
// r_paths.h -- debug patrol-path overlay (cvars r_drawpaths, r_drawpaths_what).

#ifndef SDLQUAKE_R_PATHS_H
#define SDLQUAKE_R_PATHS_H

void RPaths_Init(void);   // register cvars; call after Cvar/Cmd are up
void RPaths_Draw(void);   // rasterise paths into vid.buffer; call before VID_Update blit

#endif
```

- [ ] **Step 2: Create `sdlquake/engine/r_paths.c` skeleton**

```c
// r_paths.c -- patrol-path debug overlay for the software renderer.
//
// Toggle with the cvar r_drawpaths (0..1 density, like r_drawbboxes).
// r_drawpaths_what is a bitmask: bit 0 = static path_corner network,
// bit 1 = each live monster's current goalentity link. Default 3.
//
// Walks sv.edicts each frame. No PVS culling — debug overlay shows the
// whole network through walls, like r_drawbboxes.

#include <string.h>

#include "quakedef.h"
#include "r_debugdraw.h"
#include "r_paths.h"

extern float scr_con_current;

// Sky blue (244) - same as BBOX_COLOR_TRIGGER in r_bbox.c. path_corner is a
// SOLID_TRIGGER so when both overlays are on, trigger boxes and the path
// graph share a colour and read as the same system.
#define PATHS_COLOR_STATIC 244
// Bright lime green (220). Distinct from monster red (251) and trigger sky
// blue (244) so when all three layers compose they stay readable.
#define PATHS_COLOR_LIVE   220

cvar_t r_drawpaths      = {"r_drawpaths",      "0"};
cvar_t r_drawpaths_what = {"r_drawpaths_what", "3"};

void RPaths_Init(void)
{
    Cvar_RegisterVariable(&r_drawpaths);
    Cvar_RegisterVariable(&r_drawpaths_what);
}

void RPaths_Draw(void)
{
    int what;

    if (r_drawpaths.value <= 0.0f) return;
    if (cls.state != ca_connected) return;
    if (!cl.worldmodel) return;
    if (!vid.buffer) return;
    if (!sv.active) return;
    if (key_dest != key_game) return;
    if (scr_con_current > 0) return;

    what = (int)r_drawpaths_what.value;
    if (what == 0) return;

    RDD_BeginFrame(r_drawpaths.value);
    if (!RDD_Visible()) return;

    // Static graph (bit 0) and live monster links (bit 1) — implemented in
    // later tasks.
    (void)what;
}
```

- [ ] **Step 3: Wire `RPaths_Init` and `RPaths_Draw` into `vid_sdl.c`**

Open `sdlquake/platform/vid_sdl.c`. Near the top with the other engine includes, add:

```c
#include "r_paths.h"
```

(Place it adjacent to the existing `#include "r_bbox.h"` line — find that line first to confirm where the bbox header is included; mirror it.)

At line 180 (just after `RBBox_Init();` inside the `if (!sys_headless)` block):

```c
        RBBox_Init();
        RPaths_Init();
```

At line 214 (the call site of `RBBox_Draw`), reorder so paths render under bboxes:

```c
    RPaths_Draw();
    RBBox_Draw();
```

- [ ] **Step 4: Wire `r_paths.c` into the build**

Edit `build.zig` so `r_paths.c` is in `platform_files` immediately after `r_bbox.c`:

```zig
        "sdlquake/engine/r_debugdraw.c",
        "sdlquake/engine/r_bbox.c",
        "sdlquake/engine/r_paths.c",
    };
```

- [ ] **Step 5: Build**

Run: `zig build`
Expected: build succeeds with no errors.

- [ ] **Step 6: Visual smoke test**

Run: `zig build run -- +map e1m1`

In the console, type each of:
```
r_drawpaths 1
r_drawpaths_what 3
r_drawpaths 0
```

Expected: no crashes, no rendering changes (the draw function is still a stub). `r_drawbboxes 1` still works.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/engine/r_paths.h sdlquake/engine/r_paths.c sdlquake/platform/vid_sdl.c build.zig
git commit -m "feat(engine): r_drawpaths skeleton — register cvars, wire into VID_Update

No drawing yet; subsequent commits implement the static path_corner
graph and live monster goal links."
```

---

## Task 3: Implement static path_corner graph (bit 0) — edges + corner dots

**Files:**
- Modify: `sdlquake/engine/r_paths.c` (replace the `(void)what;` stub)

After this task, `r_drawpaths 1` with `r_drawpaths_what 1` (or 3) draws sky-blue lines between every `path_corner` pair, with a 3×3 sky-blue dot at every corner. No arrowheads yet.

- [ ] **Step 1: Add the `path_corner` walk + edge drawing**

Open `sdlquake/engine/r_paths.c`. Add these helpers above `RPaths_Draw`:

```c
// Find an edict by string field (classname / target / targetname) match.
// Returns NULL if not found. Mirrors what game/ai.c does via eng->ED_Find,
// but engine-side we call it directly since no game ABI hop is needed.
static edict_t *find_by_targetname(const char *name)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 1; i < sv.num_edicts; i++)
    {
        edict_t *e = EDICT_NUM(i);
        if (e->free) continue;
        if (e->v.targetname && !strcmp(e->v.targetname, name))
            return e;
    }
    return NULL;
}

// Project a world-space point to screen coords. Returns 1 on success
// (point in front of near plane); 0 if behind (caller should skip).
static int project_world(const vec3_t world, float *out_sx, float *out_sy)
{
    vec3_t view;
    RDD_ToView(world, view);
    return RDD_Project(view, out_sx, out_sy);
}

// Draw a 3x3 solid block centred on a screen pixel — bigger than the 1-pixel
// vertex used in r_bbox.c so corner dots stay visible at distance.
static void plot_dot_3x3(int sx, int sy, int color)
{
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            RDD_DrawSolidPixel(sx + dx, sy + dy, color);
}

// Thin world-space wrapper over RDD_DrawLine3D_View — does the world->view
// transform for both endpoints, then hands off to the shared module.
static void draw_line_3d(const vec3_t a_world, const vec3_t b_world, int color)
{
    vec3_t va, vb;
    RDD_ToView(a_world, va);
    RDD_ToView(b_world, vb);
    RDD_DrawLine3D_View(va, vb, color);
}
```

Now replace the `(void)what;` line in `RPaths_Draw` with the static-graph block:

```c
    if (what & 1)
    {
        int i;
        for (i = 1; i < sv.num_edicts; i++)
        {
            edict_t *ed = EDICT_NUM(i);
            float    sx, sy;

            if (ed->free) continue;
            if (!ed->v.classname) continue;
            if (strcmp(ed->v.classname, "path_corner") != 0) continue;

            // Corner dot.
            if (project_world(ed->v.origin, &sx, &sy))
                plot_dot_3x3((int)sx, (int)sy, PATHS_COLOR_STATIC);

            // Outgoing edge to the next corner (if this corner has a target).
            if (ed->v.target && ed->v.target[0])
            {
                edict_t *next = find_by_targetname(ed->v.target);
                if (next && next != ed)
                    draw_line_3d(ed->v.origin, next->v.origin, PATHS_COLOR_STATIC);
            }
        }
    }
```

- [ ] **Step 2: Build**

Run: `zig build`
Expected: build succeeds.

If `EDICT_NUM`, `sv.num_edicts`, or `edict_t` field accesses don't compile, double-check the `quakedef.h` include is in place — `r_bbox.c` uses the same accessors successfully, so no extra header is needed.

- [ ] **Step 3: Visual check on e1m1**

Run: `zig build run -- +map e1m1`

In console:
```
r_drawpaths 1
```

Expected: sky-blue dots visible at every `path_corner` location, with sky-blue Bresenham lines between corners that target each other. The patrol path the dogs and grunts walk in the start area should be visibly drawn.

Try `r_drawpaths 0.5` — stipple density halves. Try `r_drawpaths_what 0` — paths disappear (without changing `r_drawpaths`).

If no paths appear: the most likely cause is `sv.num_edicts == 0` — check that `sv.active` is true on a freshly-loaded map. Second-most-likely: `ed->v.classname` is null on real edicts. Print one entity's classname via `Con_Printf` to verify.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine/r_paths.c
git commit -m "feat(engine): r_drawpaths bit 0 — static path_corner graph

Walks sv.edicts each frame, drawing sky-blue corner dots and edges
to each corner's targetname. Shared rasteriser via r_debugdraw."
```

---

## Task 4: Add arrowheads (midpoint + near destination) to static-graph edges

**Files:**
- Modify: `sdlquake/engine/r_paths.c`

After this task, every static edge has a chevron arrowhead at its screen-space midpoint plus a second one offset ~8 px back from the destination, both pointing along the edge.

- [ ] **Step 1: Add the arrowhead helper above `RPaths_Draw`**

```c
// Draw a chevron arrowhead pointing from (sx0,sy0) toward (sx1,sy1), placed
// at parametric position t along the segment (0=start, 1=end). Drawn in
// 2D after projection so the on-screen size is independent of edge length.
//
// Arms are LEN pixels long and form a 2*HALF_ANGLE chevron.
static void draw_arrowhead_2d(float sx0, float sy0, float sx1, float sy1,
                              float t, int color)
{
    const float LEN = 6.0f;
    const float COS_HA = 0.9063f;   // cos(25 deg)
    const float SIN_HA = 0.4226f;   // sin(25 deg)
    float dx = sx1 - sx0, dy = sy1 - sy0;
    float len = (float)sqrt(dx*dx + dy*dy);
    float ux, uy;     // unit vector along edge
    float tip_x, tip_y;
    float ax, ay, bx, by;

    if (len < 1.0f) return;          // degenerate
    ux = dx / len; uy = dy / len;
    tip_x = sx0 + dx * t;
    tip_y = sy0 + dy * t;

    // Each arm = tip - LEN * (R(+/-HA) * unit). The arrowhead points along
    // the edge direction, so arms project backward from the tip.
    ax = tip_x - LEN * ( ux * COS_HA + uy * SIN_HA);
    ay = tip_y - LEN * (-ux * SIN_HA + uy * COS_HA);
    bx = tip_x - LEN * ( ux * COS_HA - uy * SIN_HA);
    by = tip_y - LEN * ( ux * SIN_HA + uy * COS_HA);

    RDD_DrawLine2D((int)tip_x, (int)tip_y, (int)ax, (int)ay, color);
    RDD_DrawLine2D((int)tip_x, (int)tip_y, (int)bx, (int)by, color);
}
```

Add `#include <math.h>` near the top of the file if it isn't already there (for `sqrt`).

- [ ] **Step 2: Replace the simple `draw_line_3d` call in the static-graph block with a wrapper that also draws arrowheads**

Add this helper above `RPaths_Draw` (right after `draw_line_3d`):

```c
// Like draw_line_3d but also draws a midpoint arrowhead and a second
// arrowhead 8 px back from the destination. Skips arrowheads if either
// endpoint is behind the near plane (no meaningful screen midpoint).
static void draw_edge_with_arrows(const vec3_t a_world, const vec3_t b_world, int color)
{
    vec3_t va, vb;
    float  sax, say, sbx, sby, dx, dy, len, t_back;

    draw_line_3d(a_world, b_world, color);

    RDD_ToView(a_world, va);
    RDD_ToView(b_world, vb);
    if (va[2] < RDD_NEAR_CLIP || vb[2] < RDD_NEAR_CLIP) return;

    RDD_Project(va, &sax, &say);
    RDD_Project(vb, &sbx, &sby);

    // Midpoint arrowhead.
    draw_arrowhead_2d(sax, say, sbx, sby, 0.5f, color);

    // Second arrowhead 8 px back from the destination tip.
    dx = sbx - sax; dy = sby - say;
    len = (float)sqrt(dx*dx + dy*dy);
    if (len < 16.0f) return;       // edge too short for a second arrow
    t_back = 1.0f - (8.0f / len);
    draw_arrowhead_2d(sax, say, sbx, sby, t_back, color);
}
```

In `RPaths_Draw`, change the static-graph edge call:

```c
                if (next && next != ed)
                    draw_edge_with_arrows(ed->v.origin, next->v.origin, PATHS_COLOR_STATIC);
```

- [ ] **Step 3: Build**

Run: `zig build`
Expected: build succeeds.

- [ ] **Step 4: Visual check**

Run: `zig build run -- +map e1m1`

In console:
```
r_drawpaths 1
```

Expected: each static edge now has a small chevron at its midpoint pointing toward the next corner, and a second chevron just shy of the destination dot. Walk to a corner so an edge fills most of the screen — the arrowheads should remain crisp at fixed pixel size, not stretch.

If arrowheads appear but rotate/skew incorrectly: the rotation matrix in `draw_arrowhead_2d` uses screen-coord conventions (y-down). If they look mirrored, swap `+/-` on the SIN_HA terms; if they point the wrong way, swap `tip - LEN*...` to `tip + LEN*...`.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine/r_paths.c
git commit -m "feat(engine): r_drawpaths arrowheads on static graph edges

Each path_corner->next edge now has a chevron at its screen midpoint
plus a second chevron ~8 px back from the destination."
```

---

## Task 5: Implement live monster goalentity link (bit 1)

**Files:**
- Modify: `sdlquake/engine/r_paths.c`

After this task, with `r_drawpaths_what` having bit 1 set (e.g. `2` or `3`), each living monster has a lime-green arrowed line drawn from its origin to whatever its current `goalentity` is. Combat redirects the line to the player.

- [ ] **Step 1: Add the live-monster block to `RPaths_Draw`**

Inside `RPaths_Draw`, after the existing `if (what & 1)` block, add:

```c
    if (what & 2)
    {
        int i;
        for (i = 1; i < sv.num_edicts; i++)
        {
            edict_t *ed = EDICT_NUM(i);
            edict_t *goal;

            if (ed->free) continue;
            if (!((int)ed->v.flags & FL_MONSTER)) continue;
            if (ed->v.health <= 0) continue;       // skip corpses

            goal = ed->v.goalentity;
            if (!goal || goal == sv.edicts) continue;   // null or world
            if (goal == ed) continue;                    // self-loop
            if (goal->free) continue;

            draw_edge_with_arrows(ed->v.origin, goal->v.origin, PATHS_COLOR_LIVE);
        }
    }
```

`sv.edicts` is the world edict (index 0). Comparing `goal == sv.edicts` is the engine-side equivalent of the game-DLL `goal == g->world` check.

- [ ] **Step 2: Build**

Run: `zig build`
Expected: build succeeds.

If `FL_MONSTER` doesn't compile: include the right header. `r_bbox.c` uses `FL_MONSTER` and `FL_CLIENT` already with just `#include "quakedef.h"`, so it should already work.

- [ ] **Step 3: Visual check on e1m1**

Run: `zig build run -- +map e1m1`

In console:
```
r_drawpaths 1
r_drawpaths_what 2
```

Expected: lime-green lines from each living monster (grunt, dog, etc.) to the corner it's currently heading toward, with arrowheads. Watch a grunt walk between two corners — the line endpoint switches when the grunt touches its current corner.

Now provoke combat (shoot a grunt). The grunt's lime-green line should redirect to the player and follow the player as they move.

Then:
```
r_drawpaths_what 3
```

Expected: both static (sky blue) and live (lime green) overlays compose. They should be visually distinguishable.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine/r_paths.c
git commit -m "feat(engine): r_drawpaths bit 1 — live monster goal links

Draws a lime-green line from each living monster (FL_MONSTER, health>0)
to its current goalentity. During combat the line follows the player."
```

---

## Task 6: Verification pass — density gradient, bitmask, regression

**Files:** none (verification only). If any check fails, fix in place and amend the relevant commit.

- [ ] **Step 1: Density gradient**

Run: `zig build run -- +map e1m1`

In console, in this order:
```
r_drawpaths 0
r_drawpaths 0.25
r_drawpaths 0.5
r_drawpaths 0.75
r_drawpaths 1
```

Expected: progressively denser stipple from invisible → fully solid. Edges and arrowheads dither together; corner dots stay solid throughout.

- [ ] **Step 2: Bitmask**

In the same session, with `r_drawpaths 1`:
```
r_drawpaths_what 0    # nothing visible
r_drawpaths_what 1    # static graph only (sky blue, no green)
r_drawpaths_what 2    # live monster links only (green, no static)
r_drawpaths_what 3    # both layers
```

- [ ] **Step 3: r_drawbboxes regression**

In the same session:
```
r_drawpaths 0
r_drawbboxes 1
```

Expected: bboxes render exactly as on master before this branch — sky-blue triggers, red monsters, white player, yellow brushes, etc. No cosmetic regression.

Then turn both on:
```
r_drawpaths 1
r_drawbboxes 1
```

Expected: bboxes render *over* paths (paths drawn first, bboxes second). path_corner trigger boxes (sky blue) sit on top of the path graph (also sky blue) — they should align visually.

- [ ] **Step 4: No-server case**

Open the main menu (Esc, then exit any in-progress game with `disconnect` or by going to the main menu). Type:
```
r_drawpaths 1
```

Expected: no crash, no draws (because `sv.active` is false and `cls.state != ca_connected`). Closing the menu and starting a new game should resume drawing.

- [ ] **Step 5: Cvar reset across launches**

Quit the engine entirely. Relaunch:
```
zig build run -- +map e1m1
```

Expected: `r_drawpaths` defaults back to 0 — values do not persist (matches `r_drawbboxes`).

- [ ] **Step 6: If any check failed**

Fix the issue in the relevant source file. Re-run that check. Once green, amend the commit from the task that introduced the bug:

```bash
git add sdlquake/engine/r_paths.c
git commit --amend --no-edit
```

If everything passed, no further commit is needed for this task.

---

## Self-review checklist (already performed by the plan author)

- **Spec coverage:** Cvars ✓ (Task 2), file layout ✓ (Task 1, 2), render hook ✓ (Task 2 step 3), graph walk ✓ (Task 3, 5), visual style — colours ✓ (Task 2 constants), corner dots ✓ (Task 3), arrowheads ✓ (Task 4), dither ✓ (Task 1 + Task 2 step 2), edge cases — server gates ✓ (Task 2), behind-near-plane ✓ (Task 3 `draw_line_3d` + Task 4 arrowhead skip), cycles ✓ (Task 3 each-edge-once, no recursion), duplicate targetname ✓ (`find_by_targetname` returns first match), terminal corner ✓ (Task 3 the `if (target && target[0])` guard), dead monsters ✓ (Task 5 `health <= 0` skip), hot-reload ✓ (no game ABI change touched).
- **Type consistency:** `RPaths_Init` / `RPaths_Draw`, `r_drawpaths` / `r_drawpaths_what`, `PATHS_COLOR_STATIC` / `PATHS_COLOR_LIVE`, `RDD_*` family, `find_by_targetname`, `project_world`, `plot_dot_3x3`, `draw_line_3d`, `draw_arrowhead_2d`, `draw_edge_with_arrows` — all named consistently across tasks.
- **Placeholder scan:** no TBD/TODO/"add error handling"/"similar to Task N" text.

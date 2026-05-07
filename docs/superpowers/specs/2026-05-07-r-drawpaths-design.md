# r_drawpaths — patrol path debug overlay

**Date:** 2026-05-07
**Status:** Design approved, ready for implementation plan

## Goal

Add a software-renderer debug overlay that visualises Quake's `path_corner` patrol network and each live monster's current movement goal. Modeled after the existing `r_drawbboxes` overlay (`sdlquake/engine/r_bbox.c`) so the two compose cleanly when both are enabled.

## Cvars

```c
cvar_t r_drawpaths      = {"r_drawpaths",      "0"};  // density 0..1
cvar_t r_drawpaths_what = {"r_drawpaths_what", "3"};  // bitmask
```

- `r_drawpaths` — Bayer-stippled density, identical semantics to `r_drawbboxes`. `0` = off, `1` = solid, fractions render via the same 4×4 ordered-dither matrix used in `r_bbox.c`.
- `r_drawpaths_what` — bitmask. Bit 0: render the static `path_corner` graph. Bit 1: render each live monster's current `goalentity` link. Default `3` shows both. `0` is equivalent to `r_drawpaths 0` (early-out).

Both cvars are registered from `RPaths_Init()`, called next to `RBBox_Init()` in `sdlquake/platform/vid_sdl.c`.

## File layout

```
sdlquake/engine/
  r_debugdraw.h     # new — shared projection / line / dither helpers
  r_debugdraw.c     # new — extracted from r_bbox.c (no behaviour change)
  r_bbox.c          # refactored to call into r_debugdraw
  r_paths.h         # new — RPaths_Init, RPaths_Draw
  r_paths.c         # new — patrol-path overlay
```

The shared module exposes:

```c
void RDD_BeginFrame(float density_0_1);                            // sets bayer_threshold
int  RDD_Visible(void);                                            // bayer_threshold > 0
void RDD_ToView(const vec3_t world, vec3_t out_view);              // world -> view space
int  RDD_Project(const vec3_t view, float *sx, float *sy);         // 0 if behind near plane
void RDD_DrawLine3D_View(const vec3_t va, const vec3_t vb, int color); // near-clip + project + 2D draw
void RDD_DrawLine2D(int x0, int y0, int x1, int y1, int color);    // dithered, viewport-clipped
void RDD_DrawSolidPixel(int x, int y, int color);                  // no dither, viewport-clipped
```

`RDD_DrawLine3D_View` takes view-space endpoints (so `r_bbox.c` can pass its pre-cached `view[8]` corner array without redundant transforms) and performs the near-plane clip + projection + 2D draw via `RDD_DrawLine2D`. `RDD_DrawLine2D` is the Bresenham raster + Bayer dither + viewport-rect clip currently inlined as `draw_line` in `r_bbox.c`. `RDD_DrawSolidPixel` is the no-dither vertex plot from `plot_vertex` in `r_bbox.c`. Refactor preserves existing `r_drawbboxes` behaviour.

`r_paths.c` keeps a thin private wrapper `draw_line_3d(world_a, world_b, color)` that calls `RDD_ToView` for both endpoints then `RDD_DrawLine3D_View`. Arrowheads are computed in screen space after projection and use `RDD_DrawLine2D` directly.

## Render hook

`RPaths_Draw()` is called from `vid_sdl.c` immediately before `RBBox_Draw()` (i.e. paths render *under* bboxes). Both run before the framebuffer blit in `VID_Update`, so the overlay lands on top of the 3D scene but under the HUD/console/menu.

Same gates as `r_bbox.c:215-225`:
- `cls.state == ca_connected`
- `cl.worldmodel != NULL`
- `vid.buffer != NULL`
- `key_dest == key_game`
- `scr_con_current == 0`
- additionally `sv.active` (we read server edicts directly)

## Walking the graph

`RPaths_Draw()` loops `for (i = 1; i < sv.num_edicts; i++)`. For each non-free edict:

**Static graph (bit 0 of `r_drawpaths_what`):**
- skip unless `classname == "path_corner"`
- draw a 3×3 solid dot at `ed->origin` (skipped if behind near plane)
- if `ed->v.target` is non-empty, look up `next = ED_Find(world, "targetname", ed->v.target)`
- if `next != world`, draw an edge from `ed->origin` to `next->origin`, plus an arrowhead at the screen midpoint and a second arrowhead near the destination end

**Live monster links (bit 1 of `r_drawpaths_what`):**
- skip unless `(flags & FL_MONSTER) && health > 0`
- if `goalentity` is set, not `world`, and not `self`, draw an edge from `ed->origin` to `goalentity->origin` plus arrowheads (midpoint and near destination)
- live links to *any* goal are drawn, including the player during combat — this is intentional AI debug signal
- no dot is drawn at either end: the source monster has its own `r_drawbboxes` entry and the destination entity (corner, player, monster) does too. With bit 0 also set, static `path_corner` dots still appear underneath

Cost: O(`num_edicts`) per frame, max a few hundred on real maps. No allocations, no caching.

## Visual style

Palette indices (verified against `id1/PAK0.PAK gfx/palette.lmp`, distinct from `r_bbox.c`'s set):

- **Static path edges + corner dots:** `244` (sky blue) — same as `BBOX_COLOR_TRIGGER` in `r_bbox.c`. `path_corner` is a `SOLID_TRIGGER`, so when both overlays are on, the trigger boxes and the path graph share a color and read as the same system.
- **Live monster→goal link:** `220` (bright lime green) — distinct from the red monster bbox (251) and the sky-blue static graph, so all three layers stay readable when composed.

**Corner dots:** 3×3 solid block at the projected pixel. Skipped if the corner is behind the near plane.

**Arrowheads:** at the screen-space midpoint of each edge, plus a second one offset ~8 px back from the destination along the edge. Each arrowhead is two short lines forming a chevron pointing along the edge direction. Length ≈ 6 px, half-angle ≈ 25°. Computed in 2D after projection so they scale naturally with edge length. Only drawn when both endpoints are in front of the near plane.

**Dither:** edges and arrowheads use the per-frame Bayer threshold from `RDD_BeginFrame(r_drawpaths.value)`. Corner dots are drawn solid (no dither) so nodes stay readable at low density — same trick `r_bbox.c` uses for bbox vertex pixels.

## Edge cases

- **No server / not connected / console / menu:** early-out via the gates above.
- **Endpoints behind near plane:** edge endpoints clipped to the near plane using the same logic as `r_bbox.c:185-201`. Arrowheads suppressed when either endpoint is clipped (no meaningful screen midpoint).
- **Cyclic paths:** no special handling needed; each edge is drawn once from its source corner, so cycles render as closed loops. No recursion.
- **Duplicate `targetname`:** `ED_Find` returns the first match — matches the runtime AI's behaviour. We render that one.
- **Terminal `path_corner` (no `target`):** dot only, no outgoing edge.
- **Dead monster:** `health > 0` filter prevents corpses from drawing a stale link to their last goal. (Bare `FL_MONSTER` would match corpses, since the flag persists past death.)
- **Hot-reload of `game.dll`:** entity field offsets are stable in `progdefs.h`; the engine reads `target`, `targetname`, `flags`, `health`, `goalentity`, `origin`, `classname` the same way `r_bbox.c` already does. No `engine_api_t` ABI change.

## Out of scope

- PVS culling (the overlay is meant to show the network through walls).
- Configurable colors / arrowhead size (hardcoded constants are fine for a dev overlay).
- Visualising `monster.movetarget` separately from `goalentity` (they diverge only briefly during combat-to-patrol transitions; not worth the extra signal).
- Saving cvar state to config; values reset to defaults each launch, like `r_drawbboxes`.

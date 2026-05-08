// render_wire.c -- M1 wireframe pass + screen<->world projection helpers.
//
// Project each face's vertex loop, clip line segments at the near plane,
// Bresenham-draw 8-bit lines into vid.buffer. No z-buffer, no occlusion;
// editor brushes overdraw the world (intentional — they need to be visible
// when authoring inside an existing room).
//
// Also exposes Editor_ScreenToRay used by picking + gizmo.

#include "quakedef.h"
#include "r_local.h"
#include "edit_scene.h"
#include "editor.h"
#include "editor_internal.h"

#include <math.h>

// Engine projection state (defined in r_main.c / r_misc.c).
extern float        xcenter, ycenter;
extern float        xscale, yscale;
extern vec3_t       vpn, vright, vup;
extern vec3_t       r_origin;

// -----------------------------------------------------------------------------
// Projection
// -----------------------------------------------------------------------------

// World point -> view space (X right, Y up, Z forward).
static void world_to_view(const vec3_t world, vec3_t view)
{
    vec3_t local;
    VectorSubtract(world, r_origin, local);
    view[0] = DotProduct(local, vright);
    view[1] = DotProduct(local, vup);
    view[2] = DotProduct(local, vpn);
}

// View -> screen. Returns 1 if in front of near plane.
static int view_to_screen(const vec3_t view, float *sx, float *sy)
{
    float z = view[2];
    if (z < NEAR_CLIP) return 0;
    *sx = xcenter + (xscale / z) * view[0];
    *sy = ycenter - (yscale / z) * view[1];
    return 1;
}

int Editor_ProjectWorld(const vec3_t world, float *out_sx, float *out_sy)
{
    vec3_t view;
    world_to_view(world, view);
    return view_to_screen(view, out_sx, out_sy);
}

// Build a world-space ray from a screen pixel.
void Editor_ScreenToRay(float sx, float sy, vec3_t out_origin, vec3_t out_dir)
{
    float ux = (sx - xcenter) / xscale;
    float uy = -(sy - ycenter) / yscale;
    int i;
    for (i = 0; i < 3; i++)
        out_dir[i] = ux * vright[i] + uy * vup[i] + vpn[i];
    {
        float l = sqrtf(out_dir[0]*out_dir[0] + out_dir[1]*out_dir[1] + out_dir[2]*out_dir[2]);
        if (l > 1e-6f) { out_dir[0] /= l; out_dir[1] /= l; out_dir[2] /= l; }
    }
    VectorCopy(r_origin, out_origin);
}

// -----------------------------------------------------------------------------
// Line drawing
// -----------------------------------------------------------------------------

// Bresenham line into vid.buffer (8-bit indexed). Clips to viewport.
static void draw_line8(int x0, int y0, int x1, int y1, byte color)
{
    int W = (int)vid.width, H = (int)vid.height;
    int dx, dy, sx, sy, err, e2;
    byte *base = vid.buffer;
    int  rb   = (int)vid.rowbytes;

    // Cohen-Sutherland-ish clip — give up on segments fully outside.
    if ((x0 < 0 && x1 < 0) || (x0 >= W && x1 >= W) ||
        (y0 < 0 && y1 < 0) || (y0 >= H && y1 >= H))
        return;

    dx = abs(x1 - x0);
    dy = -abs(y1 - y0);
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;)
    {
        if ((unsigned)x0 < (unsigned)W && (unsigned)y0 < (unsigned)H)
            base[y0 * rb + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Project a world-space line, clip at the near plane in view space, then
// Bresenham-draw. Both endpoints are passed in world space.
void Editor_DrawLine3D(const vec3_t a_world, const vec3_t b_world, byte color)
{
    vec3_t va, vb;
    float sx0, sy0, sx1, sy1;
    world_to_view(a_world, va);
    world_to_view(b_world, vb);

    // Both behind near plane: cull.
    if (va[2] < NEAR_CLIP && vb[2] < NEAR_CLIP) return;

    // Clip the segment in view space at z = NEAR_CLIP.
    if (va[2] < NEAR_CLIP)
    {
        float t = (NEAR_CLIP - va[2]) / (vb[2] - va[2]);
        va[0] = va[0] + (vb[0] - va[0]) * t;
        va[1] = va[1] + (vb[1] - va[1]) * t;
        va[2] = NEAR_CLIP;
    }
    else if (vb[2] < NEAR_CLIP)
    {
        float t = (NEAR_CLIP - vb[2]) / (va[2] - vb[2]);
        vb[0] = vb[0] + (va[0] - vb[0]) * t;
        vb[1] = vb[1] + (va[1] - vb[1]) * t;
        vb[2] = NEAR_CLIP;
    }

    if (!view_to_screen(va, &sx0, &sy0)) return;
    if (!view_to_screen(vb, &sx1, &sy1)) return;

    draw_line8((int)(sx0 + 0.5f), (int)(sy0 + 0.5f),
               (int)(sx1 + 0.5f), (int)(sy1 + 0.5f), color);
}

// -----------------------------------------------------------------------------
// Cull + draw a brush
// -----------------------------------------------------------------------------

// Cheap reject: every face vertex behind near plane => skip. Not a true frustum
// reject (left/right/top/bottom planes not tested), but good enough for M1 —
// off-screen lines are clipped by draw_line8.
static int brush_visible(const edit_brush_t *b)
{
    int i, k;
    for (i = 0; i < b->numfaces; i++)
    {
        for (k = 0; k < b->faces[i].numverts; k++)
        {
            vec3_t v;
            world_to_view(b->faces[i].verts[k], v);
            if (v[2] >= NEAR_CLIP) return 1;
        }
    }
    return 0;
}

static void draw_brush(const edit_brush_t *b, byte color)
{
    int i, k;
    for (i = 0; i < b->numfaces; i++)
    {
        const edit_face_t *f = &b->faces[i];
        for (k = 0; k < f->numverts; k++)
        {
            const float *a = f->verts[k];
            const float *bb = f->verts[(k + 1) % f->numverts];
            vec3_t aw, bw;
            VectorCopy(a, aw); VectorCopy(bb, bw);
            Editor_DrawLine3D(aw, bw, color);
        }
    }
}

// -----------------------------------------------------------------------------
// Public entry point — called from r_main.c R_RenderView_
// -----------------------------------------------------------------------------

#define EDIT_COLOR_BRUSH        15      // white
#define EDIT_COLOR_SELECTED     79      // bright yellow

void Editor_RenderScene(void)
{
    int i, j;
    edit_brush_t *sel = Scene_GetSelectedBrush();

    if (edit_scene.numentities == 0) return;

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            if (!b->valid) continue;
            if (!brush_visible(b)) continue;
            draw_brush(b, (b == sel) ? EDIT_COLOR_SELECTED : EDIT_COLOR_BRUSH);
        }
    }

    Editor_GizmoDraw();
}

// -----------------------------------------------------------------------------
// Picking
// -----------------------------------------------------------------------------

// Möller-Trumbore-ish ray-vs-convex-polygon. Polygon is in `verts[0..n]`,
// assumed planar and convex. Returns 1 if hit, with t along ray.
static int ray_vs_face(const vec3_t origin, const vec3_t dir,
                       const vec3_t *verts, int n,
                       const vec3_t plane_normal, float plane_dist,
                       float *out_t)
{
    float denom, t;
    vec3_t hit;
    int i;

    denom = DotProduct(dir, plane_normal);
    if (fabsf(denom) < 1e-6f) return 0;
    t = (plane_dist - DotProduct(origin, plane_normal)) / denom;
    if (t < 0.001f) return 0;

    hit[0] = origin[0] + dir[0] * t;
    hit[1] = origin[1] + dir[1] * t;
    hit[2] = origin[2] + dir[2] * t;

    // Inside-test: for each edge, check that the hit is on the same side as
    // the polygon centre. Convex polygon — this is sufficient.
    {
        vec3_t centroid = {0,0,0};
        for (i = 0; i < n; i++)
        {
            centroid[0] += verts[i][0];
            centroid[1] += verts[i][1];
            centroid[2] += verts[i][2];
        }
        centroid[0] /= n; centroid[1] /= n; centroid[2] /= n;

        for (i = 0; i < n; i++)
        {
            const float *a = verts[i];
            const float *b = verts[(i + 1) % n];
            vec3_t edge, edge_normal, va, vc;
            float da, dc;
            VectorSubtract(b, a, edge);
            CrossProduct(edge, plane_normal, edge_normal);
            VectorSubtract(hit, a, va);
            VectorSubtract(centroid, a, vc);
            da = DotProduct(va, edge_normal);
            dc = DotProduct(vc, edge_normal);
            // hit must be on the same side of edge as the centroid.
            if (da * dc < -1e-3f) return 0;
        }
    }
    *out_t = t;
    return 1;
}

int Editor_PickAt(float sx, float sy, int *out_ent, int *out_brush)
{
    vec3_t origin, dir;
    float best_t = 1e30f;
    int best_ent = -1, best_brush = -1;
    int i, j, k;

    Editor_ScreenToRay(sx, sy, origin, dir);

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            if (!b->valid) continue;
            for (k = 0; k < b->numfaces; k++)
            {
                edit_face_t *f = &b->faces[k];
                edit_plane_t *pl = &b->planes[f->plane_idx];
                float t;
                if (ray_vs_face(origin, dir,
                                (const vec3_t *)f->verts, f->numverts,
                                pl->normal, pl->dist, &t))
                {
                    if (t < best_t)
                    {
                        best_t = t;
                        best_ent = i;
                        best_brush = j;
                    }
                }
            }
        }
    }
    if (best_ent < 0) return 0;
    if (out_ent)   *out_ent   = best_ent;
    if (out_brush) *out_brush = best_brush;
    return 1;
}

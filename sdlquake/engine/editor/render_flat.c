// render_flat.c -- M3 flat-shaded fill render style.
//
// For each face: clip the convex polygon at the near plane in view space,
// project to screen space, then scanline-fill into vid.buffer with a single
// palette index. The shade index is a Lambert dot against a fixed world
// light direction, so faces pointing toward the light come out brighter.
//
// Intentionally pretty: no z-buffer, faces overdraw in scene order. With
// editor brushes that don't depth-overlap each other this is fine; for the
// rare overlapping case a back-to-front sort would help but isn't worth the
// cost at editor scales.

#include "quakedef.h"
#include "r_local.h"        // NEAR_CLIP
#include "edit_scene.h"
#include "editor_internal.h"

#include <math.h>

extern vec3_t   vpn, vright, vup;
extern vec3_t   r_origin;
extern float    xcenter, ycenter;
extern float    xscale, yscale;

#define FLAT_MAX_VERTS  (EDIT_MAX_VERTS_PER_FACE + 4)

// World -> view-space (X right, Y up, Z forward).
static void w2v(const vec3_t world, vec3_t view)
{
    vec3_t local;
    VectorSubtract(world, r_origin, local);
    view[0] = DotProduct(local, vright);
    view[1] = DotProduct(local, vup);
    view[2] = DotProduct(local, vpn);
}

// Sutherland-Hodgman clip of a convex view-space polygon against z = NEAR_CLIP.
// Returns the number of vertices in `out` (0 if culled, < 0 on overflow).
static int clip_near(const vec3_t *in, int n_in, vec3_t *out, int max_out)
{
    int n_out = 0;
    int i;
    for (i = 0; i < n_in; i++)
    {
        const float *a = in[i];
        const float *b = in[(i + 1) % n_in];
        int a_in = a[2] >= NEAR_CLIP;
        int b_in = b[2] >= NEAR_CLIP;
        if (a_in)
        {
            if (n_out >= max_out) return -1;
            VectorCopy(a, out[n_out]); n_out++;
        }
        if (a_in != b_in)
        {
            float t = (NEAR_CLIP - a[2]) / (b[2] - a[2]);
            if (n_out >= max_out) return -1;
            out[n_out][0] = a[0] + (b[0] - a[0]) * t;
            out[n_out][1] = a[1] + (b[1] - a[1]) * t;
            out[n_out][2] = NEAR_CLIP;
            n_out++;
        }
    }
    return n_out;
}

// Project view-space (X right, Y up, +Z forward) to integer screen pixels.
static void view_to_screen_i(const vec3_t v, int *out_x, int *out_y)
{
    float inv_z = 1.0f / v[2];
    float sx = xcenter + (xscale * inv_z) * v[0];
    float sy = ycenter - (yscale * inv_z) * v[1];
    *out_x = (int)(sx + 0.5f);
    *out_y = (int)(sy + 0.5f);
}

// Scanline-fill a convex polygon (already in screen-pixel coords) with a
// single palette index. Uses the "min/max X per row" trick — convex polygons
// produce exactly two edge crossings per scanline, so we just walk every
// edge, compute the X at this row, and take min+max.
static void fill_convex_8(const int *xs, const int *ys, int n, byte color)
{
    int W = (int)vid.width, H = (int)vid.height;
    int ymin = ys[0], ymax = ys[0];
    int i, y;
    byte *base = vid.buffer;
    int  rb   = (int)vid.rowbytes;

    for (i = 1; i < n; i++)
    {
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= H) ymax = H - 1;

    for (y = ymin; y <= ymax; y++)
    {
        int xL = 1 << 30, xR = -(1 << 30);
        for (i = 0; i < n; i++)
        {
            int j = (i + 1) % n;
            int y0 = ys[i], y1 = ys[j];
            int yLo = y0 < y1 ? y0 : y1;
            int yHi = y0 > y1 ? y0 : y1;
            if (y0 == y1) continue;
            if (y < yLo || y > yHi) continue;
            // Avoid double-counting at vertex pixels: include the lower
            // y endpoint, exclude the upper. Standard half-open convention.
            if (y == yHi) continue;
            {
                int x0 = xs[i], x1 = xs[j];
                int x  = x0 + (int)((float)(x1 - x0) * (float)(y - y0) / (float)(y1 - y0));
                if (x < xL) xL = x;
                if (x > xR) xR = x;
            }
        }
        if (xL > xR) continue;
        if (xL < 0) xL = 0;
        if (xR >= W) xR = W - 1;
        {
            byte *row = base + y * rb;
            int x;
            for (x = xL; x <= xR; x++) row[x] = color;
        }
    }
}

// Map a face-plane normal to a palette index along the dark-to-light gray
// ramp using a simple Lambert dot against a fixed world-space light.
// Quake's stock palette: indices ~0..15 ramp from black to white on the gray
// row; we offset slightly to avoid pure black so back-faces stay readable.
static byte shade_index(const vec3_t normal)
{
    static const vec3_t light = { 0.4082f, 0.4082f, 0.8165f };  // (1,1,2) normalised
    float d = DotProduct(normal, light);  // [-1, 1]
    float t = (d + 1.0f) * 0.5f;          // [0, 1]
    int   idx = 4 + (int)(t * 11.0f);     // [4, 15]
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return (byte)idx;
}

// -----------------------------------------------------------------------------
// Public: draw all visible faces of a brush in flat-shaded mode.
// -----------------------------------------------------------------------------

void Editor_FlatDrawBrush(const edit_brush_t *b)
{
    int i, k;
    for (i = 0; i < b->numfaces; i++)
    {
        const edit_face_t *f = &b->faces[i];
        const edit_plane_t *pl = &b->planes[f->plane_idx];

        // Back-face cull: skip faces pointing away from the camera. Without
        // this we'd overdraw front faces with back faces (no z-buffer).
        {
            vec3_t to_face;
            VectorSubtract(f->verts[0], r_origin, to_face);
            if (DotProduct(to_face, pl->normal) >= 0) continue;
        }

        // World -> view space.
        vec3_t vview[FLAT_MAX_VERTS];
        if (f->numverts > FLAT_MAX_VERTS) continue;
        for (k = 0; k < f->numverts; k++)
            w2v(f->verts[k], vview[k]);

        // Near-plane clip in view space.
        vec3_t vclip[FLAT_MAX_VERTS];
        int n_clip = clip_near(vview, f->numverts, vclip, FLAT_MAX_VERTS);
        if (n_clip < 3) continue;

        // Project to integer pixels.
        int xs[FLAT_MAX_VERTS], ys[FLAT_MAX_VERTS];
        for (k = 0; k < n_clip; k++)
            view_to_screen_i(vclip[k], &xs[k], &ys[k]);

        fill_convex_8(xs, ys, n_clip, shade_index(pl->normal));
    }
}

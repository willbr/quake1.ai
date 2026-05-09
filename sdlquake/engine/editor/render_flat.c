// render_flat.c -- M3 flat-shaded fill render style.
//
// For each face: clip the convex polygon at the near plane in view space,
// project to screen space, then scanline-fill into vid.buffer with a single
// palette index. The shade index is a Lambert dot against a fixed world
// light direction, so faces pointing toward the light come out brighter.
//
// Per-pixel depth test+write against the engine's d_pzbuffer using the
// standard Quake convention (izi = (int)(1/z * 0x8000), pass = "existing
// <= new"). That way editor brushes occlude / are occluded by the BSP world
// behind them and by each other without needing a manual sort.

#include "quakedef.h"
#include "r_local.h"        // NEAR_CLIP
#include "edit_scene.h"
#include "editor_internal.h"

#include <math.h>

extern vec3_t   vpn, vright, vup;
extern vec3_t   r_origin;
extern float    xcenter, ycenter;
extern float    xscale, yscale;
extern short   *d_pzbuffer;
extern unsigned int d_zwidth;

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

// Per-vertex projection: screen (x, y) plus inv_z for depth interpolation.
typedef struct { int x, y; float inv_z; } pvf_t;

static void project_pvf(const vec3_t v, pvf_t *out)
{
    float inv_z = 1.0f / v[2];
    float sx = xcenter + (xscale * inv_z) * v[0];
    float sy = ycenter - (yscale * inv_z) * v[1];
    out->x     = (int)(sx + 0.5f);
    out->y     = (int)(sy + 0.5f);
    out->inv_z = inv_z;
}

// Scanline-fill a convex polygon with a single palette index, depth-testing
// per pixel against d_pzbuffer. inv_z is interpolated linearly in screen
// space — fine for the editor where the ranges are small. (Quake's surface
// renderer interpolates 1/z linearly in screen space too, then z-test.)
static void fill_convex_8(const pvf_t *pv, int n, byte color)
{
    int W = (int)vid.width, H = (int)vid.height;
    int ymin = pv[0].y, ymax = pv[0].y;
    int i, y;
    byte *base = vid.buffer;
    int  rb   = (int)vid.rowbytes;

    for (i = 1; i < n; i++)
    {
        if (pv[i].y < ymin) ymin = pv[i].y;
        if (pv[i].y > ymax) ymax = pv[i].y;
    }
    if (ymin < 0) ymin = 0;
    if (ymax >= H) ymax = H - 1;

    for (y = ymin; y <= ymax; y++)
    {
        int   xL = 1 << 30, xR = -(1 << 30);
        float izL = 0, izR = 0;
        for (i = 0; i < n; i++)
        {
            int j = (i + 1) % n;
            int y0 = pv[i].y, y1 = pv[j].y;
            int yLo = y0 < y1 ? y0 : y1;
            int yHi = y0 > y1 ? y0 : y1;
            if (y0 == y1) continue;
            if (y < yLo || y > yHi) continue;
            if (y == yHi) continue;
            {
                float dt = (float)(y - y0) / (float)(y1 - y0);
                int   x  = pv[i].x + (int)((pv[j].x - pv[i].x) * dt);
                float iz = pv[i].inv_z + (pv[j].inv_z - pv[i].inv_z) * dt;
                if (x < xL) { xL = x; izL = iz; }
                if (x > xR) { xR = x; izR = iz; }
            }
        }
        if (xL > xR) continue;
        {
            int    span = xR - xL;
            float  t_step = span > 0 ? 1.0f / (float)span : 0;
            float  iz = izL;
            float  di = (izR - izL) * t_step;
            int    xa = xL < 0  ? 0     : xL;
            int    xb = xR >= W ? W - 1 : xR;
            byte  *row  = base + y * rb;
            short *zrow = d_pzbuffer + y * d_zwidth;
            int    x;
            if (xL < 0) iz += di * (float)(-xL);
            for (x = xa; x <= xb; x++)
            {
                if (iz > 1e-9f)
                {
                    int izi = (int)(iz * 32768.0f);
                    if (zrow[x] <= izi)
                    {
                        row[x]  = color;
                        zrow[x] = (short)izi;
                    }
                }
                iz += di;
            }
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

        // Project to integer pixels (with inv_z for depth).
        pvf_t pv[FLAT_MAX_VERTS];
        for (k = 0; k < n_clip; k++)
            project_pvf(vclip[k], &pv[k]);

        fill_convex_8(pv, n_clip, shade_index(pl->normal));
    }
}

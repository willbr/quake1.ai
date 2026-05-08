// render_tex.c -- M4 textured render style.
//
// Same scanline structure as render_flat.c but per pixel we sample a 2D
// texture via perspective-correct UV. The texture coords for each brush face
// follow qbsp's QuakeEd convention:
//
//   1. base_s_axis, base_t_axis = TextureAxisFromPlane(plane.normal) — pick
//      from the 6 cardinal "wall/floor/ceiling" presets, snapping to the
//      plane's dominant axis.
//   2. rotate the (s, t) axes by `rotation` degrees about the dominant axis
//      pair.
//   3. divide by (s_scale, t_scale) so they become "texels per world unit".
//   4. add (s_shift, t_shift) at lookup time.
//
// We sample a built-in 64x64 procedural grid pattern (palette indices) so
// every face has visible UV detail without needing a texture WAD.
// Loading real textures is a follow-up — the math is the same once we
// have a byte[texw*texh].

#include "quakedef.h"
#include "r_local.h"        // NEAR_CLIP
#include "edit_scene.h"
#include "editor_internal.h"

#include <math.h>
#include <string.h>

extern vec3_t   vpn, vright, vup;
extern vec3_t   r_origin;
extern float    xcenter, ycenter;
extern float    xscale, yscale;

#define TEX_W       64
#define TEX_H       64
#define TEX_W_MASK  (TEX_W - 1)
#define TEX_H_MASK  (TEX_H - 1)
#define MAX_VERTS   (EDIT_MAX_VERTS_PER_FACE + 4)

static byte s_tex[TEX_W * TEX_H];

// QuakeEd's six base axes — picks s and t world directions for the plane.
static const vec3_t base_axes[18] = {
    {0,0, 1}, {1, 0, 0}, {0,-1, 0},     // floor   (n.z largest, +)
    {0,0,-1}, {1, 0, 0}, {0,-1, 0},     // ceiling (n.z largest, -)
    {1,0, 0}, {0, 1, 0}, {0, 0,-1},     // east    (n.x largest, +)
    {-1,0,0}, {0, 1, 0}, {0, 0,-1},     // west    (n.x largest, -)
    {0,1, 0}, {1, 0, 0}, {0, 0,-1},     // north   (n.y largest, +)
    {0,-1,0}, {1, 0, 0}, {0, 0,-1}      // south   (n.y largest, -)
};

static void init_texture(void)
{
    static int done = 0;
    int s, t;
    if (done) return;
    done = 1;
    for (t = 0; t < TEX_H; t++)
    {
        for (s = 0; s < TEX_W; s++)
        {
            // 16-cell grid: thin black lines on a 2-tone gray checker.
            byte c;
            if ((s % 16) == 0 || (t % 16) == 0) c = 0;       // grid line
            else if (((s / 16) ^ (t / 16)) & 1) c = 11;      // light cell
            else                                 c = 6;       // dark cell
            s_tex[t * TEX_W + s] = c;
        }
    }
}

// Compute scaled+rotated s/t axes (in world space) and the s/t shifts for a
// face plane. Public (Editor_PlaneUVAxes) so Brush_Translate can use it for
// texture lock.
void Editor_PlaneUVAxes(const edit_plane_t *p,
                        vec3_t out_s, vec3_t out_t,
                        float *out_s_shift, float *out_t_shift)
{
    int   best = 0;
    float best_dot = 0;
    int   i, sv, tv;
    float ang, cosv, sinv;
    float s_scale, t_scale;

    for (i = 0; i < 6; i++)
    {
        float d = DotProduct(p->normal, base_axes[i*3]);
        if (d > best_dot) { best_dot = d; best = i; }
    }
    VectorCopy(base_axes[best*3 + 1], out_s);
    VectorCopy(base_axes[best*3 + 2], out_t);

    // Rotation acts in the (sv, tv) plane — i.e. the two non-zero axes of
    // the base s/t.
    sv = (out_s[0] != 0) ? 0 : ((out_s[1] != 0) ? 1 : 2);
    tv = (out_t[0] != 0) ? 0 : ((out_t[1] != 0) ? 1 : 2);
    if (p->rotation != 0)
    {
        ang = p->rotation * (3.14159265358979f / 180.0f);
        cosv = cosf(ang); sinv = sinf(ang);
        for (i = 0; i < 2; i++)
        {
            vec_t *v = (i == 0) ? out_s : out_t;
            float ns = cosv * v[sv] - sinv * v[tv];
            float nt = sinv * v[sv] + cosv * v[tv];
            v[sv] = ns; v[tv] = nt;
        }
    }

    s_scale = (p->s_scale != 0) ? p->s_scale : 1.0f;
    t_scale = (p->t_scale != 0) ? p->t_scale : 1.0f;
    out_s[0] /= s_scale; out_s[1] /= s_scale; out_s[2] /= s_scale;
    out_t[0] /= t_scale; out_t[1] /= t_scale; out_t[2] /= t_scale;

    *out_s_shift = p->s_shift;
    *out_t_shift = p->t_shift;
}

// View transform.
static void w2v(const vec3_t world, vec3_t view)
{
    vec3_t local;
    VectorSubtract(world, r_origin, local);
    view[0] = DotProduct(local, vright);
    view[1] = DotProduct(local, vup);
    view[2] = DotProduct(local, vpn);
}

// Per-vertex render data — keeps world position alongside the clipped view
// coords so we can recompute UV after the near-plane clip splits an edge.
typedef struct {
    vec3_t  world;      // world-space position
    vec3_t  view;       // view-space position
} vrt_t;

// Sutherland-Hodgman clip a polygon's view+world verts at z = NEAR_CLIP.
// Both world and view pos are interpolated (lerp_t in view space, applied
// to both buffers).
static int clip_near(const vrt_t *in, int n_in, vrt_t *out, int max_out)
{
    int n_out = 0, i;
    for (i = 0; i < n_in; i++)
    {
        const vrt_t *a = &in[i];
        const vrt_t *b = &in[(i + 1) % n_in];
        int a_in = a->view[2] >= NEAR_CLIP;
        int b_in = b->view[2] >= NEAR_CLIP;
        if (a_in)
        {
            if (n_out >= max_out) return -1;
            out[n_out++] = *a;
        }
        if (a_in != b_in)
        {
            float dt = (NEAR_CLIP - a->view[2]) / (b->view[2] - a->view[2]);
            if (n_out >= max_out) return -1;
            out[n_out].world[0] = a->world[0] + (b->world[0] - a->world[0]) * dt;
            out[n_out].world[1] = a->world[1] + (b->world[1] - a->world[1]) * dt;
            out[n_out].world[2] = a->world[2] + (b->world[2] - a->world[2]) * dt;
            out[n_out].view [0] = a->view [0] + (b->view [0] - a->view [0]) * dt;
            out[n_out].view [1] = a->view [1] + (b->view [1] - a->view [1]) * dt;
            out[n_out].view [2] = NEAR_CLIP;
            n_out++;
        }
    }
    return n_out;
}

// Per-vertex interpolants: screen pos + (s/z, t/z, 1/z).
typedef struct {
    int   x, y;
    float so_z, to_z, inv_z;
} pv_t;

static void make_pv(const vrt_t *v, const vec3_t s_axis, const vec3_t t_axis,
                    float s_shift, float t_shift, pv_t *out)
{
    float inv_z = 1.0f / v->view[2];
    float s = DotProduct(v->world, s_axis) + s_shift;
    float t = DotProduct(v->world, t_axis) + t_shift;
    float sx = xcenter + (xscale * inv_z) * v->view[0];
    float sy = ycenter - (yscale * inv_z) * v->view[1];
    out->x     = (int)(sx + 0.5f);
    out->y     = (int)(sy + 0.5f);
    out->so_z  = s * inv_z;
    out->to_z  = t * inv_z;
    out->inv_z = inv_z;
}

// Scanline-fill a convex polygon, sampling the procedural texture per pixel
// with perspective-correct (s, t).
static void fill_textured(const pv_t *pv, int n)
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
        // Find left and right edge crossings, and the interpolated values.
        int   xL = 1<<30, xR = -(1<<30);
        float so_zL = 0, to_zL = 0, inv_zL = 0;
        float so_zR = 0, to_zR = 0, inv_zR = 0;
        for (i = 0; i < n; i++)
        {
            int j = (i + 1) % n;
            int y0 = pv[i].y, y1 = pv[j].y;
            int yLo = y0 < y1 ? y0 : y1;
            int yHi = y0 > y1 ? y0 : y1;
            if (y0 == y1) continue;
            if (y < yLo || y > yHi) continue;
            if (y == yHi) continue;     // half-open, avoid double counting at corners
            {
                float dt = (float)(y - y0) / (float)(y1 - y0);
                int   x = pv[i].x + (int)((pv[j].x - pv[i].x) * dt);
                float so = pv[i].so_z  + (pv[j].so_z  - pv[i].so_z ) * dt;
                float to = pv[i].to_z  + (pv[j].to_z  - pv[i].to_z ) * dt;
                float iz = pv[i].inv_z + (pv[j].inv_z - pv[i].inv_z) * dt;
                if (x < xL) { xL = x; so_zL = so; to_zL = to; inv_zL = iz; }
                if (x > xR) { xR = x; so_zR = so; to_zR = to; inv_zR = iz; }
            }
        }
        if (xL > xR) continue;

        // Span pixel walk, perspective-correct UV at every step.
        {
            int   span = xR - xL;
            float t_step = span > 0 ? 1.0f / (float)span : 0;
            float so = so_zL, to = to_zL, iz = inv_zL;
            float ds = (so_zR  - so_zL ) * t_step;
            float dt = (to_zR  - to_zL ) * t_step;
            float di = (inv_zR - inv_zL) * t_step;
            int   xa = xL < 0  ? 0     : xL;
            int   xb = xR >= W ? W - 1 : xR;
            byte *row = base + y * rb;
            int   x;
            // Pre-step in case xL was clipped.
            if (xL < 0)
            {
                so += ds * (float)(-xL);
                to += dt * (float)(-xL);
                iz += di * (float)(-xL);
            }
            for (x = xa; x <= xb; x++)
            {
                if (iz > 1e-9f)
                {
                    float u = so / iz;
                    float v = to / iz;
                    int   us = ((int)u) & TEX_W_MASK;
                    int   vs = ((int)v) & TEX_H_MASK;
                    row[x] = s_tex[vs * TEX_W + us];
                }
                so += ds; to += dt; iz += di;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Public
// -----------------------------------------------------------------------------

void Editor_TexDrawBrush(const edit_brush_t *b)
{
    int i, k;
    init_texture();

    for (i = 0; i < b->numfaces; i++)
    {
        const edit_face_t *f = &b->faces[i];
        const edit_plane_t *pl = &b->planes[f->plane_idx];

        // Back-face cull.
        {
            vec3_t to_face;
            VectorSubtract(f->verts[0], r_origin, to_face);
            if (DotProduct(to_face, pl->normal) >= 0) continue;
        }

        if (f->numverts > MAX_VERTS) continue;

        // Build vrt_t (world + view-space) array, then clip at near plane.
        vrt_t in_v[MAX_VERTS], cl_v[MAX_VERTS];
        for (k = 0; k < f->numverts; k++)
        {
            VectorCopy(f->verts[k], in_v[k].world);
            w2v(f->verts[k], in_v[k].view);
        }
        int n_clip = clip_near(in_v, f->numverts, cl_v, MAX_VERTS);
        if (n_clip < 3) continue;

        // Compute UV axes for this plane and project each clipped vertex.
        vec3_t s_axis, t_axis;
        float  s_shift, t_shift;
        Editor_PlaneUVAxes(pl, s_axis, t_axis, &s_shift, &t_shift);

        pv_t pv[MAX_VERTS];
        for (k = 0; k < n_clip; k++)
            make_pv(&cl_v[k], s_axis, t_axis, s_shift, t_shift, &pv[k]);

        fill_textured(pv, n_clip);
    }
}

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

// Liang-Barsky parametric clip of a line against [xmin,xmax) x [ymin,ymax).
// Updates endpoints (and iz interpolation) in place; returns 0 if the line is
// fully outside the viewport. Without this, debug overlays that project to
// extreme screen coords (e.g. navmesh edges with one endpoint near the camera
// at NEAR_CLIP=0.01) would still walk Bresenham across thousands of off-screen
// pixels — the per-pixel viewport guard suppresses the *write* but not the
// step, so 60k navmesh edges collapsed framerate to ~1 fps.
static int clip_line_to_rect(int *x0, int *y0, float *iz0,
                             int *x1, int *y1, float *iz1,
                             int xmin, int ymin, int xmax, int ymax)
{
    float fx0 = (float)*x0, fy0 = (float)*y0;
    float fx1 = (float)*x1, fy1 = (float)*y1;
    float dx  = fx1 - fx0,  dy  = fy1 - fy0;
    float t0 = 0.0f, t1 = 1.0f;
    // xmax/ymax are exclusive — clip against the inclusive max (xmax-1, ymax-1).
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { fx0 - (float)xmin,        (float)(xmax - 1) - fx0,
                   fy0 - (float)ymin,        (float)(ymax - 1) - fy0 };
    int i;
    for (i = 0; i < 4; i++)
    {
        if (p[i] == 0.0f)
        {
            if (q[i] < 0.0f) return 0;   // parallel and outside
        }
        else
        {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) { if (t > t0) t0 = t; }
            else             { if (t < t1) t1 = t; }
        }
    }
    if (t0 > t1) return 0;
    {
        float niz0 = *iz0 + t0 * (*iz1 - *iz0);
        float niz1 = *iz0 + t1 * (*iz1 - *iz0);
        float nx0  = fx0 + t0 * dx;
        float ny0  = fy0 + t0 * dy;
        float nx1  = fx0 + t1 * dx;
        float ny1  = fy0 + t1 * dy;
        // Truncate to int and clamp defensively — float→int rounding could
        // push a clipped endpoint one pixel past the bound.
        int   ix0  = (int)nx0, iy0 = (int)ny0;
        int   ix1  = (int)nx1, iy1 = (int)ny1;
        if (ix0 < xmin) ix0 = xmin; if (ix0 >= xmax) ix0 = xmax - 1;
        if (iy0 < ymin) iy0 = ymin; if (iy0 >= ymax) iy0 = ymax - 1;
        if (ix1 < xmin) ix1 = xmin; if (ix1 >= xmax) ix1 = xmax - 1;
        if (iy1 < ymin) iy1 = ymin; if (iy1 >= ymax) iy1 = ymax - 1;
        *x0 = ix0; *y0 = iy0; *x1 = ix1; *y1 = iy1;
        *iz0 = niz0; *iz1 = niz1;
    }
    return 1;
}

// Internal — Bresenham with optional iz interpolation + d_pzbuffer test.
// When ztest is 0, iz0/iz1 are unused. When ztest is 1, the iz values are
// linearly interpolated across the line steps and each pixel is gated on
// `*zp <= izi` (<=: lines tie-win against opaque world surfaces sharing
// the plane, same convention as the editor wireframe pass uses for brush
// outlines that hug their fill).
extern short       *d_pzbuffer;
extern unsigned int d_zwidth;
static void draw_line2d_ext(int x0, int y0, float iz0,
                            int x1, int y1, float iz1,
                            int color, int ztest)
{
    const int xmin = r_refdef.vrect.x;
    const int ymin = r_refdef.vrect.y;
    const int xmax = r_refdef.vrect.x + r_refdef.vrect.width;   // exclusive
    const int ymax = r_refdef.vrect.y + r_refdef.vrect.height;  // exclusive
    int dx, dy, sx, sy, err, e2;
    int adx, ady, steps;
    float iz, di;

    if (!clip_line_to_rect(&x0, &y0, &iz0, &x1, &y1, &iz1,
                           xmin, ymin, xmax, ymax))
        return;

    adx =  abs(x1 - x0);
    ady =  abs(y1 - y0);
    dx  =  adx;
    dy  = -ady;
    sx  = x0 < x1 ? 1 : -1;
    sy  = y0 < y1 ? 1 : -1;
    err = dx + dy;
    steps = adx > ady ? adx : ady;
    iz = iz0;
    di = steps > 0 ? (iz1 - iz0) / (float)steps : 0.0f;

    // Endpoints are now guaranteed inside [xmin,xmax) x [ymin,ymax), so the
    // per-pixel viewport guard can be dropped — Bresenham only walks visible
    // pixels.
    for (;;)
    {
        if (bayer4x4[y0 & 3][x0 & 3] < bayer_threshold)
        {
            if (ztest)
            {
                int izi = (int)(iz * 32768.0f);
                short *zp = d_pzbuffer + y0 * d_zwidth + x0;
                if (*zp <= izi)
                {
                    vid.buffer[y0 * vid.rowbytes + x0] = (byte)color;
                    *zp = (short)izi;
                }
            }
            else
            {
                vid.buffer[y0 * vid.rowbytes + x0] = (byte)color;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        iz += di;
    }
}

void RDD_DrawLine3D_ViewZ(const vec3_t va, const vec3_t vb, int color, int ztest)
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
    draw_line2d_ext((int)sax, (int)say, 1.0f / ca[2],
                    (int)sbx, (int)sby, 1.0f / cb[2],
                    color, ztest);
}

void RDD_DrawLine3D_View(const vec3_t va, const vec3_t vb, int color)
{
    RDD_DrawLine3D_ViewZ(va, vb, color, 0);
}

void RDD_DrawLine2D(int x0, int y0, int x1, int y1, int color)
{
    draw_line2d_ext(x0, y0, 0.0f, x1, y1, 0.0f, color, 0);
}

void RDD_DrawSolidPixel(int x, int y, int color)
{
    if (x >= r_refdef.vrect.x && x < r_refdef.vrect.x + r_refdef.vrect.width &&
        y >= r_refdef.vrect.y && y < r_refdef.vrect.y + r_refdef.vrect.height)
        vid.buffer[y * vid.rowbytes + x] = (byte)color;
}

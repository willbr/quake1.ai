// brush_compile.c -- "fake BSP compile":
//
// Take a brush's plane-set and produce one convex polygon per plane, clipped
// against every other plane. Ported from id's qbsp tools (BaseWindingForPlane
// + ChopWindingInPlace) but with stack-bounded windings — no heap, no error
// callbacks. Bad input (degenerate or non-closed brush) just leaves
// brush->valid = 0 and the renderer skips it.

#include "quakedef.h"
#include "edit_scene.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define BOGUS_RANGE     8192.0f
#define WIND_EPSILON    0.001f
#define WIND_MAX_PTS    (EDIT_MAX_VERTS_PER_FACE)

// Local fixed-size winding (stack-allocatable).
typedef struct {
    int     n;
    vec3_t  p[WIND_MAX_PTS + 4];
} wind_t;

#define SIDE_FRONT 0
#define SIDE_BACK  1
#define SIDE_ON    2

// Compute (normal, dist) from three points p0/p1/p2 using qbsp's QuakeEd
// convention: normal = (p0-p1) x (p2-p1), then dist = dot(p1, normal). With
// this convention the .map's three points wind counter-clockwise as viewed
// from OUTSIDE the brush, so the normal points outward; the brush is the
// intersection of half-spaces { p : dot(p, normal) <= dist }.
static int plane_from_points(const vec3_t p0, const vec3_t p1, const vec3_t p2,
                             vec3_t out_normal, float *out_dist)
{
    vec3_t t1, t2, n;
    float len;
    VectorSubtract(p0, p1, t1);
    VectorSubtract(p2, p1, t2);
    CrossProduct(t1, t2, n);
    len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    if (len < 1e-6f) return 0;
    n[0] /= len; n[1] /= len; n[2] /= len;
    VectorCopy(n, out_normal);
    *out_dist = DotProduct(p1, n);
    return 1;
}

// Big quad on the plane, axis-projected (qbsp BaseWindingForPlane).
static void base_winding(const vec3_t normal, float dist, wind_t *out)
{
    int     i, x;
    float   max, v;
    vec3_t  org, vr, vu, scaled_up;

    max = -BOGUS_RANGE;
    x = -1;
    for (i = 0; i < 3; i++)
    {
        v = fabsf(normal[i]);
        if (v > max) { x = i; max = v; }
    }
    // x==-1 only if normal is zero — caller should have rejected.
    if (x == -1) { out->n = 0; return; }

    VectorCopy(vec3_origin, vu);
    if (x == 0 || x == 1) vu[2] = 1;
    else                  vu[0] = 1;

    v = DotProduct(vu, normal);
    VectorMA(vu, -v, normal, vu);
    {
        float l = sqrtf(vu[0]*vu[0] + vu[1]*vu[1] + vu[2]*vu[2]);
        if (l < 1e-6f) { out->n = 0; return; }
        vu[0] /= l; vu[1] /= l; vu[2] /= l;
    }

    VectorScale(normal, dist, org);
    CrossProduct(vu, normal, vr);
    VectorScale(vu, BOGUS_RANGE, scaled_up);
    VectorScale(vr, BOGUS_RANGE, vr);
    VectorCopy(scaled_up, vu);

    VectorSubtract(org, vr, out->p[0]); VectorAdd(out->p[0], vu, out->p[0]);
    VectorAdd     (org, vr, out->p[1]); VectorAdd(out->p[1], vu, out->p[1]);
    VectorAdd     (org, vr, out->p[2]); VectorSubtract(out->p[2], vu, out->p[2]);
    VectorSubtract(org, vr, out->p[3]); VectorSubtract(out->p[3], vu, out->p[3]);
    out->n = 4;
}

// Clip `inout` to the front side of (normal, dist). Returns 0 if entirely
// behind (winding empties) or if max-pts exceeded; 1 otherwise.
static int chop_in_place(wind_t *inout, const vec3_t normal, float dist)
{
    float   dists[WIND_MAX_PTS + 4];
    int     sides[WIND_MAX_PTS + 4];
    int     counts[3] = {0,0,0};
    int     i, j, max_pts;
    float   dot;
    wind_t  out;

    if (inout->n <= 0) return 0;

    for (i = 0; i < inout->n; i++)
    {
        dot = DotProduct(inout->p[i], normal) - dist;
        dists[i] = dot;
        if      (dot >  WIND_EPSILON) sides[i] = SIDE_FRONT;
        else if (dot < -WIND_EPSILON) sides[i] = SIDE_BACK;
        else                          sides[i] = SIDE_ON;
        counts[sides[i]]++;
    }
    sides[i] = sides[0];
    dists[i] = dists[0];

    if (!counts[SIDE_FRONT])
    {
        inout->n = 0;
        return 0;
    }
    if (!counts[SIDE_BACK])
        return 1;   // unchanged

    max_pts = inout->n + 4;
    if (max_pts > WIND_MAX_PTS + 4) return 0;
    out.n = 0;

    for (i = 0; i < inout->n; i++)
    {
        const float *p1 = inout->p[i];
        const float *p2;
        vec3_t mid;

        if (sides[i] == SIDE_ON)
        {
            if (out.n >= WIND_MAX_PTS + 4) return 0;
            VectorCopy(p1, out.p[out.n]); out.n++;
            continue;
        }
        if (sides[i] == SIDE_FRONT)
        {
            if (out.n >= WIND_MAX_PTS + 4) return 0;
            VectorCopy(p1, out.p[out.n]); out.n++;
        }
        if (sides[i+1] == SIDE_ON || sides[i+1] == sides[i])
            continue;

        p2 = inout->p[(i+1) % inout->n];
        dot = dists[i] / (dists[i] - dists[i+1]);
        for (j = 0; j < 3; j++)
        {
            if      (normal[j] ==  1.0f) mid[j] =  dist;
            else if (normal[j] == -1.0f) mid[j] = -dist;
            else                         mid[j] = p1[j] + dot * (p2[j] - p1[j]);
        }
        if (out.n >= WIND_MAX_PTS + 4) return 0;
        VectorCopy(mid, out.p[out.n]); out.n++;
    }
    if (out.n > WIND_MAX_PTS) return 0;
    *inout = out;
    return 1;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void Brush_FreeFaces(edit_brush_t *b)
{
    if (b->faces) { free(b->faces); b->faces = NULL; }
    b->numfaces = 0;
}

void Brush_Compile(edit_brush_t *b)
{
    int i, j, k;
    int valid_faces = 0;
    edit_face_t *out;
    extern cvar_t developer;

    Brush_FreeFaces(b);
    b->valid = 0;
    b->numfaces = 0;
    if (b->numplanes < 4) return;       // need at least 4 planes for a closed solid

    // Compute (normal, dist) for every plane from its three points.
    for (i = 0; i < b->numplanes; i++)
    {
        edit_plane_t *p = &b->planes[i];
        if (!plane_from_points(p->points[0], p->points[1], p->points[2],
                               p->normal, &p->dist))
        {
            if (developer.value)
                Con_Printf("brush_compile: plane %d degenerate\n", i);
            return;     // degenerate
        }
        if (developer.value)
            Con_Printf("brush_compile: plane %d n=(%.2f %.2f %.2f) d=%.2f\n",
                       i, p->normal[0], p->normal[1], p->normal[2], p->dist);
    }

    out = (edit_face_t *)calloc(b->numplanes, sizeof(edit_face_t));
    if (!out) return;

    for (i = 0; i < b->numplanes; i++)
    {
        wind_t w;
        edit_plane_t *p = &b->planes[i];
        base_winding(p->normal, p->dist, &w);
        if (w.n == 0)
        {
            if (developer.value)
                Con_Printf("brush_compile: plane %d base winding empty\n", i);
            continue;
        }

        for (j = 0; j < b->numplanes && w.n > 0; j++)
        {
            edit_plane_t *q;
            vec3_t n_neg;
            if (j == i) continue;
            q = &b->planes[j];
            n_neg[0] = -q->normal[0];
            n_neg[1] = -q->normal[1];
            n_neg[2] = -q->normal[2];
            chop_in_place(&w, n_neg, -q->dist);
            if (developer.value && w.n == 0)
            {
                Con_Printf("brush_compile: plane %d emptied by clip vs plane %d\n", i, j);
                break;
            }
        }
        if (w.n < 3)
        {
            if (developer.value)
                Con_Printf("brush_compile: plane %d final n=%d (need >=3)\n", i, w.n);
            continue;
        }

        // Materialise as a face.
        out[valid_faces].plane_idx = i;
        out[valid_faces].numverts  = w.n;
        for (k = 0; k < w.n; k++)
            VectorCopy(w.p[k], out[valid_faces].verts[k]);
        valid_faces++;
    }

    if (valid_faces == 0) { free(out); return; }

    b->faces    = out;
    b->numfaces = valid_faces;

    // bbox
    b->mins[0] = b->mins[1] = b->mins[2] =  1e30f;
    b->maxs[0] = b->maxs[1] = b->maxs[2] = -1e30f;
    for (i = 0; i < valid_faces; i++)
    {
        for (k = 0; k < out[i].numverts; k++)
        {
            if (out[i].verts[k][0] < b->mins[0]) b->mins[0] = out[i].verts[k][0];
            if (out[i].verts[k][1] < b->mins[1]) b->mins[1] = out[i].verts[k][1];
            if (out[i].verts[k][2] < b->mins[2]) b->mins[2] = out[i].verts[k][2];
            if (out[i].verts[k][0] > b->maxs[0]) b->maxs[0] = out[i].verts[k][0];
            if (out[i].verts[k][1] > b->maxs[1]) b->maxs[1] = out[i].verts[k][1];
            if (out[i].verts[k][2] > b->maxs[2]) b->maxs[2] = out[i].verts[k][2];
        }
    }
    b->valid = 1;
}

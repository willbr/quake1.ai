// collide.c -- M2 brute-force collision against editor brushes.
//
// Hooked into SV_Move (world.c). For every brush in the edit scene, we run a
// standard "moving AABB vs. convex polyhedron" plane sweep:
//
//   1. Each brush plane is expanded outward by the box's extents along the
//      plane normal — Minkowski sum, so we can treat the moving body as a
//      single point.
//   2. Sweep the point from start to end, accumulating the latest
//      "entering" plane fraction (enter) and the earliest "exiting" plane
//      fraction (exit).
//   3. If enter < exit and enter < the trace's current best fraction, we
//      have a closer hit — update fraction, endpos, plane.
//
// No BSP / no clipnodes — just N brushes * up to 64 planes, called per move.
// For editor-sized scenes (~hundreds of brushes max) this is well under the
// per-frame budget.

#include "quakedef.h"     // pulls in world.h (trace_t, plane_t)
#include "edit_scene.h"

#include <math.h>
#include <string.h>

#define EDIT_TRACE_EPSILON   (1.0f / 32.0f)   // matches Quake's DIST_EPSILON

// Distance to push a brush plane outward so we can treat the moving AABB as
// a single point. For each axis the offset uses the box extent on the side
// OPPOSITE to the plane's outward normal — that's the side the player can
// poke into the brush from. Equivalently: -min over c in [mins,maxs] of
// dot(c, normal).
static float plane_box_offset(const vec3_t normal, const vec3_t mins, const vec3_t maxs)
{
    float min_dot = 0;
    int i;
    for (i = 0; i < 3; i++)
        min_dot += (normal[i] >= 0) ? normal[i] * mins[i] : normal[i] * maxs[i];
    return -min_dot;
}

// Trace a moving AABB (mins/maxs around the moving point) from start to end
// against one editor brush. Updates *best_t/best_normal if this brush blocks
// the trace earlier than the current best. Sets *out_startsolid if the start
// position is already inside the expanded brush. Returns 1 if it produced a
// hit (i.e. fraction was lowered), 0 otherwise.
static int trace_brush(const vec3_t start, const vec3_t end,
                       const vec3_t mins,  const vec3_t maxs,
                       const edit_brush_t *b,
                       float *best_t, vec3_t best_normal,
                       int *out_startsolid, int *out_allsolid)
{
    float enter = -1.0f;
    float exit  =  1.0f;
    int   start_outside = 0;
    int   end_outside   = 0;
    vec3_t enter_normal = {0,0,0};
    int i;

    for (i = 0; i < b->numplanes; i++)
    {
        const edit_plane_t *p = &b->planes[i];
        float ofs  = plane_box_offset(p->normal, mins, maxs);
        float dist = p->dist + ofs;
        float d1 = DotProduct(start, p->normal) - dist;
        float d2 = DotProduct(end,   p->normal) - dist;

        if (d1 > 0) start_outside = 1;
        if (d2 > 0) end_outside   = 1;

        // Trace stays fully outside this expanded plane: misses the brush.
        if (d1 > 0 && d2 > 0) return 0;

        // Both endpoints "inside" this plane: doesn't bound enter/exit.
        if (d1 <= 0 && d2 <= 0) continue;

        if (d1 > d2)
        {
            // crossing INTO the brush across this plane
            float f = (d1 - EDIT_TRACE_EPSILON) / (d1 - d2);
            if (f > enter)
            {
                enter = f;
                VectorCopy(p->normal, enter_normal);
            }
        }
        else
        {
            // crossing OUT of the brush across this plane
            float f = (d1 + EDIT_TRACE_EPSILON) / (d1 - d2);
            if (f < exit) exit = f;
        }
    }

    // Start point is inside the (expanded) brush — set startsolid. If the
    // entire trace stays inside, also set allsolid + fraction=0 so the
    // physics layer won't push the player further into the solid (e.g. the
    // gravity-step of a player resting on the +Z face of the brush).
    if (!start_outside)
    {
        if (out_startsolid) *out_startsolid = 1;
        if (!end_outside)
        {
            if (out_allsolid) *out_allsolid = 1;
            if (*best_t > 0)
            {
                *best_t = 0;
                // No single bounding plane was crossed during the move, so
                // there's no obvious "impact" normal. Leave best_normal
                // untouched — Quake's player physics handles fraction=0
                // without strictly needing a normal.
                return 1;
            }
        }
        return 0;
    }

    if (enter < exit && enter > -1.0f && enter < *best_t)
    {
        if (enter < 0) enter = 0;
        *best_t = enter;
        VectorCopy(enter_normal, best_normal);
        return 1;
    }
    return 0;
}

// Public API: clip a moving AABB trace against every editor brush. Updates
// *trace in place if any brush is hit earlier than the existing trace. Called
// from SV_Move after the world+entity clip.
void EditorCollide_ClipMove(const vec3_t start, const vec3_t mins,
                            const vec3_t maxs,  const vec3_t end,
                            trace_t *trace)
{
    vec3_t hit_normal = {0,0,0};
    int    hit       = 0;
    int    startsolid = 0;
    int    allsolid   = 0;
    float  best_t    = trace->fraction;
    int    i, j;

    {
        extern int Editor_IsOpen(void);
        extern cvar_t editor_view_mode;
        int view_map = (int)editor_view_mode.value == 1;
        if (!Editor_IsOpen() && !view_map) return;
    }

    if (edit_scene.numentities == 0) return;

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            if (!b->valid) continue;
            // Cheap AABB reject: the move's swept bbox vs. the brush bbox
            // (expanded by the moving box's extents).
            {
                vec3_t mn, mx;
                int k;
                for (k = 0; k < 3; k++)
                {
                    float a = start[k] < end[k] ? start[k] : end[k];
                    float c = start[k] > end[k] ? start[k] : end[k];
                    mn[k] = a + mins[k];
                    mx[k] = c + maxs[k];
                }
                if (mn[0] > b->maxs[0] || mx[0] < b->mins[0]) continue;
                if (mn[1] > b->maxs[1] || mx[1] < b->mins[1]) continue;
                if (mn[2] > b->maxs[2] || mx[2] < b->mins[2]) continue;
            }
            if (trace_brush(start, end, mins, maxs, b,
                            &best_t, hit_normal, &startsolid, &allsolid))
                hit = 1;
        }
    }

    if (startsolid) trace->startsolid = true;
    if (allsolid)   trace->allsolid   = true;

    if (hit)
    {
        trace->fraction = best_t;
        trace->endpos[0] = start[0] + (end[0] - start[0]) * best_t;
        trace->endpos[1] = start[1] + (end[1] - start[1]) * best_t;
        trace->endpos[2] = start[2] + (end[2] - start[2]) * best_t;
        if (hit_normal[0] || hit_normal[1] || hit_normal[2])
        {
            VectorCopy(hit_normal, trace->plane.normal);
            trace->plane.dist = DotProduct(trace->endpos, hit_normal);
        }
        trace->ent = sv.edicts;     // pretend it's the world for entity-aware code
    }
}

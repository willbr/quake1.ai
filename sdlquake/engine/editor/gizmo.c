// gizmo.c -- M1 translate gizmo.
//
// Three axis-aligned arrows at the selected brush's centroid. Click an arrow
// to start a drag along that axis; mouse-move applies an incremental world-
// space translate via Brush_Translate (which also recompiles the windings).
// The arrow length scales with distance to the camera so it stays a roughly
// fixed pixel size.

#include "quakedef.h"
#include "edit_scene.h"
#include "editor_internal.h"

#include <math.h>

extern vec3_t r_origin;

// -----------------------------------------------------------------------------
// Drag state
// -----------------------------------------------------------------------------

static int      s_drag_axis = -1;       // 0/1/2 = X/Y/Z, -1 = inactive
static float    s_drag_t0;              // closest-t-on-axis at mouse down
static vec3_t   s_drag_origin;          // brush centroid at mouse down

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

void Editor_BrushCentroid(const edit_brush_t *b, vec3_t out)
{
    int i, k, n = 0;
    out[0] = out[1] = out[2] = 0;
    for (i = 0; i < b->numfaces; i++)
    {
        for (k = 0; k < b->faces[i].numverts; k++)
        {
            out[0] += b->faces[i].verts[k][0];
            out[1] += b->faces[i].verts[k][1];
            out[2] += b->faces[i].verts[k][2];
            n++;
        }
    }
    if (n > 0)
    {
        out[0] /= n; out[1] /= n; out[2] /= n;
    }
    else
    {
        // bbox center fallback
        out[0] = (b->mins[0] + b->maxs[0]) * 0.5f;
        out[1] = (b->mins[1] + b->maxs[1]) * 0.5f;
        out[2] = (b->mins[2] + b->maxs[2]) * 0.5f;
    }
}

// Closest t on infinite line (origin, dir) to ray (r_origin, r_dir).
// Returns the parameter t along the line (so line point = origin + t*dir).
static float closest_t_line_ray(const vec3_t l_org, const vec3_t l_dir,
                                const vec3_t r_org, const vec3_t r_dir)
{
    vec3_t w0;
    float a, b, c, d, e, denom;
    VectorSubtract(l_org, r_org, w0);
    a = DotProduct(l_dir, l_dir);
    b = DotProduct(l_dir, r_dir);
    c = DotProduct(r_dir, r_dir);
    d = DotProduct(l_dir, w0);
    e = DotProduct(r_dir, w0);
    denom = a*c - b*b;
    if (fabsf(denom) < 1e-6f)
        // parallel — project w0 onto l_dir.
        return -d / (a + 1e-12f);
    return (b*e - c*d) / denom;
}

// Distance from a 3D point to an infinite ray (origin, dir).
static float point_to_ray(const vec3_t p, const vec3_t r_org, const vec3_t r_dir)
{
    vec3_t w, c;
    float t;
    VectorSubtract(p, r_org, w);
    t = DotProduct(w, r_dir);
    c[0] = r_org[0] + r_dir[0] * t;
    c[1] = r_org[1] + r_dir[1] * t;
    c[2] = r_org[2] + r_dir[2] * t;
    {
        float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        return sqrtf(dx*dx + dy*dy + dz*dz);
    }
}

// World-length-per-pixel at distance `dist` from camera, so the arrow stays
// roughly fixed-size on screen. xscale is in pixels-per-radian-ish.
static float pixel_to_world(float dist)
{
    extern float xscale;
    if (xscale < 1e-3f) return 1.0f;
    return dist / xscale;
}

static void axis_endpoint(const vec3_t centroid, int axis, float arrow_len, vec3_t out)
{
    VectorCopy(centroid, out);
    out[axis] += arrow_len;
}

// -----------------------------------------------------------------------------
// Render
// -----------------------------------------------------------------------------

void Editor_GizmoDraw(void)
{
    edit_brush_t *b = Scene_GetSelectedBrush();
    vec3_t centroid, end;
    float dist, arrow_len;
    static const byte axis_colors[3] = {
        EDIT_COLOR_AXIS_X, EDIT_COLOR_AXIS_Y, EDIT_COLOR_AXIS_Z
    };
    int i;
    if (!b || !b->valid) return;

    Editor_BrushCentroid(b, centroid);
    {
        vec3_t d;
        VectorSubtract(centroid, r_origin, d);
        dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dist < 1.0f) dist = 1.0f;
    }
    arrow_len = pixel_to_world(dist) * 30.0f;   // ~30 pixel-equivalent length

    for (i = 0; i < 3; i++)
    {
        byte col = (s_drag_axis == i) ? EDIT_COLOR_AXIS_HOT : axis_colors[i];
        axis_endpoint(centroid, i, arrow_len, end);
        Editor_DrawLine3DOver(centroid, end, col);
        // small cross at the tip so the arrow head reads
        {
            vec3_t a, c;
            int u = (i + 1) % 3;
            int v = (i + 2) % 3;
            float tip = arrow_len * 0.15f;
            VectorCopy(end, a); a[u] += tip;
            VectorCopy(end, c); c[u] -= tip;
            Editor_DrawLine3DOver(a, c, col);
            VectorCopy(end, a); a[v] += tip;
            VectorCopy(end, c); c[v] -= tip;
            Editor_DrawLine3DOver(a, c, col);
        }
    }
}

// -----------------------------------------------------------------------------
// Mouse interaction
// -----------------------------------------------------------------------------

#define GIZMO_PICK_PIXELS 6.0f          // hit radius (in screen pixels at gizmo distance)

int Editor_GizmoMouseDown(float sx, float sy)
{
    edit_brush_t *b = Scene_GetSelectedBrush();
    vec3_t centroid;
    vec3_t r_org, r_dir;
    float dist, arrow_len, pick_world;
    int i, best_axis = -1;
    float best_d = 1e30f;

    if (!b || !b->valid) return 0;

    Editor_BrushCentroid(b, centroid);
    {
        vec3_t d;
        VectorSubtract(centroid, r_origin, d);
        dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dist < 1.0f) dist = 1.0f;
    }
    arrow_len = pixel_to_world(dist) * 30.0f;
    pick_world = pixel_to_world(dist) * GIZMO_PICK_PIXELS;

    Editor_ScreenToRay(sx, sy, r_org, r_dir);

    for (i = 0; i < 3; i++)
    {
        // Closest point on this axis line to the mouse ray.
        vec3_t l_dir = {0,0,0};
        l_dir[i] = 1.0f;
        float t = closest_t_line_ray(centroid, l_dir, r_org, r_dir);
        if (t < 0.0f) continue;
        if (t > arrow_len) continue;
        {
            vec3_t pt = { centroid[0], centroid[1], centroid[2] };
            pt[i] += t;
            float d = point_to_ray(pt, r_org, r_dir);
            if (d < pick_world && d < best_d)
            {
                best_d = d;
                best_axis = i;
            }
        }
    }
    if (best_axis < 0) return 0;

    s_drag_axis = best_axis;
    VectorCopy(centroid, s_drag_origin);
    {
        vec3_t l_dir = {0,0,0};
        l_dir[best_axis] = 1.0f;
        s_drag_t0 = closest_t_line_ray(centroid, l_dir, r_org, r_dir);
    }
    return 1;
}

void Editor_GizmoMouseMove(float sx, float sy)
{
    edit_brush_t *b;
    vec3_t r_org, r_dir, l_dir = {0,0,0};
    vec3_t delta;
    float t_now;
    int i;
    if (s_drag_axis < 0) return;
    b = Scene_GetSelectedBrush();
    if (!b || !b->valid) { s_drag_axis = -1; return; }

    Editor_ScreenToRay(sx, sy, r_org, r_dir);
    l_dir[s_drag_axis] = 1.0f;
    t_now = closest_t_line_ray(s_drag_origin, l_dir, r_org, r_dir);

    for (i = 0; i < 3; i++) delta[i] = 0;
    delta[s_drag_axis] = t_now - s_drag_t0;
    s_drag_t0 = t_now;

    Brush_Translate(b, delta);
}

void Editor_GizmoMouseUp(void)
{
    s_drag_axis = -1;
}

int Editor_GizmoIsActive(void)
{
    return s_drag_axis >= 0;
}

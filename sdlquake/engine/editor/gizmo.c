// gizmo.c -- M1 translate gizmo.
//
// Three axis-aligned arrows at the selected brush's centroid. Click an arrow
// to start a drag along that axis; mouse-move applies an incremental world-
// space translate via Brush_Translate (which also recompiles the windings).
// The arrow length scales with distance to the camera so it stays a roughly
// fixed pixel size.

#include "quakedef.h"
#include "edit_scene.h"
#include "edit_history.h"
#include "editor_internal.h"

#include <math.h>

extern vec3_t r_origin;

// -----------------------------------------------------------------------------
// Drag state
// -----------------------------------------------------------------------------

// Two mutually exclusive drag modes:
//   axis-translate: s_drag_axis = 0/1/2 (X/Y/Z)
//   face-resize:    s_drag_plane_idx >= 0 (the plane being pushed along normal)
// Whichever is non-negative is the active drag.
static int      s_drag_axis = -1;
static int      s_drag_plane_idx = -1;
static float    s_drag_t_start;         // closest-t-on-drag-line at mouse down
static float    s_drag_applied;         // sum of grid-snapped offsets applied so far
static vec3_t   s_drag_origin;          // brush centroid (translate) / face centroid (resize) at mouse down
static vec3_t   s_drag_dir;             // axis basis vec (translate) / face normal (resize)
static float    s_drag_start_dist;      // plane.dist at drag start (face-resize only — for absolute snap)

extern cvar_t   editor_grid_snap;
extern cvar_t   editor_grid_size;
extern cvar_t   editor_grid_absolute;

static float snap_to_grid(float v)
{
    if (editor_grid_snap.value == 0) return v;
    float g = editor_grid_size.value;
    if (g <= 0.0f) return v;
    return floorf(v / g + 0.5f) * g;
}

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

static void face_centroid(const edit_face_t *f, vec3_t out)
{
    int k;
    if (f->numverts <= 0) { out[0] = out[1] = out[2] = 0; return; }
    out[0] = out[1] = out[2] = 0;
    for (k = 0; k < f->numverts; k++)
    {
        out[0] += f->verts[k][0];
        out[1] += f->verts[k][1];
        out[2] += f->verts[k][2];
    }
    out[0] /= f->numverts;
    out[1] /= f->numverts;
    out[2] /= f->numverts;
}

// Pick the dominant-axis colour for a face — gives the user a visual cue
// for which way the face points (X=red, Y=yellowgreen, Z=blue).
static byte face_axis_color(const vec3_t normal)
{
    static const byte axis_colors[3] = {
        EDIT_COLOR_AXIS_X, EDIT_COLOR_AXIS_Y, EDIT_COLOR_AXIS_Z
    };
    float ax = fabsf(normal[0]);
    float ay = fabsf(normal[1]);
    float az = fabsf(normal[2]);
    if (ax >= ay && ax >= az) return axis_colors[0];
    if (ay >= az) return axis_colors[1];
    return axis_colors[2];
}

// -----------------------------------------------------------------------------
// Render
// -----------------------------------------------------------------------------

// Average of all selected brushes' centroids — used as the translate-gizmo
// anchor when multi-select is active.
static int selection_centroid(vec3_t out)
{
    int i, n = 0;
    int e_idx, b_idx;
    out[0] = out[1] = out[2] = 0;
    for (i = 0; i < Scene_NumSelected(); i++)
    {
        if (!Scene_GetSelected(i, &e_idx, &b_idx)) continue;
        if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
        edit_entity_t *e = &edit_scene.entities[e_idx];
        if (b_idx < 0 || b_idx >= e->numbrushes) continue;
        edit_brush_t *b = &e->brushes[b_idx];
        if (!b->valid) continue;
        vec3_t c;
        Editor_BrushCentroid(b, c);
        out[0] += c[0]; out[1] += c[1]; out[2] += c[2];
        n++;
    }
    if (n == 0) return 0;
    out[0] /= n; out[1] /= n; out[2] /= n;
    return 1;
}

void Editor_GizmoDraw(void)
{
    edit_brush_t *b = Scene_GetSelectedBrush();
    vec3_t centroid, end;
    float dist, arrow_len;
    static const byte axis_colors[3] = {
        EDIT_COLOR_AXIS_X, EDIT_COLOR_AXIS_Y, EDIT_COLOR_AXIS_Z
    };
    int i;
    int multi = Scene_NumSelected() > 1;
    if (!b || !b->valid) return;

    // Anchor on the selection centroid for multi-select; use the primary
    // brush's centroid for single (preserves the old 1-brush behaviour).
    if (multi)
    {
        if (!selection_centroid(centroid)) return;
    }
    else
    {
        Editor_BrushCentroid(b, centroid);
    }
    {
        vec3_t d;
        VectorSubtract(centroid, r_origin, d);
        dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dist < 1.0f) dist = 1.0f;
    }
    arrow_len = pixel_to_world(dist) * 30.0f;   // ~30 pixel-equivalent length

    // Translate axes — three arrows from centroid.
    for (i = 0; i < 3; i++)
    {
        byte col = (s_drag_axis == i) ? EDIT_COLOR_AXIS_HOT : axis_colors[i];
        axis_endpoint(centroid, i, arrow_len, end);
        Editor_DrawLine3DOver(centroid, end, col);
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

    // Face handles — small "+" cross at each face centroid plus a short
    // outward stub along the face normal so it reads as a push handle. Each
    // face uses the dominant-axis colour of its normal. Skip in multi-
    // select: face-resize is single-brush only, so showing handles on the
    // primary while ignoring the rest would be misleading.
    if (!multi)
    {
        vec3_t fc, p_dist;
        float handle_len = pixel_to_world(dist) * 12.0f;
        int j;
        for (i = 0; i < b->numfaces; i++)
        {
            const edit_face_t *f = &b->faces[i];
            const edit_plane_t *pl = &b->planes[f->plane_idx];
            byte col = (s_drag_plane_idx == f->plane_idx)
                       ? EDIT_COLOR_AXIS_HOT
                       : face_axis_color(pl->normal);
            face_centroid(f, fc);

            // Outward stub.
            for (j = 0; j < 3; j++)
                p_dist[j] = fc[j] + pl->normal[j] * handle_len;
            Editor_DrawLine3DOver(fc, p_dist, col);

            // Two perpendicular bars across the face centroid for the "+"
            // — pick any two unit vectors orthogonal to the normal.
            {
                vec3_t u, v, a, b2;
                float ax = fabsf(pl->normal[0]);
                float ay = fabsf(pl->normal[1]);
                if (ax < 0.9f && ay < 0.9f) {
                    u[0] = 1; u[1] = 0; u[2] = 0;
                } else {
                    u[0] = 0; u[1] = 0; u[2] = 1;
                }
                {
                    float d2 = DotProduct(u, pl->normal);
                    u[0] -= pl->normal[0] * d2;
                    u[1] -= pl->normal[1] * d2;
                    u[2] -= pl->normal[2] * d2;
                    {
                        float l = sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
                        if (l > 1e-6f) { u[0]/=l; u[1]/=l; u[2]/=l; }
                    }
                }
                CrossProduct(pl->normal, u, v);
                {
                    float bar = handle_len * 0.6f;
                    for (j = 0; j < 3; j++) { a[j] = fc[j] + u[j]*bar; b2[j] = fc[j] - u[j]*bar; }
                    Editor_DrawLine3DOver(a, b2, col);
                    for (j = 0; j < 3; j++) { a[j] = fc[j] + v[j]*bar; b2[j] = fc[j] - v[j]*bar; }
                    Editor_DrawLine3DOver(a, b2, col);
                }
            }
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
    int i, best_axis = -1, best_face = -1;
    float best_d_axis = 1e30f, best_d_face = 1e30f;
    int multi = Scene_NumSelected() > 1;

    if (!b || !b->valid) return 0;

    if (multi)
    {
        if (!selection_centroid(centroid)) return 0;
    }
    else
    {
        Editor_BrushCentroid(b, centroid);
    }
    {
        vec3_t d;
        VectorSubtract(centroid, r_origin, d);
        dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dist < 1.0f) dist = 1.0f;
    }
    arrow_len = pixel_to_world(dist) * 30.0f;
    pick_world = pixel_to_world(dist) * GIZMO_PICK_PIXELS;

    Editor_ScreenToRay(sx, sy, r_org, r_dir);

    // Pass 1: face handles (preferred over axis arrows when both are near
    // the cursor — face picks are always at the brush surface and so are
    // closer to the eye than axis arrows centred on the centroid). Face
    // resize is single-brush only; in multi-select we skip face pick so
    // the user can't grab one face thinking it'll resize all of them.
    for (i = 0; !multi && i < b->numfaces; i++)
    {
        vec3_t fc;
        float d;
        face_centroid(&b->faces[i], fc);
        d = point_to_ray(fc, r_org, r_dir);
        if (d < pick_world && d < best_d_face)
        {
            best_d_face = d;
            best_face = i;
        }
    }

    // Pass 2: translate-axis arrows.
    for (i = 0; i < 3; i++)
    {
        vec3_t l_dir = {0,0,0};
        l_dir[i] = 1.0f;
        float t = closest_t_line_ray(centroid, l_dir, r_org, r_dir);
        if (t < 0.0f) continue;
        if (t > arrow_len) continue;
        {
            vec3_t pt = { centroid[0], centroid[1], centroid[2] };
            pt[i] += t;
            float d = point_to_ray(pt, r_org, r_dir);
            if (d < pick_world && d < best_d_axis)
            {
                best_d_axis = d;
                best_axis = i;
            }
        }
    }

    // Face picks win over axis picks (the user clicked on the brush surface).
    if (best_face >= 0)
    {
        const edit_face_t *f = &b->faces[best_face];
        const edit_plane_t *pl = &b->planes[f->plane_idx];
        vec3_t fc;
        face_centroid(f, fc);
        // Snapshot pre-drag state so the whole drag is one undo unit.
        History_Push("resize face");
        s_drag_axis      = -1;
        s_drag_plane_idx = f->plane_idx;
        VectorCopy(fc, s_drag_origin);
        VectorCopy(pl->normal, s_drag_dir);
        s_drag_start_dist = pl->dist;
        s_drag_t_start    = closest_t_line_ray(s_drag_origin, s_drag_dir, r_org, r_dir);
        s_drag_applied    = 0.0f;
        return 1;
    }
    if (best_axis < 0) return 0;

    History_Push("translate");
    s_drag_axis      = best_axis;
    s_drag_plane_idx = -1;
    VectorCopy(centroid, s_drag_origin);
    {
        s_drag_dir[0] = s_drag_dir[1] = s_drag_dir[2] = 0;
        s_drag_dir[best_axis] = 1.0f;
        s_drag_t_start = closest_t_line_ray(s_drag_origin, s_drag_dir, r_org, r_dir);
        s_drag_applied = 0.0f;
    }
    return 1;
}

// Shared snap math. `start_basis` is the value to snap against in absolute
// mode (the axis coord of the drag origin for translate; the plane.dist for
// face-resize). Returns the snapped absolute offset since drag start.
static float compute_snapped_offset(float raw_offset, float start_basis)
{
    if (editor_grid_snap.value != 0.0f && editor_grid_absolute.value != 0.0f)
    {
        float target = snap_to_grid(start_basis + raw_offset);
        return target - start_basis;
    }
    return snap_to_grid(raw_offset);
}

void Editor_GizmoMouseMove(float sx, float sy)
{
    edit_brush_t *b;
    vec3_t r_org, r_dir, delta;
    float t_now, raw_offset, snapped_offset, step;
    int i;
    if (s_drag_axis < 0 && s_drag_plane_idx < 0) return;
    b = Scene_GetSelectedBrush();
    if (!b || !b->valid)
    {
        s_drag_axis = -1;
        s_drag_plane_idx = -1;
        return;
    }

    Editor_ScreenToRay(sx, sy, r_org, r_dir);
    t_now = closest_t_line_ray(s_drag_origin, s_drag_dir, r_org, r_dir);
    raw_offset = t_now - s_drag_t_start;

    if (s_drag_plane_idx >= 0)
    {
        // Face-resize. Absolute snap pins plane.dist to grid lines.
        snapped_offset = compute_snapped_offset(raw_offset, s_drag_start_dist);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        s_drag_applied = snapped_offset;
        Brush_TranslateFace(b, s_drag_plane_idx, step);
    }
    else
    {
        // Axis translate. Absolute snap pins centroid axis coord to grid.
        // In multi-select, every selected brush moves by the same delta.
        int n_sel, k, e_idx, b_idx;
        snapped_offset = compute_snapped_offset(raw_offset,
                                                s_drag_origin[s_drag_axis]);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[s_drag_axis] = step;
        s_drag_applied = snapped_offset;

        n_sel = Scene_NumSelected();
        for (k = 0; k < n_sel; k++)
        {
            edit_brush_t *bs;
            edit_entity_t *e;
            if (!Scene_GetSelected(k, &e_idx, &b_idx)) continue;
            if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
            e = &edit_scene.entities[e_idx];
            if (b_idx < 0 || b_idx >= e->numbrushes) continue;
            bs = &e->brushes[b_idx];
            if (!bs->valid) continue;
            Brush_Translate(bs, delta);
        }
    }
}

void Editor_GizmoMouseUp(void)
{
    s_drag_axis      = -1;
    s_drag_plane_idx = -1;
}

int Editor_GizmoIsActive(void)
{
    return s_drag_axis >= 0 || s_drag_plane_idx >= 0;
}

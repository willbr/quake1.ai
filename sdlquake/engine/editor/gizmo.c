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
#include "editor.h"             // Editor_IsOpen
#include "editor_internal.h"

#include <math.h>

extern vec3_t r_origin;

// -----------------------------------------------------------------------------
// Drag state
// -----------------------------------------------------------------------------

// Three mutually exclusive drag modes:
//   axis-translate: s_drag_axis = 0/1/2 (X/Y/Z)
//   face-resize:    s_drag_plane_idx >= 0 (the plane being pushed along normal)
//   axis-rotate:    s_drag_rotate_axis = 0/1/2 (X/Y/Z) — rotation around that
//                   world axis, pivot is selection centroid at drag start
// Whichever is non-negative is the active drag.
static int      s_drag_axis = -1;
static int      s_drag_plane_idx = -1;
static int      s_drag_rotate_axis = -1;
static int      s_drag_planar_axis = -1;    // normal axis of 2D drag plane (0/1/2), -1 = inactive
static float    s_drag_t_start;         // closest-t-on-drag-line at mouse down
static float    s_drag_applied;         // sum of grid-snapped offsets applied so far
static vec3_t   s_drag_origin;          // brush centroid (translate) / face centroid (resize) at mouse down
static vec3_t   s_drag_dir;             // axis basis vec (translate) / face normal (resize)
static float    s_drag_start_dist;      // plane.dist at drag start (face-resize only — for absolute snap)
static vec3_t   s_drag_planar_hit;      // hit point on drag plane at mouse down
static vec3_t   s_drag_planar_applied;  // snapped delta already applied per axis (planar drag)
static vec3_t   s_rotate_pivot;         // selection centroid captured at rotate-drag start
static float    s_rotate_prev_angle;    // last frame's mouse angle in radians, for incremental step rotation
static float    s_rotate_total_raw;     // accumulated raw angle since drag-start (radians)
static float    s_rotate_total_applied; // total snapped angle already applied (radians)
static float    s_rotate_start_angle;   // primary entity's angle on drag axis at drag-start (absolute snap)

extern cvar_t   editor_grid_snap;
extern cvar_t   editor_grid_size;
extern cvar_t   editor_grid_absolute;
extern cvar_t   editor_rotate_snap;
extern cvar_t   editor_rotate_snap_size;
extern cvar_t   editor_rotate_snap_absolute;
extern cvar_t   editor_snap_surface;

static float snap_to_grid(float v)
{
    if (editor_grid_snap.value == 0) return v;
    float g = editor_grid_size.value;
    if (g <= 0.0f) return v;
    return floorf(v / g + 0.5f) * g;
}

// Signed distance from the selection bbox centroid to the bbox face whose
// outward normal is most opposite to `n`. For axial n=(0,0,1) and an
// axis-aligned bbox, this is just (maxs[2]-mins[2])*0.5. For oblique n,
// project all 8 corners onto -n and take the max — that's the corner
// sticking out the most against the surface. Used as the offset to add
// along `n` after a surface hit so the bbox face lands flush on the
// surface rather than the centroid landing on it.
static float surface_offset_along_normal(const vec3_t mins, const vec3_t maxs,
                                         const vec3_t n)
{
    vec3_t centroid;
    float best = -1e30f;
    int c, k;
    for (k = 0; k < 3; k++) centroid[k] = (mins[k] + maxs[k]) * 0.5f;
    for (c = 0; c < 8; c++)
    {
        vec3_t corner;
        float  proj;
        corner[0] = (c & 1) ? maxs[0] : mins[0];
        corner[1] = (c & 2) ? maxs[1] : mins[1];
        corner[2] = (c & 4) ? maxs[2] : mins[2];
        // -dot(corner - centroid, n) — how far the corner sticks out
        // OPPOSITE to n (toward the surface that has outward-normal n).
        proj = -((corner[0] - centroid[0]) * n[0]
               + (corner[1] - centroid[1]) * n[1]
               + (corner[2] - centroid[2]) * n[2]);
        if (proj > best) best = proj;
    }
    return best;
}

// Cursor-ray surface trace for the active drag. Same as
// Editor_RaycastForPlacement but excludes every currently-selected brush
// (and the brushes of every selected entity) so a brush being moved
// doesn't snap onto its own faces. Point-entity selections add no skip
// entries — they have no faces to skip in the trace.
static int surface_hit_for_drag(float sx, float sy,
                                vec3_t out_hit, vec3_t out_normal)
{
    int n_sel = Scene_NumSelected();
    int n_skip = 0;
    int i, e_idx, b_idx, hit;
    edit_skip_pair_t *skip;
    if (n_sel <= 0)
        return Editor_RaycastForPlacement_Ex(sx, sy, NULL, 0,
                                             out_hit, out_normal);
    skip = (edit_skip_pair_t *)malloc(n_sel * sizeof(*skip));
    for (i = 0; i < n_sel; i++)
    {
        if (!Scene_GetSelected(i, &e_idx, &b_idx)) continue;
        if (b_idx < 0) continue;            // entity ref — no brush face to skip
        skip[n_skip].e_idx = e_idx;
        skip[n_skip].b_idx = b_idx;
        n_skip++;
    }
    hit = Editor_RaycastForPlacement_Ex(sx, sy, skip, n_skip,
                                        out_hit, out_normal);
    free(skip);
    return hit;
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
// Scale the result by vid.height/200 so the gizmo's apparent size in the
// window stays constant as the render resolution grows — otherwise a 30-
// framebuffer-pixel arrow shrinks to a quarter of its 1x size at 4x render.
static float pixel_to_world(float dist)
{
    extern float xscale;
    int s = vid.height / 200;
    if (s < 1) s = 1;
    if (xscale < 1e-3f) return 1.0f;
    return dist * s / xscale;
}

static void axis_endpoint(const vec3_t centroid, int axis, float arrow_len, vec3_t out)
{
    VectorCopy(centroid, out);
    out[axis] += arrow_len;
}

// Intersect a ray with the world-axis-aligned plane through `pivot`
// perpendicular to `axis` (0=X, 1=Y, 2=Z). Writes the intersection point
// to `out`. Returns 1 on hit (front of camera), 0 if ray is parallel to
// the plane or behind.
static int ray_vs_axis_plane(int axis, const vec3_t pivot,
                             const vec3_t r_org, const vec3_t r_dir,
                             vec3_t out)
{
    float denom = r_dir[axis];
    float t;
    if (fabsf(denom) < 1e-6f) return 0;
    t = (pivot[axis] - r_org[axis]) / denom;
    if (t < 0.0f) return 0;
    out[0] = r_org[0] + r_dir[0] * t;
    out[1] = r_org[1] + r_dir[1] * t;
    out[2] = r_org[2] + r_dir[2] * t;
    return 1;
}

// Mouse-cursor angle on the rotation plane perpendicular to `axis` at
// `pivot`. Returns 1 with `out_angle` set in radians (atan2 range), 0 if
// the ray doesn't meet the plane in front of the camera.
static int rotate_mouse_angle(int axis, const vec3_t pivot,
                              const vec3_t r_org, const vec3_t r_dir,
                              float *out_angle)
{
    vec3_t hit;
    int u, v;
    if (!ray_vs_axis_plane(axis, pivot, r_org, r_dir, hit)) return 0;
    u = (axis + 1) % 3;
    v = (axis + 2) % 3;
    *out_angle = atan2f(hit[v] - pivot[v], hit[u] - pivot[u]);
    return 1;
}

// Wrap a delta angle into (-PI, PI] so a drag that crosses the atan2
// branch cut (e.g. user sweeps from 179° to -179°) reads as a -2° step
// rather than a +358° step.
static float wrap_delta(float d)
{
    const float PI = 3.14159265f;
    while (d >  PI) d -= 2.0f * PI;
    while (d < -PI) d += 2.0f * PI;
    return d;
}

// Rotate a 3-vec around world axis (0=X, 1=Y, 2=Z) by ang radians, in place.
static void rotate_vec_axis(int axis, float ang, vec3_t v)
{
    float c = cosf(ang), s = sinf(ang);
    float x = v[0], y = v[1], z = v[2];
    if (axis == 0)      { v[1] = y * c - z * s; v[2] = y * s + z * c; }
    else if (axis == 1) { v[0] = x * c + z * s; v[2] = -x * s + z * c; }
    else                { v[0] = x * c - y * s; v[1] = x * s + y * c; }
}

// Apply a rotation delta to a point entity's facing / movedir. `axis` is a
// world axis (0=X, 1=Y, 2=Z). For yaw (Z) we just spin v.angles[1]. For
// pitch/roll we rebuild pitch+yaw from the rotated v.movedir (movers) or
// from a synthesised forward vector derived from existing angles (facers
// with no movedir — monsters etc., where pitch/roll is normally hidden
// behind the gating logic but this still needs to be safe). Writes the
// .map kv back as scalar "angle" when pitch/roll collapse to 0, full
// "angles" vec3 otherwise. Live edict v.angles + v.movedir synced.
static void entity_apply_rotation_delta(edit_entity_t *e, int axis,
                                        float delta_radians)
{
    int k;
    int angles_idx = -1, angle_idx = -1;
    float pitch = 0, yaw = 0, roll = 0;
    vec3_t dir = {1, 0, 0};
    int have_movedir = 0;
    char buf[64];

    for (k = 0; k < e->numkv; k++)
    {
        if      (!strcmp(e->kv[k].key, "angles")) angles_idx = k;
        else if (!strcmp(e->kv[k].key, "angle"))  angle_idx  = k;
    }

    if (angles_idx >= 0)
        sscanf(e->kv[angles_idx].value, "%f %f %f", &pitch, &yaw, &roll);
    else if (angle_idx >= 0)
    {
        float a = (float)atof(e->kv[angle_idx].value);
        if (a == -1.0f || a == -2.0f) yaw = 0;     // sentinel collapses on first rotation
        else                          yaw = a;
    }

    // Source the direction we're rotating: prefer the live edict's
    // movedir (set by SetMovedir for func_door/button/plat — angles[1]
    // was zeroed at that point so reading from kv would read stale).
    if (e->live_ent && !e->live_ent->free)
    {
        const float *md = e->live_ent->v.movedir;
        if (md[0] != 0.0f || md[1] != 0.0f || md[2] != 0.0f)
        {
            VectorCopy(md, dir);
            have_movedir = 1;
        }
    }
    if (!have_movedir)
    {
        // Synthesise from angles: forward = (cos(yaw)cos(pitch), sin(yaw)cos(pitch), -sin(pitch))
        float yr = yaw   * (3.14159265f / 180.0f);
        float pr = pitch * (3.14159265f / 180.0f);
        dir[0] = cosf(pr) * cosf(yr);
        dir[1] = cosf(pr) * sinf(yr);
        dir[2] = -sinf(pr);
    }

    rotate_vec_axis(axis, delta_radians, dir);

    // Re-extract pitch + yaw from the rotated direction. Roll is fully
    // ambiguous from forward alone; carry whatever roll the .map had.
    {
        float horiz = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        pitch = -atan2f(dir[2], horiz) * (180.0f / 3.14159265f);
        if (horiz > 1e-4f) yaw = atan2f(dir[1], dir[0]) * (180.0f / 3.14159265f);
    }
    while (yaw >= 360.0f) yaw -= 360.0f;
    while (yaw <    0.0f) yaw += 360.0f;
    while (pitch >  180.0f) pitch -= 360.0f;
    while (pitch < -180.0f) pitch += 360.0f;

    if (pitch == 0.0f && roll == 0.0f)
    {
        snprintf(buf, sizeof(buf), "%g", yaw);
        Entity_SetKV(e, "angle", buf);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%g %g %g", pitch, yaw, roll);
        Entity_SetKV(e, "angles", buf);
    }

    if (e->live_ent && !e->live_ent->free)
    {
        e->live_ent->v.angles[0] = pitch;
        e->live_ent->v.angles[1] = yaw;
        e->live_ent->v.angles[2] = roll;
        if (have_movedir)
            VectorCopy(dir, e->live_ent->v.movedir);
    }
}

// Polyline ring (24 segments) at `radius` perpendicular to `axis` through
// `pivot`. Drawn with depth-bypass like the rest of the gizmo handles so
// it's pickable even behind a wall.
static void draw_rotation_ring(const vec3_t pivot, int axis,
                               float radius, byte color)
{
    enum { N = 24 };
    int u = (axis + 1) % 3;
    int v = (axis + 2) % 3;
    vec3_t prev = {0, 0, 0}, p;
    int i;
    const float twopi = 6.2831853f;
    for (i = 0; i <= N; i++)
    {
        float t  = (float)i * twopi / (float)N;
        float ct = cosf(t), st = sinf(t);
        VectorCopy(pivot, p);
        p[u] = pivot[u] + radius * ct;
        p[v] = pivot[v] + radius * st;
        if (i > 0) Editor_DrawLine3DOver(prev, p, color);
        VectorCopy(p, prev);
    }
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

static void apply_translate_delta(const vec3_t delta)
{
    int n_sel = Scene_NumSelected();
    int k, e_idx, b_idx;
    for (k = 0; k < n_sel; k++)
    {
        edit_entity_t *e;
        if (!Scene_GetSelected(k, &e_idx, &b_idx)) continue;
        if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
        e = &edit_scene.entities[e_idx];
        if (b_idx < 0)
        {
            Entity_TranslateOrigin(e, delta);
            if (e->live_ent && !e->live_ent->free)
            {
                vec3_t o;
                int en;
                Entity_GetOrigin(e, o);
                VectorCopy(o, e->live_ent->v.origin);
                SV_LinkEdict(e->live_ent, false);
                en = NUM_FOR_EDICT(e->live_ent);
                if (en > 0 && en < cl.num_entities)
                {
                    entity_t *ce = &cl_entities[en];
                    VectorCopy(o, ce->origin);
                    VectorCopy(o, ce->msg_origins[0]);
                    VectorCopy(o, ce->msg_origins[1]);
                    VectorCopy(e->live_ent->v.angles, ce->angles);
                    VectorCopy(e->live_ent->v.angles, ce->msg_angles[0]);
                    VectorCopy(e->live_ent->v.angles, ce->msg_angles[1]);
                    ce->msgtime   = cl.mtime[0];
                    ce->forcelink = true;
                }
            }
            else if (e->live_static)
            {
                vec3_t o;
                Entity_GetOrigin(e, o);
                R_RemoveEfrags(e->live_static);
                VectorCopy(o, e->live_static->origin);
                R_AddEfrags(e->live_static);
            }
        }
        else
        {
            edit_brush_t *bs;
            if (b_idx >= e->numbrushes) continue;
            bs = &e->brushes[b_idx];
            if (!bs->valid) continue;
            Brush_Translate(bs, delta);
        }
    }
}

// -----------------------------------------------------------------------------
// Render
// -----------------------------------------------------------------------------

// Average of all selected items' centroids — used as the translate-gizmo
// anchor when multi-select is active. Brushes contribute their face-vertex
// centroid; entity refs contribute their resolved anchor (origin key,
// edict absmin/absmax center, or static-entity origin). Entries with no
// resolvable position (e.g. an empty func_group with no brushes / no
// origin / no edict) are skipped so they don't drag the average to (0,0,0).
static int selection_centroid(vec3_t out)
{
    int i, n = 0;
    int e_idx, b_idx;
    out[0] = out[1] = out[2] = 0;
    for (i = 0; i < Scene_NumSelected(); i++)
    {
        edit_entity_t *e;
        vec3_t c;
        if (!Scene_GetSelected(i, &e_idx, &b_idx)) continue;
        if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
        e = &edit_scene.entities[e_idx];
        if (b_idx < 0)
        {
            if (!Editor_EntityAnchor(e, c)) continue;
        }
        else
        {
            edit_brush_t *b;
            if (b_idx >= e->numbrushes) continue;
            b = &e->brushes[b_idx];
            if (!b->valid) continue;
            Editor_BrushCentroid(b, c);
        }
        out[0] += c[0]; out[1] += c[1]; out[2] += c[2];
        n++;
    }
    if (n == 0) return 0;
    out[0] /= n; out[1] /= n; out[2] /= n;
    return 1;
}

// Resolve the translate-gizmo anchor for the *primary* selection. Returns
// 1 on success; out_brush is non-null only when the primary is a brush
// (face handles draw + face-resize pick depend on it). Returns 0 when the
// primary has no resolvable position (e.g. an empty func_group) so the
// caller skips the gizmo entirely instead of pinning it at world origin.
// BSP-loaded brush entities like func_bossgate fall through to
// Editor_EntityAnchor — they have no .map brushes and no origin key, but
// their live edict's absmin/absmax span the compiled brush model.
static int primary_anchor(vec3_t out, edit_brush_t **out_brush)
{
    edit_brush_t  *b = Scene_GetSelectedBrush();
    edit_entity_t *e = Scene_GetSelectedEntity();
    if (out_brush) *out_brush = NULL;
    if (b && b->valid)
    {
        Editor_BrushCentroid(b, out);
        if (out_brush) *out_brush = b;
        return 1;
    }
    if (e && Entity_IsPoint(e))
        return Editor_EntityAnchor(e, out);
    return 0;
}

void Editor_GizmoDraw(void)
{
    edit_brush_t *b = NULL;
    vec3_t centroid, end;
    float dist, arrow_len;
    static const byte axis_colors[3] = {
        EDIT_COLOR_AXIS_X, EDIT_COLOR_AXIS_Y, EDIT_COLOR_AXIS_Z
    };
    int i;
    int multi = Scene_NumSelected() > 1;

    // The whole gizmo is an editing affordance — hide it during play even
    // if a selection persists from the last editor session. Editor_RenderScene
    // still calls us unconditionally so brush wireframes can stay visible.
    if (!Editor_IsOpen()) return;

    // Anchor on the selection centroid for multi-select; use the primary
    // item's centroid (brush or point entity origin) for single. Face
    // handles only render when the primary is a brush.
    if (multi)
    {
        if (!selection_centroid(centroid)) return;
        b = Scene_GetSelectedBrush();      // may be NULL (e.g. point ent primary)
    }
    else
    {
        if (!primary_anchor(centroid, &b)) return;
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
    // select: face-resize is single-brush only. Skip when primary isn't a
    // brush (point entities have no faces).
    if (!multi && b && b->valid)
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

    // Rotation rings — three circles, one per world axis, at the pivot
    // (= centroid). Radius matches the translate-arrow length so the rings
    // outline the arrow tips. The active rotate axis (during a drag) goes
    // hot-white; otherwise each ring takes its axis colour. Pitch (X) and
    // roll (Y) rings only show when the selection contains at least one
    // brush — for pure point-entity selections those axes don't do
    // anything (entity_apply_yaw_delta only handles yaw), so showing them
    // would mislead the user into clicking dead handles.
    {
        float ring_r = arrow_len;
        int show_pitch_roll = 0;
        int k, e_sel, b_sel;
        // Pitch/roll only show when rotating non-yaw makes sense:
        //   - any selected ent has a brush (worldspawn brushes, .map-
        //     authored func brushes), OR
        //   - any selected ent is a func_* (BSP-loaded brush entities
        //     have numbrushes==0 in our scene but their movedir is
        //     still rotatable via entity_apply_rotation_delta).
        for (k = 0; k < Scene_NumSelected(); k++)
        {
            if (!Scene_GetSelected(k, &e_sel, &b_sel)) continue;
            if (b_sel >= 0) { show_pitch_roll = 1; break; }
            if (e_sel >= 0 && e_sel < edit_scene.numentities)
            {
                edit_entity_t *se = &edit_scene.entities[e_sel];
                if (se->classname_idx >= 0
                 && !strncmp(se->kv[se->classname_idx].value, "func_", 5))
                {
                    show_pitch_roll = 1;
                    break;
                }
            }
        }
        for (i = 0; i < 3; i++)
        {
            byte col;
            if (!show_pitch_roll && i != 2) continue;
            col = (s_drag_rotate_axis == i)
                  ? EDIT_COLOR_AXIS_HOT : axis_colors[i];
            draw_rotation_ring(centroid, i, ring_r, col);
        }
    }

    // Planar move handles — one square per axis pair, colored by the normal
    // axis (the constrained direction). Positioned at 35% of arrow_len along
    // each of the two free axes; side is 24% of arrow_len.
    {
        float pd = arrow_len * 0.35f;
        float ph = arrow_len * 0.12f;
        int n;
        for (n = 0; n < 3; n++)
        {
            int u = (n + 1) % 3;
            int v = (n + 2) % 3;
            byte col = (s_drag_planar_axis == n) ? EDIT_COLOR_AXIS_HOT : axis_colors[n];
            vec3_t c00, c10, c11, c01;
            VectorCopy(centroid, c00); c00[u] += pd - ph; c00[v] += pd - ph;
            VectorCopy(centroid, c10); c10[u] += pd + ph; c10[v] += pd - ph;
            VectorCopy(centroid, c11); c11[u] += pd + ph; c11[v] += pd + ph;
            VectorCopy(centroid, c01); c01[u] += pd - ph; c01[v] += pd + ph;
            Editor_DrawLine3DOver(c00, c10, col);
            Editor_DrawLine3DOver(c10, c11, col);
            Editor_DrawLine3DOver(c11, c01, col);
            Editor_DrawLine3DOver(c01, c00, col);
        }
    }
}

// -----------------------------------------------------------------------------
// Mouse interaction
// -----------------------------------------------------------------------------

#define GIZMO_PICK_PIXELS 6.0f          // hit radius (in screen pixels at gizmo distance)

int Editor_GizmoMouseDown(float sx, float sy)
{
    edit_brush_t *b = NULL;
    vec3_t centroid;
    vec3_t r_org, r_dir;
    float dist, arrow_len, ring_r, pick_world;
    int i, best_axis = -1, best_face = -1, best_ring_axis = -1, best_planar = -1;
    float best_d_axis = 1e30f, best_d_face = 1e30f, best_d_ring = 1e30f, best_d_planar = 1e30f;
    int multi = Scene_NumSelected() > 1;

    if (multi)
    {
        if (!selection_centroid(centroid)) return 0;
        b = Scene_GetSelectedBrush();      // may be NULL
    }
    else
    {
        if (!primary_anchor(centroid, &b)) return 0;
    }
    {
        vec3_t d;
        VectorSubtract(centroid, r_origin, d);
        dist = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dist < 1.0f) dist = 1.0f;
    }
    arrow_len  = pixel_to_world(dist) * 30.0f;
    ring_r     = arrow_len;             // matches draw_rotation_ring
    pick_world = pixel_to_world(dist) * GIZMO_PICK_PIXELS;

    Editor_ScreenToRay(sx, sy, r_org, r_dir);

    // Pass 1: face handles (preferred over axis arrows when both are near
    // the cursor — face picks are always at the brush surface and so are
    // closer to the eye than axis arrows centred on the centroid). Face
    // resize is single-brush only; in multi-select we skip face pick so
    // the user can't grab one face thinking it'll resize all of them.
    // Also requires a brush primary (point entities have no faces).
    for (i = 0; !multi && b && b->valid && i < b->numfaces; i++)
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

    // Pass 2: rotation rings. For each axis, intersect the click ray with
    // the world-axis-aligned plane through the centroid (perpendicular to
    // that axis); if the in-plane radial distance is close to ring_r, hit.
    // Pitch/roll picks gated to "rotation makes sense" — same rule as the
    // ring-draw pass: any brush in selection OR any func_* classname.
    {
        int show_pitch_roll = 0;
        int k, e_sel, b_sel;
        for (k = 0; k < Scene_NumSelected(); k++)
        {
            if (!Scene_GetSelected(k, &e_sel, &b_sel)) continue;
            if (b_sel >= 0) { show_pitch_roll = 1; break; }
            if (e_sel >= 0 && e_sel < edit_scene.numentities)
            {
                edit_entity_t *se = &edit_scene.entities[e_sel];
                if (se->classname_idx >= 0
                 && !strncmp(se->kv[se->classname_idx].value, "func_", 5))
                {
                    show_pitch_roll = 1;
                    break;
                }
            }
        }
        for (i = 0; i < 3; i++)
        {
            vec3_t hit;
            float du, dv, r_in;
            int u, v;
            if (!show_pitch_roll && i != 2) continue;
            if (!ray_vs_axis_plane(i, centroid, r_org, r_dir, hit)) continue;
            u = (i + 1) % 3;
            v = (i + 2) % 3;
            du = hit[u] - centroid[u];
            dv = hit[v] - centroid[v];
            r_in = sqrtf(du * du + dv * dv);
            {
                float d = fabsf(r_in - ring_r);
                if (d < pick_world && d < best_d_ring)
                {
                    best_d_ring  = d;
                    best_ring_axis = i;
                }
            }
        }
    }

    // Pass 3: planar handles. Intersect the click ray with each axis-aligned
    // plane through the centroid; accept if the hit falls within the square.
    {
        float pd = arrow_len * 0.35f;
        float ph = arrow_len * 0.12f;
        for (i = 0; i < 3; i++)
        {
            vec3_t hit;
            int u = (i + 1) % 3;
            int v = (i + 2) % 3;
            float du, dv, d;
            if (!ray_vs_axis_plane(i, centroid, r_org, r_dir, hit)) continue;
            du = hit[u] - (centroid[u] + pd);
            dv = hit[v] - (centroid[v] + pd);
            if (fabsf(du) > ph + pick_world) continue;
            if (fabsf(dv) > ph + pick_world) continue;
            d = sqrtf(du*du + dv*dv);
            if (d < best_d_planar)
            {
                best_d_planar = d;
                best_planar   = i;
            }
        }
    }

    // Pass 4: translate-axis arrows.
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

    // Priority: face > ring > arrow. The user clicked on a brush surface
    // (face) for resize, an outer ring for rotate, or an inner arrow for
    // translate — the visual order matches.
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
        s_drag_rotate_axis = -1;
        VectorCopy(fc, s_drag_origin);
        VectorCopy(pl->normal, s_drag_dir);
        s_drag_start_dist = pl->dist;
        s_drag_t_start    = closest_t_line_ray(s_drag_origin, s_drag_dir, r_org, r_dir);
        s_drag_applied    = 0.0f;
        return 1;
    }
    if (best_ring_axis >= 0)
    {
        History_Push("rotate");
        s_drag_axis        = -1;
        s_drag_plane_idx   = -1;
        s_drag_rotate_axis = best_ring_axis;
        VectorCopy(centroid, s_rotate_pivot);
        if (!rotate_mouse_angle(best_ring_axis, s_rotate_pivot, r_org, r_dir,
                                &s_rotate_prev_angle))
            s_rotate_prev_angle = 0;
        s_rotate_total_raw     = 0.0f;
        s_rotate_total_applied = 0.0f;
        s_rotate_start_angle   = 0.0f;
        {
            edit_entity_t *pe = Scene_GetSelectedEntity();
            if (pe)
            {
                int k, ai = -1, angle_idx = -1;
                float pitch = 0, yaw = 0, roll = 0;
                for (k = 0; k < pe->numkv; k++)
                {
                    if      (!strcmp(pe->kv[k].key, "angles")) { ai        = k; break; }
                    else if (!strcmp(pe->kv[k].key, "angle"))    angle_idx = k;
                }
                if (ai >= 0)
                    sscanf(pe->kv[ai].value, "%f %f %f", &pitch, &yaw, &roll);
                else if (angle_idx >= 0)
                    yaw = (float)atof(pe->kv[angle_idx].value);
                if      (best_ring_axis == 2) s_rotate_start_angle = yaw   * (3.14159265f / 180.0f);
                else if (best_ring_axis == 0) s_rotate_start_angle = pitch * (3.14159265f / 180.0f);
                else                          s_rotate_start_angle = roll  * (3.14159265f / 180.0f);
            }
        }
        return 1;
    }
    if (best_planar >= 0)
    {
        History_Push("translate");
        s_drag_axis        = -1;
        s_drag_plane_idx   = -1;
        s_drag_rotate_axis = -1;
        s_drag_planar_axis = best_planar;
        VectorCopy(centroid, s_drag_origin);
        ray_vs_axis_plane(best_planar, centroid, r_org, r_dir, s_drag_planar_hit);
        s_drag_planar_applied[0] = 0;
        s_drag_planar_applied[1] = 0;
        s_drag_planar_applied[2] = 0;
        return 1;
    }
    if (best_axis < 0) return 0;

    History_Push("translate");
    s_drag_axis      = best_axis;
    s_drag_plane_idx = -1;
    s_drag_rotate_axis = -1;
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
    vec3_t r_org, r_dir, delta;
    float t_now, raw_offset, snapped_offset, step;
    int i;
    if (s_drag_axis < 0 && s_drag_plane_idx < 0 && s_drag_rotate_axis < 0 && s_drag_planar_axis < 0)
        return;

    Editor_ScreenToRay(sx, sy, r_org, r_dir);

    if (s_drag_rotate_axis >= 0)
    {
        // Axis-rotate. Project the cursor onto the rotation plane, take
        // the signed angle from last frame's projection (wrapped to handle
        // the atan2 branch cut), and apply that step to every selected
        // brush around the saved pivot. Point entities only respond to
        // yaw rotation (axis 2) — pitch/roll on a monster reads as
        // tilted, which is rarely intended; the user can still drag X/Y
        // rings to rotate selected brushes alongside.
        float a_now;
        int n_sel, k, e_idx, b_idx;
        if (!rotate_mouse_angle(s_drag_rotate_axis, s_rotate_pivot,
                                r_org, r_dir, &a_now))
            return;
        {
            float raw_delta = wrap_delta(a_now - s_rotate_prev_angle);
            s_rotate_prev_angle  = a_now;
            s_rotate_total_raw  += raw_delta;
            if (editor_rotate_snap.value != 0.0f)
            {
                float snap_rad = editor_rotate_snap_size.value * (3.14159265f / 180.0f);
                float snapped_total;
                if (snap_rad <= 0.0f) snap_rad = 3.14159265f / 4.0f;
                if (editor_rotate_snap_absolute.value != 0.0f)
                    snapped_total = floorf((s_rotate_start_angle + s_rotate_total_raw) / snap_rad + 0.5f) * snap_rad
                                    - s_rotate_start_angle;
                else
                    snapped_total = floorf(s_rotate_total_raw / snap_rad + 0.5f) * snap_rad;
                step = snapped_total - s_rotate_total_applied;
                s_rotate_total_applied = snapped_total;
            }
            else
            {
                step = raw_delta;
            }
        }
        if (step == 0.0f) return;

        n_sel = Scene_NumSelected();
        // Apply rotation per unique entity (not per brush) — so a
        // func_door whose N brushes are group-selected gets one angle
        // bump, not N. ang_done[i] flips after first hit. For yaw (Z)
        // we update every non-worldspawn entity. For pitch/roll (X/Y)
        // we only update func_*-classed entities — pitch on a monster
        // looks like a tipped-over corpse (rarely the user's intent),
        // but on a func it rotates the slide direction in 3D, useful
        // for re-orienting a downward-mover (movedir = -Z).
        {
            int *ang_done = (int *)calloc(edit_scene.numentities, sizeof(int));
            for (k = 0; k < n_sel; k++)
            {
                edit_entity_t *e;
                if (!Scene_GetSelected(k, &e_idx, &b_idx)) continue;
                if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
                e = &edit_scene.entities[e_idx];
                if (b_idx >= 0)
                {
                    edit_brush_t *bs;
                    if (b_idx >= e->numbrushes) goto ang_check;
                    bs = &e->brushes[b_idx];
                    if (!bs->valid) goto ang_check;
                    Brush_Rotate(bs, s_drag_rotate_axis, step, s_rotate_pivot);
                }
ang_check:
                if (!ang_done[e_idx])
                {
                    const char *cls = (e->classname_idx >= 0)
                                    ? e->kv[e->classname_idx].value : NULL;
                    int is_world = cls && !strcmp(cls, "worldspawn");
                    int is_func  = cls && !strncmp(cls, "func_", 5);
                    ang_done[e_idx] = 1;
                    if (is_world) continue;
                    if (s_drag_rotate_axis == 2 || is_func)
                        entity_apply_rotation_delta(e, s_drag_rotate_axis, step);
                }
            }
            free(ang_done);
        }
        return;
    }

    if (s_drag_planar_axis >= 0)
    {
        vec3_t hit;
        int u = (s_drag_planar_axis + 1) % 3;
        int v = (s_drag_planar_axis + 2) % 3;
        float raw_u, raw_v, snap_u, snap_v, step_u, step_v;
        if (!ray_vs_axis_plane(s_drag_planar_axis, s_drag_origin, r_org, r_dir, hit))
            return;
        raw_u  = hit[u] - s_drag_planar_hit[u];
        raw_v  = hit[v] - s_drag_planar_hit[v];
        snap_u = compute_snapped_offset(raw_u, s_drag_origin[u]);
        snap_v = compute_snapped_offset(raw_v, s_drag_origin[v]);
        step_u = snap_u - s_drag_planar_applied[u];
        step_v = snap_v - s_drag_planar_applied[v];
        if (step_u == 0.0f && step_v == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[u] = step_u;
        delta[v] = step_v;
        s_drag_planar_applied[u] = snap_u;
        s_drag_planar_applied[v] = snap_v;
        apply_translate_delta(delta);
        return;
    }

    t_now = closest_t_line_ray(s_drag_origin, s_drag_dir, r_org, r_dir);
    raw_offset = t_now - s_drag_t_start;

    if (s_drag_plane_idx >= 0)
    {
        edit_brush_t *b = Scene_GetSelectedBrush();
        if (!b || !b->valid)
        {
            s_drag_plane_idx = -1;
            return;
        }
        snapped_offset = compute_snapped_offset(raw_offset, s_drag_start_dist);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        s_drag_applied = snapped_offset;
        Brush_TranslateFace(b, s_drag_plane_idx, step);
    }
    else
    {
        // Surface snap: cursor over a BSP/brush face → snap so the
        // selection's bbox face nearest the hit normal lies flush with
        // the surface, projected onto the dragged axis. Other axes
        // untouched. Bypasses grid snap on this axis — surface contact
        // is the user-visible behaviour and grid would lift it off.
        if (editor_snap_surface.value != 0.0f)
        {
            vec3_t hit, n, mins, maxs;
            if (surface_hit_for_drag(sx, sy, hit, n)
             && Editor_SelectionBBox(mins, maxs))
            {
                float off    = surface_offset_along_normal(mins, maxs, n);
                float target = hit[s_drag_axis] + n[s_drag_axis] * off;
                float abs_off = target - s_drag_origin[s_drag_axis];
                step = abs_off - s_drag_applied;
                if (step == 0.0f) return;
                for (i = 0; i < 3; i++) delta[i] = 0;
                delta[s_drag_axis] = step;
                s_drag_applied = abs_off;
                apply_translate_delta(delta);
                return;
            }
        }
        snapped_offset = compute_snapped_offset(raw_offset, s_drag_origin[s_drag_axis]);
        step = snapped_offset - s_drag_applied;
        if (step == 0.0f) return;
        for (i = 0; i < 3; i++) delta[i] = 0;
        delta[s_drag_axis] = step;
        s_drag_applied = snapped_offset;
        apply_translate_delta(delta);
    }
}

void Editor_GizmoMouseUp(void)
{
    s_drag_axis        = -1;
    s_drag_plane_idx   = -1;
    s_drag_rotate_axis = -1;
    s_drag_planar_axis = -1;
}

int Editor_GizmoIsActive(void)
{
    return s_drag_axis >= 0 || s_drag_plane_idx >= 0 || s_drag_rotate_axis >= 0
        || s_drag_planar_axis >= 0;
}

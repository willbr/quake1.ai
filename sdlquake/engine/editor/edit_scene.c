// edit_scene.c -- in-memory scene state + selection + lifecycle.

#include "quakedef.h"
#include "edit_scene.h"
#include "editor_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

edit_scene_t edit_scene;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void Scene_Init(void)
{
    memset(&edit_scene, 0, sizeof(edit_scene));
    edit_scene.active_face_ent   = -1;
    edit_scene.active_face_brush = -1;
    edit_scene.active_face_plane = -1;
}

void Scene_Clear(void)
{
    int i, j;
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (e->kv) { free(e->kv); e->kv = NULL; }
        for (j = 0; j < e->numbrushes; j++)
            Brush_FreeFaces(&e->brushes[j]);
        if (e->brushes) { free(e->brushes); e->brushes = NULL; }
    }
    if (edit_scene.entities)  { free(edit_scene.entities);  edit_scene.entities  = NULL; }
    if (edit_scene.selection) { free(edit_scene.selection); edit_scene.selection = NULL; }
    edit_scene.numentities       = 0;
    edit_scene.num_selected      = 0;
    edit_scene.sel_cap           = 0;
    edit_scene.active_face_ent   = -1;
    edit_scene.active_face_brush = -1;
    edit_scene.active_face_plane = -1;
}

void Scene_Shutdown(void)
{
    extern void MapIO_FreeSnapshot(void);
    Scene_Clear();
    MapIO_FreeSnapshot();
}

// -----------------------------------------------------------------------------
// Selection
// -----------------------------------------------------------------------------

// Primary = last entry in the selection list. Single-select call sites
// just see the last-clicked brush, which matches the old behaviour.
static int primary_ent  (void) { return edit_scene.num_selected ? edit_scene.selection[edit_scene.num_selected - 1].entity : -1; }
static int primary_brush(void) { return edit_scene.num_selected ? edit_scene.selection[edit_scene.num_selected - 1].brush  : -1; }

edit_brush_t *Scene_GetSelectedBrush(void)
{
    int e_idx = primary_ent();
    int b_idx = primary_brush();
    edit_entity_t *e;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return NULL;
    e = &edit_scene.entities[e_idx];
    if (b_idx < 0 || b_idx >= e->numbrushes) return NULL;
    return &e->brushes[b_idx];
}

edit_entity_t *Scene_GetSelectedEntity(void)
{
    int e_idx = primary_ent();
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return NULL;
    return &edit_scene.entities[e_idx];
}

int Scene_NumSelected(void) { return edit_scene.num_selected; }

int Scene_GetSelected(int i, int *out_ent, int *out_brush)
{
    if (i < 0 || i >= edit_scene.num_selected) return 0;
    *out_ent   = edit_scene.selection[i].entity;
    *out_brush = edit_scene.selection[i].brush;
    return 1;
}

int Scene_SelectionContains(int ent, int brush)
{
    int i;
    for (i = 0; i < edit_scene.num_selected; i++)
        if (edit_scene.selection[i].entity == ent
            && edit_scene.selection[i].brush == brush)
            return 1;
    return 0;
}

void Scene_SelectionClear(void)
{
    edit_scene.num_selected = 0;
    Scene_ClearActiveFace();
}

// -----------------------------------------------------------------------------
// Active face
// -----------------------------------------------------------------------------

void Scene_ClearActiveFace(void)
{
    edit_scene.active_face_ent   = -1;
    edit_scene.active_face_brush = -1;
    edit_scene.active_face_plane = -1;
}

void Scene_SetActiveFace(int ent, int brush, int plane_idx)
{
    edit_entity_t *e;
    edit_brush_t  *b;
    if (Scene_NumSelected() != 1) return;
    if (!Scene_SelectionContains(ent, brush)) return;
    if (ent < 0 || ent >= edit_scene.numentities) return;
    e = &edit_scene.entities[ent];
    if (brush < 0 || brush >= e->numbrushes) return;
    b = &e->brushes[brush];
    if (plane_idx < 0 || plane_idx >= b->numplanes) return;
    edit_scene.active_face_ent   = ent;
    edit_scene.active_face_brush = brush;
    edit_scene.active_face_plane = plane_idx;
}

int Scene_GetActiveFace(int *out_ent, int *out_brush, int *out_plane)
{
    if (edit_scene.active_face_plane < 0) return 0;
    if (out_ent)   *out_ent   = edit_scene.active_face_ent;
    if (out_brush) *out_brush = edit_scene.active_face_brush;
    if (out_plane) *out_plane = edit_scene.active_face_plane;
    return 1;
}

static void selection_push_unique(int ent, int brush)
{
    if (Scene_SelectionContains(ent, brush)) return;
    if (edit_scene.num_selected >= edit_scene.sel_cap)
    {
        int new_cap = edit_scene.sel_cap ? edit_scene.sel_cap * 2 : 8;
        edit_scene.selection = (edit_selref_t *)realloc(
            edit_scene.selection, new_cap * sizeof(edit_selref_t));
        edit_scene.sel_cap = new_cap;
    }
    edit_scene.selection[edit_scene.num_selected].entity = ent;
    edit_scene.selection[edit_scene.num_selected].brush  = brush;
    edit_scene.num_selected++;
}

// Brush entities other than worldspawn act as a single group: selecting any
// brush in them selects all of them. That covers both editor-created
// func_group containers and gameplay entities (func_door, func_button…)
// where the multiple brushes are conceptually one object. Point entities
// (no brushes) are handled separately: they're stored as (ent, -1) refs.
static int entity_is_brush_group(int ent)
{
    edit_entity_t *e;
    if (ent < 0 || ent >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[ent];
    if (e->numbrushes == 0) return 0;       // point entity, not a brush group
    if (e->classname_idx < 0) return 0;
    return strcmp(e->kv[e->classname_idx].value, "worldspawn") != 0;
}

int Entity_IsPoint(const edit_entity_t *e)
{
    return e && e->numbrushes == 0;
}

void Scene_SelectionAdd(int ent, int brush)
{
    edit_entity_t *e;
    if (ent < 0 || ent >= edit_scene.numentities) return;
    e = &edit_scene.entities[ent];
    if (Entity_IsPoint(e))
    {
        selection_push_unique(ent, -1);
    }
    else if (entity_is_brush_group(ent))
    {
        int j;
        for (j = 0; j < e->numbrushes; j++)
            selection_push_unique(ent, j);
    }
    else
    {
        selection_push_unique(ent, brush);
    }
    Scene_ClearActiveFace();
}

static void selection_remove_one(int ent, int brush)
{
    int i, j;
    for (i = 0; i < edit_scene.num_selected; i++)
    {
        if (edit_scene.selection[i].entity == ent
            && edit_scene.selection[i].brush == brush)
        {
            for (j = i + 1; j < edit_scene.num_selected; j++)
                edit_scene.selection[j - 1] = edit_scene.selection[j];
            edit_scene.num_selected--;
            return;
        }
    }
}

void Scene_SelectionRemove(int ent, int brush)
{
    edit_entity_t *e;
    if (ent < 0 || ent >= edit_scene.numentities) return;
    e = &edit_scene.entities[ent];
    if (Entity_IsPoint(e))
    {
        selection_remove_one(ent, -1);
        return;
    }
    if (entity_is_brush_group(ent))
    {
        // Remove every selection entry for this entity.
        int i = 0;
        while (i < edit_scene.num_selected)
        {
            if (edit_scene.selection[i].entity == ent)
            {
                int j;
                for (j = i + 1; j < edit_scene.num_selected; j++)
                    edit_scene.selection[j - 1] = edit_scene.selection[j];
                edit_scene.num_selected--;
            }
            else i++;
        }
    }
    else
    {
        selection_remove_one(ent, brush);
    }
    Scene_ClearActiveFace();
}

void Scene_SelectionToggle(int ent, int brush)
{
    edit_entity_t *e;
    if (ent < 0 || ent >= edit_scene.numentities) return;
    e = &edit_scene.entities[ent];
    if (Entity_IsPoint(e))
    {
        if (Scene_SelectionContains(ent, -1))
            selection_remove_one(ent, -1);
        else
            selection_push_unique(ent, -1);
        return;
    }
    if (entity_is_brush_group(ent))
    {
        // Whole group toggles.
        int has = 0, i;
        for (i = 0; i < edit_scene.num_selected; i++)
            if (edit_scene.selection[i].entity == ent) { has = 1; break; }
        if (has) Scene_SelectionRemove(ent, brush);
        else     Scene_SelectionAdd   (ent, brush);
    }
    else
    {
        if (Scene_SelectionContains(ent, brush))
            selection_remove_one(ent, brush);
        else
            selection_push_unique(ent, brush);
    }
    Scene_ClearActiveFace();
}

void Scene_ForEachBrush(Scene_BrushIter_fn cb, void *user)
{
    int i, j;
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            if (!b->valid) continue;
            if (!cb(e, i, b, j, user)) return;
        }
    }
}

// -----------------------------------------------------------------------------
// Brush translate (recompiles faces afterwards)
// -----------------------------------------------------------------------------

void Brush_Translate(edit_brush_t *b, const vec3_t delta)
{
    int i, j;
    for (i = 0; i < b->numplanes; i++)
    {
        edit_plane_t *p = &b->planes[i];
        // Texture lock: keep the projected texture pinned to the brush by
        // subtracting the texel-shift induced by the world translation.
        // s_world = dot(P, s_axis) + s_shift, so a +delta in P needs an
        // equal -dot(delta, s_axis) in s_shift to leave s_world unchanged.
        {
            vec3_t s_axis, t_axis;
            float  s_shift, t_shift;
            Editor_PlaneUVAxes(p, s_axis, t_axis, &s_shift, &t_shift);
            p->s_shift -= DotProduct(delta, s_axis);
            p->t_shift -= DotProduct(delta, t_axis);
        }
        for (j = 0; j < 3; j++)
            VectorAdd(p->points[j], delta, p->points[j]);
    }
    Brush_Compile(b);
}

// Rotate a single point about `pivot` by `ang` radians around world axis
// (0=X, 1=Y, 2=Z). In-place.
static void rotate_point_about_axis(int axis, float ang,
                                    const vec3_t pivot, vec3_t pt)
{
    float c = cosf(ang), s = sinf(ang);
    float dx = pt[0] - pivot[0];
    float dy = pt[1] - pivot[1];
    float dz = pt[2] - pivot[2];
    if (axis == 0)
    {
        pt[0] = pivot[0] + dx;
        pt[1] = pivot[1] + dy * c - dz * s;
        pt[2] = pivot[2] + dy * s + dz * c;
    }
    else if (axis == 1)
    {
        pt[0] = pivot[0] + dx * c + dz * s;
        pt[1] = pivot[1] + dy;
        pt[2] = pivot[2] - dx * s + dz * c;
    }
    else
    {
        pt[0] = pivot[0] + dx * c - dy * s;
        pt[1] = pivot[1] + dx * s + dy * c;
        pt[2] = pivot[2] + dz;
    }
}

void Brush_Rotate(edit_brush_t *b, int axis, float ang, const vec3_t pivot)
{
    int i, k;
    for (i = 0; i < b->numplanes; i++)
        for (k = 0; k < 3; k++)
            rotate_point_about_axis(axis, ang, pivot, b->planes[i].points[k]);
    Brush_Compile(b);
}

void Brush_TranslateFace(edit_brush_t *b, int plane_idx, float delta)
{
    edit_plane_t *p, saved;
    vec3_t move;
    int j;
    if (plane_idx < 0 || plane_idx >= b->numplanes) return;
    if (delta == 0.0f) return;
    p = &b->planes[plane_idx];
    saved = *p;

    move[0] = p->normal[0] * delta;
    move[1] = p->normal[1] * delta;
    move[2] = p->normal[2] * delta;

    // Texture lock for the moving face only — same trick as Brush_Translate
    // but scoped to this plane, so the texture stays pinned to the face as
    // it slides along its normal.
    {
        vec3_t s_axis, t_axis;
        float  s_shift, t_shift;
        Editor_PlaneUVAxes(p, s_axis, t_axis, &s_shift, &t_shift);
        p->s_shift -= DotProduct(move, s_axis);
        p->t_shift -= DotProduct(move, t_axis);
    }
    for (j = 0; j < 3; j++)
        VectorAdd(p->points[j], move, p->points[j]);

    Brush_Compile(b);

    // If the face crossed through the rest of the brush, the compile
    // produces no faces (or fewer than expected). Roll back so the user
    // doesn't lose their selection to an invisible degenerate.
    if (!b->valid)
    {
        *p = saved;
        Brush_Compile(b);
    }
}

// Scene_Revert lives in map_io.c — it re-parses the in-memory snapshot text
// captured at Scene_Load time, so it gives back the load-time scene even
// after the user has saved over the .map on disk.

// -----------------------------------------------------------------------------
// Brush creation
// -----------------------------------------------------------------------------

// Find or create the worldspawn entity. New scenes start empty so the
// "Add cube" button has to be able to bootstrap one.
static int worldspawn_index(void)
{
    int i;
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (e->classname_idx >= 0
            && !strcmp(e->kv[e->classname_idx].value, "worldspawn"))
            return i;
    }
    // Create one.
    {
        edit_entity_t e;
        memset(&e, 0, sizeof(e));
        e.kv = (edit_kv_t *)calloc(1, sizeof(edit_kv_t));
        Q_strncpy(e.kv[0].key,   "classname",   EDIT_KEY_LEN - 1);
        Q_strncpy(e.kv[0].value, "worldspawn",  EDIT_VAL_LEN - 1);
        e.numkv = 1;
        e.classname_idx = 0;
        e.origin_idx    = -1;
        edit_scene.entities = (edit_entity_t *)realloc(
            edit_scene.entities,
            (edit_scene.numentities + 1) * sizeof(edit_entity_t));
        edit_scene.entities[edit_scene.numentities++] = e;
        return edit_scene.numentities - 1;
    }
}

// Set one plane from three points, the texture name and default uv params.
// Caller picks p0/p1/p2 such that cross(p0-p1, p2-p1) gives the outward
// normal — this is qbsp's "CCW from outside" convention.
static void set_plane(edit_plane_t *pl,
                      const vec3_t p0, const vec3_t p1, const vec3_t p2,
                      const char *texname)
{
    memset(pl, 0, sizeof(*pl));
    VectorCopy(p0, pl->points[0]);
    VectorCopy(p1, pl->points[1]);
    VectorCopy(p2, pl->points[2]);
    Q_strncpy(pl->texname, texname, EDIT_TEX_NAME_LEN - 1);
    pl->s_shift = 0;
    pl->t_shift = 0;
    pl->rotation = 0;
    pl->s_scale = 1;
    pl->t_scale = 1;
}

// Internal: write 6 axis-aligned plane definitions for the cube spanning
// mins..maxs into `b`, then compile. `b` must be memset already. Returns 1
// on a successful compile (i.e. the cube has volume), 0 otherwise.
static int build_cube_brush(edit_brush_t *b,
                            const vec3_t mins, const vec3_t maxs,
                            const char *tex)
{
    // Three points per face, chosen so cross(p0-p1, p2-p1) gives the
    // outward normal — qbsp's "CCW from outside" convention. Each pX is
    // one of the 8 cube corners written as (x_idx, y_idx, z_idx) with
    // idx 0=mins, 1=maxs.
    static const int verts[6][3][3] = {
        { {1,1,1}, {1,1,0}, {1,0,0} },   // +X
        { {0,0,1}, {0,0,0}, {0,1,0} },   // -X
        { {0,1,1}, {0,1,0}, {1,1,0} },   // +Y
        { {1,0,1}, {1,0,0}, {0,0,0} },   // -Y
        { {1,1,1}, {1,0,1}, {0,0,1} },   // +Z
        { {0,1,0}, {0,0,0}, {1,0,0} },   // -Z
    };
    int f, v;
    const vec_t *axis[2];
    if (maxs[0] <= mins[0] || maxs[1] <= mins[1] || maxs[2] <= mins[2])
        return 0;
    axis[0] = mins;
    axis[1] = maxs;
    for (f = 0; f < 6; f++)
    {
        vec3_t pp[3];
        for (v = 0; v < 3; v++)
        {
            pp[v][0] = axis[verts[f][v][0]][0];
            pp[v][1] = axis[verts[f][v][1]][1];
            pp[v][2] = axis[verts[f][v][2]][2];
        }
        set_plane(&b->planes[f], pp[0], pp[1], pp[2], tex);
    }
    b->numplanes = 6;
    Brush_Compile(b);
    return b->valid;
}

int Scene_AddCubeBrush(const vec3_t mins, const vec3_t maxs, const char *texname)
{
    int wi;
    edit_entity_t *e;
    edit_brush_t *b;
    const char *tex = texname && texname[0] ? texname : "wbrick1_5";

    if (maxs[0] <= mins[0] || maxs[1] <= mins[1] || maxs[2] <= mins[2])
    {
        Con_Printf("editor: Scene_AddCubeBrush: maxs <= mins\n");
        return 0;
    }

    wi = worldspawn_index();
    e  = &edit_scene.entities[wi];
    e->brushes = (edit_brush_t *)realloc(
        e->brushes, (e->numbrushes + 1) * sizeof(edit_brush_t));
    b = &e->brushes[e->numbrushes];
    memset(b, 0, sizeof(*b));

    if (!build_cube_brush(b, mins, maxs, tex))
    {
        Con_Printf("editor: Scene_AddCubeBrush: compile produced no faces\n");
        return 0;
    }

    e->numbrushes++;
    Scene_SelectionClear();
    Scene_SelectionAdd(wi, e->numbrushes - 1);
    return 1;
}

// Replace a solid brush with 6 wall slabs spanning the same outer bbox —
// a "make hollow" operation. The walls are axis-disjoint so they never
// overlap (floor + ceiling cover the full XY extent; ±Y walls cover full
// X minus the corner-space taken by ±X walls). Each wall inherits the
// source brush's plane[0] texname so the user keeps their texture choice.
// Walls always go to worldspawn — even if the source was in a func_*,
// putting wall slabs on a non-world entity is rarely what the user wants.
int Scene_HollowBrush(int e_idx, int b_idx, float thickness)
{
    edit_entity_t *e, *ws;
    edit_brush_t *src;
    vec3_t mins, maxs, wmin[6], wmax[6];
    char tex[EDIT_TEX_NAME_LEN];
    int wi, k;

    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];
    if (b_idx < 0 || b_idx >= e->numbrushes) return 0;
    src = &e->brushes[b_idx];
    if (!src->valid)
    {
        Con_Printf("editor: Scene_HollowBrush: source brush is invalid\n");
        return 0;
    }

    if (thickness <= 0.0f) thickness = 16.0f;
    VectorCopy(src->mins, mins);
    VectorCopy(src->maxs, maxs);
    if (thickness * 2.0f >= maxs[0] - mins[0]
     || thickness * 2.0f >= maxs[1] - mins[1]
     || thickness * 2.0f >= maxs[2] - mins[2])
    {
        Con_Printf("editor: Scene_HollowBrush: thickness %g leaves no room\n",
                   thickness);
        return 0;
    }

    Q_strncpy(tex,
              src->planes[0].texname[0] ? src->planes[0].texname : "wbrick1_5",
              EDIT_TEX_NAME_LEN - 1);
    tex[EDIT_TEX_NAME_LEN - 1] = '\0';

    // Floor / ceiling — full XY extent.
    wmin[0][0]=mins[0]; wmin[0][1]=mins[1]; wmin[0][2]=mins[2];
    wmax[0][0]=maxs[0]; wmax[0][1]=maxs[1]; wmax[0][2]=mins[2]+thickness;
    wmin[1][0]=mins[0]; wmin[1][1]=mins[1]; wmin[1][2]=maxs[2]-thickness;
    wmax[1][0]=maxs[0]; wmax[1][1]=maxs[1]; wmax[1][2]=maxs[2];
    // -Y / +Y walls — full X, fit between floor and ceiling.
    wmin[2][0]=mins[0]; wmin[2][1]=mins[1];           wmin[2][2]=mins[2]+thickness;
    wmax[2][0]=maxs[0]; wmax[2][1]=mins[1]+thickness; wmax[2][2]=maxs[2]-thickness;
    wmin[3][0]=mins[0]; wmin[3][1]=maxs[1]-thickness; wmin[3][2]=mins[2]+thickness;
    wmax[3][0]=maxs[0]; wmax[3][1]=maxs[1];           wmax[3][2]=maxs[2]-thickness;
    // -X / +X walls — fit between floor/ceiling and the Y walls.
    wmin[4][0]=mins[0];           wmin[4][1]=mins[1]+thickness; wmin[4][2]=mins[2]+thickness;
    wmax[4][0]=mins[0]+thickness; wmax[4][1]=maxs[1]-thickness; wmax[4][2]=maxs[2]-thickness;
    wmin[5][0]=maxs[0]-thickness; wmin[5][1]=mins[1]+thickness; wmin[5][2]=mins[2]+thickness;
    wmax[5][0]=maxs[0];           wmax[5][1]=maxs[1]-thickness; wmax[5][2]=maxs[2]-thickness;

    // Drop the source brush first (face arrays freed). Selection refs to
    // the deleted index will be invalidated by the SelectionClear below.
    Brush_FreeFaces(src);
    for (k = b_idx; k < e->numbrushes - 1; k++)
        e->brushes[k] = e->brushes[k + 1];
    e->numbrushes--;

    wi = worldspawn_index();
    ws = &edit_scene.entities[wi];
    ws->brushes = (edit_brush_t *)realloc(ws->brushes,
        (ws->numbrushes + 6) * sizeof(edit_brush_t));

    Scene_SelectionClear();
    for (k = 0; k < 6; k++)
    {
        edit_brush_t *b = &ws->brushes[ws->numbrushes];
        memset(b, 0, sizeof(*b));
        if (!build_cube_brush(b, wmin[k], wmax[k], tex))
        {
            Con_Printf("editor: Scene_HollowBrush: wall %d failed to compile\n", k);
            continue;
        }
        Scene_SelectionAdd(wi, ws->numbrushes);
        ws->numbrushes++;
    }
    return 1;
}

// -----------------------------------------------------------------------------
// Point entities
// -----------------------------------------------------------------------------

int Entity_GetOrigin(const edit_entity_t *e, vec3_t out)
{
    out[0] = out[1] = out[2] = 0;
    if (!e || e->origin_idx < 0) return 0;
    {
        // Quake .map origin format is three space-separated floats. sscanf
        // tolerates extra whitespace and missing fields (zeros the rest).
        const char *v = e->kv[e->origin_idx].value;
        float a = 0, b = 0, c = 0;
        sscanf(v, "%f %f %f", &a, &b, &c);
        out[0] = a; out[1] = b; out[2] = c;
    }
    return 1;
}

// Find the kv slot for `key`. Returns the index, or -1 if absent.
static int kv_find(edit_entity_t *e, const char *key)
{
    int i;
    for (i = 0; i < e->numkv; i++)
        if (!strcmp(e->kv[i].key, key)) return i;
    return -1;
}

// Append a kv slot with `key` initialised; the caller fills in the value.
// Returns the new index. Refreshes classname_idx / origin_idx if relevant.
static int kv_append(edit_entity_t *e, const char *key)
{
    int idx = e->numkv;
    e->kv = (edit_kv_t *)realloc(e->kv, (idx + 1) * sizeof(edit_kv_t));
    memset(&e->kv[idx], 0, sizeof(edit_kv_t));
    Q_strncpy(e->kv[idx].key, key, EDIT_KEY_LEN - 1);
    e->numkv++;
    if (!strcmp(key, "classname")) e->classname_idx = idx;
    else if (!strcmp(key, "origin")) e->origin_idx = idx;
    return idx;
}

void Entity_TranslateOrigin(edit_entity_t *e, const vec3_t delta)
{
    vec3_t o;
    int idx;
    if (!e) return;
    Entity_GetOrigin(e, o);
    o[0] += delta[0]; o[1] += delta[1]; o[2] += delta[2];
    idx = e->origin_idx;
    if (idx < 0) idx = kv_append(e, "origin");
    snprintf(e->kv[idx].value, EDIT_VAL_LEN, "%g %g %g", o[0], o[1], o[2]);
}

void Entity_SetKV(edit_entity_t *e, const char *key, const char *value)
{
    int idx;
    if (!e || !key || !key[0]) return;
    idx = kv_find(e, key);
    if (idx < 0) idx = kv_append(e, key);
    Q_strncpy(e->kv[idx].value, value ? value : "", EDIT_VAL_LEN - 1);
    e->kv[idx].value[EDIT_VAL_LEN - 1] = '\0';
}

int Entity_RemoveKV(edit_entity_t *e, const char *key)
{
    int idx, j;
    if (!e || !key || !key[0]) return 0;
    idx = kv_find(e, key);
    if (idx < 0) return 0;
    for (j = idx + 1; j < e->numkv; j++)
        e->kv[j - 1] = e->kv[j];
    e->numkv--;
    // Rebuild classname/origin caches — both the index of the removed key
    // matters (hit) and the indices after the removal point shift down.
    e->classname_idx = kv_find(e, "classname");
    e->origin_idx    = kv_find(e, "origin");
    return 1;
}

int Scene_AddPointEntity(const char *classname, const vec3_t origin)
{
    edit_entity_t e;
    int idx;
    if (!classname || !classname[0]) return 0;

    memset(&e, 0, sizeof(e));
    e.classname_idx = -1;
    e.origin_idx    = -1;
    kv_append(&e, "classname");
    Q_strncpy(e.kv[0].value, classname, EDIT_VAL_LEN - 1);
    kv_append(&e, "origin");
    snprintf(e.kv[1].value, EDIT_VAL_LEN, "%g %g %g",
             origin[0], origin[1], origin[2]);

    edit_scene.entities = (edit_entity_t *)realloc(edit_scene.entities,
        (edit_scene.numentities + 1) * sizeof(edit_entity_t));
    idx = edit_scene.numentities;
    edit_scene.entities[idx] = e;
    edit_scene.numentities++;

    Scene_SelectionClear();
    Scene_SelectionAdd(idx, -1);
    return 1;
}

// -----------------------------------------------------------------------------
// Delete
// -----------------------------------------------------------------------------

void Scene_DeleteSelected(void)
{
    int i, j, k, n_sel = Scene_NumSelected();
    int worldspawn = -1;
    int *delete_ent;
    int *delete_brush = NULL;

    if (n_sel == 0) return;

    // Worldspawn is special — never delete the entity itself, only its
    // selected brushes. (Deleting worldspawn would orphan every brush
    // that should belong to the world geometry.)
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (e->classname_idx >= 0
         && !strcmp(e->kv[e->classname_idx].value, "worldspawn"))
        {
            worldspawn = i;
            break;
        }
    }

    delete_ent = (int *)calloc(edit_scene.numentities, sizeof(int));
    if (!delete_ent) return;
    if (worldspawn >= 0 && edit_scene.entities[worldspawn].numbrushes > 0)
    {
        delete_brush = (int *)calloc(
            edit_scene.entities[worldspawn].numbrushes, sizeof(int));
        if (!delete_brush) { free(delete_ent); return; }
    }

    // Build the deletion plan. For non-worldspawn entries we always
    // delete the whole entity (Scene_SelectionAdd group-expanded to
    // every brush already, so there's no per-brush deletion for brush
    // entities). Worldspawn gets per-brush deletion.
    for (i = 0; i < n_sel; i++)
    {
        int e_idx, b_idx;
        if (!Scene_GetSelected(i, &e_idx, &b_idx)) continue;
        if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
        if (e_idx == worldspawn)
        {
            if (b_idx >= 0 && delete_brush
             && b_idx < edit_scene.entities[worldspawn].numbrushes)
                delete_brush[b_idx] = 1;
        }
        else
        {
            delete_ent[e_idx] = 1;
        }
    }

    // ED_Free the live edict so the engine stops simulating it, then
    // clear cl_entities[N].model — that's what CL_RelinkEntities checks
    // before pushing into cl_visedicts each frame, so without zeroing it
    // the engine keeps drawing the entity at its last position until the
    // editor closes and a server frame finally networks the change.
    // SV_MakeStatic'd ents (torches) live in cl_static_entities[] and
    // render via the BSP-leaf efrag chain — pull the efrags + null the
    // model to stop the render this frame.
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (!delete_ent[i]) continue;
        if (e->live_ent && !e->live_ent->free)
        {
            int en = NUM_FOR_EDICT(e->live_ent);
            ED_Free(e->live_ent);
            if (en > 0 && en < cl.num_entities)
            {
                cl_entities[en].model     = NULL;
                cl_entities[en].forcelink = false;
            }
        }
        if (e->live_static)
        {
            R_RemoveEfrags(e->live_static);
            e->live_static->model = NULL;
        }
    }

    // Worldspawn brush removal first, in reverse so the lower indices
    // we still need to process stay valid.
    if (worldspawn >= 0 && delete_brush)
    {
        edit_entity_t *ws = &edit_scene.entities[worldspawn];
        for (j = ws->numbrushes - 1; j >= 0; j--)
        {
            if (!delete_brush[j]) continue;
            Brush_FreeFaces(&ws->brushes[j]);
            for (k = j; k < ws->numbrushes - 1; k++)
                ws->brushes[k] = ws->brushes[k + 1];
            ws->numbrushes--;
        }
    }

    // Entity removal (reverse for the same reason).
    for (i = edit_scene.numentities - 1; i >= 0; i--)
    {
        if (!delete_ent[i]) continue;
        {
            edit_entity_t *e = &edit_scene.entities[i];
            if (e->kv) free(e->kv);
            for (k = 0; k < e->numbrushes; k++)
                Brush_FreeFaces(&e->brushes[k]);
            if (e->brushes) free(e->brushes);
        }
        for (k = i; k < edit_scene.numentities - 1; k++)
            edit_scene.entities[k] = edit_scene.entities[k + 1];
        edit_scene.numentities--;
    }

    free(delete_ent);
    if (delete_brush) free(delete_brush);

    // Selection indices reference the pre-delete layout — discard.
    Scene_SelectionClear();
}

// -----------------------------------------------------------------------------
// Group / Ungroup
// -----------------------------------------------------------------------------

// Move a brush's data from src->brushes[src_idx] into dst->brushes (appended).
// Removes the slot from src by shifting later brushes down. After this, all
// brush indices >= src_idx in src shift down by one.
static void move_brush(edit_entity_t *src, int src_idx, edit_entity_t *dst)
{
    edit_brush_t b = src->brushes[src_idx];
    int j;
    dst->brushes = (edit_brush_t *)realloc(dst->brushes,
        (dst->numbrushes + 1) * sizeof(edit_brush_t));
    dst->brushes[dst->numbrushes++] = b;
    for (j = src_idx + 1; j < src->numbrushes; j++)
        src->brushes[j - 1] = src->brushes[j];
    src->numbrushes--;
}

void Scene_GroupSelected(void)
{
    int i, n_moved = 0, src_ent;
    edit_entity_t group_e;
    int new_ent_idx;

    if (edit_scene.num_selected == 0)
    {
        Con_Printf("editor: nothing selected to group\n");
        return;
    }

    // Snapshot the (entity, brush) pairs so we can iterate stably even as
    // brush arrays shift under us.
    edit_selref_t *snap = (edit_selref_t *)malloc(
        edit_scene.num_selected * sizeof(edit_selref_t));
    int n = edit_scene.num_selected;
    for (i = 0; i < n; i++) snap[i] = edit_scene.selection[i];

    // Build the new func_group entity (kv list with classname only).
    memset(&group_e, 0, sizeof(group_e));
    group_e.kv = (edit_kv_t *)calloc(1, sizeof(edit_kv_t));
    Q_strncpy(group_e.kv[0].key,   "classname",  EDIT_KEY_LEN - 1);
    Q_strncpy(group_e.kv[0].value, "func_group", EDIT_VAL_LEN - 1);
    group_e.numkv         = 1;
    group_e.classname_idx = 0;
    group_e.origin_idx    = -1;

    // Append entity slot (don't move into it yet — we need a stable pointer).
    edit_scene.entities = (edit_entity_t *)realloc(edit_scene.entities,
        (edit_scene.numentities + 1) * sizeof(edit_entity_t));
    new_ent_idx = edit_scene.numentities;
    edit_scene.entities[new_ent_idx] = group_e;
    edit_scene.numentities++;

    // Process moves in reverse-index order so removal-shifts within the
    // same source entity don't invalidate later-indexed brushes. Sort the
    // snapshot by (ent desc, brush desc) — small N, simple bubble sort.
    {
        int a, c;
        for (a = 0; a < n; a++)
            for (c = a + 1; c < n; c++)
            {
                int swap = 0;
                if (snap[a].entity < snap[c].entity) swap = 1;
                else if (snap[a].entity == snap[c].entity
                         && snap[a].brush < snap[c].brush) swap = 1;
                if (swap)
                {
                    edit_selref_t t = snap[a]; snap[a] = snap[c]; snap[c] = t;
                }
            }
    }

    for (i = 0; i < n; i++)
    {
        src_ent = snap[i].entity;
        if (src_ent == new_ent_idx) continue;       // already there (paranoia)
        if (src_ent < 0 || src_ent >= edit_scene.numentities) continue;
        move_brush(&edit_scene.entities[src_ent], snap[i].brush,
                   &edit_scene.entities[new_ent_idx]);
        n_moved++;
    }
    free(snap);

    // New selection: every brush in the new group entity.
    Scene_SelectionClear();
    Scene_SelectionAdd(new_ent_idx, 0);

    Con_Printf("editor: grouped %d brushes into func_group\n", n_moved);
}

void Scene_UngroupSelected(void)
{
    int i, n, n_moved = 0;
    int wi;
    edit_selref_t *snap;
    if (edit_scene.num_selected == 0)
    {
        Con_Printf("editor: nothing selected to ungroup\n");
        return;
    }

    wi = worldspawn_index();    // creates if needed

    n = edit_scene.num_selected;
    snap = (edit_selref_t *)malloc(n * sizeof(edit_selref_t));
    for (i = 0; i < n; i++) snap[i] = edit_scene.selection[i];

    // Reverse order, same as group.
    {
        int a, c;
        for (a = 0; a < n; a++)
            for (c = a + 1; c < n; c++)
            {
                int swap = 0;
                if (snap[a].entity < snap[c].entity) swap = 1;
                else if (snap[a].entity == snap[c].entity
                         && snap[a].brush < snap[c].brush) swap = 1;
                if (swap)
                {
                    edit_selref_t t = snap[a]; snap[a] = snap[c]; snap[c] = t;
                }
            }
    }

    Scene_SelectionClear();
    for (i = 0; i < n; i++)
    {
        edit_entity_t *e;
        const char *cls;
        if (snap[i].entity == wi) continue;     // already worldspawn
        if (snap[i].entity < 0 || snap[i].entity >= edit_scene.numentities) continue;
        e = &edit_scene.entities[snap[i].entity];
        if (e->classname_idx < 0) continue;
        cls = e->kv[e->classname_idx].value;
        // Only ungroup func_group (editor containers). Leave func_door etc.
        // alone — their grouping has gameplay meaning.
        if (strcmp(cls, "func_group") != 0) continue;
        move_brush(e, snap[i].brush, &edit_scene.entities[wi]);
        n_moved++;
    }
    free(snap);

    // Drop now-empty func_group containers so the brushes panel doesn't
    // accumulate ghost entries. Iterate backwards so removal-shift doesn't
    // skip anything.
    {
        int e_idx, j;
        for (e_idx = edit_scene.numentities - 1; e_idx >= 0; e_idx--)
        {
            edit_entity_t *e = &edit_scene.entities[e_idx];
            if (e->numbrushes != 0) continue;
            if (e->classname_idx < 0) continue;
            if (strcmp(e->kv[e->classname_idx].value, "func_group") != 0) continue;
            if (e->kv) free(e->kv);
            if (e->brushes) free(e->brushes);
            for (j = e_idx + 1; j < edit_scene.numentities; j++)
                edit_scene.entities[j - 1] = edit_scene.entities[j];
            edit_scene.numentities--;
        }
    }

    Con_Printf("editor: ungrouped %d brushes back into worldspawn\n", n_moved);
    // Selection is left empty since indices are now stale — user can re-pick.
}

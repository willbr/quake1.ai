// edit_scene.c -- in-memory scene state + selection + lifecycle.

#include "quakedef.h"
#include "edit_scene.h"
#include "editor_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

edit_scene_t edit_scene;

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void Scene_Init(void)
{
    memset(&edit_scene, 0, sizeof(edit_scene));
    edit_scene.sel_entity = -1;
    edit_scene.sel_brush  = -1;
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
    if (edit_scene.entities) { free(edit_scene.entities); edit_scene.entities = NULL; }
    edit_scene.numentities = 0;
    edit_scene.sel_entity  = -1;
    edit_scene.sel_brush   = -1;
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

edit_brush_t *Scene_GetSelectedBrush(void)
{
    edit_entity_t *e;
    if (edit_scene.sel_entity < 0 || edit_scene.sel_entity >= edit_scene.numentities)
        return NULL;
    e = &edit_scene.entities[edit_scene.sel_entity];
    if (edit_scene.sel_brush < 0 || edit_scene.sel_brush >= e->numbrushes)
        return NULL;
    return &e->brushes[edit_scene.sel_brush];
}

edit_entity_t *Scene_GetSelectedEntity(void)
{
    if (edit_scene.sel_entity < 0 || edit_scene.sel_entity >= edit_scene.numentities)
        return NULL;
    return &edit_scene.entities[edit_scene.sel_entity];
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

int Scene_AddCubeBrush(const vec3_t mins, const vec3_t maxs, const char *texname)
{
    int wi;
    edit_entity_t *e;
    edit_brush_t *b;
    const char *tex = texname && texname[0] ? texname : "wbrick1_5";

    // Sanity: maxs must be strictly greater than mins on every axis or the
    // plane intersection will collapse to nothing.
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

    {
        // Three points per face, chosen so cross(p0-p1, p2-p1) gives the
        // outward normal — qbsp's "CCW from outside" convention. The 6 face
        // entries below are (face, p0, p1, p2) where each pX is one of the
        // 8 cube corners written as (x_idx, y_idx, z_idx) with idx 0=mins,
        // 1=maxs.
        static const int verts[6][3][3] = {
            // +X
            { {1,1,1}, {1,1,0}, {1,0,0} },
            // -X
            { {0,0,1}, {0,0,0}, {0,1,0} },
            // +Y
            { {0,1,1}, {0,1,0}, {1,1,0} },
            // -Y
            { {1,0,1}, {1,0,0}, {0,0,0} },
            // +Z
            { {1,1,1}, {1,0,1}, {0,0,1} },
            // -Z
            { {0,1,0}, {0,0,0}, {1,0,0} },
        };
        int f, v;
        const vec_t *axis[2];
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
    }
    b->numplanes = 6;

    Brush_Compile(b);
    if (!b->valid)
    {
        Con_Printf("editor: Scene_AddCubeBrush: compile produced no faces\n");
        return 0;
    }

    e->numbrushes++;
    edit_scene.sel_entity = wi;
    edit_scene.sel_brush  = e->numbrushes - 1;
    return 1;
}

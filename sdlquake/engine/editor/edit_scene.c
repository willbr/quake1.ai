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

// Scene_Revert lives in map_io.c — it re-parses the in-memory snapshot text
// captured at Scene_Load time, so it gives back the load-time scene even
// after the user has saved over the .map on disk.

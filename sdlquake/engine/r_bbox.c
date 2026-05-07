// r_bbox.c -- debug entity bounding-box overlay for the software renderer.
//
// Toggle with the cvar r_drawbboxes (0/1). Walks cl_visedicts each frame
// and rasterises the 12 edges of each entity's model-space bbox into
// vid.buffer using Bresenham. Called from VID_Update before the framebuffer
// blit so the lines appear on top of the 3D scene.

#include <stdlib.h>

#include "quakedef.h"
#include "r_bbox.h"
#include "r_debugdraw.h"

extern float scr_con_current;  // pixel height the dropped-down console covers

// Color encodes what the entity *is*, not just its solid type. Indices verified
// against id1/PAK0.PAK gfx/palette.lmp. Mapping (in priority order):
//   SOLID_NOT (any)            → grey     : non-colliding (gibs, dead bodies)
//   SLIDEBOX + FL_MONSTER      → red      : live enemy
//   SLIDEBOX + FL_CLIENT       → white    : the player
//   SOLID_TRIGGER              → sky blue : pickups, trigger volumes
//   SOLID_BBOX                 → orange   : projectiles (incoming damage)
//   SOLID_BSP                  → yellow   : moving brush (door, plat, button)
//   static entity (torch etc.) → tan      : decoration, no collision/edict
//   anything else              → pink     : unknown — should be rare, signals a bug
#define BBOX_COLOR_DEAD       8     // #7b7b7b mid grey
#define BBOX_COLOR_MONSTER    251   // #ff0000 pure red
#define BBOX_COLOR_PLAYER     254   // #ffffff pure white
#define BBOX_COLOR_TRIGGER    244   // #7fbfff bright sky blue
#define BBOX_COLOR_PROJECTILE 235   // #db7f3b bright orange
#define BBOX_COLOR_BRUSH      192   // #fff31b bright yellow
#define BBOX_COLOR_STATIC     124   // #b7876b warm tan (decoration)
#define BBOX_COLOR_UNKNOWN    144   // #bb739f pink (debug fallback)

// Value is the dither density (opacity) in [0,1]: 0 = off, 0.5 = ~half-opacity
// stipple, 1 = fully opaque. Values >1 are clamped to 1.
cvar_t r_drawbboxes = {"r_drawbboxes", "0"};

void RBBox_Init(void)
{
    Cvar_RegisterVariable(&r_drawbboxes);
}

static int color_for_edict(edict_t *ed)
{
    int solid, flags;

    if (!ed) return BBOX_COLOR_UNKNOWN;
    solid = (int)ed->v.solid;
    flags = (int)ed->v.flags;

    if (solid == SOLID_NOT)            return BBOX_COLOR_DEAD;       // gibs, corpses
    if (solid == SOLID_SLIDEBOX)
    {
        if (flags & FL_MONSTER)        return BBOX_COLOR_MONSTER;    // live enemy
        if (flags & FL_CLIENT)         return BBOX_COLOR_PLAYER;     // the player
        return BBOX_COLOR_PLAYER;                                    // fallback
    }
    if (solid == SOLID_TRIGGER)        return BBOX_COLOR_TRIGGER;    // pickups / triggers
    if (solid == SOLID_BBOX)           return BBOX_COLOR_PROJECTILE; // missiles
    if (solid == SOLID_BSP)            return BBOX_COLOR_BRUSH;      // door / plat / button
    return BBOX_COLOR_UNKNOWN;
}

// Edge index pairs over corner indices; corner bits: bit0=x, bit1=y, bit2=z
// (0 = use mins on that axis, 1 = use maxs).
static const unsigned char bbox_edges[12][2] = {
    {0,1},{2,3},{4,5},{6,7},   // along X
    {0,2},{1,3},{4,6},{5,7},   // along Y
    {0,4},{1,5},{2,6},{3,7},   // along Z
};

static void draw_bbox(const vec3_t mins, const vec3_t maxs, int color)
{
    vec3_t view[8];
    int    c, e;

    for (c = 0; c < 8; c++)
    {
        vec3_t corner;
        corner[0] = (c & 1) ? maxs[0] : mins[0];
        corner[1] = (c & 2) ? maxs[1] : mins[1];
        corner[2] = (c & 4) ? maxs[2] : mins[2];
        RDD_ToView(corner, view[c]);
    }

    // Solid dot at every visible vertex so corners read clearly even when
    // the lines are heavily dithered. Vertices behind the near plane are
    // skipped (no useful screen coord); the lines themselves still clip and
    // draw partials.
    for (c = 0; c < 8; c++)
    {
        if (view[c][2] >= RDD_NEAR_CLIP)
        {
            float vx, vy;
            RDD_Project(view[c], &vx, &vy);
            RDD_DrawSolidPixel((int)vx, (int)vy, color);
        }
    }

    for (e = 0; e < 12; e++)
    {
        int a = bbox_edges[e][0], b = bbox_edges[e][1];
        RDD_DrawLine3D_View(view[a], view[b], color);
    }
}

extern cvar_t chase_active;

void RBBox_Draw(void)
{
    int i;

    if (r_drawbboxes.value <= 0.0f) return;
    if (cls.state != ca_connected) return;
    if (!cl.worldmodel) return;
    if (!vid.buffer) return;
    // Don't overdraw the console or menu — both sit on top of the 3D view.
    if (key_dest != key_game) return;
    if (scr_con_current > 0) return;

    RDD_BeginFrame(r_drawbboxes.value);
    if (!RDD_Visible()) return;

    for (i = 0; i < cl_numvisedicts; i++)
    {
        entity_t *ent = cl_visedicts[i];
        vec3_t mins, maxs;
        edict_t *ed = NULL;
        int color;
        ptrdiff_t off;
        int is_dynamic;

        if (!ent || !ent->model) continue;

        // Static entities (torches etc.) live in cl_static_entities, not
        // cl_entities, and have no server edict. Detect that and treat them
        // as decoration.
        off = ent - cl_entities;
        is_dynamic = (off >= 0 && off < MAX_EDICTS);

        // Prefer the server-side collision bbox (absmin/absmax) when we can
        // find the edict — model->mins/maxs is the geometric bbox and is
        // usually smaller than the entity's setsize() collision bbox.
        if (is_dynamic && sv.active && off > 0 && off < sv.num_edicts)
        {
            ed = EDICT_NUM((int)off);
            if (ed && ed->free) ed = NULL;
        }
        if (ed)
        {
            VectorCopy(ed->v.absmin, mins);
            VectorCopy(ed->v.absmax, maxs);
            color = color_for_edict(ed);
        }
        else
        {
            VectorAdd(ent->origin, ent->model->mins, mins);
            VectorAdd(ent->origin, ent->model->maxs, maxs);
            color = is_dynamic ? BBOX_COLOR_UNKNOWN : BBOX_COLOR_STATIC;
        }

        draw_bbox(mins, maxs, color);
    }

    // The view entity (player) is excluded from cl_visedicts in first-person
    // (cl_main.c skips it unless chase_active). Draw it explicitly so the
    // player can see their own collision box. In chase cam it's already in the
    // list above, so skip to avoid double-drawing.
    if (sv.active && cl.viewentity > 0 && cl.viewentity < sv.num_edicts &&
        !chase_active.value)
    {
        edict_t *ed = EDICT_NUM(cl.viewentity);
        if (ed && !ed->free)
            draw_bbox(ed->v.absmin, ed->v.absmax, color_for_edict(ed));
    }
}

// r_bbox.c -- debug entity bounding-box overlay for the software renderer.
//
// Toggle with the cvar r_drawbboxes (0/1). Walks cl_visedicts each frame
// and rasterises the 12 edges of each entity's model-space bbox into
// vid.buffer using Bresenham. Called from VID_Update before the framebuffer
// blit so the lines appear on top of the 3D scene.

#include <stdlib.h>

#include "quakedef.h"
#include "r_bbox.h"

// Software-renderer view-projection state lives in r_shared.h, but that header
// drags in lots of internal driver state. Forward-declare just what we need.
extern float xscale, yscale, xcenter, ycenter;
extern float scr_con_current;  // pixel height the dropped-down console covers

#define BBOX_NEAR_CLIP 0.01f

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

// 4×4 Bayer ordered-dither matrix, values 0..15. Pixel is written when
// bayer[x%4][y%4] < density*16. Gives a clean stipple at any density.
static const unsigned char bayer4x4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};
static int bayer_threshold;  // density * 16, rounded; 0..16. Set per-frame.

void RBBox_Init(void)
{
    Cvar_RegisterVariable(&r_drawbboxes);
}

// Transform a world-space point into view space (right, up, forward).
static void to_view_space(const vec3_t world, vec3_t out_view)
{
    vec3_t local;
    VectorSubtract(world, r_origin, local);
    out_view[0] = DotProduct(local, vright);
    out_view[1] = DotProduct(local, vup);
    out_view[2] = DotProduct(local, vpn);
}

// Project a view-space point to vid.buffer pixel coords. Caller must ensure
// view[2] >= BBOX_NEAR_CLIP.
static void project_view(const vec3_t view, float *out_sx, float *out_sy)
{
    float inv_z = 1.0f / view[2];
    *out_sx = xcenter + xscale * view[0] * inv_z;
    *out_sy = ycenter - yscale * view[1] * inv_z;
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

static void draw_line(int x0, int y0, int x1, int y1, int color)
{
    // Clip to the 3D viewport (r_refdef.vrect) so lines don't overdraw the HUD.
    const int xmin = r_refdef.vrect.x;
    const int ymin = r_refdef.vrect.y;
    const int xmax = r_refdef.vrect.x + r_refdef.vrect.width;   // exclusive
    const int ymax = r_refdef.vrect.y + r_refdef.vrect.height;  // exclusive
    int dx, dy, sx, sy, err, e2;

    if ((x0 < xmin && x1 < xmin) || (x0 >= xmax && x1 >= xmax) ||
        (y0 < ymin && y1 < ymin) || (y0 >= ymax && y1 >= ymax))
        return;

    dx =  abs(x1 - x0); sx = x0 < x1 ? 1 : -1;
    dy = -abs(y1 - y0); sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;)
    {
        // Bayer ordered dither — vertices are drawn solid by plot_vertex.
        if (x0 >= xmin && x0 < xmax && y0 >= ymin && y0 < ymax &&
            bayer4x4[y0 & 3][x0 & 3] < bayer_threshold)
            vid.buffer[y0 * vid.rowbytes + x0] = (byte)color;
        if (x0 == x1 && y0 == y1) break;
        e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Draw a single solid pixel at a vertex, clipped to the 3D viewport.
// Solid (no dither) so corners stay readable at low r_drawbboxes density.
static void plot_vertex(int x, int y, int color)
{
    if (x >= r_refdef.vrect.x && x < r_refdef.vrect.x + r_refdef.vrect.width &&
        y >= r_refdef.vrect.y && y < r_refdef.vrect.y + r_refdef.vrect.height)
        vid.buffer[y * vid.rowbytes + x] = (byte)color;
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
        to_view_space(corner, view[c]);
    }

    // Solid 2x2 dot at every visible vertex so corners read clearly even when
    // the lines are heavily dithered. Vertices behind the near plane are
    // skipped (no useful screen coord); the lines themselves still clip and
    // draw partials.
    for (c = 0; c < 8; c++)
    {
        if (view[c][2] >= BBOX_NEAR_CLIP)
        {
            float vx, vy;
            project_view(view[c], &vx, &vy);
            plot_vertex((int)vx, (int)vy, color);
        }
    }

    for (e = 0; e < 12; e++)
    {
        int a = bbox_edges[e][0], b = bbox_edges[e][1];
        const float *va = view[a];
        const float *vb = view[b];
        int a_in = va[2] >= BBOX_NEAR_CLIP;
        int b_in = vb[2] >= BBOX_NEAR_CLIP;
        vec3_t ca, cb;  // possibly clipped endpoints in view space
        float sax, say, sbx, sby;

        if (!a_in && !b_in) continue;  // both behind near plane

        if (a_in) { VectorCopy(va, ca); } else {
            // Clip A to the near plane along the line AB.
            float t = (BBOX_NEAR_CLIP - va[2]) / (vb[2] - va[2]);
            ca[0] = va[0] + t * (vb[0] - va[0]);
            ca[1] = va[1] + t * (vb[1] - va[1]);
            ca[2] = BBOX_NEAR_CLIP;
        }
        if (b_in) { VectorCopy(vb, cb); } else {
            float t = (BBOX_NEAR_CLIP - vb[2]) / (va[2] - vb[2]);
            cb[0] = vb[0] + t * (va[0] - vb[0]);
            cb[1] = vb[1] + t * (va[1] - vb[1]);
            cb[2] = BBOX_NEAR_CLIP;
        }

        project_view(ca, &sax, &say);
        project_view(cb, &sbx, &sby);
        draw_line((int)sax, (int)say, (int)sbx, (int)sby, color);
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

    {
        float d = r_drawbboxes.value;
        if (d > 1.0f) d = 1.0f;
        bayer_threshold = (int)(d * 16.0f + 0.5f);
        if (bayer_threshold <= 0) return;       // density rounded to 0 → nothing visible
        if (bayer_threshold > 16) bayer_threshold = 16;
    }

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

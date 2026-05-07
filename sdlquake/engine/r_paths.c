// r_paths.c -- patrol-path debug overlay for the software renderer.
//
// Toggle with the cvar r_drawpaths (0..1 density, like r_drawbboxes).
// r_drawpaths_what is a bitmask: bit 0 = static path_corner network,
// bit 1 = each live monster's current goalentity link. Default 3.
//
// Walks sv.edicts each frame. No PVS culling — debug overlay shows the
// whole network through walls, like r_drawbboxes.

#include <math.h>
#include <string.h>

#include "quakedef.h"
#include "r_debugdraw.h"
#include "r_paths.h"

extern float scr_con_current;

// Sky blue (244) - same as BBOX_COLOR_TRIGGER in r_bbox.c. path_corner is a
// SOLID_TRIGGER so when both overlays are on, trigger boxes and the path
// graph share a colour and read as the same system.
#define PATHS_COLOR_STATIC 244
// Bright lime green (220). Distinct from monster red (251) and trigger sky
// blue (244) so when all three layers compose they stay readable.
#define PATHS_COLOR_LIVE   220

cvar_t r_drawpaths      = {"r_drawpaths",      "0"};
cvar_t r_drawpaths_what = {"r_drawpaths_what", "3"};

void RPaths_Init(void)
{
    Cvar_RegisterVariable(&r_drawpaths);
    Cvar_RegisterVariable(&r_drawpaths_what);
}

// Find the first live edict whose v.targetname matches name. Returns NULL
// if name is NULL/empty or no match. If two corners share a targetname
// (mapper error), the first hit wins — matches the engine's runtime AI.
static edict_t *find_by_targetname(const char *name)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 1; i < sv.num_edicts; i++)
    {
        edict_t *e = EDICT_NUM(i);
        if (e->free) continue;
        if (e->v.targetname && !strcmp(e->v.targetname, name))
            return e;
    }
    return NULL;
}

// Project a world-space point to screen coords. Returns 1 on success
// (point in front of near plane); 0 if behind (caller should skip).
static int project_world(const vec3_t world, float *out_sx, float *out_sy)
{
    vec3_t view;
    RDD_ToView(world, view);
    return RDD_Project(view, out_sx, out_sy);
}

// Draw a 3x3 solid block centred on a screen pixel — bigger than the 1-pixel
// vertex used in r_bbox.c so corner dots stay visible at distance.
static void plot_dot_3x3(int sx, int sy, int color)
{
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            RDD_DrawSolidPixel(sx + dx, sy + dy, color);
}

// Thin world-space wrapper over RDD_DrawLine3D_View — does the world->view
// transform for both endpoints, then hands off to the shared module.
static void draw_line_3d(const vec3_t a_world, const vec3_t b_world, int color)
{
    vec3_t va, vb;
    RDD_ToView(a_world, va);
    RDD_ToView(b_world, vb);
    RDD_DrawLine3D_View(va, vb, color);
}

// Draw a chevron arrowhead pointing from (sx0,sy0) toward (sx1,sy1), placed
// at parametric position t along the segment (0=start, 1=end). Drawn in
// 2D after projection so the on-screen size is independent of edge length.
//
// Arms are LEN pixels long and form a 2*HALF_ANGLE chevron.
static void draw_arrowhead_2d(float sx0, float sy0, float sx1, float sy1,
                              float t, int color)
{
    const float LEN = 6.0f;
    const float COS_HA = 0.9063f;   // cos(25 deg)
    const float SIN_HA = 0.4226f;   // sin(25 deg)
    float dx = sx1 - sx0, dy = sy1 - sy0;
    float len = (float)sqrt(dx*dx + dy*dy);
    float ux, uy;     // unit vector along edge
    float tip_x, tip_y;
    float ax, ay, bx, by;

    if (len < 1.0f) return;          // degenerate
    ux = dx / len; uy = dy / len;
    tip_x = sx0 + dx * t;
    tip_y = sy0 + dy * t;

    // Each arm = tip - LEN * (R(+/-HA) * unit). The arrowhead points along
    // the edge direction, so arms project backward from the tip.
    ax = tip_x - LEN * ( ux * COS_HA + uy * SIN_HA);
    ay = tip_y - LEN * (-ux * SIN_HA + uy * COS_HA);
    bx = tip_x - LEN * ( ux * COS_HA - uy * SIN_HA);
    by = tip_y - LEN * ( ux * SIN_HA + uy * COS_HA);

    RDD_DrawLine2D((int)tip_x, (int)tip_y, (int)ax, (int)ay, color);
    RDD_DrawLine2D((int)tip_x, (int)tip_y, (int)bx, (int)by, color);
}

// Like draw_line_3d but also draws a midpoint arrowhead and a second
// arrowhead 8 px back from the destination. Skips arrowheads if either
// endpoint is behind the near plane (no meaningful screen midpoint).
static void draw_edge_with_arrows(const vec3_t a_world, const vec3_t b_world, int color)
{
    vec3_t va, vb;
    float  sax, say, sbx, sby, dx, dy, len, t_back;

    draw_line_3d(a_world, b_world, color);

    RDD_ToView(a_world, va);
    RDD_ToView(b_world, vb);
    if (va[2] < RDD_NEAR_CLIP || vb[2] < RDD_NEAR_CLIP) return;

    RDD_Project(va, &sax, &say);
    RDD_Project(vb, &sbx, &sby);

    // Midpoint arrowhead.
    draw_arrowhead_2d(sax, say, sbx, sby, 0.5f, color);

    // Second arrowhead 8 px back from the destination tip.
    dx = sbx - sax; dy = sby - say;
    len = (float)sqrt(dx*dx + dy*dy);
    if (len < 16.0f) return;       // edge too short for a second arrow
    t_back = 1.0f - (8.0f / len);
    draw_arrowhead_2d(sax, say, sbx, sby, t_back, color);
}

void RPaths_Draw(void)
{
    int what;

    if (r_drawpaths.value <= 0.0f) return;
    if (cls.state != ca_connected) return;
    if (!cl.worldmodel) return;
    if (!vid.buffer) return;
    if (!sv.active) return;
    if (key_dest != key_game) return;
    if (scr_con_current > 0) return;

    what = (int)r_drawpaths_what.value;
    if (what == 0) return;

    RDD_BeginFrame(r_drawpaths.value);
    if (!RDD_Visible()) return;

    if (what & 1)
    {
        int i;
        for (i = 1; i < sv.num_edicts; i++)
        {
            edict_t *ed = EDICT_NUM(i);
            float    sx, sy;

            if (ed->free) continue;
            if (!ed->v.classname) continue;
            if (strcmp(ed->v.classname, "path_corner") != 0) continue;

            // Corner dot.
            if (project_world(ed->v.origin, &sx, &sy))
                plot_dot_3x3((int)sx, (int)sy, PATHS_COLOR_STATIC);

            // Outgoing edge to the next corner (if this corner has a target).
            if (ed->v.target && ed->v.target[0])
            {
                edict_t *next = find_by_targetname(ed->v.target);
                if (next && next != ed)
                    draw_edge_with_arrows(ed->v.origin, next->v.origin, PATHS_COLOR_STATIC);
            }
        }
    }

    if (what & 2)
    {
        int i;
        for (i = 1; i < sv.num_edicts; i++)
        {
            edict_t *ed = EDICT_NUM(i);
            edict_t *goal;

            if (ed->free) continue;
            if (!((int)ed->v.flags & FL_MONSTER)) continue;
            if (ed->v.health <= 0) continue;       // skip corpses

            goal = ed->v.goalentity;
            if (!goal || goal == sv.edicts) continue;   // null or world
            if (goal == ed) continue;                    // self-loop
            if (goal->free) continue;

            draw_edge_with_arrows(ed->v.origin, goal->v.origin, PATHS_COLOR_LIVE);
        }
    }
}

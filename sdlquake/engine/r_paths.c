// r_paths.c -- patrol-path debug overlay for the software renderer.
//
// Toggle with the cvar r_drawpaths (0..1 density, like r_drawbboxes).
// r_drawpaths_what is a bitmask: bit 0 = static path_corner network,
// bit 1 = each live monster's current goalentity link. Default 3.
//
// Walks sv.edicts each frame. No PVS culling — debug overlay shows the
// whole network through walls, like r_drawbboxes.

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

    // Static graph (bit 0) and live monster links (bit 1) — implemented in
    // later tasks.
    (void)what;
}

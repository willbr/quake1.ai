// sim_light.c -- Light tier (Phase 8 / M5).
//
// Light_TierAt(pos) reads the engine's lightmap sample, applies DLL-side
// "torch extinguished" overrides, and thresholds at 128 -> {0.5 shadowed,
// 1.0 lit}. The renderer's lightmap is never modified; overrides are
// purely a side-table consulted by the AI sense filter so AI vision
// reacts to gust-extinguished torches even though the rendered scene
// hasn't been re-baked.

#include "sim.h"
#include "../game_defs.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

#define LIGHT_THRESHOLD     128.0f
#define LIGHT_TIER_LIT      1.0f
#define LIGHT_TIER_SHADOWED 0.5f
/* Sized to fit every light entity in any Quake map we care about
 * (start.bsp has 267, jam maps go higher; editor uses 1024 in
 * MAX_LIGHT_CANDIDATES for the same reason). Lifted from 64 so the
 * player can extinguish every torch in a map. */
#define MAX_OVERRIDES       1024

typedef struct {
    vec3_t pos;
    float  radius;
    float  delta;     // signed; negative darkens, positive brightens
} light_override_t;

static light_override_t s_overrides[MAX_OVERRIDES];
static int              s_override_count;

void Light_Init(void) {
    s_override_count = 0;
    eng->Cvar_Register("sim_light_debug", "0");
}

void Light_LevelInit(void) {
    s_override_count = 0;
    /* Engine-side gust overrides are also cleared on level change via
     * Lightmap_ClearAll in CL_ClearState; this is a belt-and-braces
     * for the case where Light_LevelInit fires after re-entering a
     * map via 'changelevel' without a full CL_ClearState. */
    if (eng->Lightmap_ClearOwner)
        eng->Lightmap_ClearOwner(2 /* GAMEPLAY */);
}

void Light_AddOverride(const vec3_t pos, float radius, float delta) {
    if (s_override_count >= MAX_OVERRIDES) return;
    light_override_t *o = &s_overrides[s_override_count++];
    o->pos[0] = pos[0];
    o->pos[1] = pos[1];
    o->pos[2] = pos[2];
    o->radius = radius;
    o->delta  = delta;

    /* Mirror the override into the renderer's live lightmap so the
     * player visually sees the room dim (not just the AI sense filter).
     * `delta` here is a signed scalar luminance; broadcast across RGB.
     * Approximation matches the AI side: no shadow occlusion, so walls
     * behind the torch will dim too. The bake remains authoritative
     * for "correct" results. */
    if (eng->Lightmap_AddDelta) {
        vec3_t color = { delta, delta, delta };
        eng->Lightmap_AddDelta(pos, radius, color, 2 /* GAMEPLAY */);
    }
}

float Light_TierAt(const vec3_t pos) {
    if (!eng->Sample_Lightmap) return LIGHT_TIER_LIT;
    int   raw = eng->Sample_Lightmap((float *)pos);
    float v   = (float)raw;
    for (int i = 0; i < s_override_count; i++) {
        light_override_t *o = &s_overrides[i];
        float dx = pos[0] - o->pos[0];
        float dy = pos[1] - o->pos[1];
        float dz = pos[2] - o->pos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > o->radius * o->radius) continue;
        float r  = sqrtf(d2);
        float t  = 1.0f - r / o->radius;
        v += o->delta * t;
    }
    return v >= LIGHT_THRESHOLD ? LIGHT_TIER_LIT : LIGHT_TIER_SHADOWED;
}

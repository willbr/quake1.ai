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
#define MAX_OVERRIDES       64

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
}

void Light_AddOverride(const vec3_t pos, float radius, float delta) {
    if (s_override_count >= MAX_OVERRIDES) return;
    light_override_t *o = &s_overrides[s_override_count++];
    o->pos[0] = pos[0];
    o->pos[1] = pos[1];
    o->pos[2] = pos[2];
    o->radius = radius;
    o->delta  = delta;
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

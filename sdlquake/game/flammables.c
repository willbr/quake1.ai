// M8 / F4 flammables -- oil barrels, breakable props, (re)lightable torches.
// All DLL-side; no engine ABI change (GAME_API_VERSION stays 36).
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "game_types.h"
#include "flammables.h"
#include "sim/sim.h"

extern engine_api_t   *eng;
extern game_globals_t *g;

// Defined in misc.c (de-static'd by this task) and combat.c.
extern void barrel_explode(edict_t *self);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);

void Flammables_Init(void) {
    // Reused world models -- precache so map placement and debug spawns work.
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    eng->PrecacheSound("weapons/ax1.wav");   // breakable "crack" placeholder
}

// ---------------------------------------------------------------------------
// Oil barrel: a misc_explobox whose death spills oil first. barrel_explode's
// T_RadiusDamage(160) then lights the spill (combat.c:478) -> burning pool.
// ---------------------------------------------------------------------------
static void oilbarrel_explode(edict_t *self) {
    g->self = self;
    vec3_t c = { self->v.origin[0], self->v.origin[1], self->v.origin[2] };
    Fire_AddOil(c, 72.0f, 2.0f);                          // central pool
    for (int i = 0; i < 6; i++) {                         // surrounding ring
        float a = (float)i / 6.0f * 6.2831853f;
        vec3_t p = { c[0] + (float)cos(a) * 80.0f,
                     c[1] + (float)sin(a) * 80.0f,
                     c[2] };
        Fire_AddOil(p, 48.0f, 1.0f);
    }
    barrel_explode(self);   // flips takedamage/classname, T_RadiusDamage(160)
                            // (lights the oil), boom sound, TE_EXPLOSION, BecomeExplosion
}

void spawn_misc_oilbarrel(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->SV_SetModel(e, "maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    e->v.health     = 20;
    e->v.th_die     = oilbarrel_explode;
    e->v.takedamage = DAMAGE_AIM;
    e->v.origin[2] += 2;
    float oldz = e->v.origin[2];
    eng->SV_DropToFloor(e);
    if (oldz - e->v.origin[2] > 250) {
        eng->Con_DPrintf("item fell out of level\n");
        eng->ED_Free(e);
    }
}

// ---------------------------------------------------------------------------
// Debug spawn (impulse 213): drop an oil barrel ~96u in front of the player.
// ---------------------------------------------------------------------------
void Flammables_DebugSpawnBarrel(edict_t *player) {
    eng->MakeVectors(player->v.v_angle);
    edict_t *e = eng->ED_Alloc();
    e->v.origin[0] = player->v.origin[0] + g->v_forward[0] * 96.0f;
    e->v.origin[1] = player->v.origin[1] + g->v_forward[1] * 96.0f;
    e->v.origin[2] = player->v.origin[2];
    e->v.classname = "misc_oilbarrel";
    spawn_misc_oilbarrel(e);
    eng->Con_Print("fire: spawned oil barrel ahead\n");
}

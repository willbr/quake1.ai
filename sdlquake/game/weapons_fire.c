// M8 / F3 fire weapons -- oil gun + flamethrower.
// Built on the Phase 6 weapon2 selector; held-fire cadence modeled on the
// lightning gun (player_light1/2_think, weapons.c:1485 + player.c:294-318).
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "weapons_fire.h"
#include "weapons_phase6.h"
#include "sim/sim.h"

extern engine_api_t  *eng;
extern game_globals_t *g;

// Non-static game functions defined elsewhere (called cross-file already).
extern void  player_run(edict_t *self);
extern float W_BestWeapon(void);
extern void  W_SetCurrentAmmo(void);

#define FR_FIRE_BODY 113   // FR_SHOTATT1 -- generic shooting stance (player.c:39)

void WeaponsFire_Init(void) {
    // Flamethrower cone + ignition.
    eng->Cvar_Register("fire_flame_range", "220");   // cone length (units)
    eng->Cvar_Register("fire_flame_cone",  "25");    // cone half-angle (degrees)
    eng->Cvar_Register("fire_flame_secs",  "3");     // ignite duration on a direct hit
    eng->Cvar_Register("fire_flame_tick",  "0.1");   // think interval = fire/drain rate
    eng->Cvar_Register("fire_flame_cost",  "1");     // cells drained per tick
    // Oil gun.
    eng->Cvar_Register("fire_oilgun_tick", "0.12");  // think interval
    eng->Cvar_Register("fire_oilgun_cost", "1");     // cells drained per deposit
    // (Flamethrower ignite DPS reuses the existing fire_dps cvar.)
}

// impulse 212: grant both fire weapons, top up cells, select the oil gun.
void Fire_GiveWeapons(edict_t *self) {
    self->v.items2 = (float)((int)self->v.items2 | IT2_OILGUN | IT2_FLAMETHROWER);
    if (self->v.ammo_cells < 200) self->v.ammo_cells = 200;
    self->v.weapon  = 0;
    self->v.weapon2 = (float)IT2_OILGUN;
    W_SetCurrentAmmo();
    eng->Con_Print("fire: granted oil gun + flamethrower (200 cells)\n");
}

// --- Fire entrypoints (stubs until Tasks 2-4) ---------------------------------
void W_FireOilGun(void) {
    edict_t *self = g->self;
    self->v.attack_finished = g->time + 0.2f;
}

void W_FireFlamethrower(void) {
    edict_t *self = g->self;
    self->v.attack_finished = g->time + 0.2f;
}

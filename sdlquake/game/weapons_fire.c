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

// --- Oil gun implementation ---------------------------------------------------

// Emit a STIM_SOUND so the immersive-sim AI hears the player firing
// (mirrors weapons.c::emit_weapon_sound, which is static to that file).
static void weaponsfire_sound_stim(edict_t *shooter, float intensity) {
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_SOUND;
    s.origin[0]    = shooter->v.origin[0];
    s.origin[1]    = shooter->v.origin[1];
    s.origin[2]    = shooter->v.origin[2];
    s.intensity    = intensity;
    s.source_edict = eng->ED_GetNum(shooter);
    Stim_Emit(&s);
}

// One oil-gun tick. Returns 0 when out of fuel (caller ends the loop).
static int OilGun_DoFire(edict_t *self) {
    int cost = (int)eng->Cvar_VariableValue("fire_oilgun_cost");
    if (self->v.ammo_cells < cost) {
        self->v.weapon2 = 0;                  // out of fuel: drop to a stock weapon
        self->v.weapon  = W_BestWeapon();
        W_SetCurrentAmmo();
        return 0;
    }

    // Deposit oil at the crosshair floor-trace. Fire_PourOil also coats any
    // monster standing in the fresh patch (Fire_AddOil's coat loop). Only spend
    // fuel when oil actually lands (aiming at the sky deposits nothing).
    if (Fire_PourOil(self)) {
        self->v.ammo_cells -= cost;
        self->v.currentammo = self->v.ammo_cells;
    }

    if (self->v.t_width < g->time) {          // throttle the looping spray sound
        eng->SV_StartSound(self, CHAN_WEAPON, "misc/water1.wav", 1, ATTN_NORM);
        self->v.t_width = g->time + 0.3f;
    }
    weaponsfire_sound_stim(self, 0.4f);
    return 1;
}

// Self-perpetuating held-fire loop (modeled on player_light1/2_think). Runs
// while +attack is held and the oil gun stays selected.
static void oilgun_think(edict_t *self) {
    g->self = self;
    if (!self->v.button0 || (int)self->v.weapon2 != IT2_OILGUN) {
        player_run(self);
        return;
    }
    self->v.frame = FR_FIRE_BODY;
    self->v.weaponframe++;
    if (self->v.weaponframe > 4) self->v.weaponframe = 1;
    if (!OilGun_DoFire(self)) {               // out of fuel -> stop
        player_run(self);
        return;
    }
    self->v.nextthink       = g->time + eng->Cvar_VariableValue("fire_oilgun_tick");
    self->v.think           = oilgun_think;
    self->v.attack_finished = g->time + 0.2f; // keep W_WeaponFrame from re-entering W_Attack
}

void W_FireOilGun(void) {
    edict_t *self = g->self;
    if (self->v.ammo_cells < 1) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
        self->v.attack_finished = g->time + 0.5f;
        return;
    }
    self->v.attack_finished = g->time + 0.1f; // hand off to the think loop
    oilgun_think(self);
}

void W_FireFlamethrower(void) {
    edict_t *self = g->self;
    self->v.attack_finished = g->time + 0.2f;
}

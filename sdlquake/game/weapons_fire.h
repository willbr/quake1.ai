#ifndef WEAPONS_FIRE_H
#define WEAPONS_FIRE_H

#include "game_api.h"

// M8 / F3 fire weapons: oil gun + flamethrower, dispatched through the Phase 6
// weapon2 selector (IT2_OILGUN / IT2_FLAMETHROWER). All logic is DLL-side and
// uses only existing engine + fire/oil API -- no ABI bump (GAME_API_VERSION 36).

void WeaponsFire_Init(void);        // register cvars; call once from W_Precache

void W_FireOilGun(void);            // dispatched from W_Attack_Phase6 (weapon2 == IT2_OILGUN)
void W_FireFlamethrower(void);      // dispatched from W_Attack_Phase6 (weapon2 == IT2_FLAMETHROWER)

void Fire_GiveWeapons(edict_t *self);   // impulse 212 cheat: grant both + cells, select oil gun

// World pickups (Task 5) -- registered in spawn.c's s_spawns[] table.
void spawn_weapon_oilgun(edict_t *e);
void spawn_weapon_flamethrower(edict_t *e);

#endif // WEAPONS_FIRE_H

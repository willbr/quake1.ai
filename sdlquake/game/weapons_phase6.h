// weapons_phase6.h -- Wolf3D + Doom1 weapon module declarations.
//
// All Phase 6 fire functions, dispatch helpers, animation chain entry points,
// pickup/precache routines, and impulse mapping live in weapons_phase6.c,
// player_phase6.c, and items_phase6.c. The main weapons.c / player.c /
// items.c only need to call these functions and forward to them when the
// player has a Phase 6 weapon active (self->v.weapon2 != 0).

#ifndef WEAPONS_PHASE6_H
#define WEAPONS_PHASE6_H

#include "game_api.h"
#include "game_types.h"

// ---------------------------------------------------------------------------
// Module init / per-level setup
// ---------------------------------------------------------------------------
void Phase6_PrecacheCommon (void);     // call from worldspawn

// ---------------------------------------------------------------------------
// Dispatch from main weapons.c W_Attack / W_SetCurrentAmmo
// ---------------------------------------------------------------------------
void W_Attack_Phase6           (void);                         // active weapon = self->v.weapon2
void W_SetCurrentAmmo_Phase6   (int it2_flag);                 // sets weaponmodel + currentammo

// ---------------------------------------------------------------------------
// Per-weapon fire functions (called by W_Attack_Phase6 dispatcher)
// ---------------------------------------------------------------------------
void W_FirePhase6_DoomFist      (void);
void W_FirePhase6_DoomPistol    (void);
void W_FirePhase6_DoomShotgun   (void);
void W_FirePhase6_DoomChaingun  (void);
void W_FirePhase6_DoomRocket    (void);
void W_FirePhase6_DoomChainsaw  (void);
void W_FirePhase6_WolfKnife     (void);
void W_FirePhase6_WolfPistol    (void);
void W_FirePhase6_WolfMG        (void);
void W_FirePhase6_WolfChaingun  (void);

// ---------------------------------------------------------------------------
// Impulse mapping (12..21 → IT2_*) + give-all cheat (impulse 100)
// ---------------------------------------------------------------------------
void Phase6_ChangeWeapon (int impulse);
void Phase6_CheatGiveAll (void);

// ---------------------------------------------------------------------------
// Animation chains (declared so weapons.c can fire-trigger them and
// player.c's extern block can forward-declare without circular includes).
// ---------------------------------------------------------------------------
void player_doompistol1 (edict_t *self);

#endif // WEAPONS_PHASE6_H

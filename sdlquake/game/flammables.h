// M8 / F4 flammables -- oil barrels, breakable props, (re)lightable torches.
#ifndef FLAMMABLES_H
#define FLAMMABLES_H

#include "game_types.h"

void Flammables_Init(void);   // precache F4 assets + register F4 cvars

// World-pickup / map spawn functions (registered in spawn.c).
void spawn_misc_oilbarrel(edict_t *e);
void spawn_func_breakable(edict_t *e);
void spawn_misc_breakable(edict_t *e);

// Torch interaction (live-edict flame entities). Both self-check classname +
// current lit state and no-op if the edict is not an (extinguished/lit) torch.
void Torch_Extinguish(edict_t *t, edict_t *source);   // hide flame, darken, stim
void Torch_Relight(edict_t *t, edict_t *source);       // restore flame, brighten, stim

// Debug impulse hooks (wired in weapons.c ImpulseCommands).
void Flammables_DebugSpawnBarrel(edict_t *player);     // impulse 213
void Flammables_DebugSpawnBreakable(edict_t *player);  // impulse 214
void Flammables_DebugToggleNearestTorch(edict_t *player); // impulse 215

#endif // FLAMMABLES_H

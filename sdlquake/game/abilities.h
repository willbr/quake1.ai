// abilities.h -- Player Blink + Gust (Phase 8 / M3).
// All state is module-static in abilities.c; we expose only lifecycle hooks
// and the grate registry used by spawn.c.

#ifndef ABILITIES_H
#define ABILITIES_H

#include "game_api.h"

void Abilities_Init(void);                  // once on game-DLL init
void Abilities_LevelInit(void);             // each map load
void Abilities_Frame(edict_t *client);      // called from PlayerPostThink

// spawn.c calls this for every func_grate entity it spawns. Blink validity
// traces temporarily disable these so the player can pass through them.
void Abilities_RegisterGrate(edict_t *e);

#endif // ABILITIES_H

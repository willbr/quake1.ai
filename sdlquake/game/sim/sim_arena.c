// sim_arena.c -- Procedural test arena spawned via the `sim_arena_go` cvar latch.

#include "sim.h"
#include "../game_defs.h"
#include <stdio.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void game_entity_spawn(edict_t *e, const char *classname);

void Sim_Arena_Init(void) {
    eng->Cvar_Register("sim_arena_go", "0");
}

void Sim_Arena_Spawn(void) {
    // Find the player edict (entity 1 in single-player).
    edict_t *player = 0;
    for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
        if (eng->ED_GetNum(cur) == 1) { player = cur; break; }
    }
    if (!player) { eng->Con_Print("sim_arena: no player\n"); return; }

    vec3_t base = {
        player->v.origin[0],
        player->v.origin[1],
        player->v.origin[2]
    };

    // 4 patrol nodes in a 512-unit square around the player.
    vec3_t nodes[4] = {
        { base[0] + 256, base[1] + 256, base[2] },
        { base[0] - 256, base[1] + 256, base[2] },
        { base[0] - 256, base[1] - 256, base[2] },
        { base[0] + 256, base[1] - 256, base[2] },
    };

    for (int i = 0; i < 4; i++) {
        edict_t *node = eng->ED_Alloc();
        node->v.solid    = SOLID_NOT;
        node->v.movetype = MOVETYPE_NONE;
        eng->SV_SetOrigin(node, nodes[i]);
        Sim_Patrol_RegisterArenaNode(/*route=*/0, i, node);
        Sim_Patrol_RegisterNode(node);
    }

    // Spawn 2 soldiers assigned to patrol route 0.
    for (int i = 0; i < 2; i++) {
        edict_t *m = eng->ED_Alloc();
        eng->SV_SetOrigin(m, nodes[i * 2]);
        game_entity_spawn(m, "monster_army");
        ai_brain_t *b = Sim_AI_RegisterMonster(m);
        if (b) {
            b->patrol_route_id = 0;
            b->patrol_node_idx = i * 2;
        }
    }
    eng->Con_Print("sim_arena: spawned 4 patrol nodes + 2 soldiers\n");
}

void Sim_Arena_Poll(void) {
    if (eng->Cvar_VariableValue("sim_arena_go") > 0.5f) {
        eng->Cvar_SetValue("sim_arena_go", 0.0f);
        Sim_Arena_Spawn();
    }
}

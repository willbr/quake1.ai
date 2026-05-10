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

    // 4 patrol nodes in a 256-unit square around the player.
    // Use a probe entity to drop each candidate to the floor and skip
    // positions in water/slime/lava or that don't land on solid ground.
    static const float offsets[4][2] = {
        { 128,  128 }, { -128,  128 },
        {-128, -128 }, {  128, -128 },
    };

    edict_t *probe = eng->ED_Alloc();
    probe->v.movetype = MOVETYPE_STEP;
    probe->v.solid    = SOLID_SLIDEBOX;
    eng->SV_SetSize(probe, (vec3_t){-16,-16,-24}, (vec3_t){16,16,32});

    int node_count = 0;
    for (int i = 0; i < 4; i++) {
        vec3_t candidate = {
            base[0] + offsets[i][0],
            base[1] + offsets[i][1],
            base[2],
        };
        eng->SV_SetOrigin(probe, candidate);
        if (!eng->SV_DropToFloor(probe)) continue;             // no solid below
        int contents = eng->SV_PointContents(probe->v.origin);
        if (contents <= -3) continue;                          // water/slime/lava

        edict_t *node = eng->ED_Alloc();
        node->v.solid    = SOLID_NOT;
        node->v.movetype = MOVETYPE_NONE;
        eng->SV_SetOrigin(node, probe->v.origin);
        Sim_Patrol_RegisterArenaNode(0, node_count, node);
        Sim_Patrol_RegisterNode(node);
        node_count++;
    }
    eng->ED_Free(probe);

    if (node_count < 2) {
        eng->Con_Print("sim_arena: too few valid nodes, aborting\n");
        return;
    }

    // Spawn 2 soldiers, one at node 0 and one at node 1.
    int spawned = 0;
    for (int i = 0; i < 2 && i < node_count; i++) {
        edict_t *node_e = Sim_Patrol_FindArenaNode(0, i);
        if (!node_e) continue;
        edict_t *m = eng->ED_Alloc();
        eng->SV_SetOrigin(m, node_e->v.origin);
        game_entity_spawn(m, "monster_army");
        ai_brain_t *b = Sim_AI_RegisterMonster(m);
        if (b) {
            b->patrol_route_id = 0;
            b->patrol_node_idx = i;
        }
        spawned++;
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "sim_arena: %d nodes, %d soldiers\n", node_count, spawned);
    eng->Con_Print(buf);
}

void Sim_Arena_Poll(void) {
    if (eng->Cvar_VariableValue("sim_arena_go") > 0.5f) {
        eng->Cvar_SetValue("sim_arena_go", 0.0f);
        Sim_Arena_Spawn();
    }
}

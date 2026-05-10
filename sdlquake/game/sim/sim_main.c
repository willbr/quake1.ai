// sim_main.c -- Top-level lifecycle dispatcher for the sim layer.

#include "sim.h"
#include <string.h>

extern game_globals_t *g;

void Sim_Init(void) {
    Stim_Init();
    Sim_AI_Init();
    Sim_Nav_Init();
    Sim_Arena_Init();
}

void Sim_LevelInit(const char *mapname) {
    Stim_LevelInit();
    Sim_AI_LevelInit();
    Sim_Nav_LevelInit(mapname);
    Sim_Patrol_Resolve();
}

void Sim_Frame(void) {
    // game_api_t has no level-init hook, so detect level changes here.
    // Two triggers: map name changed, or g->time went backward (same map
    // reloaded after disconnect without changing the name).
    static char  s_current_map[64];
    static float s_last_time = -1.0f;

    int time_reset  = (g->time < s_last_time && s_last_time > 1.0f);
    int map_changed = (g->mapname && strcmp(g->mapname, s_current_map) != 0);
    s_last_time = g->time;

    if (time_reset || map_changed) {
        if (g->mapname) {
            strncpy(s_current_map, g->mapname, sizeof(s_current_map) - 1);
            s_current_map[sizeof(s_current_map) - 1] = '\0';
        }
        Sim_LevelInit(s_current_map);
    }

    Sim_AI_Frame();
    Sim_Nav_Frame();
    Sim_Arena_Poll();
}

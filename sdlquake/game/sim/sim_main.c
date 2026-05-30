// sim_main.c -- Top-level lifecycle dispatcher for the sim layer.

#include "sim.h"
#include "../abilities.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

int Sim_DebugZTest(const char *cvar_name) {
    float v = eng->Cvar_VariableValue(cvar_name);
    if (v <= 0.0f) return -1;
    return (v < 1.5f) ? 1 : 0;   // 1 -> ztested, 2+ -> xray
}

void Sim_Init(void) {
    Stim_Init();
    Sim_AI_Init();
    Sim_Nav_Init();
    Wind_Init();
    Light_Init();
    Fire_Init();
    Sim_Arena_Init();
}

void Sim_LevelInit(const char *mapname) {
    Stim_LevelInit();
    Sim_AI_LevelInit();
    Sim_Nav_LevelInit(mapname);
    Sim_Patrol_Resolve();
    Wind_LevelInit();
    Light_LevelInit();
    Fire_LevelInit();
    Sim_Retrofit_LevelInit();
    Abilities_LevelInit();
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

    SIM_PERF("Sim_Retrofit") Sim_Retrofit_Frame();
    SIM_PERF("Fire")         Fire_Frame();
    SIM_PERF("Sim_AI")       Sim_AI_Frame();
    SIM_PERF("Wind")         Wind_Frame();
    // Sim_Nav_Frame is now driven by game_api->debug_draw_overlays so the
    // navmesh overlay stays visible while the editor / engine has paused
    // the sim.
    SIM_PERF("Sim_Arena")    Sim_Arena_Poll();
}

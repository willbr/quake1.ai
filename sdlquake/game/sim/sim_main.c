// sim_main.c -- Top-level lifecycle dispatcher for the sim layer.

#include "sim.h"

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
    Sim_AI_Frame();
    Sim_Arena_Poll();
}

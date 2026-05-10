// sim_ai.c -- AI brain side-table, sense filter, FSM.
// State for each monster lives in s_brains[edict_num], not in edict_t.

#include "sim.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static ai_brain_t s_brains[SIM_MAX_BRAINS];

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_AI_Init(void) {
    memset(s_brains, 0, sizeof(s_brains));
}

void Sim_AI_LevelInit(void) {
    memset(s_brains, 0, sizeof(s_brains));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
ai_brain_t *Sim_AI_GetBrain(edict_t *e) {
    if (!e) return 0;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= SIM_MAX_BRAINS) return 0;
    if (!s_brains[n].in_use) return 0;
    return &s_brains[n];
}

ai_brain_t *Sim_AI_RegisterMonster(edict_t *e) {
    if (!e) return 0;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= SIM_MAX_BRAINS) return 0;
    ai_brain_t *b = &s_brains[n];
    if (b->in_use) return b;
    memset(b, 0, sizeof(*b));
    b->in_use             = 1;
    b->edict_num          = n;
    b->state              = AI_IDLE;
    b->state_entered_time = g->time;
    b->target_edict       = -1;
    b->patrol_route_id    = -1;
    b->sense_sight_range  = 1024.0f;
    b->sense_hearing_mult = 1.0f;
    b->next_tick_time     = g->time;
    return b;
}

void Sim_AI_UnregisterByEdictNum(int n) {
    if (n < 0 || n >= SIM_MAX_BRAINS) return;
    s_brains[n].in_use = 0;
}

// ---------------------------------------------------------------------------
// Iteration (for the imgui panel)
// ---------------------------------------------------------------------------
ai_brain_t *Sim_AI_IterFirst(void) {
    for (int i = 0; i < SIM_MAX_BRAINS; i++)
        if (s_brains[i].in_use) return &s_brains[i];
    return 0;
}

ai_brain_t *Sim_AI_IterNext(ai_brain_t *prev) {
    if (!prev) return 0;
    int i = (int)(prev - s_brains) + 1;
    for (; i < SIM_MAX_BRAINS; i++)
        if (s_brains[i].in_use) return &s_brains[i];
    return 0;
}

// ---------------------------------------------------------------------------
// Frame tick (no-op until Task 10 adds sense filter)
// ---------------------------------------------------------------------------
void Sim_AI_Frame(void) {}

// ---------------------------------------------------------------------------
// Patrol routes (stubs until Task 13)
// ---------------------------------------------------------------------------
void Sim_Patrol_RegisterNode(edict_t *e) { (void)e; }
void Sim_Patrol_Resolve(void) {}
edict_t *Sim_Patrol_FindByTargetname(const char *n) { (void)n; return 0; }

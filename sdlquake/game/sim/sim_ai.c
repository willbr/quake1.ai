#include "sim.h"
void        Sim_AI_Init(void) {}
void        Sim_AI_LevelInit(void) {}
void        Sim_AI_Frame(void) {}
ai_brain_t *Sim_AI_GetBrain(edict_t *e) { (void)e; return 0; }
ai_brain_t *Sim_AI_RegisterMonster(edict_t *e) { (void)e; return 0; }
void        Sim_AI_UnregisterByEdictNum(int n) { (void)n; }
ai_brain_t *Sim_AI_IterFirst(void) { return 0; }
ai_brain_t *Sim_AI_IterNext(ai_brain_t *p) { (void)p; return 0; }
void        Sim_Patrol_RegisterNode(edict_t *e) { (void)e; }
void        Sim_Patrol_Resolve(void) {}
edict_t    *Sim_Patrol_FindByTargetname(const char *n) { (void)n; return 0; }

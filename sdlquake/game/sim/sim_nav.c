#include "sim.h"
void           Sim_Nav_Init(void) {}
void           Sim_Nav_LevelInit(const char *m) { (void)m; }
int            Sim_Nav_IsReady(void) { return 0; }
sim_navmesh_t *Sim_Nav_Get(void) { return 0; }
int Sim_Nav_PathTo(const vec3_t a, const vec3_t b, vec3_t *o, int n) {
    (void)a; (void)b; (void)o; (void)n; return 0;
}

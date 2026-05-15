// sim_retrofit.c -- Phase 8 / M6.
//
// Runs once per level after the navmesh is ready: walks every registered
// AI brain (monsters that called Sim_AI_RegisterMonster) and assigns it a
// short patrol route built from nearby navmesh points. The result is that
// vanilla id1 maps -- which never had info_patrol_node entities placed --
// get behaviourally-sensible patrols based on level geometry, so a grunt
// in IDLE state no longer just stands still.
//
// Implementation notes:
// * Patrol nodes are synthesised as info_null edicts at chosen positions
//   and registered with Sim_Patrol_RegisterArenaNode using a per-monster
//   route_id (the monster's edict number, which is unique).
// * Nodes are chosen by scanning all navmesh points and keeping the 4
//   closest that lie within RETROFIT_RANGE units. If fewer than 2 are
//   found, the monster stays patrol-less (the M2 stand-and-sweep
//   fallback covers the case).

#include "sim.h"
#include "../game_defs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

#define RETROFIT_RANGE       512.0f
#define RETROFIT_NODES_MAX   4
#define RETROFIT_NODES_MIN   2

typedef struct sim_navmesh_s sim_navmesh_t;
typedef struct { float pos[3]; int kind; } sim_nav_point_t;

// Hand-rolled access to the navmesh internals -- we only need the point
// array and count. Keep this in sync with sim_nav.c's definitions; it is
// the smallest surface that doesn't require leaking the type.
struct sim_navmesh_s {
    sim_nav_point_t *points;
    int              point_count;
    void            *edges;
    int              edge_count;
    int             *adj_offsets;
    int             *adj;
};

void Sim_Retrofit_LevelInit(void) {
    // Nothing to reset per-level: assignment is tracked on the brain itself
    // (b->patrol_route_id), and Sim_AI_LevelInit zeroes those.
}

static int score_nearest(const float monster[3], const sim_nav_point_t *pts,
                         int n, int chosen[RETROFIT_NODES_MAX], float dists[RETROFIT_NODES_MAX])
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        float dx = pts[i].pos[0] - monster[0];
        float dy = pts[i].pos[1] - monster[1];
        float dz = pts[i].pos[2] - monster[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > RETROFIT_RANGE * RETROFIT_RANGE) continue;
        // Insertion sort into the top-RETROFIT_NODES_MAX bucket.
        int insert_at = count;
        for (int k = 0; k < count; k++) {
            if (d2 < dists[k]) { insert_at = k; break; }
        }
        if (insert_at >= RETROFIT_NODES_MAX) continue;
        int last = (count < RETROFIT_NODES_MAX) ? count : RETROFIT_NODES_MAX - 1;
        for (int k = last; k > insert_at; k--) {
            chosen[k] = chosen[k-1];
            dists[k]  = dists[k-1];
        }
        chosen[insert_at] = i;
        dists[insert_at]  = d2;
        if (count < RETROFIT_NODES_MAX) count++;
    }
    return count;
}

static edict_t *spawn_synthetic_node(const float pos[3]) {
    edict_t *e = eng->ED_Alloc();
    if (!e) return 0;
    e->v.classname = "info_patrol_node";
    e->v.solid     = SOLID_NOT;
    e->v.movetype  = MOVETYPE_NONE;
    vec3_t p = { pos[0], pos[1], pos[2] };
    eng->SV_SetOrigin(e, p);
    return e;
}

void Sim_Retrofit_Frame(void) {
    // Run every frame: monsters call Sim_AI_RegisterMonster from their
    // deferred *_start_go thinks (fired 0–0.5 s after spawn), so a single
    // pass at level-load would always see an empty brain table. Walking
    // every frame is cheap because brains with patrol_route_id >= 0 short
    // out before the inner edict walk.
    if (!Sim_Nav_IsReady()) return;
    sim_navmesh_t *mesh = Sim_Nav_Get();
    if (!mesh || mesh->point_count < RETROFIT_NODES_MIN) return;

    int assigned = 0;
    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (b->patrol_route_id >= 0) continue;   // already has a route

        // Walk the live edict list for this brain's edict_num.
        edict_t *me = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == b->edict_num) { me = cur; break; }
        }
        if (!me) continue;

        int   chosen[RETROFIT_NODES_MAX];
        float dists[RETROFIT_NODES_MAX] = {0};
        int   n = score_nearest(me->v.origin, mesh->points, mesh->point_count,
                                chosen, dists);
        if (n < RETROFIT_NODES_MIN) continue;

        int route_id = b->edict_num;
        for (int i = 0; i < n; i++) {
            edict_t *node = spawn_synthetic_node(mesh->points[chosen[i]].pos);
            if (!node) break;
            Sim_Patrol_RegisterArenaNode(route_id, i, node);
        }
        b->patrol_route_id = route_id;
        b->patrol_node_idx = 0;
        assigned++;
    }

    if (assigned > 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "sim_retrofit: assigned patrols to %d monsters\n", assigned);
        eng->Con_Print(buf);
    }
}

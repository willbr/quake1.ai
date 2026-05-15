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
// * If the monster already has a BSP-placed path_corner chain (its
//   v.target wired to a chain of path_corners by the level author),
//   that chain is used directly -- no synthetic nodes are created. In
//   vanilla Quake those chains are dead-on-arrival because of the
//   well-known walkmonster_start_go bug (th_stand is unconditionally
//   called after th_walk, leaving the monster frozen in stand mode);
//   our brain reanimates them.
// * Otherwise: patrol nodes are synthesised as info_null edicts at
//   chosen positions and registered with Sim_Patrol_RegisterArenaNode
//   using a per-monster route_id (the monster's edict number).
//   Selection is farthest-first within RETROFIT_RANGE of the monster:
//   anchor = nearest navmesh point, then greedily pick K-1 more that
//   maximise the minimum distance to the already-chosen set. This gives
//   a patrol that spans the available area regardless of navmesh density
//   (the old "nearest 4" strategy degenerated to a tight cluster on dense
//   meshes). The 4 chosen nodes are then reordered into the shortest of
//   the 3 unique Hamilton cycles so the traversal doesn't zigzag.

#include "sim.h"
#include "../game_defs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

#define RETROFIT_RANGE       768.0f
#define RETROFIT_NODES_MAX   4
#define RETROFIT_NODES_MIN   2
#define RETROFIT_MIN_SPACING 96.0f   // skip points within this of an already-chosen one
#define RETROFIT_BFS_HOPS    14      // max graph hops from anchor when seeding candidates
#define RETROFIT_MAX_NODES   8192    // matches sim_nav.c's MAX_NODES; scratch-buffer cap
#define RETROFIT_CHAIN_MAX   32      // cap on path_corner chain length we'll follow

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

static float d2_between(const sim_nav_point_t *a, const sim_nav_point_t *b) {
    float dx = a->pos[0] - b->pos[0];
    float dy = a->pos[1] - b->pos[1];
    float dz = a->pos[2] - b->pos[2];
    return dx*dx + dy*dy + dz*dz;
}

static float d2_to_pos(const sim_nav_point_t *p, const float pos[3]) {
    float dx = p->pos[0] - pos[0];
    float dy = p->pos[1] - pos[1];
    float dz = p->pos[2] - pos[2];
    return dx*dx + dy*dy + dz*dz;
}

// Farthest-first selection over a graph-connected candidate set.
// Step 1: anchor = navmesh point nearest the monster.
// Step 2: BFS on the navmesh adjacency from the anchor, bounded by
//         RETROFIT_BFS_HOPS, gathering reachable points that lie within
//         RETROFIT_RANGE of the monster. The BFS guarantees every
//         candidate is connected to the anchor through actual navmesh
//         edges -- so legs between them won't cross walls or gaps.
// Step 3: farthest-first within the reached set, with a min-spacing
//         filter to avoid duplicating near-coincident seats.
// Returns count placed in chosen[] (up to RETROFIT_NODES_MAX).
static int score_spread_bfs(const float monster[3], sim_navmesh_t *mesh,
                            int chosen[RETROFIT_NODES_MAX])
{
    const int   N        = mesh->point_count;
    const float range2   = RETROFIT_RANGE * RETROFIT_RANGE;
    const float spacing2 = RETROFIT_MIN_SPACING * RETROFIT_MIN_SPACING;
    if (N <= 0 || N > RETROFIT_MAX_NODES) return 0;

    // Anchor: nearest navmesh point (no range gate -- the monster might
    // have spawned just outside the range but the anchor still grounds
    // the BFS).
    int   anchor = -1;
    float best   = 1e30f;
    for (int i = 0; i < N; i++) {
        float d2 = d2_to_pos(&mesh->points[i], monster);
        if (d2 < best) { best = d2; anchor = i; }
    }
    if (anchor < 0 || best > range2) return 0;

    // BFS scratch -- static so we don't churn the heap. Sized to the
    // navmesh cap.
    static unsigned char visited[RETROFIT_MAX_NODES];
    static unsigned char hops   [RETROFIT_MAX_NODES];
    static int           queue  [RETROFIT_MAX_NODES];
    static int           reached[RETROFIT_MAX_NODES];
    memset(visited, 0, N);

    int qhead = 0, qtail = 0;
    int reached_n = 0;
    queue[qtail++]       = anchor;
    visited[anchor]      = 1;
    hops[anchor]         = 0;
    reached[reached_n++] = anchor;

    while (qhead < qtail) {
        int cur = queue[qhead++];
        if (hops[cur] >= RETROFIT_BFS_HOPS) continue;
        int o = mesh->adj_offsets[cur];
        int e = mesh->adj_offsets[cur + 1];
        for (int k = o; k < e; k++) {
            int nb = mesh->adj[k];
            if (nb < 0 || nb >= N) continue;
            if (visited[nb]) continue;
            visited[nb] = 1;
            hops[nb]    = (unsigned char)(hops[cur] + 1);
            queue[qtail++] = nb;
            if (d2_to_pos(&mesh->points[nb], monster) <= range2)
                reached[reached_n++] = nb;
        }
    }

    chosen[0] = anchor;
    int count = 1;

    while (count < RETROFIT_NODES_MAX) {
        int   pick      = -1;
        float pick_min2 = 0.0f;
        for (int r = 0; r < reached_n; r++) {
            int i = reached[r];

            float min2 = 1e30f;
            int   skip = 0;
            for (int k = 0; k < count; k++) {
                if (chosen[k] == i) { skip = 1; break; }
                float d2 = d2_between(&mesh->points[i], &mesh->points[chosen[k]]);
                if (d2 < spacing2) { skip = 1; break; }
                if (d2 < min2) min2 = d2;
            }
            if (skip) continue;
            if (min2 > pick_min2) { pick_min2 = min2; pick = i; }
        }
        if (pick < 0) break;
        chosen[count++] = pick;
    }
    return count;
}

// Brute-force shortest Hamilton cycle through `count` points. For count<=4
// there are at most 3 unique cycles (fixing chosen[0] and dividing out
// reversal). Permutes chosen[] in place.
static void order_cycle(const sim_nav_point_t *pts, int chosen[RETROFIT_NODES_MAX],
                        int count)
{
    if (count < 4) return;   // trivially the only cycle

    static const int perms[3][4] = {
        {0, 1, 2, 3},
        {0, 1, 3, 2},
        {0, 2, 1, 3},
    };
    int   best_perm = 0;
    float best_len2 = 1e30f;
    for (int p = 0; p < 3; p++) {
        float total = 0.0f;
        for (int i = 0; i < 4; i++) {
            int a = chosen[perms[p][i]];
            int b = chosen[perms[p][(i + 1) % 4]];
            total += d2_between(&pts[a], &pts[b]);
        }
        if (total < best_len2) { best_len2 = total; best_perm = p; }
    }
    int reordered[4];
    for (int i = 0; i < 4; i++) reordered[i] = chosen[perms[best_perm][i]];
    for (int i = 0; i < 4; i++) chosen[i] = reordered[i];
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
    int from_bsp = 0;
    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (b->patrol_route_id >= 0) continue;   // already has a route

        // Walk the live edict list for this brain's edict_num.
        edict_t *me = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == b->edict_num) { me = cur; break; }
        }
        if (!me) continue;

        int route_id = b->edict_num;

        // Preferred path: a BSP-wired path_corner chain via v.movetarget.
        // walkmonster_start_go set this if the monster's `target` keyvalue
        // resolved to a path_corner. Vanilla Quake leaves these dormant
        // (th_stand override after th_walk -- the classic QC bug); we
        // adopt the chain as our patrol route and the brain takes over.
        {
            edict_t *mt = me->v.movetarget;
            if (mt && mt != g->world && mt->v.classname &&
                strcmp(mt->v.classname, "path_corner") == 0)
            {
                int      idx   = 0;
                edict_t *cur   = mt;
                edict_t *first = cur;
                while (idx < RETROFIT_CHAIN_MAX) {
                    Sim_Patrol_RegisterArenaNode(route_id, idx++, cur);
                    if (!cur->v.target) break;
                    edict_t *nxt = eng->ED_Find(g->world, "targetname", cur->v.target);
                    if (!nxt || nxt == g->world) break;
                    if (nxt == first) break;   // closed loop
                    if (!nxt->v.classname ||
                        strcmp(nxt->v.classname, "path_corner") != 0) break;
                    cur = nxt;
                }
                if (idx >= RETROFIT_NODES_MIN) {
                    b->patrol_route_id = route_id;
                    b->patrol_node_idx = 0;
                    assigned++;
                    from_bsp++;
                    continue;
                }
            }
        }

        // Fallback: synthesise a patrol from the navmesh.
        int chosen[RETROFIT_NODES_MAX];
        int n = score_spread_bfs(me->v.origin, mesh, chosen);
        if (n < RETROFIT_NODES_MIN) continue;
        order_cycle(mesh->points, chosen, n);

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
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "sim_retrofit: assigned patrols to %d monsters (%d from BSP)\n",
                 assigned, from_bsp);
        eng->Con_Print(buf);
    }
}

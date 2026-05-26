// sim_nav.c -- Anchor-seeded flood-fill navmesh bake + A* pathfinding.
//
// Bake algorithm:
//   1. Iterate spawned entities to gather anchor positions:
//        info_player_start / _coop / _deathmatch / testplayerstart
//        info_teleport_destination
//        trigger_teleport      (bbox centre)
//        trigger_changelevel   (bbox centre)
//   2. For each anchor: SetOrigin probe, DropToFloor; on success add a seed
//      node and push to the BFS queue. Record entity refs for teleporters
//      so we can wire edges in step 4.
//   3. BFS expand each node by attempting SV_WalkMove in 8 compass
//      directions at FLOOD_STEP units. Each successful move yields a new
//      origin; dedupe against existing nodes within FLOOD_DEDUPE, then add
//      a walk edge to the (existing or new) target node.
//   4. Resolve teleporter pairs by `target`/`targetname` and add a directed
//      zero-cost edge from each trigger_teleport source node to its
//      info_teleport_destination node.
//
// The probe has classname "navmesh_probe" and health 0 so multi_touch /
// teleport_touch / changelevel_touch all bail before doing anything.
//
// The cache file format is bumped to v2 to invalidate caches baked by the
// old centroid-based extractor.
//
// Public API (Sim_Nav_Init / _LevelInit / _IsReady / _Get / _Frame /
// _PathTo) is unchanged.

#include "sim.h"
#include "../game_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#  include <direct.h>
#  define sim_mkdir(p) _mkdir(p)
#else
#  define sim_mkdir(p) mkdir((p), 0755)
#endif

extern engine_api_t   *eng;
extern game_globals_t *g;

#define NAV_MAGIC      0x4E41564D    // 'NAVM'
#define NAV_VERSION    20

#define FLOOD_STEP     32.0f
#define FLOOD_DEDUPE   16.0f
#define DEDUPE_Z       48.0f         // don't merge nodes on different floors
#define MAX_NODES      8192
#define MAX_EXPAND_ITERS 65536       // BFS iter safety cap

#define PROBE_HALF_X   16.0f
#define PROBE_HALF_Y   16.0f
#define PROBE_MIN_Z   (-24.0f)
#define PROBE_MAX_Z   ( 32.0f)

// Max vertical change accepted after a walkmove+drop. Keeps the flood from
// falling into bottomless pits / off cliffs while still tolerating Quake
// staircase heights (STEPSIZE = 18 up; we add slack for ramps and small
// drops between adjacent steps).
#define POST_WALK_MAX_DROP_Z  48.0f

// Jump / drop edge synthesis (phase 3.5). Walkmove caps step-up at 18, so
// ledges higher than that are unreachable via the BFS. After the flood
// settles we look for pairs of nodes within these ranges and add directed
// edges if a point trace at the higher endpoint is clear. JUMP_MAX_UP
// corresponds to a single Quake player jump (~64 units of vertical gain).
//
// Drops are kept aggressive on the vertical axis (you can survive most
// Quake falls) but tight on the horizontal: limiting drop xy to ~32
// (roughly the player bbox half-width plus a margin) gives "step off the
// ledge" edges without burying the overlay in long diagonal drops that
// are almost always redundant with a walkable path around.
#define JUMP_MAX_XY        96.0f
#define JUMP_MIN_UP        20.0f   // skip ranges walkmove already handles
// Quake's max vertical jump from a flat surface: jumpspeed^2 / (2*gravity)
// = 270^2 / 1600 ≈ 45.6. Anything above this can be DROPPED off but not
// jumped onto, so bake only the achievable up-range.
#define JUMP_MAX_UP        44.0f
#define DROP_MAX_XY        40.0f
#define DROP_MAX_DOWN      192.0f
#define JUMP_COST_BIAS     48.0f
#define DROP_COST_BIAS     16.0f

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

// Per-node classification, set during anchor seeding. BFS-expanded nodes
// stay NAV_NODE_GENERIC. Used by Sim_Nav_Frame to draw colored markers on
// points of interest in the debug overlay.
enum {
    NAV_NODE_GENERIC      = 0,
    NAV_NODE_PLAYER_SPAWN = 1,   // info_player_start / coop / deathmatch
    NAV_NODE_EXIT         = 2,   // trigger_changelevel
    NAV_NODE_ITEM         = 3,   // item_* / weapon_*
    NAV_NODE_MONSTER      = 4,   // monster_*
    NAV_NODE_TELEPORT_SRC = 5,   // trigger_teleport
    NAV_NODE_TELEPORT_DST = 6,   // resolved teleport destination
    NAV_NODE_DOOR_BUTTON  = 7,   // func_door / func_button
};

typedef struct {
    vec3_t pos;
    int    kind;   // NAV_NODE_*
} nav_point_t;

// Per-edge traversal type. The bot uses this to decide what input to
// produce (press jump for NAV_EDGE_JUMP_UP, stand on lift for
// NAV_EDGE_PLAT_RIDE, etc.).
enum {
    NAV_EDGE_WALK       = 0,
    NAV_EDGE_JUMP_UP    = 1,
    NAV_EDGE_DROP_DOWN  = 2,
    NAV_EDGE_PLAT_RIDE  = 3,
    NAV_EDGE_TELEPORT   = 4,
    NAV_EDGE_PLAT_LINK  = 5,   // walk on/off a lift's standing position
    NAV_EDGE_SHOOT_LINK = 6,   // bot must aim+fire at a shootable
    NAV_EDGE_BUTTON_LINK = 7,  // bot must touch a func_button at source
};

// Per-edge phase tag — which sub-block of bake_floodfill emitted the
// edge. Distinct from NAV_EDGE_* (which describes traversal style)
// because two phases can emit the same kind (e.g. Phase 4.5 emits
// both PLAT_LINK and BUTTON_LINK). Used by the debug overlay's phase
// mask and by the nav_edges_near MCP tool. Keep in sync with the
// table in mcp_server.c::tool_nav_bake_phases.
enum {
    NAV_PHASE_BFS_WALK          = 0,  // Phase 3:   adjacency WALK edges
    NAV_PHASE_JUMP_DROP         = 1,  // Phase 3.5: JUMP_UP / DROP_DOWN pairs
    NAV_PHASE_TELE_SRC          = 2,  // Phase 4:   TELEPORT from trigger anchor
    NAV_PHASE_TELE_NEAR         = 3,  // Phase 4:   TELEPORT from nearby-node fallback
    NAV_PHASE_LIFT_RIDE         = 4,  // Phase 4.5: PLAT_RIDE top<->bot
    NAV_PHASE_LIFT_PLAT_LINK    = 5,  // Phase 4.5: PLAT_LINK walk-on/off
    NAV_PHASE_LIFT_BUTTON_SHOOT = 6,  // Phase 4.5: BUTTON_LINK + SHOOT_LINK
    NAV_PHASE_COUNT             = 7,
};

typedef struct {
    int      from, to;
    float    weight;
    unsigned char kind;        // NAV_EDGE_*
    unsigned char phase;       // NAV_PHASE_* — which bake sub-block emitted this
    unsigned char _pad[2];
    unsigned int  requires_items;  // bitmask matched against player.items
} nav_edge_t;

struct sim_navmesh_s {
    nav_point_t *points;
    int          point_count;
    nav_edge_t  *edges;
    int          edge_count;
    int         *adj_offsets;
    int         *adj;
};

static sim_navmesh_t *s_mesh;
static int            s_ready;

// ---------------------------------------------------------------------------
// Spatial dedupe grid (xy hash; z checked per-candidate).
//
// The linked list is index-based, not pointer-based, so a `pool` realloc
// can't dangle existing references. Buckets store -1 for empty; otherwise
// they store an index into `pool`, whose `next` is itself an index.
// ---------------------------------------------------------------------------
#define GRID_CELL    64.0f
#define GRID_BUCKETS 4093

typedef struct {
    int idx;    // index into sim_navmesh_s::points
    int next;   // index into pool, or -1
} grid_node_t;

typedef struct {
    int          buckets[GRID_BUCKETS];   // index into pool, or -1
    grid_node_t *pool;
    int          pool_used;
    int          pool_cap;
} grid_t;

static unsigned grid_hash(int cx, int cy) {
    return ((unsigned)cx * 73856093u ^ (unsigned)cy * 19349663u) % GRID_BUCKETS;
}

static void grid_init(grid_t *grd, int cap) {
    grd->pool      = calloc(cap > 0 ? cap : 1, sizeof(grid_node_t));
    grd->pool_used = 0;
    grd->pool_cap  = cap;
    for (int i = 0; i < GRID_BUCKETS; i++) grd->buckets[i] = -1;
}

static void grid_free(grid_t *grd) {
    free(grd->pool);
    grd->pool     = NULL;
    grd->pool_cap = 0;
    grd->pool_used = 0;
    for (int i = 0; i < GRID_BUCKETS; i++) grd->buckets[i] = -1;
}

static int grid_find(const grid_t *grd, const sim_navmesh_t *m, const vec3_t pos) {
    int   cx = (int)floorf(pos[0] / GRID_CELL);
    int   cy = (int)floorf(pos[1] / GRID_CELL);
    float r2 = FLOOD_DEDUPE * FLOOD_DEDUPE;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            unsigned h = grid_hash(cx + dx, cy + dy);
            for (int ni = grd->buckets[h]; ni != -1; ni = grd->pool[ni].next) {
                int          pi = grd->pool[ni].idx;
                const float *p  = m->points[pi].pos;
                float ez = p[2] - pos[2];
                if (ez >  DEDUPE_Z) continue;
                if (ez < -DEDUPE_Z) continue;
                float ex = p[0] - pos[0];
                float ey = p[1] - pos[1];
                if (ex*ex + ey*ey + ez*ez <= r2)
                    return pi;
            }
        }
    }
    return -1;
}

static int grid_insert(grid_t *grd, const vec3_t pos, int idx) {
    if (grd->pool_used >= grd->pool_cap) return 0;   // pool exhausted
    int      cx = (int)floorf(pos[0] / GRID_CELL);
    int      cy = (int)floorf(pos[1] / GRID_CELL);
    unsigned h  = grid_hash(cx, cy);
    int      ni = grd->pool_used++;
    grd->pool[ni].idx  = idx;
    grd->pool[ni].next = grd->buckets[h];
    grd->buckets[h]    = ni;
    return 1;
}

// ---------------------------------------------------------------------------
// Mesh helpers
// ---------------------------------------------------------------------------
static void free_mesh(sim_navmesh_t *m) {
    if (!m) return;
    free(m->points);
    free(m->edges);
    free(m->adj_offsets);
    free(m->adj);
    free(m);
}

static int add_point(sim_navmesh_t *m, int *cap, grid_t *grd, const vec3_t pos) {
    if (m->point_count >= MAX_NODES) return -1;
    if (m->point_count >= *cap) {
        int nc = *cap ? *cap * 2 : 256;
        nav_point_t *p = realloc(m->points, sizeof(nav_point_t) * nc);
        if (!p) return -1;
        m->points = p;
        *cap      = nc;
    }
    int idx = m->point_count++;
    m->points[idx].pos[0] = pos[0];
    m->points[idx].pos[1] = pos[1];
    m->points[idx].pos[2] = pos[2];
    m->points[idx].kind   = NAV_NODE_GENERIC;   // anchor-seeded nodes upgrade
    grid_insert(grd, pos, idx);
    return idx;
}

static int add_edge(sim_navmesh_t *m, int *cap, int from, int to,
                    float weight, unsigned char kind,
                    unsigned int req_items, unsigned char phase) {
    if (m->edge_count >= *cap) {
        int nc = *cap ? *cap * 2 : 1024;
        nav_edge_t *e = realloc(m->edges, sizeof(nav_edge_t) * nc);
        if (!e) return 0;
        m->edges = e;
        *cap     = nc;
    }
    nav_edge_t *e = &m->edges[m->edge_count++];
    e->from   = from;
    e->to     = to;
    e->weight = weight;
    e->kind   = kind;
    e->phase  = phase;
    e->_pad[0] = e->_pad[1] = 0;
    e->requires_items = req_items;
    return 1;
}

static void build_adjacency(sim_navmesh_t *m) {
    m->adj_offsets = calloc(m->point_count + 1, sizeof(int));
    if (!m->adj_offsets) return;
    for (int i = 0; i < m->edge_count; i++)
        m->adj_offsets[m->edges[i].from + 1]++;
    for (int i = 1; i <= m->point_count; i++)
        m->adj_offsets[i] += m->adj_offsets[i-1];

    int *cursor = calloc(m->point_count, sizeof(int));
    if (!cursor) { free(m->adj_offsets); m->adj_offsets = NULL; return; }
    m->adj = malloc(sizeof(int) * (m->edge_count > 0 ? m->edge_count : 1));
    if (!m->adj) { free(m->adj_offsets); m->adj_offsets = NULL; free(cursor); return; }
    for (int i = 0; i < m->edge_count; i++) {
        int from = m->edges[i].from;
        m->adj[m->adj_offsets[from] + cursor[from]++] = i;
    }
    free(cursor);
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------
static edict_t *alloc_probe(void) {
    edict_t *e = eng->ED_Alloc();
    // Distinct classname so multi_touch / changelevel_touch (which gate on
    // classname == "player") leave us alone. Zero health makes
    // teleport_touch bail before relocating us.
    e->v.classname  = "navmesh_probe";
    e->v.movetype   = MOVETYPE_STEP;
    e->v.solid      = SOLID_SLIDEBOX;
    e->v.health     = 0;
    e->v.takedamage = DAMAGE_NO;
    // FL_PARTIALGROUND tells SV_movestep to accept positions where the bbox
    // bottom isn't fully supported by floor and to advance through "walked
    // off an edge" cases. Without it, the flood stalls at every doorway
    // threshold, pit lip, and stair edge in start.bsp. We compensate by
    // dropping the probe to floor after every successful walkmove and
    // rejecting moves whose vertical drop exceeds POST_WALK_MAX_DROP_Z.
    e->v.flags      = (float)FL_PARTIALGROUND;
    eng->SV_SetSize(e,
        (vec3_t){-PROBE_HALF_X, -PROBE_HALF_Y, PROBE_MIN_Z},
        (vec3_t){ PROBE_HALF_X,  PROBE_HALF_Y, PROBE_MAX_Z});
    return e;
}

// SetOrigin to `pos` then DropToFloor. On success, copies the resulting
// floor-seated origin to `out` and returns 1. DropToFloor also sets
// FL_ONGROUND, which SV_WalkMove requires.
static int seat_probe(edict_t *probe, const vec3_t pos, vec3_t out) {
    eng->SV_SetOrigin(probe, (float *)pos);
    if (!eng->SV_DropToFloor(probe)) return 0;
    out[0] = probe->v.origin[0];
    out[1] = probe->v.origin[1];
    out[2] = probe->v.origin[2];
    return 1;
}

static void entity_center(edict_t *e, vec3_t out) {
    out[0] = e->v.origin[0] + (e->v.mins[0] + e->v.maxs[0]) * 0.5f;
    out[1] = e->v.origin[1] + (e->v.mins[1] + e->v.maxs[1]) * 0.5f;
    out[2] = e->v.origin[2] + (e->v.mins[2] + e->v.maxs[2]) * 0.5f;
}

// Validate a Phase 4.5 PLAT_LINK candidate: trace from the floor sample
// `a` (lifted to ~eye height) toward the plat anchor `b`. The lift brush
// itself is kept solid during bake, so a successful link will usually
// fail the trace by hitting the lift surface — that's not a wall, just
// the lift. We accept the failure when trace_endpos lies inside (or
// just outside) the lift's XY footprint, and reject only when the trace
// hit something else (a real wall between p and the lift). This is the
// "check where it hit" variant of the trace_link_clear that was reverted
// in 924bc8f for false-negativing every link onto plat_top.
static int trace_link_clear_plat(const vec3_t a, const vec3_t b, edict_t *plat) {
    vec3_t a_eye = { a[0], a[1], a[2] + 16.0f };
    vec3_t b_eye = { b[0], b[1], b[2] + 16.0f };
    vec3_t zero  = { 0.0f, 0.0f, 0.0f };
    eng->SV_TraceMove(a_eye, zero, zero, b_eye, 1, NULL);
    if (g->trace_fraction >= 0.999f) return 1;

    const float pad = 8.0f;
    float ex = g->trace_endpos[0];
    float ey = g->trace_endpos[1];
    return (ex >= plat->v.mins[0] - pad && ex <= plat->v.maxs[0] + pad &&
            ey >= plat->v.mins[1] - pad && ey <= plat->v.maxs[1] + pad);
}


// ---------------------------------------------------------------------------
// Anchor / teleport collection
// ---------------------------------------------------------------------------
typedef enum {
    ANCHOR_GENERIC      = 0,
    ANCHOR_TELEPORT_SRC = 1,   // trigger_teleport (entity has `target`)
    ANCHOR_TELEPORT_DST = 2,   // info_teleport_destination (`targetname`)
    ANCHOR_PLAT_TOP     = 3,   // func_plat standing pos at top of travel
    ANCHOR_PLAT_BOTTOM  = 4,   // func_plat standing pos at bottom of travel
} anchor_kind_t;

typedef struct {
    vec3_t        pos;
    anchor_kind_t kind;
    int           node_kind;    // NAV_NODE_*, propagated to nav_point_t.kind
    edict_t      *entity;       // valid for TELEPORT_SRC / TELEPORT_DST
    int           node_index;   // assigned after seating; -1 if no floor
} anchor_t;

static int anchors_push(anchor_t **arr, int *cap, int *n,
                        const vec3_t pos, anchor_kind_t kind, int node_kind,
                        edict_t *ent)
{
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        anchor_t *p = realloc(*arr, sizeof(anchor_t) * nc);
        if (!p) return 0;
        *arr = p;
        *cap = nc;
    }
    anchor_t *a = &(*arr)[(*n)++];
    a->pos[0]     = pos[0];
    a->pos[1]     = pos[1];
    a->pos[2]     = pos[2];
    a->kind       = kind;
    a->node_kind  = node_kind;
    a->entity     = ent;
    a->node_index = -1;
    return 1;
}

static void collect_anchors_by_classname(anchor_t **arr, int *cap, int *n,
                                         const char *classname,
                                         anchor_kind_t kind, int node_kind,
                                         int use_bbox_center)
{
    edict_t *e = eng->ED_Find(g->world, "classname", classname);
    while (e != g->world) {
        vec3_t pos;
        if (use_bbox_center) entity_center(e, pos);
        else {
            pos[0] = e->v.origin[0];
            pos[1] = e->v.origin[1];
            pos[2] = e->v.origin[2];
        }
        anchors_push(arr, cap, n, pos, kind, node_kind, e);
        e = eng->ED_Find(e, "classname", classname);
    }
}

// ---------------------------------------------------------------------------
// Save / load (cache)
// ---------------------------------------------------------------------------
static int save_mesh(const char *path, const sim_navmesh_t *m) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int magic = NAV_MAGIC;
    int ver   = NAV_VERSION;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);
    fwrite(&m->point_count, 4, 1, f);
    fwrite(&m->edge_count,  4, 1, f);
    fwrite(m->points, sizeof(nav_point_t), m->point_count, f);
    fwrite(m->edges,  sizeof(nav_edge_t),  m->edge_count, f);
    fclose(f);
    return 1;
}

static sim_navmesh_t *load_mesh(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int magic, ver, np, ne;
    if (fread(&magic, 4, 1, f) != 1 ||
        fread(&ver,   4, 1, f) != 1 ||
        fread(&np,    4, 1, f) != 1 ||
        fread(&ne,    4, 1, f) != 1)
    { fclose(f); return 0; }
    if (magic != NAV_MAGIC || ver != NAV_VERSION) { fclose(f); return 0; }
    if (np < 0 || np > MAX_NODES || ne < 0)       { fclose(f); return 0; }

    sim_navmesh_t *m = calloc(1, sizeof(*m));
    if (!m) { fclose(f); return 0; }
    m->point_count = np;
    m->edge_count  = ne;
    m->points = malloc(sizeof(nav_point_t) * (np > 0 ? np : 1));
    m->edges  = malloc(sizeof(nav_edge_t)  * (ne > 0 ? ne : 1));
    if (!m->points || !m->edges) { fclose(f); free_mesh(m); return 0; }
    if (np > 0 && fread(m->points, sizeof(nav_point_t), np, f) != (size_t)np) {
        fclose(f); free_mesh(m); return 0;
    }
    if (ne > 0 && fread(m->edges, sizeof(nav_edge_t), ne, f) != (size_t)ne) {
        fclose(f); free_mesh(m); return 0;
    }
    fclose(f);
    build_adjacency(m);
    return m;
}

// ---------------------------------------------------------------------------
// Push current mesh to imgui dev overlay (2D xy-only).
// ---------------------------------------------------------------------------
static void push_to_imgui(const sim_navmesh_t *m) {
    int np = m->point_count;
    if (np <= 0) { eng->ImguiNav_Set(NULL, 0, NULL, 0); return; }
    if (np > 4096) np = 4096;
    float *xy = malloc(sizeof(float) * 2 * np);
    if (!xy) return;
    for (int i = 0; i < np; i++) {
        xy[2*i+0] = m->points[i].pos[0];
        xy[2*i+1] = m->points[i].pos[1];
    }
    int ne = m->edge_count;
    unsigned short *eds = NULL;
    int wrote = 0;
    if (ne > 0) {
        eds = malloc(sizeof(unsigned short) * 2 * ne);
        if (eds) {
            for (int i = 0; i < ne; i++) {
                int a = m->edges[i].from;
                int b = m->edges[i].to;
                if (a < np && b < np && a < 65536 && b < 65536) {
                    eds[2*wrote+0] = (unsigned short)a;
                    eds[2*wrote+1] = (unsigned short)b;
                    wrote++;
                }
            }
        }
    }
    eng->ImguiNav_Set(xy, np, eds, wrote);
    free(eds);
    free(xy);
}

// ---------------------------------------------------------------------------
// Flood-fill bake
// ---------------------------------------------------------------------------
static const float k_yaws[8] = {
    0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f
};

static int bake_floodfill(sim_navmesh_t *m) {
    anchor_t *anchors     = NULL;
    int       anchor_cap  = 0;
    int       anchor_n    = 0;

    // --- Phase 1: collect anchors ----------------------------------------
    // Done BEFORE allocating the probe so we can bail cheaply when the bake
    // fires before ED_LoadFromFile has populated entities (the engine
    // currently fires start_frame inside SV_SpawnServer at a point where the
    // map is named but its entities lump has not been parsed yet).
    collect_anchors_by_classname(&anchors, &anchor_cap, &anchor_n,
        "info_player_start",       ANCHOR_GENERIC, NAV_NODE_PLAYER_SPAWN, 0);
    collect_anchors_by_classname(&anchors, &anchor_cap, &anchor_n,
        "info_player_coop",        ANCHOR_GENERIC, NAV_NODE_PLAYER_SPAWN, 0);
    collect_anchors_by_classname(&anchors, &anchor_cap, &anchor_n,
        "info_player_deathmatch",  ANCHOR_GENERIC, NAV_NODE_PLAYER_SPAWN, 0);
    collect_anchors_by_classname(&anchors, &anchor_cap, &anchor_n,
        "testplayerstart",         ANCHOR_GENERIC, NAV_NODE_PLAYER_SPAWN, 0);
    collect_anchors_by_classname(&anchors, &anchor_cap, &anchor_n,
        "trigger_teleport",        ANCHOR_TELEPORT_SRC, NAV_NODE_TELEPORT_SRC, 1);
    collect_anchors_by_classname(&anchors, &anchor_cap, &anchor_n,
        "trigger_changelevel",     ANCHOR_GENERIC, NAV_NODE_EXIT, 1);

    // Walk every remaining edict and add it as a generic anchor. Mappers
    // place items, monsters, buttons, doors, path_corners, info_notnull
    // markers and lights at hand-tuned positions; many of them sit on or
    // immediately above walkable floor, so they make excellent extra seeds
    // for the flood. Entities we can't drop to floor (ceiling lights, brush
    // interiors) are filtered out by seat_probe at phase 2. We skip:
    //   - the world entity itself
    //   - already-collected anchor entities (de-dup by pointer)
    //   - entities with no classname (client slots, freed slots)
    //   - the trigger_teleport / changelevel classes we already handle
    //     above with proper kinds
    {
        edict_t *e = eng->ED_Next(g->world);
        while (e) {
            const char *cn = e->v.classname;
            if (!cn || !cn[0])                                 goto next_e;
            if (!strcmp(cn, "navmesh_probe"))                  goto next_e;
            if (!strcmp(cn, "info_player_start"))              goto next_e;
            if (!strcmp(cn, "info_player_coop"))               goto next_e;
            if (!strcmp(cn, "info_player_deathmatch"))         goto next_e;
            if (!strcmp(cn, "testplayerstart"))                goto next_e;
            if (!strcmp(cn, "trigger_teleport"))               goto next_e;
            if (!strcmp(cn, "trigger_changelevel"))            goto next_e;
            // func_plat (renamed to "plat" by spawn_func_plat) — push two
            // anchors: one for the standing position when the lift is at
            // TOP, one for BOTTOM. The pos1 / pos2 fields hold the lift's
            // brush origin at the two extremes; the player stands on the
            // brush's TOP surface (origin + maxs.z) at either end. seat_probe
            // is skipped for these (Phase 2 uses the synthetic position
            // directly) because the brush is non-solid during bake and
            // would otherwise drop straight to the shaft floor.
            if (!strcmp(cn, "plat") || !strcmp(cn, "func_plat")) {
                vec3_t top, bot;
                float stand_z_top, stand_z_bot;
                // Player feet land at brush top surface + ~4u clearance.
                stand_z_top = e->v.pos1[2] + e->v.maxs[2] + 4.f;
                stand_z_bot = e->v.pos2[2] + e->v.maxs[2] + 4.f;
                top[0] = e->v.origin[0] + (e->v.mins[0] + e->v.maxs[0]) * 0.5f;
                top[1] = e->v.origin[1] + (e->v.mins[1] + e->v.maxs[1]) * 0.5f;
                top[2] = stand_z_top;
                bot[0] = top[0];
                bot[1] = top[1];
                bot[2] = stand_z_bot;
                anchors_push(&anchors, &anchor_cap, &anchor_n,
                             top, ANCHOR_PLAT_TOP, NAV_NODE_GENERIC, e);
                anchors_push(&anchors, &anchor_cap, &anchor_n,
                             bot, ANCHOR_PLAT_BOTTOM, NAV_NODE_GENERIC, e);
                goto next_e;
            }
            // Vertical func_door used as a lift (e.g. e1m1 first lift).
            // A func_door is a "lift-door" when its travel is mostly
            // vertical AND its top surface is wide enough for the player
            // to stand on. Bake TOP / BOTTOM anchors at the standing
            // positions and let the existing Phase 4.5 logic wire ride
            // + button edges. (Post-spawn classname is "door"; accept
            // "func_door" defensively.)
            //
            // We don't filter on overall brush height because e1m1's t1
            // lift includes a CLIP sub-brush that extends ~224 units
            // below the visible platform to define the shaft — the AABB
            // is 128x128x240 but the standable slab is just the top.
            // Footprint width is enough to tell a lift from a regular
            // sliding door (which is typically narrow).
            if (!strcmp(cn, "door") || !strcmp(cn, "func_door")) {
                float dz = e->v.pos2[2] - e->v.pos1[2];
                float dx = e->v.pos2[0] - e->v.pos1[0];
                float dy = e->v.pos2[1] - e->v.pos1[1];
                float adz = dz < 0 ? -dz : dz;
                float adx = dx < 0 ? -dx : dx;
                float ady = dy < 0 ? -dy : dy;
                float sx  = e->v.maxs[0] - e->v.mins[0];
                float sy  = e->v.maxs[1] - e->v.mins[1];
                float sz  = e->v.maxs[2] - e->v.mins[2];
                int vertical  = (adz > 24.f) && (adz > adx) && (adz > ady);
                int standable = (sx > 48.f) && (sy > 48.f);
                if (vertical && standable) {
                    float z_lo = e->v.pos1[2] < e->v.pos2[2] ? e->v.pos1[2] : e->v.pos2[2];
                    float z_hi = e->v.pos1[2] < e->v.pos2[2] ? e->v.pos2[2] : e->v.pos1[2];
                    vec3_t top, bot;
                    top[0] = e->v.origin[0] + (e->v.mins[0] + e->v.maxs[0]) * 0.5f;
                    top[1] = e->v.origin[1] + (e->v.mins[1] + e->v.maxs[1]) * 0.5f;
                    top[2] = z_hi + e->v.maxs[2] + 4.f;
                    bot[0] = top[0];
                    bot[1] = top[1];
                    bot[2] = z_lo + e->v.maxs[2] + 4.f;
                    anchors_push(&anchors, &anchor_cap, &anchor_n,
                                 top, ANCHOR_PLAT_TOP, NAV_NODE_GENERIC, e);
                    anchors_push(&anchors, &anchor_cap, &anchor_n,
                                 bot, ANCHOR_PLAT_BOTTOM, NAV_NODE_GENERIC, e);
                    char dbg[160];
                    snprintf(dbg, sizeof(dbg),
                        "sim_nav: lift-door '%s' travel=%.0fz size=%.0fx%.0fx%.0f\n",
                        e->v.targetname ? e->v.targetname : "(no targetname)",
                        adz, sx, sy, sz);
                    eng->Con_Print(dbg);
                    goto next_e;
                }
            }
            // Brush entities have origin (0,0,0) and meaningful mins/maxs;
            // point entities have meaningful origin. Pick the right one.
            vec3_t pos;
            if (e->v.origin[0] == 0.0f && e->v.origin[1] == 0.0f &&
                e->v.origin[2] == 0.0f)
            {
                // Brush; only useful if it has a bbox.
                if (e->v.maxs[0] == e->v.mins[0] &&
                    e->v.maxs[1] == e->v.mins[1] &&
                    e->v.maxs[2] == e->v.mins[2])
                    goto next_e;
                entity_center(e, pos);
            } else {
                pos[0] = e->v.origin[0];
                pos[1] = e->v.origin[1];
                pos[2] = e->v.origin[2];
            }
            // Classify by classname prefix for the debug-overlay color
            // coding. Anything that doesn't match a known category stays
            // generic; A* uses the same anchor_kind for all of these.
            int nk = NAV_NODE_GENERIC;
            if (!strncmp(cn, "item_", 5) || !strncmp(cn, "weapon_", 7))
                nk = NAV_NODE_ITEM;
            else if (!strncmp(cn, "monster_", 8))
                nk = NAV_NODE_MONSTER;
            else if (!strcmp(cn, "func_door") ||
                     !strcmp(cn, "func_door_secret") ||
                     !strcmp(cn, "func_button"))
                nk = NAV_NODE_DOOR_BUTTON;
            anchors_push(&anchors, &anchor_cap, &anchor_n,
                         pos, ANCHOR_GENERIC, nk, e);
        next_e:
            e = eng->ED_Next(e);
        }
    }

    // Resolve each trigger_teleport's destination by `target` -> `targetname`
    // lookup (the destination can be info_teleport_destination, info_notnull,
    // path_corner, or any entity the mapper chose to target). Without this
    // step, destination rooms have no anchor and the flood never reaches
    // them.
    int src_count_before_resolve = anchor_n;
    for (int i = 0; i < src_count_before_resolve; i++) {
        if (anchors[i].kind != ANCHOR_TELEPORT_SRC) continue;
        edict_t *src = anchors[i].entity;
        if (!src || !src->v.target) continue;
        edict_t *dst = eng->ED_Find(g->world, "targetname", src->v.target);
        if (dst == g->world) continue;
        int already = 0;
        for (int j = 0; j < anchor_n; j++) {
            if (anchors[j].kind == ANCHOR_TELEPORT_DST &&
                anchors[j].entity == dst)
            { already = 1; break; }
        }
        if (already) continue;
        anchors_push(&anchors, &anchor_cap, &anchor_n,
                     dst->v.origin, ANCHOR_TELEPORT_DST,
                     NAV_NODE_TELEPORT_DST, dst);
    }

    if (anchor_n == 0) {
        free(anchors);
        return 0;
    }

    eng->Con_Print("sim_nav: baking via flood-fill...\n");

    edict_t  *probe       = alloc_probe();
    grid_t    grd;
    grid_init(&grd, MAX_NODES);
    int       cap_points  = 0;
    int       cap_edges   = 0;
    int      *queue       = NULL;
    int       q_head = 0, q_tail = 0, q_cap = 0;
    int       iters       = 0;

    // Temporarily clear every non-world, non-probe entity's `solid` so the
    // probe doesn't collide with monsters, doors, plats, buttons, etc. The
    // walkmove probe inherits Quake's normal bbox-vs-bbox trace clipping;
    // without this step the probe stops at every monster spawn, creating
    // holes in the navmesh around each enemy. We restore solid values
    // after the bake completes. World (edict 0) is left solid so the probe
    // continues to collide with the level geometry it needs to walk on.
    //
    // EXCEPTION: lifts stay solid. Wall-mounted button anchors near a
    // lift use seat_probe (a downward trace) to find a standing floor.
    // If the lift were non-solid, the probe would drop straight through
    // the shaft to the floor below, putting the button anchor far below
    // its actual approach position and breaking the walk edge from
    // plat_top to button_anchor. Keeping lifts solid means BFS won't
    // expand *through* them, but plat_top/bottom anchors are handled by
    // Phase 4.5's explicit link logic, not BFS expansion, so that's
    // fine.
    typedef struct { edict_t *e; float solid; } solid_save_t;
    solid_save_t *solid_saves     = NULL;
    int           solid_saves_cap = 0;
    int           solid_saves_n   = 0;
    {
        edict_t *it = eng->ED_Next(g->world);
        while (it) {
            if (it != probe && it->v.solid > (float)SOLID_NOT) {
                const char *icn = it->v.classname;
                int keep_solid = 0;
                if (icn) {
                    if (!strcmp(icn, "plat") || !strcmp(icn, "func_plat")) {
                        keep_solid = 1;  // lift
                    } else if (!strcmp(icn, "door") || !strcmp(icn, "func_door")) {
                        float dz  = it->v.pos2[2] - it->v.pos1[2];
                        float dx  = it->v.pos2[0] - it->v.pos1[0];
                        float dy  = it->v.pos2[1] - it->v.pos1[1];
                        float adz = dz < 0 ? -dz : dz;
                        float adx = dx < 0 ? -dx : dx;
                        float ady = dy < 0 ? -dy : dy;
                        float sx  = it->v.maxs[0] - it->v.mins[0];
                        float sy  = it->v.maxs[1] - it->v.mins[1];
                        int vertical  = (adz > 24.f) && (adz > adx) && (adz > ady);
                        int standable = (sx > 48.f) && (sy > 48.f);
                        int is_lift = vertical && standable;
                        // Secret doors get health=10000 on spawn; they
                        // can't be opened by walking in, only by shoot
                        // damage or a remote trigger. Keep them solid
                        // so BFS routes around them entirely.
                        //
                        // NOT including targetname-driven doors here:
                        // many maps gate their main path behind button
                        // -> door pairs, and blocking those at the bake
                        // stage strands the bot. Proper handling needs
                        // button -> door edges (same shape as the lift
                        // logic) which is queued.
                        int is_secret = ((int)it->v.health != 0);
                        keep_solid = is_lift || is_secret;
                    }
                }
                if (keep_solid) { it = eng->ED_Next(it); continue; }
                if (solid_saves_n >= solid_saves_cap) {
                    int nc = solid_saves_cap ? solid_saves_cap * 2 : 128;
                    solid_save_t *ns = realloc(solid_saves,
                                               sizeof(solid_save_t) * nc);
                    if (!ns) break;
                    solid_saves     = ns;
                    solid_saves_cap = nc;
                }
                solid_saves[solid_saves_n].e     = it;
                solid_saves[solid_saves_n].solid = it->v.solid;
                solid_saves_n++;
                it->v.solid = (float)SOLID_NOT;
            }
            it = eng->ED_Next(it);
        }
    }

    // --- Phase 2: seat seeds + push to queue ------------------------------
    for (int i = 0; i < anchor_n; i++) {
        anchor_t *a = &anchors[i];
        vec3_t   seated;
        // For lift standing positions we already know where the player
        // stands (top surface of the brush at pos1/pos2). seat_probe
        // would otherwise drop the probe through the non-solid brush to
        // the shaft floor and collapse both anchors to the same node.
        if (a->kind == ANCHOR_PLAT_TOP || a->kind == ANCHOR_PLAT_BOTTOM) {
            seated[0] = a->pos[0]; seated[1] = a->pos[1]; seated[2] = a->pos[2];
        } else if (!seat_probe(probe, a->pos, seated)) continue;
        int existing = grid_find(&grd, m, seated);
        if (existing >= 0) {
            a->node_index = existing;
            // Multiple anchors can map to the same seated node. Upgrade
            // the node kind toward the more interesting category — the
            // NAV_NODE_* enum values are ordered low-to-high importance.
            if (a->node_kind > m->points[existing].kind)
                m->points[existing].kind = a->node_kind;
            continue;
        }
        int idx = add_point(m, &cap_points, &grd, seated);
        if (idx < 0) continue;
        a->node_index    = idx;
        m->points[idx].kind = a->node_kind;
        // Plat endpoints are not BFS expansion seeds — they're floating
        // mid-air nodes that would re-drop to the shaft floor on every
        // walkmove. We link them to neighbours and to each other in
        // Phase 4.5 below.
        if (a->kind == ANCHOR_PLAT_TOP || a->kind == ANCHOR_PLAT_BOTTOM)
            continue;
        if (q_tail >= q_cap) {
            int nc = q_cap ? q_cap * 2 : 256;
            int *nq = realloc(queue, sizeof(int) * nc);
            if (!nq) continue;
            queue = nq;
            q_cap = nc;
        }
        queue[q_tail++] = idx;
    }

    // --- Phase 3: BFS expand ----------------------------------------------
    while (q_head < q_tail && iters < MAX_EXPAND_ITERS) {
        iters++;
        int cur = queue[q_head++];
        if (cur < 0 || cur >= m->point_count) continue;
        vec3_t cur_pos;
        cur_pos[0] = m->points[cur].pos[0];
        cur_pos[1] = m->points[cur].pos[1];
        cur_pos[2] = m->points[cur].pos[2];

        for (int d = 0; d < 8; d++) {
            // Re-seat probe at current node every iteration. seat_probe
            // sets FL_ONGROUND (required by SV_WalkMove) and is also how
            // we recover from the previous iteration's walkmove possibly
            // leaving the probe mid-air (FL_PARTIALGROUND off-edge path).
            vec3_t scratch;
            if (!seat_probe(probe, cur_pos, scratch)) break;
            (void)scratch;

            // SV_movestep CLEARS FL_PARTIALGROUND on every normal-success
            // path (sv_move.c:215-219), so we must set it again on every
            // walkmove or only the first one is lenient about CheckBottom /
            // off-edge.
            probe->v.flags = (float)((int)probe->v.flags | FL_PARTIALGROUND);

            if (!eng->SV_WalkMove(probe, k_yaws[d], FLOOD_STEP)) continue;

            // Walkmove may have left us off-edge in mid-air (FL_PARTIALGROUND
            // off-edge case). Re-drop to floor; if no floor within ~256
            // units, reject the move so we don't add unreachable nodes.
            if (!eng->SV_DropToFloor(probe)) continue;

            vec3_t end;
            end[0] = probe->v.origin[0];
            end[1] = probe->v.origin[1];
            end[2] = probe->v.origin[2];

            // Reject moves with too large a vertical change: prevents the
            // flood from "jumping" off cliffs or down deep pits and giving
            // back-edges that the player can't actually walk back along.
            float dz = end[2] - cur_pos[2];
            if (dz < -POST_WALK_MAX_DROP_Z || dz > POST_WALK_MAX_DROP_Z)
                continue;

            int next_idx = grid_find(&grd, m, end);
            if (next_idx < 0) {
                if (m->point_count >= MAX_NODES) continue;
                next_idx = add_point(m, &cap_points, &grd, end);
                if (next_idx < 0) continue;
                if (q_tail >= q_cap) {
                    int nc = q_cap ? q_cap * 2 : 256;
                    int *nq = realloc(queue, sizeof(int) * nc);
                    if (!nq) continue;
                    queue = nq;
                    q_cap = nc;
                }
                queue[q_tail++] = next_idx;
            }
            if (next_idx == cur) continue;

            float ex = m->points[next_idx].pos[0] - cur_pos[0];
            float ey = m->points[next_idx].pos[1] - cur_pos[1];
            float ez = m->points[next_idx].pos[2] - cur_pos[2];
            float w  = (float)sqrt(ex*ex + ey*ey + ez*ez);
            if (w < 1.0f) w = 1.0f;
            add_edge(m, &cap_edges, cur, next_idx, w, NAV_EDGE_WALK, 0, NAV_PHASE_BFS_WALK);
        }
    }

    // --- Phase 3.5: jump and drop edges ---------------------------------
    // Walkmove caps step-up at STEPSIZE = 18, so any ledge higher than that
    // is its own component even when the player can clearly jump onto it
    // (e.g. the start-room ledges on e1m1 with the shells/armor pickups).
    // After the BFS settles, look for node pairs that are within jump or
    // drop range but didn't get a walk edge, and add one if a point trace
    // at the higher level is clear.
    int jump_edges_added = 0;
    int drop_edges_added = 0;
    for (int i = 0; i < m->point_count; i++) {
        for (int j = 0; j < m->point_count; j++) {
            if (i == j) continue;
            float dx = m->points[j].pos[0] - m->points[i].pos[0];
            float dy = m->points[j].pos[1] - m->points[i].pos[1];
            float dz = m->points[j].pos[2] - m->points[i].pos[2];
            float xy = (float)sqrt(dx*dx + dy*dy);
            if (xy < 1.0f) continue;

            int kind = 0;   // 1 = jump up, 2 = drop down
            if (dz > JUMP_MIN_UP && dz <= JUMP_MAX_UP && xy <= JUMP_MAX_XY)
                kind = 1;
            else if (dz < -JUMP_MIN_UP && dz >= -DROP_MAX_DOWN && xy <= DROP_MAX_XY)
                kind = 2;
            if (!kind) continue;

            // Validate the jump/drop with an L-shape bbox SWEEP from the
            // lower endpoint (not a point trace — the player's 32x32x56 body
            // doesn't fit through gaps a zero-thickness line slips through):
            //   1. vertical sweep at lower's xy, from lower's stand height up
            //      to upper's stand altitude. Catches overhead obstructions
            //      and verifies the player can rise that high here.
            //   2. horizontal sweep at upper's stand altitude from lower's xy
            //      to upper's xy. Catches walls/narrow passages between.
            // Both sweeps use player extents; if either's fraction < 1 the
            // bbox couldn't clear that segment of the L-shape.
            int    lo      = (dz > 0) ? i : j;
            int    up      = (dz > 0) ? j : i;
            vec3_t player_mins = { -16.0f, -16.0f, -24.0f };
            vec3_t player_maxs = {  16.0f,  16.0f,  32.0f };
            vec3_t lo_pos  = { m->points[lo].pos[0], m->points[lo].pos[1],
                               m->points[lo].pos[2] };
            vec3_t lo_top  = { m->points[lo].pos[0], m->points[lo].pos[1],
                               m->points[up].pos[2] };
            vec3_t up_top  = { m->points[up].pos[0], m->points[up].pos[1],
                               m->points[up].pos[2] };

            eng->SV_TraceMove(lo_pos, player_mins, player_maxs, lo_top, 1, NULL);
            if (g->trace_fraction < 0.999f) continue;
            eng->SV_TraceMove(lo_top, player_mins, player_maxs, up_top, 1, NULL);
            if (g->trace_fraction < 0.999f) continue;

            float dist = (float)sqrt(xy*xy + dz*dz);
            float bias = (kind == 1) ? JUMP_COST_BIAS : DROP_COST_BIAS;
            unsigned char edge_k = (kind == 1) ? NAV_EDGE_JUMP_UP : NAV_EDGE_DROP_DOWN;
            add_edge(m, &cap_edges, i, j, dist + bias, edge_k, 0, NAV_PHASE_JUMP_DROP);
            if (kind == 1) jump_edges_added++;
            else           drop_edges_added++;
        }
    }

    // --- Phase 4: teleporter edges ---------------------------------------
    // For each TELEPORT_SRC anchor, find its destination by matching
    // `target` (on src) to `targetname` (on dst). The source anchor's
    // bbox center frequently sits in mid-air (trigger brushes span floor
    // to ceiling) so seat_probe fails and node_index stays -1. In that
    // case, scan all baked nav points and treat any whose XY lies inside
    // the trigger bbox (with a small Z tolerance) as a real teleport
    // entry; add a one-way edge to the destination from each. This is
    // what fixed e1m1's two-component split: the underground exit room
    // was reachable only via a trigger_teleport whose center seat_probe
    // couldn't drop to the floor.
    int teleport_edges = 0;
    for (int i = 0; i < anchor_n; i++) {
        if (anchors[i].kind != ANCHOR_TELEPORT_SRC)          continue;
        if (!anchors[i].entity || !anchors[i].entity->v.target) continue;

        const char *want = anchors[i].entity->v.target;
        int dst_node = -1;
        for (int j = 0; j < anchor_n; j++) {
            if (anchors[j].kind != ANCHOR_TELEPORT_DST)         continue;
            if (anchors[j].node_index < 0)                      continue;
            if (!anchors[j].entity || !anchors[j].entity->v.targetname) continue;
            if (strcmp(anchors[j].entity->v.targetname, want) != 0) continue;
            dst_node = anchors[j].node_index;
            break;
        }
        if (dst_node < 0) continue;

        if (anchors[i].node_index >= 0) {
            // Source seated normally — single edge.
            add_edge(m, &cap_edges, anchors[i].node_index, dst_node, 0.0f,
                     NAV_EDGE_TELEPORT, 0, NAV_PHASE_TELE_SRC);
            teleport_edges++;
            continue;
        }

        // Source did not seat. Fall back: link every baked node whose
        // XY lies inside the trigger's bbox and whose Z is within (or
        // just below) the bbox vertical span — that's where a player
        // physically walks into the trigger from.
        edict_t *src = anchors[i].entity;
        float xmin = src->v.origin[0] + src->v.mins[0];
        float ymin = src->v.origin[1] + src->v.mins[1];
        float zmin = src->v.origin[2] + src->v.mins[2];
        float xmax = src->v.origin[0] + src->v.maxs[0];
        float ymax = src->v.origin[1] + src->v.maxs[1];
        float zmax = src->v.origin[2] + src->v.maxs[2];
        const float z_slack = 64.0f;  // node may sit on a floor ~step below
        int linked_here = 0;
        for (int p = 0; p < m->point_count; p++) {
            float px = m->points[p].pos[0];
            float py = m->points[p].pos[1];
            float pz = m->points[p].pos[2];
            if (px < xmin || px > xmax) continue;
            if (py < ymin || py > ymax) continue;
            if (pz < zmin - z_slack || pz > zmax) continue;
            add_edge(m, &cap_edges, p, dst_node, 0.0f, NAV_EDGE_TELEPORT, 0, NAV_PHASE_TELE_NEAR);
            // Tag the node so debug viz colour-codes it; the anchor
            // was unseated but in effect this point IS the source.
            if (m->points[p].kind < NAV_NODE_TELEPORT_SRC)
                m->points[p].kind = NAV_NODE_TELEPORT_SRC;
            teleport_edges++;
            linked_here++;
        }
        if (linked_here == 0) {
            char dbg[200];
            snprintf(dbg, sizeof(dbg),
                "sim_nav: WARN tele_src bbox (%.0f %.0f %.0f)..(%.0f %.0f %.0f) "
                "has no baked nodes inside — orphan island stays disconnected\n",
                xmin, ymin, zmin, xmax, ymax, zmax);
            eng->Con_Print(dbg);
        } else {
            char dbg[160];
            snprintf(dbg, sizeof(dbg),
                "sim_nav: tele_src bbox linked %d entry node(s) -> dst\n",
                linked_here);
            eng->Con_Print(dbg);
        }
    }

    // --- Phase 4.5: lift edges -------------------------------------------
    // Each func_plat produced two anchors (TOP / BOTTOM) at the player
    // standing positions. Connect each to nearby floor nodes (so the
    // surrounding level can walk onto the lift) and add a bidirectional
    // ride edge between top and bottom.
    int plat_link_edges = 0;
    int plat_ride_edges = 0;
    for (int i = 0; i < anchor_n; i++) {
        if (anchors[i].kind != ANCHOR_PLAT_TOP) continue;
        if (anchors[i].node_index < 0) continue;

        int top_idx = anchors[i].node_index;
        int bot_idx = -1;
        // Find matching BOTTOM anchor by entity pointer.
        for (int j = 0; j < anchor_n; j++) {
            if (anchors[j].kind != ANCHOR_PLAT_BOTTOM) continue;
            if (anchors[j].entity != anchors[i].entity) continue;
            bot_idx = anchors[j].node_index;
            break;
        }
        if (bot_idx < 0) continue;

        // Ride edge: bidirectional, cost = vertical travel + small bias
        // (a real ride takes ~lift_speed seconds). 1 unit per unit is
        // fine; the bot will still prefer walking when it's shorter.
        float ride_dz = m->points[top_idx].pos[2] - m->points[bot_idx].pos[2];
        if (ride_dz < 0) ride_dz = -ride_dz;
        float ride_cost = ride_dz + 32.0f;
        add_edge(m, &cap_edges, top_idx, bot_idx, ride_cost, NAV_EDGE_PLAT_RIDE, 0, NAV_PHASE_LIFT_RIDE);
        add_edge(m, &cap_edges, bot_idx, top_idx, ride_cost, NAV_EDGE_PLAT_RIDE, 0, NAV_PHASE_LIFT_RIDE);
        plat_ride_edges += 2;

        // Link top/bottom anchors to floor nodes near their XY, at a
        // similar Z. The bbox center XY is where the player stands;
        // they walk onto the lift from neighbouring floor of the same
        // level. Use a fairly generous XY radius to catch the doorway
        // approaches.
        edict_t *plat = anchors[i].entity;
        float half_x = (plat->v.maxs[0] - plat->v.mins[0]) * 0.5f;
        float half_y = (plat->v.maxs[1] - plat->v.mins[1]) * 0.5f;
        float r_xy   = (half_x > half_y ? half_x : half_y) + 64.0f;
        float r_xy2  = r_xy * r_xy;
        const float r_z = 32.0f;   // same-floor tolerance

        // Targetname'd plats START at TOP and only descend when their
        // .use is called (typically by a button). For these, the
        // bottom anchor cannot be reached by just walking into the
        // shaft — the player must first press the button. We route
        // accordingly: plat_top links to surrounding upper-level
        // floor as usual, plat_bottom is linked ONLY via the button
        // anchor(s) that target this plat. This forces A* to route
        // through the button before "boarding" the lift.
        int targetname_driven = (plat->v.targetname && plat->v.targetname[0]);

        // 1) plat_top: link to surrounding upper-level floor nodes.
        for (int p = 0; p < m->point_count; p++) {
            if (p == top_idx || p == bot_idx) continue;
            float dx = m->points[p].pos[0] - m->points[top_idx].pos[0];
            float dy = m->points[p].pos[1] - m->points[top_idx].pos[1];
            float dz = m->points[p].pos[2] - m->points[top_idx].pos[2];
            if (dx*dx + dy*dy > r_xy2) continue;
            if (dz < -r_z || dz > r_z) continue;
            if (!trace_link_clear_plat(m->points[p].pos, m->points[top_idx].pos, plat)) continue;
            float w = sqrtf(dx*dx + dy*dy) + 1.f;
            add_edge(m, &cap_edges, p, top_idx, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
            add_edge(m, &cap_edges, top_idx, p, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
            plat_link_edges += 2;
        }

        // 2) plat_bottom: for free (touch-trigger) plats, link to the
        //    surrounding lower floor as usual. For button-driven plats,
        //    skip the floor link and use only button→plat_bottom edges.
        if (!targetname_driven) {
            for (int p = 0; p < m->point_count; p++) {
                if (p == top_idx || p == bot_idx) continue;
                float dx = m->points[p].pos[0] - m->points[bot_idx].pos[0];
                float dy = m->points[p].pos[1] - m->points[bot_idx].pos[1];
                float dz = m->points[p].pos[2] - m->points[bot_idx].pos[2];
                if (dx*dx + dy*dy > r_xy2) continue;
                if (dz < -r_z || dz > r_z) continue;
                if (!trace_link_clear_plat(m->points[p].pos, m->points[bot_idx].pos, plat)) continue;
                float w = sqrtf(dx*dx + dy*dy) + 1.f;
                add_edge(m, &cap_edges, p, bot_idx, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
                add_edge(m, &cap_edges, bot_idx, p, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
                plat_link_edges += 2;
            }
        } else {
            // Find any "activator" entity whose target matches this plat's
            // targetname. Activators come in two flavours:
            //   * touch-buttons (func_button with health == 0) — bot walks
            //     into the bbox, edge is PLAT_LINK (walk-through).
            //   * shoot-activators (func_button with health > 0, or
            //     trigger_multiple / trigger_once with health > 0) — bot
            //     stands at the anchor and fires until the entity is
            //     killed. Edge is SHOOT_LINK; the bot's drive layer
            //     locates the shootable by proximity to the source node.
            int button_links = 0;
            int shoot_links  = 0;
            edict_t *bt = eng->ED_Next(g->world);
            while (bt) {
                const char *bcn = bt->v.classname;
                if (bcn && bt->v.target &&
                    !strcmp(bt->v.target, plat->v.targetname))
                {
                    int is_button = !strcmp(bcn, "func_button");
                    int is_shoot_trig = ((int)bt->v.health > 0) &&
                        (!strcmp(bcn, "trigger_multiple") ||
                         !strcmp(bcn, "trigger_once"));
                    int is_shoot_button = is_button && ((int)bt->v.health > 0);
                    int is_touch_button = is_button && ((int)bt->v.health == 0);
                    if (is_touch_button || is_shoot_trig || is_shoot_button) {
                        for (int k = 0; k < anchor_n; k++) {
                            if (anchors[k].entity != bt) continue;
                            if (anchors[k].node_index < 0) continue;
                            unsigned char ek = is_touch_button
                                ? NAV_EDGE_BUTTON_LINK
                                : NAV_EDGE_SHOOT_LINK;
                            add_edge(m, &cap_edges, anchors[k].node_index,
                                     bot_idx, 16.0f, ek, 0, NAV_PHASE_LIFT_BUTTON_SHOOT);
                            if (is_touch_button) button_links++;
                            else                 shoot_links++;
                            break;
                        }
                    }
                }
                bt = eng->ED_Next(bt);
            }
            char dbg[200];
            snprintf(dbg, sizeof(dbg),
                "sim_nav: plat '%s' targeted by %d touch-button(s), "
                "%d shoot-activator(s)\n",
                plat->v.targetname, button_links, shoot_links);
            eng->Con_Print(dbg);
            plat_link_edges += button_links + shoot_links;

            // Disembark: after riding the lift, the bot needs to step
            // OFF onto the surrounding lower floor. Add plat_bottom →
            // lower_floor edges (one-way OFF the lift). The reverse
            // direction is deliberately omitted so A* can't walk onto
            // the lift from below without an activator.
            int disembark_edges = 0;
            for (int p = 0; p < m->point_count; p++) {
                if (p == top_idx || p == bot_idx) continue;
                float dx = m->points[p].pos[0] - m->points[bot_idx].pos[0];
                float dy = m->points[p].pos[1] - m->points[bot_idx].pos[1];
                float dz = m->points[p].pos[2] - m->points[bot_idx].pos[2];
                if (dx*dx + dy*dy > r_xy2) continue;
                if (dz < -r_z || dz > r_z) continue;
                if (!trace_link_clear_plat(m->points[p].pos, m->points[bot_idx].pos, plat)) continue;
                float w = sqrtf(dx*dx + dy*dy) + 1.f;
                add_edge(m, &cap_edges, bot_idx, p, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
                disembark_edges++;
            }
            plat_link_edges += disembark_edges;
        }
    }
    if (plat_ride_edges || plat_link_edges) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "sim_nav: lifts: %d ride edges + %d floor-link edges\n",
            plat_ride_edges, plat_link_edges);
        eng->Con_Print(buf);
    }

    // --- Phase 4.6: key-locked door tagging ------------------------------
    // For each func_door (and func_door_secret) whose .items field is set,
    // tag every edge crossing the door's absolute bbox with that item
    // bitmask. A* will skip such edges unless the player carries the key.
    {
        int locked_edges_tagged = 0;
        edict_t *de = eng->ED_Next(g->world);
        while (de) {
            const char *cn = de->v.classname;
            if (cn && (!strcmp(cn, "func_door") || !strcmp(cn, "func_door_secret"))
                && de->v.items != 0)
            {
                unsigned int req = (unsigned int)de->v.items;
                float xmn = de->v.origin[0] + de->v.mins[0] - 8;
                float ymn = de->v.origin[1] + de->v.mins[1] - 8;
                float zmn = de->v.origin[2] + de->v.mins[2] - 8;
                float xmx = de->v.origin[0] + de->v.maxs[0] + 8;
                float ymx = de->v.origin[1] + de->v.maxs[1] + 8;
                float zmx = de->v.origin[2] + de->v.maxs[2] + 8;
                for (int k = 0; k < m->edge_count; k++) {
                    nav_edge_t *e = &m->edges[k];
                    float ax = m->points[e->from].pos[0];
                    float ay = m->points[e->from].pos[1];
                    float az = m->points[e->from].pos[2];
                    float bx = m->points[e->to].pos[0];
                    float by = m->points[e->to].pos[1];
                    float bz = m->points[e->to].pos[2];
                    float mx = (ax + bx) * 0.5f;
                    float my = (ay + by) * 0.5f;
                    float mz = (az + bz) * 0.5f;
                    // Edge counts as door-crossing if its midpoint lies
                    // within the (slightly expanded) door bbox, OR if
                    // either endpoint does. (Cheap aabb test rather
                    // than a full segment-vs-aabb intersection.)
                    int hit = 0;
                    if (mx >= xmn && mx <= xmx && my >= ymn && my <= ymx &&
                        mz >= zmn && mz <= zmx) hit = 1;
                    else if (ax >= xmn && ax <= xmx && ay >= ymn && ay <= ymx &&
                             az >= zmn && az <= zmx) hit = 1;
                    else if (bx >= xmn && bx <= xmx && by >= ymn && by <= ymx &&
                             bz >= zmn && bz <= zmx) hit = 1;
                    if (hit) {
                        e->requires_items |= req;
                        locked_edges_tagged++;
                    }
                }
            }
            de = eng->ED_Next(de);
        }
        if (locked_edges_tagged > 0) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                "sim_nav: %d edges tagged with key requirements\n",
                locked_edges_tagged);
            eng->Con_Print(buf);
        }
    }

    {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "sim_nav: bake %d nodes, %d edges (%d walk, %d jump, "
                 "%d drop, %d teleport, %d iters)\n",
                 m->point_count, m->edge_count,
                 m->edge_count - jump_edges_added - drop_edges_added - teleport_edges,
                 jump_edges_added, drop_edges_added, teleport_edges, iters);
        eng->Con_Print(buf);
    }

    // Tag histogram for debug-overlay color coding.
    {
        int kc[8] = {0};
        for (int i = 0; i < m->point_count; i++) {
            int k = m->points[i].kind;
            if (k >= 0 && k < 8) kc[k]++;
        }
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "sim_nav: tagged %d spawn, %d exit, %d item, %d monster, "
                 "%d tele_src, %d tele_dst, %d door/button\n",
                 kc[NAV_NODE_PLAYER_SPAWN], kc[NAV_NODE_EXIT],
                 kc[NAV_NODE_ITEM], kc[NAV_NODE_MONSTER],
                 kc[NAV_NODE_TELEPORT_SRC], kc[NAV_NODE_TELEPORT_DST],
                 kc[NAV_NODE_DOOR_BUTTON]);
        eng->Con_Print(buf);
    }

    // Connected-components diagnostic: how many islands did we end up with,
    // and how big is the biggest? Treat walk edges as bidirectional for this
    // purpose (the BFS may have added an edge in only one direction, but for
    // reachability analysis we want undirected). Teleport edges are directed
    // but counted here anyway so we report the *intended* reachability.
    {
        int *comp = calloc(m->point_count, sizeof(int));
        int *stk  = malloc(sizeof(int) * m->point_count);
        if (comp && stk) {
            int n_comp = 0, biggest = 0;
            for (int s = 0; s < m->point_count; s++) {
                if (comp[s]) continue;
                n_comp++;
                int top = 0;
                stk[top++] = s;
                comp[s] = n_comp;
                int size = 0;
                while (top > 0) {
                    int v = stk[--top];
                    size++;
                    for (int k = 0; k < m->edge_count; k++) {
                        if (m->edges[k].from == v && !comp[m->edges[k].to]) {
                            comp[m->edges[k].to] = n_comp;
                            stk[top++] = m->edges[k].to;
                        } else if (m->edges[k].to == v && !comp[m->edges[k].from]) {
                            comp[m->edges[k].from] = n_comp;
                            stk[top++] = m->edges[k].from;
                        }
                    }
                }
                if (size > biggest) biggest = size;
            }
            char buf[240];
            snprintf(buf, sizeof(buf),
                     "sim_nav: connectivity (undirected) %d components, largest=%d (%.0f%%)\n",
                     n_comp, biggest,
                     100.0 * biggest / (m->point_count > 0 ? m->point_count : 1));
            eng->Con_Print(buf);

            // Directed reachability from each player-spawn node. This is
            // what A* actually sees — undirected connectivity oversells
            // it when one-way edges (drops, teleports, plat-links from
            // the button-driven branch) prevent traversing back.
            // Adjacency wasn't built until after bake_floodfill returned;
            // build it now so the directed BFS has indexes to walk.
            build_adjacency(m);
            {
                char *reached = calloc(m->point_count, 1);
                int  *queue   = malloc(sizeof(int) * m->point_count);
                if (reached && queue) {
                    int spawn_node = -1;
                    for (int i = 0; i < m->point_count; i++) {
                        if (m->points[i].kind == NAV_NODE_PLAYER_SPAWN) {
                            spawn_node = i; break;
                        }
                    }
                    if (spawn_node >= 0) {
                        int qh = 0, qt = 0;
                        queue[qt++] = spawn_node;
                        reached[spawn_node] = 1;
                        while (qh < qt) {
                            int v = queue[qh++];
                            int o0 = m->adj_offsets[v];
                            int o1 = m->adj_offsets[v + 1];
                            for (int k = o0; k < o1; k++) {
                                const nav_edge_t *e = &m->edges[m->adj[k]];
                                if (e->from != v) continue;   // directed
                                if (reached[e->to]) continue;
                                reached[e->to] = 1;
                                queue[qt++] = e->to;
                            }
                        }
                        int reached_n = 0;
                        int reached_exit = 0;
                        for (int i = 0; i < m->point_count; i++) {
                            if (reached[i]) {
                                reached_n++;
                                if (m->points[i].kind == NAV_NODE_EXIT)
                                    reached_exit = 1;
                            }
                        }
                        snprintf(buf, sizeof(buf),
                            "sim_nav: directed-from-spawn: %d/%d (%.0f%%) reachable, "
                            "exit-reachable=%s\n",
                            reached_n, m->point_count,
                            100.0 * reached_n / m->point_count,
                            reached_exit ? "yes" : "NO");
                        eng->Con_Print(buf);
                    }
                }
                free(reached);
                free(queue);
            }
            // Per-component bbox so we can tell which physical region
            // each island represents.
            for (int c = 1; c <= n_comp && c <= 6; c++) {
                float bxmn=1e9f, bymn=1e9f, bzmn=1e9f;
                float bxmx=-1e9f, bymx=-1e9f, bzmx=-1e9f;
                int sz = 0;
                for (int i = 0; i < m->point_count; i++) {
                    if (comp[i] != c) continue;
                    sz++;
                    if (m->points[i].pos[0] < bxmn) bxmn = m->points[i].pos[0];
                    if (m->points[i].pos[1] < bymn) bymn = m->points[i].pos[1];
                    if (m->points[i].pos[2] < bzmn) bzmn = m->points[i].pos[2];
                    if (m->points[i].pos[0] > bxmx) bxmx = m->points[i].pos[0];
                    if (m->points[i].pos[1] > bymx) bymx = m->points[i].pos[1];
                    if (m->points[i].pos[2] > bzmx) bzmx = m->points[i].pos[2];
                }
                snprintf(buf, sizeof(buf),
                    "sim_nav:  component %d: %d nodes, "
                    "xy (%.0f %.0f)..(%.0f %.0f) z (%.0f..%.0f)\n",
                    c, sz, bxmn, bymn, bxmx, bymx, bzmn, bzmx);
                eng->Con_Print(buf);
            }
        }
        free(comp);
        free(stk);
    }

    // Restore every entity's solid value before we hand control back to
    // the engine — otherwise monsters would be untouchable and doors /
    // plats / buttons inert for the rest of the level.
    for (int i = 0; i < solid_saves_n; i++) {
        solid_saves[i].e->v.solid = solid_saves[i].solid;
    }
    free(solid_saves);

    eng->ED_Free(probe);
    free(queue);
    free(anchors);
    grid_free(&grd);
    return m->point_count > 0;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_Nav_Init(void) {
    s_mesh  = 0;
    s_ready = 0;
    eng->Cvar_Register("sim_nav_debug", "0");
    eng->Cvar_Register("sim_nav_ztest", "0");
    // Edges with both endpoints beyond this radius (Quake units) from the
    // camera are skipped before submission. 0 disables culling. Default
    // 1024 keeps a generous view radius while bounding draw cost on dense
    // meshes (e1m1: ~6k nodes / 60k edges).
    eng->Cvar_Register("sim_nav_debug_range", "1024");
    // Bitmask over NAV_PHASE_* — bit N selects phase N. Default 0xFF
    // shows all phases. Special-case: a literal 0 also means "show
    // all" so accidental `set sim_nav_debug_phase_mask 0` doesn't
    // make the overlay vanish.
    eng->Cvar_Register("sim_nav_debug_phase_mask", "255");
}

int            Sim_Nav_IsReady(void) { return s_ready; }
sim_navmesh_t *Sim_Nav_Get(void)     { return s_mesh; }

void Sim_Nav_Frame(void) {
    if (!s_ready || !s_mesh) return;
    if (eng->Cvar_VariableValue("sim_nav_debug") <= 0.0f) return;
    int ztest = eng->Cvar_VariableValue("sim_nav_ztest") > 0.0f;

    // Distance cull: dropping the cross-DLL SV_DebugLine call entirely for
    // far edges saves both submission cost and downstream draw cost. We
    // mask in-range once per node (O(N)) instead of testing each edge's
    // endpoints (O(E)) since E ≈ 9N on a typical bake.
    float range     = eng->Cvar_VariableValue("sim_nav_debug_range");
    int   has_range = (range > 0.0f);
    float r2        = range * range;
    vec3_t cam;
    unsigned char in_range[MAX_NODES];
    if (has_range) {
        eng->Get_ViewOrigin(cam);
        int np = s_mesh->point_count;
        if (np > MAX_NODES) np = MAX_NODES;
        for (int i = 0; i < np; i++) {
            float dx = s_mesh->points[i].pos[0] - cam[0];
            float dy = s_mesh->points[i].pos[1] - cam[1];
            float dz = s_mesh->points[i].pos[2] - cam[2];
            in_range[i] = (dx*dx + dy*dy + dz*dz) <= r2;
        }
    }

    // Phase mask filter — independent of the kind-based colouring applied
    // below. Mask defaults to 0xFF (all); treat literal 0 as "all" too so
    // the overlay can't be accidentally blanked. Hoisted out of the loop
    // to match the range/ztest pattern; on e1m1 the loop has ~60k iters
    // and a cross-DLL Cvar_VariableValue per iter is wasted work.
    unsigned phase_mask = (unsigned)eng->Cvar_VariableValue("sim_nav_debug_phase_mask");
    if (phase_mask == 0) phase_mask = 0xFFu;

    for (int i = 0; i < s_mesh->edge_count; i++) {
        nav_edge_t *e = &s_mesh->edges[i];
        if (!(phase_mask & (1u << e->phase))) continue;
        if (has_range && !in_range[e->from] && !in_range[e->to]) continue;

        // Dedupe bidirectional pairs in the visualization. The bake stores
        // each direction independently (walk A->B and B->A; jump-up i->j and
        // drop-down j->i for the same vertical pair), so without dedupe every
        // undirected link is drawn twice on top of itself. Keep the
        // lower-`from` direction; one-way edges (teleporters) still draw
        // because they have no mirror in the data.
        if (e->from > e->to) {
            int o0 = s_mesh->adj_offsets[e->to];
            int o1 = s_mesh->adj_offsets[e->to + 1];
            int has_reverse = 0;
            for (int k = o0; k < o1; k++) {
                if (s_mesh->edges[s_mesh->adj[k]].to == e->from) {
                    has_reverse = 1;
                    break;
                }
            }
            if (has_reverse) continue;
        }

        // Edge colour-coded by traversal kind so the overlay reveals
        // what the bot would have to do to use the link:
        //   WALK         -> 244 (orange)
        //   JUMP_UP      -> 251 (red)
        //   DROP_DOWN    -> 254 (bright yellow)
        //   PLAT_RIDE    -> 220 (light blue)
        //   TELEPORT     -> 192 (white)
        //   PLAT_LINK    -> 79  (green)
        //   SHOOT_LINK   -> 232 (purple)
        //   BUTTON_LINK  -> 208 (cyan)
        int color;
        switch (e->kind) {
        case NAV_EDGE_JUMP_UP:     color = 251; break;
        case NAV_EDGE_DROP_DOWN:   color = 254; break;
        case NAV_EDGE_PLAT_RIDE:   color = 220; break;
        case NAV_EDGE_TELEPORT:    color = 192; break;
        case NAV_EDGE_PLAT_LINK:   color = 79;  break;
        case NAV_EDGE_SHOOT_LINK:  color = 232; break;
        case NAV_EDGE_BUTTON_LINK: color = 208; break;
        case NAV_EDGE_WALK:
        default:                   color = 244; break;
        }
        eng->SV_DebugLine(s_mesh->points[e->from].pos,
                          s_mesh->points[e->to].pos,
                          color,
                          ztest);
    }

    // Per-node markers — short vertical bars at points of interest. Drawn
    // after edges so they sit on top in screen-space order. Shared colors:
    //   spawn + exit  -> 79  (green)
    //   item          -> 254 (bright yellow)
    //   monster       -> 251 (red)
    //   teleport src/dst + door/button -> 220 (light blue)
    for (int i = 0; i < s_mesh->point_count; i++) {
        int k = s_mesh->points[i].kind;
        if (k == NAV_NODE_GENERIC) continue;
        if (has_range && !in_range[i]) continue;
        int color;
        switch (k) {
        case NAV_NODE_PLAYER_SPAWN:
        case NAV_NODE_EXIT:           color = 79;  break;
        case NAV_NODE_ITEM:           color = 254; break;
        case NAV_NODE_MONSTER:        color = 251; break;
        case NAV_NODE_TELEPORT_SRC:
        case NAV_NODE_TELEPORT_DST:
        case NAV_NODE_DOOR_BUTTON:    color = 220; break;
        default: continue;
        }
        vec3_t top = { s_mesh->points[i].pos[0],
                       s_mesh->points[i].pos[1],
                       s_mesh->points[i].pos[2] + 32.0f };
        eng->SV_DebugLine(s_mesh->points[i].pos, top, color, ztest);
    }
}

void Sim_Nav_LevelInit(const char *mapname) {
    if (s_mesh) { free_mesh(s_mesh); s_mesh = 0; }
    s_ready = 0;
    if (!mapname || !*mapname) return;

    char bsp_vfs_path[256];
    char nav_path[256];
    snprintf(bsp_vfs_path, sizeof(bsp_vfs_path), "maps/%s.bsp", mapname);

    int bsp_sz = 0;
    void *bsp_raw = eng->LoadFile(bsp_vfs_path, &bsp_sz);
    if (!bsp_raw) {
        eng->Con_Print("sim_nav: bsp not found\n");
        return;
    }
    free(bsp_raw);   // We only needed the size for the cache key.

    snprintf(nav_path, sizeof(nav_path),
             "id1/cache/navmesh/%s-%d.nav", mapname, bsp_sz);

    // Try cache.
    s_mesh = load_mesh(nav_path);
    if (s_mesh) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "sim_nav: loaded %d pts %d edges from cache\n",
                 s_mesh->point_count, s_mesh->edge_count);
        eng->Con_Print(buf);
        push_to_imgui(s_mesh);
        s_ready = 1;
        return;
    }

    // Bake fresh. Sim_LevelInit is called from two places: once via
    // spawn.c's "first entity_spawn" map-change hook (which runs BEFORE
    // ED_LoadFromFile has actually populated the entities lump), and again
    // from Sim_Frame's map-change check during the first SV_Physics tick
    // (by which point all entities exist). The first call collects zero
    // anchors and returns silently; the second produces the real mesh.
    s_mesh = calloc(1, sizeof(*s_mesh));
    if (!s_mesh) return;

    if (!bake_floodfill(s_mesh)) {
        free_mesh(s_mesh);
        s_mesh = 0;
        return;
    }
    // build_adjacency is already called inside bake_floodfill (so the
    // directed-reachability diagnostic can walk it); calling it again
    // would leak the existing adj/adj_offsets allocations.

    sim_mkdir("id1/cache");
    sim_mkdir("id1/cache/navmesh");
    save_mesh(nav_path, s_mesh);

    push_to_imgui(s_mesh);
    s_ready = 1;
}

// ---------------------------------------------------------------------------
// A* — small open-set, no priority queue. O(N^2) per step; fine for ~few k.
// ---------------------------------------------------------------------------
static int nearest_point(const sim_navmesh_t *m, const vec3_t pos) {
    int   best    = -1;
    float best_d2 = 1e18f;
    for (int i = 0; i < m->point_count; i++) {
        float dx = m->points[i].pos[0] - pos[0];
        float dy = m->points[i].pos[1] - pos[1];
        float dz = m->points[i].pos[2] - pos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static float dist3(const vec3_t a, const vec3_t b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)sqrt(dx*dx + dy*dy + dz*dz);
}

// Binary min-heap over (fscore, node_index). Lazy decrease-key: on relax we
// push a fresh entry; when popping, stale entries are recognised by `closed`
// and skipped. Worst-case push count is one per edge relaxation (~E), which
// caps heap size at E + 1 entries.
typedef struct { float f; int i; } pq_entry_t;

static void pq_push(pq_entry_t *h, int *n, float f, int i) {
    int c = (*n)++;
    h[c].f = f; h[c].i = i;
    while (c > 0) {
        int p = (c - 1) >> 1;
        if (h[p].f <= h[c].f) break;
        pq_entry_t t = h[p]; h[p] = h[c]; h[c] = t;
        c = p;
    }
}

static pq_entry_t pq_pop(pq_entry_t *h, int *n) {
    pq_entry_t r = h[0];
    h[0] = h[--(*n)];
    int c = 0;
    for (;;) {
        int l = 2*c + 1, rr = 2*c + 2, s = c;
        if (l  < *n && h[l ].f < h[s].f) s = l;
        if (rr < *n && h[rr].f < h[s].f) s = rr;
        if (s == c) break;
        pq_entry_t t = h[s]; h[s] = h[c]; h[c] = t;
        c = s;
    }
    return r;
}

int Sim_Nav_PathTo(const vec3_t from, const vec3_t to,
                   vec3_t *out, unsigned char *out_kinds,
                   unsigned int player_items, int max_out)
{
    if (!s_mesh || !s_ready) return 0;
    int start = nearest_point(s_mesh, from);
    int goal  = nearest_point(s_mesh, to);
    if (start < 0 || goal < 0) return 0;
    if (start == goal) {
        if (max_out >= 1) {
            out[0][0] = s_mesh->points[goal].pos[0];
            out[0][1] = s_mesh->points[goal].pos[1];
            out[0][2] = s_mesh->points[goal].pos[2];
            if (out_kinds) out_kinds[0] = NAV_EDGE_WALK;
        }
        return 1;
    }

    int    N      = s_mesh->point_count;
    int    E      = s_mesh->edge_count;
    int    heap_cap = E + 4;   // +slack so the initial seed always fits
    float *gscore = malloc(sizeof(float) * N);
    float *fscore = malloc(sizeof(float) * N);
    int   *came   = malloc(sizeof(int)   * N);
    unsigned char *came_kind = malloc(N);
    char  *closed = calloc(N, 1);
    pq_entry_t *heap = malloc(sizeof(pq_entry_t) * heap_cap);
    int    heap_n = 0;
    if (!gscore || !fscore || !came || !came_kind || !closed || !heap) {
        free(gscore); free(fscore); free(came); free(came_kind); free(closed); free(heap);
        return 0;
    }
    for (int i = 0; i < N; i++) {
        gscore[i] = 1e18f; fscore[i] = 1e18f; came[i] = -1; came_kind[i] = NAV_EDGE_WALK;
    }

    gscore[start] = 0;
    fscore[start] = dist3(s_mesh->points[start].pos, s_mesh->points[goal].pos);
    pq_push(heap, &heap_n, fscore[start], start);

    int found = 0;
    while (heap_n > 0) {
        pq_entry_t top = pq_pop(heap, &heap_n);
        int cur = top.i;
        if (closed[cur]) continue;      // stale entry — better one was popped first
        if (cur == goal) { found = 1; break; }
        closed[cur] = 1;

        int o0 = s_mesh->adj_offsets[cur];
        int o1 = s_mesh->adj_offsets[cur + 1];
        for (int k = o0; k < o1; k++) {
            const nav_edge_t *e = &s_mesh->edges[s_mesh->adj[k]];
            // Locked-edge filter: skip if any required item bit is
            // missing from the caller's item set.
            if (e->requires_items & ~player_items) continue;
            int nb = e->to;
            if (closed[nb]) continue;
            float tentative = gscore[cur] + e->weight;
            if (tentative < gscore[nb]) {
                came[nb]      = cur;
                came_kind[nb] = e->kind;
                gscore[nb] = tentative;
                fscore[nb] = tentative + dist3(s_mesh->points[nb].pos, s_mesh->points[goal].pos);
                pq_push(heap, &heap_n, fscore[nb], nb);
            }
        }
    }

    int written = 0;
    if (found) {
        int tmp[1024];
        int tn = 0;
        int c  = goal;
        while (c != -1 && tn < 1024) { tmp[tn++] = c; c = came[c]; }
        for (int i = tn - 1; i >= 0 && written < max_out; i--) {
            out[written][0] = s_mesh->points[tmp[i]].pos[0];
            out[written][1] = s_mesh->points[tmp[i]].pos[1];
            out[written][2] = s_mesh->points[tmp[i]].pos[2];
            // came_kind[node] is the kind of the edge that LED to that
            // node. The first emitted waypoint (start) has no incoming
            // edge, so report WALK there.
            if (out_kinds) {
                out_kinds[written] = (i == tn - 1) ? NAV_EDGE_WALK : came_kind[tmp[i]];
            }
            written++;
        }
    }

    free(gscore); free(fscore); free(came); free(came_kind); free(closed); free(heap);
    return written;
}

int Sim_Nav_EdgesNear(const float center[3], float radius,
                      sim_nav_edge_record_t *out, int max_records,
                      int *truncated_out) {
    if (truncated_out) *truncated_out = 0;
    if (!s_mesh || !s_ready || !out || max_records <= 0) return 0;
    if (radius <= 0.0f) return 0;

    float r2 = radius * radius;
    int written = 0;
    for (int i = 0; i < s_mesh->edge_count; i++) {
        const nav_edge_t *e = &s_mesh->edges[i];
        const float *a = s_mesh->points[e->from].pos;
        const float *b = s_mesh->points[e->to].pos;

        float da[3] = { a[0]-center[0], a[1]-center[1], a[2]-center[2] };
        float db[3] = { b[0]-center[0], b[1]-center[1], b[2]-center[2] };
        float ra2 = da[0]*da[0] + da[1]*da[1] + da[2]*da[2];
        float rb2 = db[0]*db[0] + db[1]*db[1] + db[2]*db[2];
        if (ra2 > r2 && rb2 > r2) continue;

        if (written >= max_records) {
            if (truncated_out) *truncated_out = 1;
            return written;   // bail early; flag set
        }
        out[written].from[0] = a[0]; out[written].from[1] = a[1]; out[written].from[2] = a[2];
        out[written].to[0]   = b[0]; out[written].to[1]   = b[1]; out[written].to[2]   = b[2];
        out[written].weight  = e->weight;
        out[written].kind    = e->kind;
        out[written].phase   = e->phase;
        written++;
    }
    return written;
}

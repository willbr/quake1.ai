// sim_ai.c -- AI brain side-table, sense filter, FSM.
// State for each monster lives in s_brains[edict_num], not in edict_t.

#include "sim.h"
#include "../game_defs.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static ai_brain_t s_brains[SIM_MAX_BRAINS];

// Forward declaration — defined at the bottom of this file.
static void Sim_Patrol_LevelInit_(void);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_AI_Init(void) {
    memset(s_brains, 0, sizeof(s_brains));
    eng->Cvar_Register("sim_sense_debug", "0");
}

void Sim_AI_LevelInit(void) {
    memset(s_brains, 0, sizeof(s_brains));
    Sim_Patrol_LevelInit_();
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
// Sense filter
// ---------------------------------------------------------------------------
static float falloff(float distance, float ref_radius) {
    float f = 1.0f - (distance / (2.0f * ref_radius));
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

static int los_clear(const vec3_t a, const vec3_t b) {
    eng->SV_Traceline(a, b, /*nomonsters=*/1, /*skip=*/0);
    return g->trace_fraction == 1.0f;
}

static float sense_intensity(ai_brain_t *b, edict_t *e, const stimulus_t *s) {
    float d = 0;
    {
        float dx = s->origin[0] - e->v.origin[0];
        float dy = s->origin[1] - e->v.origin[1];
        float dz = s->origin[2] - e->v.origin[2];
        d = (float)sqrt(dx*dx + dy*dy + dz*dz);
    }
    float ref = 0;
    float los = 1.0f;
    switch (s->kind) {
        case STIM_SOUND:
            ref = 1024.0f * b->sense_hearing_mult; break;
        case STIM_SIGHT_ENTITY:
            ref = b->sense_sight_range;
            los = los_clear(e->v.origin, s->origin) ? 1.0f : 0.0f;
            break;
        case STIM_CORPSE:
            ref = 512.0f;
            los = los_clear(e->v.origin, s->origin) ? 1.0f : 0.0f;
            break;
        case STIM_PROP_BROKEN:
            ref = 768.0f; break;
        case STIM_LIGHT_CHANGE:
            ref = 384.0f; break;
        case STIM_SMOKE:
            ref = b->sense_sight_range; break;
        default:
            return 0.0f;
    }
    return s->intensity * falloff(d, ref) * los;
}

static void sense_tick(ai_brain_t *b, edict_t *e) {
    // Check FL_NOTARGET on player edict (set by the "notarget" console command).
    {
        edict_t *player = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == 1) { player = cur; break; }
        }
        if (player && ((int)player->v.flags & FL_NOTARGET)) {
            b->alert_level  = 0;
            b->target_edict = -1;
            if (b->state != AI_IDLE) {
                b->state              = AI_IDLE;
                b->state_entered_time = g->time;
                b->path_len           = 0;
            }
            return;
        }
    }

    stimulus_t recents[16];
    float since = b->next_tick_time - 1.0f;     // 1s lookback overlap
    int n = Stim_QueryNear(e->v.origin,
                           b->sense_sight_range * 2.0f,
                           since, recents, 16);

    for (int i = 0; i < n; i++) {
        // Don't react to your own emissions.
        if (recents[i].source_edict == b->edict_num) continue;
        float eff = sense_intensity(b, e, &recents[i]);
        b->alert_level += eff * 0.5f;
    }

    // Decay
    b->alert_level *= 0.95f;
    if (b->alert_level < 0)    b->alert_level = 0;
    if (b->alert_level > 1.0f) b->alert_level = 1.0f;

    // Track strongest sight stim this tick for FSM bookkeeping.
    int    saw_player_full = 0;
    vec3_t player_pos = {0, 0, 0};

    for (int i = 0; i < n; i++) {
        if (recents[i].source_edict == b->edict_num) continue;
        if (recents[i].kind != STIM_SIGHT_ENTITY) continue;
        if (recents[i].source_edict != 1) continue;   // edict 1 = player in single-player
        float eff = sense_intensity(b, e, &recents[i]);
        if (eff > 0.7f) {
            saw_player_full = 1;
            player_pos[0] = recents[i].origin[0];
            player_pos[1] = recents[i].origin[1];
            player_pos[2] = recents[i].origin[2];
        }
    }

    if (saw_player_full) {
        b->last_known_pos[0] = player_pos[0];
        b->last_known_pos[1] = player_pos[1];
        b->last_known_pos[2] = player_pos[2];
        b->target_edict = 1;
    }

    // FSM transitions
    float since_state = g->time - b->state_entered_time;
    ai_state_t prev = b->state;

    switch (b->state) {
    case AI_IDLE:
        if (b->alert_level > 0.25f) b->state = AI_SUSPICIOUS;
        break;
    case AI_SUSPICIOUS:
        if (saw_player_full || b->alert_level > 0.6f) b->state = AI_SEARCHING;
        else if (n == 0 && since_state > 8.0f && b->alert_level < 0.05f) b->state = AI_IDLE;
        break;
    case AI_SEARCHING:
        if (saw_player_full && los_clear(e->v.origin, b->last_known_pos))
            b->state = AI_COMBAT;
        else if (n == 0 && since_state > 20.0f) b->state = AI_IDLE;
        break;
    case AI_COMBAT:
        if (!saw_player_full && since_state > 3.0f) b->state = AI_SEARCHING;
        break;
    }

    if (b->state != prev) {
        b->state_entered_time = g->time;
        if (b->state == AI_SEARCHING) {
            b->path_len = 0;
            b->path_idx = 0;
            // Force an immediate replan on first SEARCHING tick — without
            // this, a stale future-dated path_replan_time from a previous
            // SEARCHING bout would delay the search by up to 2 s.
            b->path_replan_time = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Behavior helpers
// ---------------------------------------------------------------------------
static void face_point(edict_t *e, const vec3_t target) {
    float dx = target[0] - e->v.origin[0];
    float dy = target[1] - e->v.origin[1];
    vec3_t v = {dx, dy, 0};
    e->v.ideal_yaw = eng->VectorToYaw(v);
    eng->SV_ChangeYaw(e);
}

static void behavior_tick(ai_brain_t *b, edict_t *e) {
    switch (b->state) {
    case AI_IDLE: {
        if (b->patrol_route_id < 0) break;
        // Look up current arena-patrol node (route, idx).
        edict_t *node = Sim_Patrol_FindArenaNode(b->patrol_route_id, b->patrol_node_idx);
        if (!node) break;
        float dx = node->v.origin[0] - e->v.origin[0];
        float dy = node->v.origin[1] - e->v.origin[1];
        float dz = node->v.origin[2] - e->v.origin[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < 32*32) {
            // Arrived at node — advance to next.
            b->patrol_node_idx++;
            // Wrap if next node not found.
            if (!Sim_Patrol_FindArenaNode(b->patrol_route_id, b->patrol_node_idx))
                b->patrol_node_idx = 0;
            break;
        }
        face_point(e, node->v.origin);
        eng->SV_WalkMove(e, e->v.angles[1], 12.0f);
        return;  // suppress vanilla AI walk this tick
    }
    case AI_SUSPICIOUS:
        face_point(e, b->last_known_pos);
        eng->SV_WalkMove(e, e->v.angles[1], 8.0f);
        break;
    case AI_SEARCHING: {
        // Replan when the replan timer expires. Advance the timer up front so
        // a failing search (path_len stays 0) does not re-trigger every brain
        // tick — that was hammering A* at 10 Hz when a brain entered SEARCHING
        // on a goal it couldn't reach (e.g. orphan teleporter destinations).
        if (g->time > b->path_replan_time) {
            b->path_replan_time = g->time + 2.0f;
            b->path_len = Sim_Nav_PathTo(e->v.origin, b->last_known_pos,
                                         b->path_pts, 32);
            b->path_idx = 0;
        }
        if (b->path_len == 0) {
            // No path (yet) — stand-and-sweep at last_known_pos.
            face_point(e, b->last_known_pos);
            eng->SV_WalkMove(e, e->v.angles[1], 8.0f);
            break;
        }
        if (b->path_idx >= b->path_len) {
            // Reached the destination — sweep until state times out.
            face_point(e, b->last_known_pos);
            break;
        }
        const float *next = b->path_pts[b->path_idx];
        float dx = next[0] - e->v.origin[0];
        float dy = next[1] - e->v.origin[1];
        if (dx*dx + dy*dy < 32*32) { b->path_idx++; break; }
        face_point(e, (vec3_t){ next[0], next[1], next[2] });
        eng->SV_WalkMove(e, e->v.angles[1], 12.0f);
    } break;
    case AI_COMBAT:
        // Fall through to vanilla Quake combat AI.
        break;
    }
}

// ---------------------------------------------------------------------------
// Frame tick — 10 Hz per brain; pushes live data to the imgui AI panel.
// ---------------------------------------------------------------------------
void Sim_AI_Frame(void) {
    float now = g->time;

    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (now < b->next_tick_time) continue;

        // Find the edict for this brain by walking entity list.
        // O(N^2) is acceptable for M1 with typical monster counts.
        edict_t *e = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == b->edict_num) { e = cur; break; }
        }
        if (!e) { b->in_use = 0; continue; }
        if (e->v.health <= 0) {
            b->state = AI_IDLE;
            b->alert_level = 0;
            continue;
        }

        sense_tick(b, e);
        behavior_tick(b, e);
        b->next_tick_time = now + (1.0f / SIM_AI_TICK_HZ);
    }

    // Push to imgui panel — always, so the panel shows current data the
    // moment F12 is opened without needing to wait for the next tick.
    eng->ImguiAI_Clear();
    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        eng->ImguiAI_Push(b->edict_num, (int)b->state, b->alert_level,
                          b->last_known_pos, b->target_edict);
    }

    // Push the path of the first SEARCHING brain for the nav minimap.
    if (eng->ImguiNav_SetPath) {
        int pushed = 0;
        for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
            if (b->state == AI_SEARCHING && b->path_len > 0) {
                float xy[64];
                int n = b->path_len > 32 ? 32 : b->path_len;
                for (int i = 0; i < n; i++) {
                    xy[2*i+0] = b->path_pts[i][0];
                    xy[2*i+1] = b->path_pts[i][1];
                }
                eng->ImguiNav_SetPath(xy, n);
                pushed = 1;
                break;
            }
        }
        if (!pushed) {
            float xy[2] = {0, 0};
            eng->ImguiNav_SetPath(xy, 0);
        }
    }

    // Debug: sight-cone visualization (two lines at ±45° of yaw)
    if (eng->Cvar_VariableValue("sim_sense_debug") > 0.0f) {
        static const float kPi = 3.14159265f;
        static const float kHalfFov = 3.14159265f / 4.0f;   // 45°
        for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
            edict_t *e = 0;
            for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
                if (eng->ED_GetNum(cur) == b->edict_num) { e = cur; break; }
            }
            if (!e || e->v.health <= 0) continue;

            int color = (b->state == AI_COMBAT || b->state == AI_SEARCHING) ? 79
                      : (b->state == AI_SUSPICIOUS) ? 251
                      : 112;

            float yaw = e->v.angles[1] * (kPi / 180.0f);
            float r   = b->sense_sight_range;

            vec3_t eye = { e->v.origin[0], e->v.origin[1], e->v.origin[2] + 28.0f };

            vec3_t tip1 = { eye[0] + r * (float)cos(yaw + kHalfFov),
                            eye[1] + r * (float)sin(yaw + kHalfFov),
                            eye[2] };
            vec3_t tip2 = { eye[0] + r * (float)cos(yaw - kHalfFov),
                            eye[1] + r * (float)sin(yaw - kHalfFov),
                            eye[2] };

            eng->SV_DebugLine(eye, tip1, color, 0);
            eng->SV_DebugLine(eye, tip2, color, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Patrol routes
// ---------------------------------------------------------------------------
#define SIM_MAX_PATROL_NODES 256

// Side-table for arena-spawned patrol nodes (not map-entity nodes).
// Keyed by (route, idx) rather than targetname, since arena nodes can't
// set string_t targetname via the engine API.
static struct { int route, idx; edict_t *e; } s_arena_nodes[SIM_MAX_PATROL_NODES];
static int s_arena_node_count;

static edict_t *s_patrol_nodes[SIM_MAX_PATROL_NODES];
static int      s_patrol_count;

static void Sim_Patrol_LevelInit_(void) {
    s_patrol_count = 0;
    s_arena_node_count = 0;
    for (int i = 0; i < SIM_MAX_PATROL_NODES; i++) {
        s_patrol_nodes[i] = 0;
        s_arena_nodes[i].e = 0;
    }
}

void Sim_Patrol_RegisterNode(edict_t *e) {
    if (s_patrol_count >= SIM_MAX_PATROL_NODES) return;
    s_patrol_nodes[s_patrol_count++] = e;
}

void Sim_Patrol_RegisterArenaNode(int route, int idx, edict_t *e) {
    if (s_arena_node_count >= SIM_MAX_PATROL_NODES) return;
    s_arena_nodes[s_arena_node_count].route = route;
    s_arena_nodes[s_arena_node_count].idx   = idx;
    s_arena_nodes[s_arena_node_count].e     = e;
    s_arena_node_count++;
}

void Sim_Patrol_Resolve(void) {
    // No pre-linking needed for v1. Nodes are found at runtime.
}

edict_t *Sim_Patrol_FindByTargetname(const char *name) {
    // For map-entity nodes: defer to engine's ED_Find.
    // This handles real info_patrol_node entities placed in maps.
    if (!name || !*name) return 0;
    return eng->ED_Find(g->world, "targetname", name);
}

edict_t *Sim_Patrol_FindArenaNode(int route, int idx) {
    for (int i = 0; i < s_arena_node_count; i++) {
        if (s_arena_nodes[i].route == route && s_arena_nodes[i].idx == idx)
            return s_arena_nodes[i].e;
    }
    return 0;
}

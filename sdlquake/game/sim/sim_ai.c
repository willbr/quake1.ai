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

// Shared scratch edict for SV_MoveToGoal — see walk_toward() below.
static edict_t *s_walkgoal;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_AI_Init(void) {
    memset(s_brains, 0, sizeof(s_brains));
    eng->Cvar_Register("sim_sense_debug",  "0");
    eng->Cvar_Register("sim_patrol_debug", "0");
}

void Sim_AI_LevelInit(void) {
    memset(s_brains, 0, sizeof(s_brains));
    Sim_Patrol_LevelInit_();
    s_walkgoal = 0;     // re-allocated lazily on first walk_toward call
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
            if (los_clear(e->v.origin, s->origin)) {
                // Wind/smoke occlusion (M4): reduce LOS intensity by the
                // average smoke density along the sight line.
                float occlusion = Wind_PathOcclusion(e->v.origin, s->origin);
                los = 1.0f - occlusion;
                if (los < 0) los = 0;
                // Light tier (M5): if the stim is from the player (edict 1),
                // multiply by the light tier at the player's location.
                // Shadowed alcoves halve visibility.
                if (s->source_edict == 1) {
                    los *= Light_TierAt(s->origin);
                }
            } else {
                los = 0;
            }
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
        // Always reset the path on transition -- a leftover SEARCHING
        // path leaking into IDLE would walk patrolling monsters back to
        // last_known_pos instead of the next patrol node.
        b->path_len = 0;
        b->path_idx = 0;
        // Re-kick walk mode on next walk_and_track. Vanilla AI may have
        // moved the monster to attack frames during COMBAT; coming back
        // out we want the walk animation again.
        b->walking  = 0;
        if (b->state == AI_SEARCHING) {
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

// Shared scratch edict used as a transient goalentity for SV_MoveToGoal.
// Declared at file scope above (Sim_AI_LevelInit clears it; it's
// re-allocated lazily on first use).
static edict_t *get_walkgoal(void) {
    if (s_walkgoal && !s_walkgoal->free) return s_walkgoal;
    s_walkgoal = eng->ED_Alloc();
    if (!s_walkgoal) return 0;
    s_walkgoal->v.classname = "sim_walkgoal";
    s_walkgoal->v.solid     = SOLID_NOT;
    s_walkgoal->v.movetype  = MOVETYPE_NONE;
    return s_walkgoal;
}

// Open a func_door directly in front of the monster, if any. Defined
// in doors.c so we can compare v.use against the static door_use /
// fd_secret_use_fn pointers without exporting them. Returns 1 on
// success.
extern int Doors_TryActivate(edict_t *door, edict_t *activator);

// Trace forward from `me` looking for a closed door within reach, and
// trigger it. Called when stuck-detection trips. Vanilla Quake monsters
// never opened doors -- this is a small immersive-sim courtesy so
// patrol routes stay alive when the navmesh runs through a door.
static void try_open_door_in_front(edict_t *me) {
    const float kPi = 3.14159265f;
    float yaw = me->v.angles[1] * (kPi / 180.0f);
    vec3_t origin = { me->v.origin[0], me->v.origin[1],
                      me->v.origin[2] + 20.0f };
    vec3_t end    = { origin[0] + cosf(yaw) * 80.0f,
                      origin[1] + sinf(yaw) * 80.0f,
                      origin[2] };
    eng->SV_Traceline(origin, end, /*nomonsters=*/1, /*skip=*/0);
    if (g->trace_fraction >= 1.0f) return;
    Doors_TryActivate(g->trace_ent, me);
}

// Walk one tick toward a point, using SV_MoveToGoal so blocked moves
// fall through SV_NewChaseDir's alternative-direction handling
// (corners, doorframes, pillars). SV_WalkMove just bumps the wall and
// returns 0 -- monsters get visibly stuck.
static void walk_toward(edict_t *me, const vec3_t target, float dist) {
    // Direction (2D) for ideal_yaw.
    float dx = target[0] - me->v.origin[0];
    float dy = target[1] - me->v.origin[1];
    me->v.ideal_yaw = eng->VectorToYaw((vec3_t){dx, dy, 0});

    edict_t *g = get_walkgoal();
    if (!g) {
        // Fallback if we couldn't alloc the scratch.
        eng->SV_ChangeYaw(me);
        eng->SV_WalkMove(me, me->v.angles[1], dist);
        return;
    }
    // Reposition the scratch to the current target; swap it in as
    // goalentity for the duration of this call; restore afterwards so
    // vanilla AI (walk/run states) keeps its real goalentity intact.
    vec3_t pos = { target[0], target[1], target[2] };
    eng->SV_SetOrigin(g, pos);
    edict_t *old_goal = me->v.goalentity;
    me->v.goalentity  = g;
    eng->SV_MoveToGoal(me, dist);
    me->v.goalentity  = old_goal;
}

// Brain-aware wrapper around walk_toward that detects stuck-against-door
// and triggers the door. Only counts a tick as "stuck" when the monster
// is already facing close to its target (so the rejected step wasn't
// just the normal turn-then-step delay).
#define STUCK_DISP2_MIN     (4.0f * 4.0f)    // < 4 units displacement = no progress
#define STUCK_TICKS_TRIGGER 10               // ~1 s at SIM_AI_TICK_HZ = 10
static void walk_and_track(ai_brain_t *b, edict_t *e, const vec3_t target,
                           float dist)
{
    // Kick the monster's think chain into walk mode the first time we
    // move it. The walk-frame functions advance v.frame each tick (and
    // chain self->v.think to walk2, walk3, ...), so the walk animation
    // cycles while our code does the actual translation. Vanilla
    // ai_walk early-returns (see ai.c) when the brain is in
    // IDLE+patrol / SUSPICIOUS / SEARCHING, so it doesn't double-move.
    if (!b->walking && e->v.th_walk) {
        e->v.th_walk(e);
        b->walking = 1;
    }

    float ox = e->v.origin[0], oy = e->v.origin[1];
    walk_toward(e, target, dist);
    float dx = e->v.origin[0] - ox;
    float dy = e->v.origin[1] - oy;

    // Only count as stuck if we're facing roughly the right way; otherwise
    // we're just in the turn-then-step phase and shouldn't accumulate.
    float delta = e->v.ideal_yaw - e->v.angles[1];
    while (delta >  180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    if (delta < 0) delta = -delta;

    if (dx*dx + dy*dy < STUCK_DISP2_MIN && delta < 45.0f) {
        b->stuck_ticks++;
        if (b->stuck_ticks >= STUCK_TICKS_TRIGGER) {
            try_open_door_in_front(e);
            b->stuck_ticks = 0;  // give the door time to open before re-trying
        }
    } else if (dx*dx + dy*dy >= STUCK_DISP2_MIN) {
        b->stuck_ticks = 0;
    }
    // else: still turning, leave counter unchanged.
}

static void behavior_tick(ai_brain_t *b, edict_t *e) {
    switch (b->state) {
    case AI_IDLE: {
        if (b->patrol_route_id < 0) break;
        edict_t *node = Sim_Patrol_FindArenaNode(b->patrol_route_id, b->patrol_node_idx);
        if (!node) break;

        // 2D arrival: ignore Z. Floor-seated nav points and monster
        // origins are at slightly different elevations, so 3D distance
        // exaggerates "remaining travel" and the monster never arrives.
        {
            float dx = node->v.origin[0] - e->v.origin[0];
            float dy = node->v.origin[1] - e->v.origin[1];
            if (dx*dx + dy*dy < 48*48) {
                b->patrol_node_idx++;
                if (!Sim_Patrol_FindArenaNode(b->patrol_route_id, b->patrol_node_idx))
                    b->patrol_node_idx = 0;
                b->path_len = 0;          // force replan toward next node
                break;
            }
        }

        // Plan (or replan) the navmesh path to the current node.
        if (b->path_len == 0 || b->path_idx >= b->path_len) {
            b->path_len = Sim_Nav_PathTo(e->v.origin, node->v.origin,
                                         b->path_pts, 32);
            b->path_idx = 0;
            if (b->path_len == 0) {
                // No graph path -- stand and face the node; let vanilla
                // AI continue. (Better than walking straight into a wall.)
                face_point(e, node->v.origin);
                break;
            }
        }

        const float *next = b->path_pts[b->path_idx];
        float wdx = next[0] - e->v.origin[0];
        float wdy = next[1] - e->v.origin[1];
        if (wdx*wdx + wdy*wdy < 32*32) {
            b->path_idx++;
            break;
        }
        walk_and_track(b, e, (vec3_t){ next[0], next[1], next[2] }, 6.0f);
        return;  // suppress vanilla AI walk this tick
    }
    case AI_SUSPICIOUS:
        walk_and_track(b, e, b->last_known_pos, 6.0f);
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
            walk_and_track(b, e, b->last_known_pos, 6.0f);
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
        walk_and_track(b, e, (vec3_t){ next[0], next[1], next[2] }, 8.0f);
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

// ---------------------------------------------------------------------------
// Patrol debug overlay (sim_patrol_debug)
// For each brain that has a route:
//   - vertical post at every node (green by default; red at the current
//     target node so you can see which way the monster is heading)
//   - dim green cycle line connecting consecutive nodes
//   - yellow arrow line from the monster's eye to the current target node
// Distance-culled around the camera so dense maps stay legible.
// ---------------------------------------------------------------------------
#define SIM_PATROL_DRAW_NODES_MAX  8
#define SIM_PATROL_DRAW_CULL       1536.0f

void Sim_Patrol_DebugDraw(void) {
    if (eng->Cvar_VariableValue("sim_patrol_debug") < 0.5f) return;

    vec3_t view;
    eng->Get_ViewOrigin(view);
    float cull2 = SIM_PATROL_DRAW_CULL * SIM_PATROL_DRAW_CULL;

    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (b->patrol_route_id < 0) continue;

        // Gather this route's nodes in idx order.
        edict_t *nodes[SIM_PATROL_DRAW_NODES_MAX];
        int n = 0;
        for (int i = 0; i < SIM_PATROL_DRAW_NODES_MAX; i++) {
            edict_t *node = Sim_Patrol_FindArenaNode(b->patrol_route_id, i);
            if (!node) break;
            nodes[n++] = node;
        }
        if (n < 2) continue;

        // Cheap cull: skip the whole route if the first node is far from the
        // camera. (Skips a few edge cases for very long routes but keeps the
        // overlay readable.)
        {
            float dx = nodes[0]->v.origin[0] - view[0];
            float dy = nodes[0]->v.origin[1] - view[1];
            float dz = nodes[0]->v.origin[2] - view[2];
            if (dx*dx + dy*dy + dz*dz > cull2) continue;
        }

        // Cycle lines between consecutive nodes, slightly raised so they
        // don't z-fight floor brushes.
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            vec3_t a = { nodes[i]->v.origin[0], nodes[i]->v.origin[1],
                         nodes[i]->v.origin[2] + 4.0f };
            vec3_t c = { nodes[j]->v.origin[0], nodes[j]->v.origin[1],
                         nodes[j]->v.origin[2] + 4.0f };
            eng->SV_DebugLine(a, c, 56, 0);   // dim green
        }

        // Vertical posts at each node; current target gets a red post.
        int cur = b->patrol_node_idx % n;
        for (int i = 0; i < n; i++) {
            vec3_t p0 = { nodes[i]->v.origin[0], nodes[i]->v.origin[1],
                          nodes[i]->v.origin[2] };
            vec3_t p1 = { p0[0], p0[1], p0[2] + 40.0f };
            int color = (i == cur) ? 79 : 53;   // red / green
            eng->SV_DebugLine(p0, p1, color, 0);
        }

        // Monster -> current target node.
        edict_t *me = 0;
        for (edict_t *it = eng->ED_Next(g->world); it; it = eng->ED_Next(it)) {
            if (eng->ED_GetNum(it) == b->edict_num) { me = it; break; }
        }
        if (me && me->v.health > 0) {
            vec3_t a = { me->v.origin[0], me->v.origin[1],
                         me->v.origin[2] + 24.0f };
            vec3_t c = { nodes[cur]->v.origin[0], nodes[cur]->v.origin[1],
                         nodes[cur]->v.origin[2] + 24.0f };
            eng->SV_DebugLine(a, c, 192, 0);   // yellow-ish
        }
    }
}

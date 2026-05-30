// sim.h -- Shared types and inter-module API for the immersive-sim layer.
// All sim/*.c files include only this header (and game_api.h / game_types.h).

#ifndef SIM_H
#define SIM_H

#include "game_api.h"
#include "game_types.h"

#define SIM_MAX_BRAINS         600    // matches engine MAX_EDICTS
#define SIM_STIM_RING_SIZE     512
#define SIM_STIM_MAX_AGE_S     5.0f
#define SIM_AI_TICK_HZ         10.0f

// Scoped per-frame profiler — push/pop pairs are visible in the engine's
// PERF_SCOPE flame graph. `eng` is the engine_api_t pointer every sim file
// already has via `extern engine_api_t *eng;`. `name` must be a literal so
// the pointer stays valid across the capture window.
#define SIM_PERF_CONCAT_(a,b) a##b
#define SIM_PERF_CONCAT(a,b)  SIM_PERF_CONCAT_(a,b)
#define SIM_PERF(name) \
    for (int SIM_PERF_CONCAT(_p_, __LINE__) = (eng->Perf_PushScope(name), 1); \
         SIM_PERF_CONCAT(_p_, __LINE__); \
         SIM_PERF_CONCAT(_p_, __LINE__) = 0, eng->Perf_PopScope())

// ---------------------------------------------------------------------------
// Stimulus bus
// ---------------------------------------------------------------------------
typedef enum {
    STIM_NONE = 0,
    STIM_SOUND,
    STIM_SIGHT_ENTITY,
    STIM_SMOKE,
    STIM_LIGHT_CHANGE,
    STIM_CORPSE,
    STIM_PROP_BROKEN,
} stim_kind_t;

typedef struct {
    stim_kind_t kind;
    vec3_t      origin;
    float       intensity;     // 0..1 at the source
    float       time;          // g->time at emission
    int         source_edict;  // edict number (-1 = world)
    int         flags;
} stimulus_t;

void Stim_Init(void);
void Stim_LevelInit(void);                 // clears bus on map change
void Stim_Emit(const stimulus_t *s);
int  Stim_QueryNear(const vec3_t pos,
                    float radius,
                    float since_time,
                    stimulus_t *out,
                    int max_out);

// ---------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------
typedef enum {
    AI_IDLE = 0,
    AI_SUSPICIOUS,
    AI_SEARCHING,
    AI_COMBAT,
} ai_state_t;

typedef struct {
    int          in_use;             // 0 if this slot is unused
    int          edict_num;          // engine entity number
    ai_state_t   state;
    float        state_entered_time;
    float        alert_level;        // 0..1
    vec3_t       last_known_pos;
    int          target_edict;       // -1 if none
    int          patrol_route_id;    // -1 if none
    int          patrol_node_idx;
    float        sense_sight_range;  // base 1024
    float        sense_hearing_mult; // base 1.0
    float        next_tick_time;
    // Navmesh path being walked (SEARCHING)
    vec3_t      path_pts[32];
    int         path_len;
    int         path_idx;
    float       path_replan_time;
    // Stuck detection: counts consecutive ticks of attempted-but-failed
    // movement (facing the target but not displacing). Resets to 0 on
    // any successful step or on state change.
    int         stuck_ticks;
    // 1 once we've kicked the monster's vanilla think chain into walk
    // mode (so the walk-frame animation cycles while our code drives
    // movement). Reset on state change so re-entering IDLE/SEARCHING
    // re-triggers the kick if vanilla took the monster elsewhere.
    int         walking;
} ai_brain_t;

void        Sim_AI_Init(void);
void        Sim_AI_LevelInit(void);
void        Sim_AI_Frame(void);                              // called once/frame
ai_brain_t *Sim_AI_GetBrain(edict_t *e);                     // returns NULL if not registered
ai_brain_t *Sim_AI_RegisterMonster(edict_t *e);              // idempotent
void        Sim_AI_UnregisterByEdictNum(int edict_num);

// Read-only iterator for the imgui overlay (returns NULL when done).
ai_brain_t *Sim_AI_IterFirst(void);
ai_brain_t *Sim_AI_IterNext(ai_brain_t *prev);

// ---------------------------------------------------------------------------
// Patrol routes
// ---------------------------------------------------------------------------
void Sim_Patrol_RegisterNode(edict_t *e);   // called from spawn dispatch
void Sim_Patrol_Resolve(void);              // links targets at level start
edict_t *Sim_Patrol_FindByTargetname(const char *name);
void Sim_Patrol_RegisterArenaNode(int route, int idx, edict_t *e);
edict_t *Sim_Patrol_FindArenaNode(int route, int idx);
void Sim_Patrol_DebugDraw(void);             // sim_patrol_debug cvar

// ---------------------------------------------------------------------------
// Navmesh
// ---------------------------------------------------------------------------
typedef struct sim_navmesh_s sim_navmesh_t;

void           Sim_Nav_Init(void);
void           Sim_Nav_LevelInit(const char *mapname);  // kicks off bake or load
void           Sim_Nav_Rebake(const char *mapname);     // deletes cache, re-bakes
void           Sim_Nav_Frame(void);                     // debug draw if sim_nav_debug set
int            Sim_Nav_IsReady(void);                   // 0 while baking
sim_navmesh_t *Sim_Nav_Get(void);                       // NULL if not ready

// Path query: fills out[] with up to max_out vec3 waypoints and (if
// non-NULL) out_kinds[] with each waypoint's entry-edge kind
// (NAV_EDGE_*). player_items is bitmasked against each edge's
// requires_items: edges whose required-item bits aren't a subset of
// player_items are skipped (key-locked doors).
// Returns the number of waypoints written, or 0 if no path.
int Sim_Nav_PathTo(const vec3_t from,
                   const vec3_t to,
                   vec3_t *out,
                   unsigned char *out_kinds,
                   unsigned int player_items,
                   int max_out);

// Dev/test: run Sim_Nav_PathTo from->to using the live serverflags
// sigil set (packed into the high item bits), print the waypoint count
// to console, and return it. Backs the engine console command
// `nav_testpath` (see hotreload.c). 0 = no path.
int Sim_Nav_TestPath(const float *from, const float *to);

// Snapshot of one nav edge for debug/inspection consumers (MCP).
// Coords are world-space copies of mesh node positions; safe to use
// even after a rebake.
typedef struct {
    float from[3];
    float to[3];
    float weight;
    unsigned char kind;   // NAV_EDGE_*
    unsigned char phase;  // NAV_PHASE_*
} sim_nav_edge_record_t;

// Fill `out` with edges whose either endpoint is within `radius` of
// `center`. Caller provides `max_records` capacity. Returns the
// number written. If `truncated_out` is non-NULL it is set to 1 when
// more edges matched than fit (in which case the function still
// returns `max_records`). Safe to call before bake -- returns 0.
int Sim_Nav_EdgesNear(const float center[3], float radius,
                      sim_nav_edge_record_t *out, int max_records,
                      int *truncated_out);

// ---------------------------------------------------------------------------
// Retrofit (Phase 8 / M6)
// ---------------------------------------------------------------------------
void Sim_Retrofit_LevelInit(void);
void Sim_Retrofit_Frame(void);

// ---------------------------------------------------------------------------
// Light tier (Phase 8 / M5)
// ---------------------------------------------------------------------------
void  Light_Init(void);
void  Light_LevelInit(void);
float Light_TierAt(const vec3_t pos);
void  Light_AddOverride(const vec3_t pos, float radius, float delta);

// ---------------------------------------------------------------------------
// Wind / smoke field (Phase 8 / M4)
// ---------------------------------------------------------------------------
void  Wind_Init(void);
void  Wind_LevelInit(void);
void  Wind_Frame(void);
void  Wind_DebugDraw(void);

float Wind_GetSmokeAt(const vec3_t pos);
void  Wind_SampleVelocity(const vec3_t pos, vec3_t out_vel);
float Wind_PathOcclusion(const vec3_t a, const vec3_t b);

void  Wind_AddImpulse(const vec3_t origin, const vec3_t dir,
                      float magnitude, float radius);
void  Wind_AddTubeImpulse(const vec3_t origin, const vec3_t axis,
                          float magnitude, float radius);
void  Wind_AddSmoke(const vec3_t origin, float amount, float radius);
void  Wind_ClearSmoke(const vec3_t origin, float amount, float radius);
void  Wind_RegisterSource(edict_t *e, const vec3_t velocity);

// ---------------------------------------------------------------------------
// Arena (test)
// ---------------------------------------------------------------------------
void Sim_Arena_Init(void);
void Sim_Arena_Spawn(void);
void Sim_Arena_Poll(void);

// ---------------------------------------------------------------------------
// Top-level lifecycle
// ---------------------------------------------------------------------------
void Sim_Init(void);                       // once on DLL init
void Sim_LevelInit(const char *mapname);   // on each map load
void Sim_Frame(void);                      // each game start_frame

// ---------------------------------------------------------------------------
// Debug-overlay cvar convention
// ---------------------------------------------------------------------------
// All sim_*_debug cvars share a tri-state encoding:
//   0 = off
//   1 = draw with z-test (lines occluded by world geometry)
//   2 = draw without z-test (xray; lines visible through walls)
// Sim_DebugZTest returns -1 (off), 0 (xray), or 1 (ztested) so the
// caller can pass it straight as the ztest arg to SV_DebugLine. The
// off case is `< 0`, so:
//
//   int z = Sim_DebugZTest("sim_patrol_debug");
//   if (z < 0) return;
//   eng->SV_DebugLine(a, b, color, z);
int Sim_DebugZTest(const char *cvar_name);

#endif // SIM_H

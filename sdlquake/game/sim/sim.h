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

// ---------------------------------------------------------------------------
// Navmesh
// ---------------------------------------------------------------------------
typedef struct sim_navmesh_s sim_navmesh_t;

void           Sim_Nav_Init(void);
void           Sim_Nav_LevelInit(const char *mapname);  // kicks off bake or load
int            Sim_Nav_IsReady(void);                   // 0 while baking
sim_navmesh_t *Sim_Nav_Get(void);                       // NULL if not ready

// Path query: fills out[] with up to max_out vec3 waypoints.
// Returns the number of waypoints written, or 0 if no path.
int Sim_Nav_PathTo(const vec3_t from,
                   const vec3_t to,
                   vec3_t *out,
                   int max_out);

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

#endif // SIM_H

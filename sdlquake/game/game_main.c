// game_main.c -- Entry point and lifecycle for the native game DLL.

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include "abilities.h"

engine_api_t   *eng;
game_globals_t *g;

// ---------------------------------------------------------------------------
// Entity dispatch — called by the engine per-frame for think and touch.
// The entity's function pointer is invoked if non-NULL.
// (Requires complete edict_t definition; wired up in Task 7.)
// ---------------------------------------------------------------------------
static void game_entity_think(edict_t *e) {
    if (e->v.think) e->v.think(e);
}
static void game_entity_touch(edict_t *e, edict_t *other) {
    if (e->v.touch) e->v.touch(e, other);
}

extern void Weapons_RegisterCvars(void);

static void game_init(engine_api_t *engine, game_globals_t *globals)
{
    eng = engine;
    g   = globals;
    Sim_Init();
    Abilities_Init();
    Weapons_RegisterCvars();
}

static void game_shutdown(void) { }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Entry points — wired to ported QC functions as files are added.
// ---------------------------------------------------------------------------
void StartFrame(void);                              // world.c
void ClientConnect(edict_t *);                      // client.c
void ClientDisconnect(edict_t *);
void PutClientInServer(edict_t *);
void PlayerPreThink(edict_t *);
void PlayerPostThink(edict_t *);
void ClientKill(edict_t *);
void SetNewParms(void);
void SetChangeParms(edict_t *);

void Spike_GibPathScan(void);                       // weapons.c
void Missile_SmokeWake(void);                       // weapons.c
extern engine_api_t *eng;                           // shared with sim/* through SIM_PERF
static void game_start_frame(void) {
    SIM_PERF("Sim_Frame")          Sim_Frame();
    SIM_PERF("Spike_GibPathScan")  Spike_GibPathScan();
    SIM_PERF("Missile_SmokeWake")  Missile_SmokeWake();
    SIM_PERF("StartFrame")         StartFrame();
}
static void game_client_connect(edict_t *c)        { ClientConnect(c); }
static void game_client_disconnect(edict_t *c)     { ClientDisconnect(c); }
static void game_put_client_in_server(edict_t *c)  { PutClientInServer(c); }
static void game_client_prethink(edict_t *c)       { PlayerPreThink(c); }
static void game_client_postthink(edict_t *c)      { PlayerPostThink(c); }
static void game_client_kill(edict_t *c)           { ClientKill(c); }
static void game_set_new_parms(void)               { SetNewParms(); }
static void game_set_change_parms(edict_t *c)      { SetChangeParms(c); }

// Defined in spawn.c — classname dispatch table + introspection
void game_entity_spawn(edict_t *e, const char *classname);
const char *const *game_list_spawn_classes(int *out_count);

// Defined in doors.c — console-command dispatch targets.
void Doors_OpenAll(void);
void Doors_OpenAllSecret(void);

// Per-render-frame debug overlay hook. Runs even while the sim is paused
// (editor open, sv.paused), so debug visualisations stay live.
static void game_debug_draw_overlays(void)
{
    Sim_Nav_Frame();
    Wind_DebugDraw();
    Sim_Patrol_DebugDraw();
}

// Debug-only damage entry point used by the MCP `damage_entity` tool. World
// is both the inflictor and the attacker so the corpse-overdamage / gib
// branches in T_Damage fire normally without simulating a weapon trace.
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
static void game_mcp_damage(edict_t *targ, float damage)
{
    if (!targ || targ->free) return;
    T_Damage(targ, g->world, g->world, damage);
}

// Bot pathfinding bridge — wraps Sim_Nav_PathTo so engine-side bot.c
// doesn't need to link against game.dll's sim module.
static int game_nav_path(const float *from, const float *to,
                         float *out_waypoints_xyz,
                         unsigned char *out_kinds,
                         unsigned int player_items,
                         int max_waypoints)
{
    vec3_t f, t;
    vec3_t *out = (vec3_t *)out_waypoints_xyz;
    if (!from || !to || !out_waypoints_xyz || max_waypoints <= 0) return 0;
    f[0] = from[0]; f[1] = from[1]; f[2] = from[2];
    t[0] = to[0];   t[1] = to[1];   t[2] = to[2];
    return Sim_Nav_PathTo(f, t, out, out_kinds, player_items, max_waypoints);
}

// MCP debug bridge — see game_api.h::nav_edges_near.
static int game_nav_edges_near(const float *center, float radius,
                               void *out_records, int max_records,
                               int *truncated_out) {
    return Sim_Nav_EdgesNear(center, radius,
                             (sim_nav_edge_record_t *)out_records,
                             max_records, truncated_out);
}

// Editor inspector query — see game_api.h for field contract.
static int game_ai_inspect(edict_t *e,
                           int *out_state,
                           float *out_alert,
                           float *out_last_known_pos,
                           int *out_target_edict,
                           float *out_next_tick_dt,
                           int *out_stuck_ticks)
{
    ai_brain_t *b;
    if (!e || e->free) return 0;
    b = Sim_AI_GetBrain(e);
    if (!b || !b->in_use) return 0;
    if (out_state)          *out_state = (int)b->state;
    if (out_alert)          *out_alert = b->alert_level;
    if (out_last_known_pos) {
        out_last_known_pos[0] = b->last_known_pos[0];
        out_last_known_pos[1] = b->last_known_pos[1];
        out_last_known_pos[2] = b->last_known_pos[2];
    }
    if (out_target_edict)   *out_target_edict = b->target_edict;
    if (out_next_tick_dt)   *out_next_tick_dt = b->next_tick_time - g->time;
    if (out_stuck_ticks)    *out_stuck_ticks = b->stuck_ticks;
    return 1;
}

static game_api_t s_api = {
    GAME_API_VERSION,
    game_init,
    game_shutdown,
    game_start_frame,
    game_entity_spawn,
    game_entity_think,
    game_entity_touch,
    game_client_connect,
    game_client_disconnect,
    game_put_client_in_server,
    game_client_prethink,
    game_client_postthink,
    game_client_kill,
    game_set_new_parms,
    game_set_change_parms,
    game_list_spawn_classes,
    game_debug_draw_overlays,
    Doors_OpenAll,
    Doors_OpenAllSecret,
    game_mcp_damage,
    Wind_SampleVelocity,
    game_ai_inspect,
    game_nav_path,
    game_nav_edges_near,
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
game_api_t *Game_GetAPI(void) { return &s_api; }

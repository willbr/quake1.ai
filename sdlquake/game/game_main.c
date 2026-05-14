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

static void game_init(engine_api_t *engine, game_globals_t *globals)
{
    eng = engine;
    g   = globals;
    Sim_Init();
    Abilities_Init();
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

static void game_start_frame(void)                 { Sim_Frame(); StartFrame(); }
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
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
game_api_t *Game_GetAPI(void) { return &s_api; }

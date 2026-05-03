// game_main.c -- Entry point and lifecycle for the native game DLL.

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"

engine_api_t   *eng;
game_globals_t *g;

// ---------------------------------------------------------------------------
// Entity dispatch — called by the engine per-frame for think and touch.
// The entity's function pointer is invoked if non-NULL.
// (Requires complete edict_t definition; wired up in Task 7.)
// ---------------------------------------------------------------------------
static void game_entity_think(edict_t *e) { (void)e; }
static void game_entity_touch(edict_t *e, edict_t *other) { (void)e; (void)other; }

static void game_init(engine_api_t *engine, game_globals_t *globals)
{
    eng = engine;
    g   = globals;
}

static void game_shutdown(void) { }

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Stub entry points — replaced file-by-file as QC files are ported.
// ---------------------------------------------------------------------------
void StartFrame(void);   // world.c
static void game_start_frame(void) { StartFrame(); }
static void game_client_connect(edict_t *client)                { (void)client; }
static void game_client_disconnect(edict_t *client)             { (void)client; }
static void game_put_client_in_server(edict_t *client)          { (void)client; }
static void game_client_prethink(edict_t *client)               { (void)client; }
static void game_client_postthink(edict_t *client)              { (void)client; }
static void game_client_kill(edict_t *client)                   { (void)client; }
static void game_set_new_parms(void)                            { }
static void game_set_change_parms(edict_t *client)              { (void)client; }

// Defined in spawn.c — classname dispatch table
void game_entity_spawn(edict_t *e, const char *classname);

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
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
game_api_t *Game_GetAPI(void) { return &s_api; }

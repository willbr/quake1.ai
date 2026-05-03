// game_main.c -- hot-reloadable game logic (Phase 3 demo)
//
// HOW TO DEMO HOT-RELOAD:
//   1. Run the game:  zig build run -- +map e1m1
//   2. Console shows: "game.dll loaded (sv_gravity=800)"
//   3. Edit GRAVITY below (e.g. 200 for moon gravity)
//   4. In another terminal: zig build game
//   5. Console immediately shows: "game.dll loaded (sv_gravity=200)"
//      and jumping feels floaty — no restart needed.

#include "game_api.h"
#include <stdio.h>

// -----------------------------------------------------------------------
// Edit this value, run `zig build game`, and feel the change instantly.
// -----------------------------------------------------------------------
static const float GRAVITY = 800.0f;

static engine_api_t *eng;

static void game_init(engine_api_t *engine, game_globals_t *globals)
{
    char msg[64];
    (void)globals;
    eng = engine;
    snprintf(msg, sizeof(msg), "game.dll loaded (sv_gravity=%.0f)\n", GRAVITY);
    eng->Con_Print(msg);
    eng->Cvar_SetValue("sv_gravity", GRAVITY);
}

static void game_shutdown(void) { }

static void game_start_frame(void) { }

static void game_entity_spawn(edict_t *e, const char *classname) { (void)e; (void)classname; }
static void game_entity_think(edict_t *e)                        { (void)e; }
static void game_entity_touch(edict_t *e, edict_t *other)        { (void)e; (void)other; }

static void game_client_connect(edict_t *client)    { (void)client; }
static void game_client_disconnect(edict_t *client) { (void)client; }
static void game_put_client_in_server(edict_t *client) { (void)client; }
static void game_client_prethink(edict_t *client)   { (void)client; }
static void game_client_postthink(edict_t *client)  { (void)client; }
static void game_client_kill(edict_t *client)       { (void)client; }

static void game_set_new_parms(void)                { }
static void game_set_change_parms(edict_t *client)  { (void)client; }

static game_api_t api = {
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
game_api_t *Game_GetAPI(void) { return &api; }

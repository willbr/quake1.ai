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

static void game_init(engine_api_t *engine)
{
    char msg[64];
    eng = engine;
    snprintf(msg, sizeof(msg), "game.dll loaded (sv_gravity=%.0f)\n", GRAVITY);
    eng->Con_Print(msg);
    eng->Cvar_SetValue("sv_gravity", GRAVITY);
}

static void game_shutdown(void) { }

static void game_server_frame(float dt) { (void)dt; }

static game_api_t api = {
    GAME_API_VERSION,
    game_init,
    game_shutdown,
    game_server_frame,
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
game_api_t *Game_GetAPI(void) { return &api; }

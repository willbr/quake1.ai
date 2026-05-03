// hotreload.c -- hot-reloadable game DLL loader
//
// Strategy:
//   - game.dll is compiled by `zig build game` into zig-out/bin/
//   - On first call to HotReload_Init(), the DLL is copied to
//     game_loaded.dll and that copy is loaded.  Keeping a separate
//     copy lets zig overwrite game.dll while we're running.
//   - HotReload_Frame() checks game.dll's mtime every ~1 s.
//     When it changes, the old DLL is unloaded, the new one copied
//     and loaded, and game_api->init(engine, globals) is called again with
//     both the engine API and game globals.
//   - All DLL calls happen on the main thread — no locking needed.

#include <SDL3/SDL.h>
#include <stdio.h>

#include "game_api.h"
#include "hotreload.h"

// Paths relative to the working directory (project root when using zig build run).
#define GAME_DLL_SRC  "zig-out/bin/game.dll"
#define GAME_DLL_LOAD "zig-out/bin/game_loaded.dll"

// Forward-declare the Quake engine functions we wrap for the game DLL.
void  Con_Printf(char *fmt, ...);
void  Cvar_Set(char *var_name, char *value);
float Cvar_VariableValue(char *var_name);
double Sys_FloatTime(void);

// ---------------------------------------------------------------------------
// Engine API passed into game DLL
// ---------------------------------------------------------------------------

static void engine_con_print(const char *msg)
{
    Con_Printf("%s", msg);
}

static void engine_cvar_set_value(const char *name, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", value);
    Cvar_Set((char *)name, buf);
}

static float engine_cvar_variable_value(const char *name)
{
    return Cvar_VariableValue((char *)name);
}

// Only the 4 Phase-3 slots below are populated.
// The remaining 52 slots are NULL and will cause a crash if called.
// Task 7 fills them in when NATIVE_GAME dispatch is wired into the engine.
static engine_api_t engine_funcs = {
    engine_con_print,
    engine_cvar_set_value,
    engine_cvar_variable_value,
    Sys_FloatTime,
};

// ---------------------------------------------------------------------------
// DLL state
// ---------------------------------------------------------------------------

static SDL_SharedObject *game_so  = NULL;
game_api_t              *g_game_api = NULL;  // exposed for NATIVE_GAME dispatch guards
static SDL_Time          dll_mtime    = 0;
// game_globals is passed to the game DLL's init() and used for all entity
// lifecycle calls. The engine must write world/time/frametime into it before
// calling any game entry point. Task 7 adds those writes.
static game_globals_t    game_globals = {0};

static SDL_Time get_mtime(const char *path)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return 0;
    return info.modify_time;
}

static void do_unload(void)
{
    if (g_game_api) { g_game_api->shutdown(); g_game_api = NULL; }
    if (game_so)    { SDL_UnloadObject(game_so); game_so = NULL; }
}

static void do_load(void)
{
    do_unload();

    // Copy so zig can overwrite game.dll while game_loaded.dll stays mapped.
    if (!SDL_CopyFile(GAME_DLL_SRC, GAME_DLL_LOAD))
    {
        Con_Printf("hotreload: copy failed: %s\n", SDL_GetError());
        return;
    }

    game_so = SDL_LoadObject(GAME_DLL_LOAD);
    if (!game_so)
    {
        Con_Printf("hotreload: load failed: %s\n", SDL_GetError());
        return;
    }

    Game_GetAPI_fn get_api =
        (Game_GetAPI_fn)(void *)SDL_LoadFunction(game_so, "Game_GetAPI");
    if (!get_api)
    {
        Con_Printf("hotreload: Game_GetAPI not found\n");
        SDL_UnloadObject(game_so); game_so = NULL;
        return;
    }

    game_api_t *api = get_api();
    if (!api || api->version != GAME_API_VERSION)
    {
        Con_Printf("hotreload: ABI version mismatch (want %d)\n", GAME_API_VERSION);
        SDL_UnloadObject(game_so); game_so = NULL;
        return;
    }

    g_game_api = api;
    Con_Printf("hotreload: game.dll reloaded\n");
    g_game_api->init(&engine_funcs, &game_globals);
    dll_mtime = get_mtime(GAME_DLL_SRC);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

#define RELOAD_CHECK_INTERVAL 60   // frames between mtime polls (~1 s at 60 fps)

void HotReload_Init(void)
{
    SDL_Time t = get_mtime(GAME_DLL_SRC);
    if (t != 0) do_load();
}

void HotReload_Frame(float dt)
{
    (void)dt;
    static int counter = 0;
    if (++counter >= RELOAD_CHECK_INTERVAL)
    {
        counter = 0;
        SDL_Time t = get_mtime(GAME_DLL_SRC);
        if (t != 0 && t != dll_mtime)
            do_load();
    }

    if (g_game_api)
        g_game_api->start_frame();
}

void HotReload_Shutdown(void)
{
    do_unload();
}

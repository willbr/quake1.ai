// game_api.h -- ABI between the engine and the hot-reloadable game DLL
//
// Bump GAME_API_VERSION whenever the struct layout changes.
// The loader rejects DLLs whose version doesn't match.

#ifndef GAME_API_H
#define GAME_API_H

#define GAME_API_VERSION 1

// Functions the engine exposes to the game DLL.
typedef struct engine_api_s {
    void   (*Con_Print)(const char *msg);          // pre-formatted string
    void   (*Cvar_SetValue)(const char *name, float value);
    float  (*Cvar_VariableValue)(const char *name);
    double (*Sys_FloatTime)(void);
} engine_api_t;

// Functions the game DLL exposes to the engine.
typedef struct game_api_s {
    int   version;                               // must equal GAME_API_VERSION
    void  (*init)(engine_api_t *engine);         // called after load/reload
    void  (*shutdown)(void);                     // called before unload/reload
    void  (*server_frame)(float dt);             // called every game frame
} game_api_t;

// Exported entry point — every game DLL must export this symbol.
// The engine calls it immediately after LoadLibrary to get the vtable.
typedef game_api_t *(*Game_GetAPI_fn)(void);

#endif // GAME_API_H

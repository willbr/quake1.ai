// hotreload.h -- hot-reloadable game DLL loader

#ifndef HOTRELOAD_H
#define HOTRELOAD_H

void HotReload_Init(void);
void HotReload_Frame(float dt);
void HotReload_Shutdown(void);
void HotReload_EnablePolling(void);

// Exposed for #if NATIVE_GAME dispatch guards in engine files.
#include "game_api.h"
extern game_api_t    *g_game_api;
extern game_globals_t game_globals;

#endif // HOTRELOAD_H

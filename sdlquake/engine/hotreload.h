// hotreload.h -- hot-reloadable game DLL loader

#ifndef HOTRELOAD_H
#define HOTRELOAD_H

void HotReload_Init(void);
void HotReload_Frame(float dt);
void HotReload_Shutdown(void);

// Exposed for #if NATIVE_GAME dispatch guards in engine files.
#if NATIVE_GAME
#include "game_api.h"
extern game_api_t *g_game_api;
#endif

#endif // HOTRELOAD_H

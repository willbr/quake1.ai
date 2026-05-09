// editor_classlist.c -- caches the game DLL's list_spawn_classes result.
//
// Lifetime is keyed on the game_api_t pointer identity: HotReload swaps
// `g_game_api` to a freshly-mmapped struct on each reload, so a pointer
// change is the cheap signal to rebuild the cache. The classname strings
// themselves live in DLL static storage and are valid until the DLL is
// unloaded — re-fetching the list returns the new copy automatically.

#include <stddef.h>             // NULL

#include "editor_classlist.h"
#include "game_api.h"
#include "hotreload.h"          // g_game_api

static const char *const *s_names    = NULL;
static int                s_count    = 0;
static const game_api_t  *s_seen_api = NULL;

const char *const *Editor_ClassList_Get(int *out_count)
{
    if ((const game_api_t *)g_game_api != s_seen_api)
    {
        s_seen_api = (const game_api_t *)g_game_api;
        s_names    = NULL;
        s_count    = 0;
        if (g_game_api && g_game_api->list_spawn_classes)
        {
            int n = 0;
            const char *const *names = g_game_api->list_spawn_classes(&n);
            if (names && n > 0) { s_names = names; s_count = n; }
        }
    }
    if (out_count) *out_count = s_count;
    return s_names;
}

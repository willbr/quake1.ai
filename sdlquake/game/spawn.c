// spawn.c -- Entity classname → spawn function registry.
// Add one entry here for each entity class as its QC file is ported.

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

typedef struct { const char *classname; void (*fn)(edict_t *); } spawn_entry_t;

// Forward declarations — added as files are ported
void spawn_worldspawn(edict_t *e);
void spawn_bodyque(edict_t *e);

void spawn_info_intermission(edict_t *e);
void spawn_info_player_start(edict_t *e);
void spawn_info_player_start2(edict_t *e);
void spawn_testplayerstart(edict_t *e);
void spawn_info_player_deathmatch(edict_t *e);
void spawn_info_player_coop(edict_t *e);
void spawn_trigger_changelevel(edict_t *e);

static const spawn_entry_t s_spawns[] = {
    { "worldspawn",              spawn_worldspawn           },
    { "bodyque",                 spawn_bodyque              },
    { "info_intermission",       spawn_info_intermission    },
    { "info_player_start",       spawn_info_player_start    },
    { "info_player_start2",      spawn_info_player_start2   },
    { "testplayerstart",         spawn_testplayerstart      },
    { "info_player_deathmatch",  spawn_info_player_deathmatch },
    { "info_player_coop",        spawn_info_player_coop     },
    { "trigger_changelevel",     spawn_trigger_changelevel  },
    { NULL, NULL }
};

void game_entity_spawn(edict_t *e, const char *classname)
{
    int n = (int)(sizeof(s_spawns)/sizeof(s_spawns[0]));
    for (int i = 0; i < n; i++) {
        if (!s_spawns[i].classname) break;
        if (!strcmp(s_spawns[i].classname, classname)) {
            s_spawns[i].fn(e);
            return;
        }
    }
    // Unknown classname: silently skip (matching original VM behaviour)
}

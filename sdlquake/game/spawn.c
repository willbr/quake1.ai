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

static const spawn_entry_t s_spawns[] = {
    { "worldspawn", spawn_worldspawn },
    { "bodyque",    spawn_bodyque    },
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

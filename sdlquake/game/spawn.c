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

// items.c
void spawn_noclass(edict_t *e);
void spawn_item_health(edict_t *e);
void spawn_item_armor1(edict_t *e);
void spawn_item_armor2(edict_t *e);
void spawn_item_armorInv(edict_t *e);
void spawn_weapon_supershotgun(edict_t *e);
void spawn_weapon_nailgun(edict_t *e);
void spawn_weapon_supernailgun(edict_t *e);
void spawn_weapon_grenadelauncher(edict_t *e);
void spawn_weapon_rocketlauncher(edict_t *e);
void spawn_weapon_lightning(edict_t *e);
void spawn_item_shells(edict_t *e);
void spawn_item_spikes(edict_t *e);
void spawn_item_rockets(edict_t *e);
void spawn_item_cells(edict_t *e);
void spawn_item_weapon(edict_t *e);
void spawn_item_key1(edict_t *e);
void spawn_item_key2(edict_t *e);
void spawn_item_sigil(edict_t *e);
void spawn_item_artifact_invulnerability(edict_t *e);
void spawn_item_artifact_envirosuit(edict_t *e);
void spawn_item_artifact_invisibility(edict_t *e);
void spawn_item_artifact_super_damage(edict_t *e);

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
    // items.c
    { "noclass",                          spawn_noclass                         },
    { "item_health",                      spawn_item_health                     },
    { "item_armor1",                      spawn_item_armor1                     },
    { "item_armor2",                      spawn_item_armor2                     },
    { "item_armorInv",                    spawn_item_armorInv                   },
    { "weapon_supershotgun",              spawn_weapon_supershotgun             },
    { "weapon_nailgun",                   spawn_weapon_nailgun                  },
    { "weapon_supernailgun",              spawn_weapon_supernailgun             },
    { "weapon_grenadelauncher",           spawn_weapon_grenadelauncher          },
    { "weapon_rocketlauncher",            spawn_weapon_rocketlauncher           },
    { "weapon_lightning",                 spawn_weapon_lightning                },
    { "item_shells",                      spawn_item_shells                     },
    { "item_spikes",                      spawn_item_spikes                     },
    { "item_rockets",                     spawn_item_rockets                    },
    { "item_cells",                       spawn_item_cells                      },
    { "item_weapon",                      spawn_item_weapon                     },
    { "item_key1",                        spawn_item_key1                       },
    { "item_key2",                        spawn_item_key2                       },
    { "item_sigil",                       spawn_item_sigil                      },
    { "item_artifact_invulnerability",    spawn_item_artifact_invulnerability   },
    { "item_artifact_envirosuit",         spawn_item_artifact_envirosuit        },
    { "item_artifact_invisibility",       spawn_item_artifact_invisibility      },
    { "item_artifact_super_damage",       spawn_item_artifact_super_damage      },
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

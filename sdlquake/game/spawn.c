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

// ai.c
void spawn_path_corner(edict_t *e);

// misc.c
void spawn_info_null(edict_t *e);
void spawn_info_notnull(edict_t *e);
void spawn_light(edict_t *e);
void spawn_light_fluoro(edict_t *e);
void spawn_light_fluorospark(edict_t *e);
void spawn_light_globe(edict_t *e);
void spawn_light_torch_small_walltorch(edict_t *e);
void spawn_light_flame_large_yellow(edict_t *e);
void spawn_light_flame_small_yellow(edict_t *e);
void spawn_light_flame_small_white(edict_t *e);
void spawn_misc_fireball(edict_t *e);
void spawn_misc_explobox(edict_t *e);
void spawn_misc_explobox2(edict_t *e);
void spawn_trap_spikeshooter(edict_t *e);
void spawn_trap_shooter(edict_t *e);
void spawn_air_bubbles(edict_t *e);
void spawn_viewthing(edict_t *e);
void spawn_func_wall(edict_t *e);
void spawn_func_illusionary(edict_t *e);
void spawn_func_episodegate(edict_t *e);
void spawn_func_bossgate(edict_t *e);
void spawn_ambient_suck_wind(edict_t *e);
void spawn_ambient_drone(edict_t *e);
void spawn_ambient_flouro_buzz(edict_t *e);
void spawn_ambient_drip(edict_t *e);
void spawn_ambient_comp_hum(edict_t *e);
void spawn_ambient_thunder(edict_t *e);
void spawn_ambient_light_buzz(edict_t *e);
void spawn_ambient_swamp1(edict_t *e);
void spawn_ambient_swamp2(edict_t *e);
void spawn_misc_noisemaker(edict_t *e);

// doors.c
void spawn_func_door(edict_t *e);
void spawn_func_door_secret(edict_t *e);

// buttons.c
void spawn_func_button(edict_t *e);

// plats.c
void spawn_func_plat(edict_t *e);
void spawn_func_train(edict_t *e);
void spawn_misc_teleporttrain(edict_t *e);

// triggers.c
void spawn_trigger_multiple(edict_t *e);
void spawn_trigger_once(edict_t *e);
void spawn_trigger_relay(edict_t *e);
void spawn_trigger_secret(edict_t *e);
void spawn_trigger_counter(edict_t *e);
void spawn_info_teleport_destination(edict_t *e);
void spawn_trigger_teleport(edict_t *e);
void spawn_trigger_setskill(edict_t *e);
void spawn_trigger_onlyregistered(edict_t *e);
void spawn_trigger_hurt(edict_t *e);
void spawn_trigger_push(edict_t *e);
void spawn_trigger_monsterjump(edict_t *e);

// monsters
void spawn_monster_fish(edict_t *e);
void spawn_monster_tarbaby(edict_t *e);
void spawn_monster_soldier(edict_t *e);
void spawn_monster_dog(edict_t *e);
void spawn_monster_enforcer(edict_t *e);
void spawn_monster_knight(edict_t *e);
void spawn_monster_demon(edict_t *e);
void spawn_monster_demon1(edict_t *e);
void spawn_monster_zombie(edict_t *e);
void spawn_monster_ogre(edict_t *e);
void spawn_monster_ogre_marksman(edict_t *e);
void spawn_monster_wizard(edict_t *e);
void spawn_monster_hell_knight(edict_t *e);
void spawn_monster_shalrath(edict_t *e);
void spawn_monster_shambler(edict_t *e);
void spawn_monster_boss(edict_t *e);
void spawn_event_lightning(edict_t *e);
void spawn_monster_oldone(edict_t *e);

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
    // ai.c
    { "path_corner",                  spawn_path_corner                     },
    // misc.c
    { "info_null",                    spawn_info_null                       },
    { "info_notnull",                 spawn_info_notnull                    },
    { "light",                        spawn_light                           },
    { "light_fluoro",                 spawn_light_fluoro                    },
    { "light_fluorospark",            spawn_light_fluorospark               },
    { "light_globe",                  spawn_light_globe                     },
    { "light_torch_small_walltorch",  spawn_light_torch_small_walltorch     },
    { "light_flame_large_yellow",     spawn_light_flame_large_yellow        },
    { "light_flame_small_yellow",     spawn_light_flame_small_yellow        },
    { "light_flame_small_white",      spawn_light_flame_small_white         },
    { "misc_fireball",                spawn_misc_fireball                   },
    { "misc_explobox",                spawn_misc_explobox                   },
    { "misc_explobox2",               spawn_misc_explobox2                  },
    { "trap_spikeshooter",            spawn_trap_spikeshooter               },
    { "trap_shooter",                 spawn_trap_shooter                    },
    { "air_bubbles",                  spawn_air_bubbles                     },
    { "viewthing",                    spawn_viewthing                       },
    { "func_wall",                    spawn_func_wall                       },
    { "func_illusionary",             spawn_func_illusionary                },
    { "func_episodegate",             spawn_func_episodegate                },
    { "func_bossgate",                spawn_func_bossgate                   },
    { "ambient_suck_wind",            spawn_ambient_suck_wind               },
    { "ambient_drone",                spawn_ambient_drone                   },
    { "ambient_flouro_buzz",          spawn_ambient_flouro_buzz             },
    { "ambient_drip",                 spawn_ambient_drip                    },
    { "ambient_comp_hum",             spawn_ambient_comp_hum                },
    { "ambient_thunder",              spawn_ambient_thunder                 },
    { "ambient_light_buzz",           spawn_ambient_light_buzz              },
    { "ambient_swamp1",               spawn_ambient_swamp1                  },
    { "ambient_swamp2",               spawn_ambient_swamp2                  },
    { "misc_noisemaker",              spawn_misc_noisemaker                 },
    // doors.c
    { "func_door",                    spawn_func_door                       },
    { "func_door_secret",             spawn_func_door_secret                },
    // buttons.c
    { "func_button",                  spawn_func_button                     },
    // plats.c
    { "func_plat",                    spawn_func_plat                       },
    { "func_train",                   spawn_func_train                      },
    { "misc_teleporttrain",           spawn_misc_teleporttrain              },
    // triggers.c
    { "trigger_multiple",             spawn_trigger_multiple                },
    { "trigger_once",                 spawn_trigger_once                    },
    { "trigger_relay",                spawn_trigger_relay                   },
    { "trigger_secret",               spawn_trigger_secret                  },
    { "trigger_counter",              spawn_trigger_counter                 },
    { "info_teleport_destination",    spawn_info_teleport_destination       },
    { "trigger_teleport",             spawn_trigger_teleport                },
    { "trigger_setskill",             spawn_trigger_setskill                },
    { "trigger_onlyregistered",       spawn_trigger_onlyregistered          },
    { "trigger_hurt",                 spawn_trigger_hurt                    },
    { "trigger_push",                 spawn_trigger_push                    },
    { "trigger_monsterjump",          spawn_trigger_monsterjump             },
    // monsters
    { "monster_fish",              spawn_monster_fish              },
    { "monster_tarbaby",           spawn_monster_tarbaby           },
    { "monster_army",              spawn_monster_soldier           },
    { "monster_dog",               spawn_monster_dog               },
    { "monster_enforcer",          spawn_monster_enforcer          },
    { "monster_knight",            spawn_monster_knight            },
    { "monster_demon",             spawn_monster_demon             },
    { "monster_demon1",            spawn_monster_demon1            },
    { "monster_zombie",            spawn_monster_zombie            },
    { "monster_ogre",              spawn_monster_ogre              },
    { "monster_ogre_marksman",     spawn_monster_ogre_marksman     },
    { "monster_wizard",            spawn_monster_wizard            },
    { "monster_hell_knight",       spawn_monster_hell_knight       },
    { "monster_shalrath",          spawn_monster_shalrath          },
    { "monster_shambler",          spawn_monster_shambler          },
    { "monster_boss",              spawn_monster_boss              },
    { "event_lightning",           spawn_event_lightning           },
    { "monster_oldone",            spawn_monster_oldone            },
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

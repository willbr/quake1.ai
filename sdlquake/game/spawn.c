// spawn.c -- Entity classname → spawn function registry.
// Add one entry here for each entity class as its QC file is ported.

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <string.h>
#include <stdlib.h>	// rand

extern game_globals_t *g;
extern engine_api_t   *eng;

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

static void spawn_info_patrol_node(edict_t *e) {
    e->v.solid    = SOLID_NOT;
    e->v.movetype = MOVETYPE_NONE;
    Sim_Patrol_RegisterNode(e);
}

// info_wind_source -- M4. Per-tick wind injection in the cell at this
// entity's origin. Reads "velocity" if present, else a default outward gust.
static void spawn_info_wind_source(edict_t *e) {
    e->v.solid    = SOLID_NOT;
    e->v.movetype = MOVETYPE_NONE;
    vec3_t v = { e->v.velocity[0], e->v.velocity[1], e->v.velocity[2] };
    float mag = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    if (mag < 1.0f) {                   // mapper left it unset
        v[0] = 0; v[1] = 0; v[2] = 200; // gentle upward draft
    }
    Wind_RegisterSource(e, v);
}

// misc_smokegrenade -- M4 testing prop. A persistent smoke emitter at this
// entity's origin. "amount" key sets per-tick injection (default 0.4),
// "radius" sets affected cells (default 96).
//
// Two channels feed off it:
//   * Wind_AddSmoke seeds the sim_wind density grid, which AI LOS and the
//     gust-extinguishes-lights logic both sample.
//   * SV_Smoke spawns pt_smoke particles directly at the nozzle so the
//     visible smoke "puffs out and dissipates" via the lifetime curve in
//     D_DrawSmokeParticle (small + dense -> big + faded).
static void smokegrenade_think(edict_t *self) {
    float amount = self->v.dmg > 0 ? self->v.dmg : 0.4f;
    float radius = self->v.distance > 0 ? self->v.distance : 96.0f;
    Wind_AddSmoke(self->v.origin, amount * 0.1f, radius);

    // Four puffs per think with wide horizontal spread and slow rise.
    // Each puff's dir is randomised so the cloud fans outward as new
    // puffs spawn; the rate (4 per 50ms = 80/s) keeps enough overlapping
    // puffs alive that the cloud reads as one thick mass rather than a
    // string of discrete balls. Vertical velocity is intentionally low
    // (~10 u/s) so the puff billows for most of its life before drifting
    // off; the ambient wind grid eventually lifts it via wind drag.
    for (int i = 0; i < 3; i++) {
        vec3_t puff_org = {
            self->v.origin[0] + ((float)(rand() & 7) - 3.5f),
            self->v.origin[1] + ((float)(rand() & 7) - 3.5f),
            self->v.origin[2] + 16.0f,
        };
        vec3_t puff_dir = {
            ((float)(rand() & 31) - 15.5f),
            ((float)(rand() & 31) - 15.5f),
            14.0f + ((float)(rand() & 7) - 3.5f),
        };
        eng->SV_Smoke(puff_org, puff_dir, 8, 1);
    }
    self->v.nextthink = g->time + 0.05f;
}
static void spawn_misc_smokegrenade(edict_t *e) {
    eng->PrecacheModel("progs/grenade.mdl");
    e->v.solid     = SOLID_NOT;
    e->v.movetype  = MOVETYPE_NONE;
    eng->SV_SetModel(e, "progs/grenade.mdl");
    e->v.think     = smokegrenade_think;
    e->v.nextthink = g->time + 0.5f;
}
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
void spawn_func_grate(edict_t *e);
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
void spawn_weapon_oilgun(edict_t *e);
void spawn_weapon_flamethrower(edict_t *e);
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
    { "info_patrol_node",             spawn_info_patrol_node                },
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
    { "func_grate",                   spawn_func_grate                      },
    { "info_wind_source",             spawn_info_wind_source                },
    { "misc_smokegrenade",            spawn_misc_smokegrenade               },
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
    { "weapon_oilgun",                    spawn_weapon_oilgun                   },
    { "weapon_flamethrower",              spawn_weapon_flamethrower             },
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
    static const char *s_last_mapname = (const char *)1;  // sentinel: non-NULL invalid ptr
    if (g->mapname != s_last_mapname) {
        s_last_mapname = g->mapname;
        Sim_LevelInit(g->mapname ? g->mapname : "");
    }

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

// Editor introspection — returns the NULL-terminated array of classname
// strings the engine's editor browses. Lazy-built on first call from the
// same s_spawns[] table game_entity_spawn dispatches against; built once
// per DLL load (the names array is static and the underlying classname
// pointers are static-lifetime literals in s_spawns[]).
const char *const *game_list_spawn_classes(int *out_count)
{
    static const char *names[sizeof(s_spawns)/sizeof(s_spawns[0])];
    static int         count = -1;

    if (count < 0) {
        int n = (int)(sizeof(s_spawns)/sizeof(s_spawns[0]));
        int i, c = 0;
        for (i = 0; i < n; i++) {
            if (!s_spawns[i].classname) break;
            names[c++] = s_spawns[i].classname;
        }
        names[c] = NULL;
        count = c;
    }
    if (out_count) *out_count = count;
    return names;
}

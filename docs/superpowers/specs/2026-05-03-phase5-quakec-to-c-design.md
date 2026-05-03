# Phase 5: QuakeC → C Design

## Goal

Replace the QuakeC VM (`pr_*.c`) with native C game logic in a hot-reloadable `game.dll`. The result is a set of clean, hackable `.c` files in `sdlquake/game/` that cover 100% of the original QuakeC progs — every weapon, every monster, every item, every trigger. The VM is removed from the build entirely once porting is complete.

## Approach

**Big-bang cutover with incremental porting.** The VM stays live and runs the game throughout the entire porting effort. In parallel, we expand the ABI to its production shape and translate `.qc` files to `.c` one at a time in dependency order. The C files accumulate in `sdlquake/game/` but do not execute yet — the VM is the regression reference throughout. When all ~30 files are done, a single build flag (`-Dnative_game=true`) drops `pr_*.c` from the build and routes all engine ↔ game communication through the DLL ABI.

## ABI (`sdlquake/game/game_api.h`)

Two structs, version-guarded. Bump `GAME_API_VERSION` on any layout change.

### `game_api_t` — what the engine calls into the game DLL

```c
typedef struct game_api_s {
    int   version;

    void  (*init)(engine_api_t *engine);
    void  (*shutdown)(void);

    // Per-frame
    void  (*start_frame)(void);

    // Entity lifecycle
    void  (*entity_spawn)(edict_t *e, const char *classname);
    void  (*entity_think)(edict_t *e);
    void  (*entity_touch)(edict_t *e, edict_t *other);

    // Client lifecycle
    void  (*client_connect)(edict_t *client);
    void  (*client_disconnect)(edict_t *client);
    void  (*put_client_in_server)(edict_t *client);
    void  (*client_think)(edict_t *client);
    void  (*client_kill)(edict_t *client);

    // Level transitions
    void  (*set_new_parms)(void);
    void  (*set_change_parms)(edict_t *client);

    // Shared globals pointer (owned by game DLL, read/written by both sides)
    game_globals_t *globals;
} game_api_t;
```

### `engine_api_t` — what the game DLL calls into the engine (~60 builtins)

Grouped by category:

```c
typedef struct engine_api_s {
    // Existing (Phase 3)
    void   (*Con_Print)(const char *msg);
    void   (*Cvar_SetValue)(const char *name, float value);
    float  (*Cvar_VariableValue)(const char *name);
    double (*Sys_FloatTime)(void);

    // Entity management
    edict_t *(*ED_Alloc)(void);
    void     (*ED_Free)(edict_t *e);
    edict_t *(*ED_Find)(edict_t *start, const char *field, const char *value);
    edict_t *(*ED_FindRadius)(vec3_t origin, float radius);
    edict_t *(*ED_Next)(edict_t *e);
    edict_t *(*ED_CheckClient)(void);

    // Spatial / physics
    void  (*SV_SetOrigin)(edict_t *e, vec3_t origin);
    void  (*SV_SetModel)(edict_t *e, const char *model);
    void  (*SV_SetSize)(edict_t *e, vec3_t mins, vec3_t maxs);
    int   (*SV_DropToFloor)(edict_t *e);
    int   (*SV_WalkMove)(edict_t *e, float yaw, float dist);
    void  (*SV_ChangeYaw)(edict_t *e);
    void  (*SV_Traceline)(vec3_t start, vec3_t end, int nomonsters, edict_t *skip);
    int   (*SV_CheckBottom)(edict_t *e);
    int   (*SV_PointContents)(vec3_t point);
    vec3_t (*SV_Aim)(edict_t *e, float speed);

    // Messaging
    void  (*SV_BPrint)(int level, const char *msg);
    void  (*SV_SPrint)(edict_t *client, int level, const char *msg);
    void  (*SV_CenterPrint)(edict_t *client, const char *msg);
    void  (*SV_StuffCmd)(edict_t *client, const char *cmd);
    void  (*Cbuf_AddText)(const char *cmd);
    void  (*Con_DPrintf)(const char *msg);

    // Network message writing
    void  (*MSG_WriteByte)(int dest, int val);
    void  (*MSG_WriteChar)(int dest, int val);
    void  (*MSG_WriteShort)(int dest, int val);
    void  (*MSG_WriteLong)(int dest, int val);
    void  (*MSG_WriteAngle)(int dest, float val);
    void  (*MSG_WriteCoord)(int dest, float val);
    void  (*MSG_WriteString)(int dest, const char *s);
    void  (*MSG_WriteEntity)(int dest, edict_t *e);

    // Audio
    void  (*SV_StartSound)(edict_t *e, int channel, const char *sample,
                           float vol, float attenuation);
    void  (*SV_AmbientSound)(vec3_t origin, const char *sample,
                             float vol, float attenuation);
    void  (*SV_LightStyle)(int style, const char *pattern);

    // Math helpers
    void   (*MakeVectors)(vec3_t angles);   // writes globals->v_forward/up/right
    vec3_t (*VectorNormalize)(vec3_t v);
    float  (*VectorLength)(vec3_t v);
    float  (*VectorToYaw)(vec3_t v);
    vec3_t (*VectorToAngles)(vec3_t v);
    float  (*Random)(void);
    float  (*FAbsF)(float f);

    // String conversion
    const char *(*FToS)(float f);
    const char *(*VToS)(vec3_t v);
    const char *(*EToS)(edict_t *e);

    // Precache (server startup only)
    const char *(*PrecacheModel)(const char *path);
    const char *(*PrecacheSound)(const char *path);
    const char *(*PrecacheFile)(const char *path);

    // Misc
    void  (*SV_ChangeLevel)(const char *map);
    void  (*SV_Particle)(vec3_t origin, vec3_t dir, float color, float count);
    void  (*SV_MakeStatic)(edict_t *e);
    void  (*SV_SetSpawnParms)(edict_t *client);
    void  (*Host_Error)(const char *msg);
} engine_api_t;
```

### `game_globals_t` — shared mutable state

Owned by the game DLL, pointed to by both sides. Engine writes `time`, `frametime`, `self`, `other`, `world` before calling game entry points; game writes `trace_*` and `v_forward/up/right` after computing them.

```c
typedef struct {
    float   time;
    float   frametime;
    edict_t *self;
    edict_t *other;
    edict_t *world;

    // makevectors output
    vec3_t  v_forward, v_up, v_right;

    // traceline output
    float   trace_allsolid;
    float   trace_startsolid;
    float   trace_fraction;
    vec3_t  trace_endpos;
    vec3_t  trace_plane_normal;
    float   trace_plane_dist;
    edict_t *trace_ent;
    float   trace_inopen;
    float   trace_inwater;

    // spawn parms
    float   parm[16];

    // misc
    float   serverflags;
    float   total_secrets, found_secrets;
    float   total_monsters, killed_monsters;
    edict_t *msg_entity;
} game_globals_t;
```

## Type System (`sdlquake/game/game_types.h`)

### entvars_t

Defined in the game DLL (not shared with the VM's version). Key changes from original:
- `func_t think / touch / use / blocked` → typed C function pointers
- `string_t classname / model / netname / …` → `const char *`

```c
typedef void (*thinkfn_t)(edict_t *self);
typedef void (*touchfn_t)(edict_t *self, edict_t *other);
typedef void (*usefn_t)(edict_t *self, edict_t *activator);
typedef void (*blockedfn_t)(edict_t *self, edict_t *blocker);

typedef struct entvars_s {
    // Identity
    const char  *classname;
    const char  *model;
    const char  *netname;
    const char  *message;
    const char  *target;
    const char  *targetname;
    const char  *noise, *noise1, *noise2, *noise3;
    const char  *weaponmodel;

    // Callbacks (C function pointers, NULL = no-op)
    thinkfn_t    think;
    touchfn_t    touch;
    usefn_t      use;
    blockedfn_t  blocked;
    float        nextthink;

    // Physics
    vec3_t  origin, oldorigin, velocity, angles, avelocity, punchangle;
    vec3_t  absmin, absmax, mins, maxs, size;
    vec3_t  movedir, view_ofs, v_angle;
    float   ltime, movetype, solid;
    float   waterlevel, watertype;

    // Stats
    float   health, max_health, frags, deadflag;
    float   takedamage, dmg_take, dmg_save;
    float   armortype, armorvalue;
    float   items, weapon, weaponframe, currentammo;
    float   ammo_shells, ammo_nails, ammo_rockets, ammo_cells;

    // Rendering
    float   modelindex, frame, skin, effects;
    float   colormap, team, spawnflags;

    // Links
    edict_t *groundentity, *chain, *enemy, *aiment;
    edict_t *goalentity, *owner, *dmg_inflictor;

    // Player input / view
    float   button0, button1, button2, impulse, fixangle;
    float   idealpitch, ideal_yaw, yaw_speed;
    float   teleport_time;
    int     flags;
} entvars_t;
```

### String fields at runtime

- Compile-time constants (classnames, model paths, sound paths): `static const char *` literals in each `.c` file.
- Map-loaded values (netname, target, targetname, message, etc.): zone-heap copies (`Z_Malloc` + `Q_strcpy`) set by the map loader when it parses entity keyvalues.

## Entity Spawn Registry (`sdlquake/game/spawn.c`)

```c
typedef struct { const char *classname; void (*fn)(edict_t *); } spawn_entry_t;

static const spawn_entry_t s_spawns[] = {
    { "worldspawn",      spawn_worldspawn      },
    { "func_door",       spawn_func_door        },
    { "trigger_once",    spawn_trigger_once     },
    { "monster_ogre",    spawn_monster_ogre     },
    // … one entry per QC entity class
};

void game_entity_spawn(edict_t *e, const char *classname) {
    for (int i = 0; i < (int)(sizeof(s_spawns)/sizeof(s_spawns[0])); i++)
        if (!strcmp(s_spawns[i].classname, classname))
            { s_spawns[i].fn(e); return; }
    // Unknown classname: silently skip (same as original VM behaviour)
}
```

## File Layout

```
sdlquake/game/
  game_api.h        -- ABI structs (engine_api_t, game_api_t, game_globals_t)
  game_types.h      -- entvars_t, type aliases, shared constants
  game_main.c       -- Game_GetAPI(), init, shutdown, start_frame
  spawn.c           -- Spawn registry + game_entity_spawn()
  game_defs.h       -- QC constants (MOVETYPE_*, SOLID_*, FL_*, IT_*, ...)
  subs.c            -- Movement helpers (walkmove wrappers, etc.)
  world.c           -- worldspawn, changelevel, intermission
  client.c          -- connect/disconnect/scores
  player.c          -- movement, view, death
  items.c           -- pickups, powerups
  weapons.c         -- shooting, projectiles, damage
  fight.c           -- monster melee/ranged attack AI
  ai.c              -- monster movement AI
  misc.c            -- lights, secrets, teleporters, info_* entities
  doors.c
  buttons.c
  triggers.c
  plats.c
  monster_dog.c
  monster_soldier.c
  monster_ogre.c
  monster_knight.c
  monster_wizard.c
  monster_demon.c
  monster_shambler.c
  monster_tarbaby.c
  monster_fish.c
  monster_shalrath.c
  monster_zombie.c
  monster_hell_knight.c
  monster_death_knight.c
  monster_enforcer.c
  monster_boss.c    -- Chthon
  monster_boss2.c   -- Shub-Niggurath
```

## Porting Order

Translate in strict dependency order so each file can be compiled and reviewed in isolation:

| Step | QC file(s) | C output | Notes |
|------|-----------|----------|-------|
| 1 | `defs.qc` | `game_defs.h` | Constants only; no code |
| 2 | `subs.qc` | `subs.c` | Movement helpers |
| 3 | `world.qc` | `world.c` | worldspawn, changelevel |
| 4 | `client.qc` | `client.c` | Player lifecycle |
| 5 | `player.qc` | `player.c` | Movement, view, death |
| 6 | `items.qc` | `items.c` | Pickups |
| 7 | `weapons.qc` | `weapons.c` | Shooting, damage |
| 8 | `fight.qc` | `fight.c` | Monster attacks |
| 9 | `ai.qc` | `ai.c` | Monster movement |
| 10 | `misc.qc` | `misc.c` | Ambient entities |
| 11 | `doors.qc` | `doors.c` | |
| 12 | `buttons.qc` | `buttons.c` | |
| 13 | `triggers.qc` | `triggers.c` | |
| 14 | `plats.qc` | `plats.c` | |
| 15–30 | `monsters/*.qc` | `monster_*.c` | One file per monster |

## Cutover Mechanism

`build.zig` exposes a boolean option:

```zig
const native_game = b.option(bool, "native_game", "Use game.dll instead of VM") orelse false;
```

When `native_game = false` (default throughout porting):
- `pr_*.c` files compile into the engine as usual
- The game DLL loads but its entry points are not called for game logic

When `native_game = true` (flip after all files ported):
- `pr_*.c` excluded from the engine build
- `#ifdef NATIVE_GAME` guards in `sv_main.c`, `sv_phys.c`, `pr_edict.c` route:
  - Map entity spawning → `game_api->entity_spawn()`
  - Entity think dispatch → `game_api->entity_think()`
  - Entity touch dispatch → `game_api->entity_touch()`
  - Client lifecycle → `game_api->client_*`
  - Frame start → `game_api->start_frame()`

The `NATIVE_GAME` guards are added progressively as files are ported, so the final cutover is adding `-Dnative_game=true` to the build command.

## Verification

- **During porting:** `zig build run -- +map e1m1` runs on the VM. Each new `.c` file can be read-reviewed against the `.qc` source.
- **At cutover:** `zig build run -Dnative_game=true -- +map e1m1` — load e1m1, walk through the level, kill enemies, pick up items, finish the level. Compare behaviour against VM build.
- **Regression:** Run both builds side-by-side on each episode map. Behaviour must be identical.

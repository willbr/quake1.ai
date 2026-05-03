# Phase 5: QuakeC → C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the QuakeC VM (`pr_*.c`) with hand-ported native C in `sdlquake/game/`, producing clean hackable files that cover 100% of the original progs.

**Architecture:** The VM stays live throughout porting (game always runs). ABI is expanded to production shape first. QC files are translated to C one at a time in dependency order. A `-Dnative_game=true` build flag flips the switch when all files are done.

**Tech Stack:** C99, Zig 0.16 build system, original QuakeC source at `Quake-Tools-master/qcc/v101qc/`

---

## Translation Reference

Apply these rules when translating every `.qc` file to `.c`.

| QC construct | C equivalent |
|---|---|
| `self.field` | `g->self->v.field` |
| `other.field` | `g->other->v.field` |
| `time` | `g->time` |
| `activator` | `g->activator` |
| `skill` | `eng->Cvar_VariableValue("skill")` |
| `self.think = func` | `g->self->v.think = func` |
| `self.think = SUB_Null` | `g->self->v.think = NULL` |
| `spawn()` | `eng->ED_Alloc()` |
| `remove(e)` | `eng->ED_Free(e)` |
| `setorigin(e, v)` | `eng->SV_SetOrigin(e, v)` |
| `setmodel(e, s)` | `eng->SV_SetModel(e, s)` |
| `setsize(e, mn, mx)` | `eng->SV_SetSize(e, mn, mx)` |
| `traceline(a,b,nm,e)` | `eng->SV_Traceline(a,b,nm,e)` then read `g->trace_*` |
| `find(start, .fld, val)` | `eng->ED_Find(start, "fld", val)` |
| `findradius(o, r)` | `eng->ED_FindRadius(o, r)` |
| `sound(e, ch, s, v, a)` | `eng->SV_StartSound(e, ch, s, v, a)` |
| `bprint(s)` | `eng->SV_BPrint(MSG_BROADCAST, s)` |
| `sprint(e, s)` | `eng->SV_SPrint(e, MSG_ONE, s)` |
| `centerprint(e, s)` | `eng->SV_CenterPrint(e, s)` |
| `stuffcmd(e, s)` | `eng->SV_StuffCmd(e, s)` |
| `localcmd(s)` | `eng->Cbuf_AddText(s)` |
| `makevectors(a)` | `eng->MakeVectors(a)` then read `g->v_forward/up/right` |
| `normalize(v)` | `eng->VectorNormalize(v, out)  // out is a vec3_t[3]` |
| `vlen(v)` | `eng->VectorLength(v)` |
| `vectoyaw(v)` | `eng->VectorToYaw(v)` |
| `vectoangles(v)` | `eng->VectorToAngles(v, out)  // out is a vec3_t[3]` |
| `random()` | `eng->Random()` |
| `fabs(f)` | `eng->FAbsF(f)` |
| `walkmove(y, d)` | `eng->SV_WalkMove(g->self, y, d)` |
| `droptofloor()` | `eng->SV_DropToFloor(g->self)` |
| `checkbottom(e)` | `eng->SV_CheckBottom(e)` |
| `pointcontents(v)` | `eng->SV_PointContents(v)` |
| `checkclient()` | `eng->ED_CheckClient()` |
| `nextent(e)` | `eng->ED_Next(e)` |
| `ChangeYaw()` | `eng->SV_ChangeYaw(g->self)` |
| `aim(e, spd)` | `eng->SV_Aim(e, spd, out)  // out is a vec3_t[3]` |
| `particle(o,d,c,n)` | `eng->SV_Particle(o,d,c,n)` |
| `lightstyle(n, s)` | `eng->SV_LightStyle(n, s)` |
| `makestatic(e)` | `eng->SV_MakeStatic(e)` |
| `changelevel(s)` | `eng->SV_ChangeLevel(s)` |
| `precache_model(s)` | `eng->PrecacheModel(s)` |
| `precache_sound(s)` | `eng->PrecacheSound(s)` |
| `precache_file(s)` | `eng->PrecacheFile(s)` |
| `cvar(s)` | `eng->Cvar_VariableValue(s)` |
| `cvar_set(n, v)` | `eng->Cvar_SetValue(n, atof(v))` (note: QC cvar_set takes string) |
| `error(s)` | `eng->Host_Error(s)` |
| `objerror(s)` | `eng->Con_Print(s); eng->ED_Free(g->self)` |
| `WriteByte(dst,v)` | `eng->MSG_WriteByte(dst, v)` |
| `WriteShort(dst,v)` | `eng->MSG_WriteShort(dst, v)` |
| `WriteLong(dst,v)` | `eng->MSG_WriteLong(dst, v)` |
| `WriteAngle(dst,v)` | `eng->MSG_WriteAngle(dst, v)` |
| `WriteCoord(dst,v)` | `eng->MSG_WriteCoord(dst, v)` |
| `WriteString(dst,s)` | `eng->MSG_WriteString(dst, s)` |
| `WriteEntity(dst,e)` | `eng->MSG_WriteEntity(dst, e)` |
| `ambientsound(o,s,v,a)` | `eng->SV_AmbientSound(o,s,v,a)` |
| `ftos(f)` | `eng->FToS(f)` |
| `vtos(v)` | `eng->VToS(v)` |
| `dprint(s)` | `eng->Con_DPrintf(s)` |
| `setspawnparms(e)` | `eng->SV_SetSpawnParms(e)` |
| `movetogoal(step)` | `eng->SV_MoveToGoal(g->self, step)` |
| `rint(f)` | `(float)(int)(f + 0.5f)` |
| `floor(f)` | `(float)(int)(f)` (only for positive values; use `floorf` in general) |
| `ceil(f)` | `ceilf(f)` |
| QC `'x y z'` vector literal | `(vec3_t){x, y, z}` |
| QC `local type name` | C local variable declaration |
| QC `if (!e)` where e is entity | `if (e == g->world)` or `if (!e)` |

**Standard file header** (use in every porting task):
```c
#include "../engine/game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>
#include <string.h>

extern engine_api_t *eng;
extern game_globals_t *g;
```

**g->self pattern:** Functions stored as callbacks (think, touch, use, blocked) receive
self/other explicitly and must set `g->self`/`g->other` at the start:
```c
static void my_think(edict_t *self) {
    g->self = self;
    // ... body using g->self->v.field
}
```
Helper functions (SUB_*, etc.) read g->self directly without taking it as a parameter,
matching QC semantics. Caller is responsible for setting g->self before calling helpers.

---

## Task 1: Expand game_api.h

**Files:**
- Modify: `sdlquake/game/game_api.h`

- [ ] **Step 1: Replace the file entirely**

```c
// game_api.h -- ABI between the engine and the hot-reloadable game DLL.
// Bump GAME_API_VERSION whenever any struct layout changes.

#ifndef GAME_API_H
#define GAME_API_H

#define GAME_API_VERSION 2

// Forward declarations (full definitions in game_types.h)
typedef struct edict_s edict_t;
typedef float vec3_t[3];

// ---------------------------------------------------------------------------
// Shared mutable globals — owned by the game DLL, pointed to by both sides.
// Engine writes time/frametime/self/other before calling any game entry point.
// Game writes trace_* and v_forward/up/right after computing them.
// ---------------------------------------------------------------------------
typedef struct {
    float    time;
    float    frametime;
    edict_t *self;
    edict_t *other;
    edict_t *world;
    edict_t *activator;    // entity that fired a trigger
    edict_t *damage_attacker;
    edict_t *msg_entity;   // destination for MSG_ONE writes

    // makevectors output
    vec3_t   v_forward, v_up, v_right;

    // traceline output
    float    trace_allsolid;
    float    trace_startsolid;
    float    trace_fraction;
    vec3_t   trace_endpos;
    vec3_t   trace_plane_normal;
    float    trace_plane_dist;
    edict_t *trace_ent;
    float    trace_inopen;
    float    trace_inwater;

    // spawn parms (carry across level changes)
    float    parm[16];

    // server state
    float    serverflags;
    float    total_secrets,   found_secrets;
    float    total_monsters,  killed_monsters;
    float    movedist;
    float    gameover;
    float    framecount;
    edict_t *newmis;         // set by launch_spike after spawning

    // map name (set at level load)
    const char *mapname;
} game_globals_t;

// ---------------------------------------------------------------------------
// engine_api_t — functions the engine exposes to the game DLL.
// ---------------------------------------------------------------------------
typedef struct engine_api_s {
    // Phase 3 (unchanged)
    void   (*Con_Print)(const char *msg);
    void   (*Cvar_SetValue)(const char *name, float value);
    float  (*Cvar_VariableValue)(const char *name);
    double (*Sys_FloatTime)(void);

    // Entity management
    edict_t    *(*ED_Alloc)(void);
    void        (*ED_Free)(edict_t *e);
    edict_t    *(*ED_Find)(edict_t *start, const char *field, const char *value);
    edict_t    *(*ED_FindRadius)(vec3_t origin, float radius);
    edict_t    *(*ED_Next)(edict_t *e);
    edict_t    *(*ED_CheckClient)(void);
    int         (*ED_GetNum)(edict_t *e);   // entity index (for network writes)

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
    void  (*SV_MoveToGoal)(edict_t *e, float step);
    vec3_t (*SV_Aim)(edict_t *e, float speed);

    // Output / messaging
    void  (*SV_BPrint)(int level, const char *msg);
    void  (*SV_SPrint)(edict_t *client, int level, const char *msg);
    void  (*SV_CenterPrint)(edict_t *client, const char *msg);
    void  (*SV_StuffCmd)(edict_t *client, const char *cmd);
    void  (*Cbuf_AddText)(const char *cmd);
    void  (*Con_DPrintf)(const char *msg);
    void  (*Host_Error)(const char *msg);

    // Network message writing (dest = MSG_BROADCAST/ONE/ALL/INIT)
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

    // Math
    void   (*MakeVectors)(vec3_t angles);
    vec3_t (*VectorNormalize)(vec3_t v);
    float  (*VectorLength)(vec3_t v);
    float  (*VectorToYaw)(vec3_t v);
    vec3_t (*VectorToAngles)(vec3_t v);
    float  (*Random)(void);
    float  (*FAbsF)(float f);

    // String conversion (return static buffer, use immediately)
    const char *(*FToS)(float f);
    const char *(*VToS)(vec3_t v);

    // Precache (call during entity spawn or level init only)
    const char *(*PrecacheModel)(const char *path);
    const char *(*PrecacheSound)(const char *path);
    const char *(*PrecacheFile)(const char *path);

    // Misc
    void  (*SV_ChangeLevel)(const char *map);
    void  (*SV_Particle)(vec3_t origin, vec3_t dir, float color, float count);
    void  (*SV_MakeStatic)(edict_t *e);
    void  (*SV_SetSpawnParms)(edict_t *client);
} engine_api_t;

// ---------------------------------------------------------------------------
// game_api_t — functions the game DLL exposes to the engine.
// ---------------------------------------------------------------------------
typedef struct game_api_s {
    int   version;   // must equal GAME_API_VERSION

    void  (*init)(engine_api_t *engine, game_globals_t *globals);
    void  (*shutdown)(void);

    // Per server frame (called after physics)
    void  (*start_frame)(void);

    // Entity lifecycle
    void  (*entity_spawn)(edict_t *e, const char *classname);
    void  (*entity_think)(edict_t *e);
    void  (*entity_touch)(edict_t *e, edict_t *other);

    // Client lifecycle
    void  (*client_connect)(edict_t *client);
    void  (*client_disconnect)(edict_t *client);
    void  (*put_client_in_server)(edict_t *client);
    void  (*client_think)(edict_t *client);   // process usercmd
    void  (*client_kill)(edict_t *client);

    // Level transitions
    void  (*set_new_parms)(void);
    void  (*set_change_parms)(edict_t *client);
} game_api_t;

typedef game_api_t *(*Game_GetAPI_fn)(void);

#endif // GAME_API_H
```

- [ ] **Step 2: Build to verify the header compiles**

```
zig build game
```
Expected: compiles (the stub game_main.c will fail — fix in Task 4).

- [ ] **Step 3: Commit**

```
git add sdlquake/game/game_api.h
git commit -m "phase5: expand game_api.h to full production ABI"
```

---

## Task 2: Create game_types.h

**Files:**
- Create: `sdlquake/game/game_types.h`

- [ ] **Step 1: Create the file**

```c
// game_types.h -- Native C entity and type definitions for the game DLL.
// Replaces the VM's entvars_t and progdefs.q1 for the NATIVE_GAME build.

#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include "../engine/game_api.h"

// ---------------------------------------------------------------------------
// Callback signatures — match the C function pointer fields in entvars_t.
// ---------------------------------------------------------------------------
typedef void (*thinkfn_t)(edict_t *self);
typedef void (*touchfn_t)(edict_t *self, edict_t *other);
typedef void (*usefn_t)(edict_t *self, edict_t *activator);
typedef void (*blockedfn_t)(edict_t *self, edict_t *blocker);
typedef void (*painfn_t)(edict_t *self, edict_t *attacker, float damage);

// ---------------------------------------------------------------------------
// entvars_t -- per-entity fields. All func_t → C fn ptrs, string_t → char*.
// This struct is embedded as edict_t.v in the engine's edict_t definition.
// IMPORTANT: The layout of the engine's edict_t shell (free, area, leafnums,
// baseline, freetime) is NOT changed. Only entvars_t v changes.
// ---------------------------------------------------------------------------
typedef struct entvars_s {
    // Identity
    const char  *classname;
    const char  *model;
    const char  *netname;
    const char  *message;
    const char  *target;
    const char  *targetname;
    const char  *killtarget;
    const char  *noise, *noise1, *noise2, *noise3, *noise4;
    const char  *weaponmodel;
    const char  *wad, *map;
    const char  *mdl;
    const char  *deathtype;

    // Callbacks (NULL = no-op)
    thinkfn_t    think;
    touchfn_t    touch;
    usefn_t      use;
    blockedfn_t  blocked;
    float        nextthink;

    // Monster AI callbacks
    thinkfn_t    th_stand;
    thinkfn_t    th_walk;
    thinkfn_t    th_run;
    thinkfn_t    th_missile;
    thinkfn_t    th_melee;
    painfn_t     th_pain;
    thinkfn_t    th_die;

    // Subs helpers
    thinkfn_t    think1;
    vec3_t       finaldest;
    vec3_t       finalangle;

    // Physics
    vec3_t  origin, oldorigin, velocity, angles, avelocity, punchangle;
    vec3_t  absmin, absmax, mins, maxs, size;
    vec3_t  movedir, view_ofs, v_angle;
    vec3_t  dest, dest1, dest2;
    vec3_t  mangle, pos1, pos2;
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
    float   worldtype;

    // Links (edict pointers)
    edict_t *groundentity, *chain, *enemy, *aiment;
    edict_t *goalentity, *owner, *dmg_inflictor;
    edict_t *movetarget, *oldenemy, *trigger_field;

    // Player / combat
    float   button0, button1, button2, impulse, fixangle;
    float   idealpitch, ideal_yaw, yaw_speed;
    float   teleport_time;
    float   attack_finished, pain_finished;
    float   invincible_finished, invisible_finished;
    float   super_damage_finished, radsuit_finished;
    float   invincible_time, invincible_sound;
    float   invisible_time, invisible_sound;
    float   super_time, super_sound, rad_time, fly_sound;
    float   axhitme, show_hostile, jump_flag, swim_flag;
    float   air_finished, bubble_count;
    float   walkframe;
    int     flags;

    // Monster AI
    float   speed, lefty, search_time, attack_state;
    float   pausetime;

    // World / doors / platforms
    float   wait, delay;
    float   state, height, lip, aflag, dmg;
    float   t_length, t_width;
    float   light_lev, style;
    float   cnt, count;
    float   sounds, volume, distance, waitmin, waitmax;

    // Doors / triggers
    float   aflag2; // unused, kept for field-count compat
} entvars_t;

#endif // GAME_TYPES_H
```

- [ ] **Step 2: Build**

```
zig build game
```
Expected: compiles (game_main.c doesn't include game_types.h yet — that's fine).

- [ ] **Step 3: Commit**

```
git add sdlquake/game/game_types.h
git commit -m "phase5: add game_types.h with native entvars_t"
```

---

## Task 3: Create game_defs.h

**Files:**
- Create: `sdlquake/game/game_defs.h`

Source reference: `Quake-Tools-master/qcc/v101qc/defs.qc` (constants section, lines 245–430)

- [ ] **Step 1: Create the file — translate all QC float constants to #defines**

```c
// game_defs.h -- QC constants ported to C. Source: defs.qc

#ifndef GAME_DEFS_H
#define GAME_DEFS_H

// edict.flags
#define FL_FLY              1
#define FL_SWIM             2
#define FL_CLIENT           8
#define FL_INWATER          16
#define FL_MONSTER          32
#define FL_GODMODE          64
#define FL_NOTARGET         128
#define FL_ITEM             256
#define FL_ONGROUND         512
#define FL_PARTIALGROUND    1024
#define FL_WATERJUMP        2048
#define FL_JUMPRELEASED     4096

// movetype
#define MOVETYPE_NONE        0
#define MOVETYPE_WALK        3
#define MOVETYPE_STEP        4
#define MOVETYPE_FLY         5
#define MOVETYPE_TOSS        6
#define MOVETYPE_PUSH        7
#define MOVETYPE_NOCLIP      8
#define MOVETYPE_FLYMISSILE  9
#define MOVETYPE_BOUNCE      10
#define MOVETYPE_BOUNCEMISSILE 11

// solid
#define SOLID_NOT      0
#define SOLID_TRIGGER  1
#define SOLID_BBOX     2
#define SOLID_SLIDEBOX 3
#define SOLID_BSP      4

// range
#define RANGE_MELEE 0
#define RANGE_NEAR  1
#define RANGE_MID   2
#define RANGE_FAR   3

// deadflag
#define DEAD_NO          0
#define DEAD_DYING       1
#define DEAD_DEAD        2
#define DEAD_RESPAWNABLE 3

// takedamage
#define DAMAGE_NO  0
#define DAMAGE_YES 1
#define DAMAGE_AIM 2

// items (IT_*)
#define IT_AXE              4096
#define IT_SHOTGUN          1
#define IT_SUPER_SHOTGUN    2
#define IT_NAILGUN          4
#define IT_SUPER_NAILGUN    8
#define IT_GRENADE_LAUNCHER 16
#define IT_ROCKET_LAUNCHER  32
#define IT_LIGHTNING        64
#define IT_EXTRA_WEAPON     128
#define IT_SHELLS           256
#define IT_NAILS            512
#define IT_ROCKETS          1024
#define IT_CELLS            2048
#define IT_ARMOR1           8192
#define IT_ARMOR2           16384
#define IT_ARMOR3           32768
#define IT_SUPERHEALTH      65536
#define IT_KEY1             131072
#define IT_KEY2             262144
#define IT_INVISIBILITY     524288
#define IT_INVULNERABILITY  1048576
#define IT_SUIT             2097152
#define IT_QUAD             4194304

// point contents
#define CONTENT_EMPTY  (-1)
#define CONTENT_SOLID  (-2)
#define CONTENT_WATER  (-3)
#define CONTENT_SLIME  (-4)
#define CONTENT_LAVA   (-5)
#define CONTENT_SKY    (-6)

// platform/door states
#define STATE_TOP    0
#define STATE_BOTTOM 1
#define STATE_UP     2
#define STATE_DOWN   3

// protocol bytes
#define SVC_TEMPENTITY  23
#define SVC_KILLEDMONSTER 27
#define SVC_FOUNDSECRET 28
#define SVC_INTERMISSION 30
#define SVC_FINALE      31
#define SVC_CDTRACK     32
#define SVC_SELLSCREEN  33

// temp entity types
#define TE_SPIKE       0
#define TE_SUPERSPIKE  1
#define TE_GUNSHOT     2
#define TE_EXPLOSION   3
#define TE_TAREXPLOSION 4
#define TE_LIGHTNING1  5
#define TE_LIGHTNING2  6
#define TE_WIZSPIKE    7
#define TE_KNIGHTSPIKE 8
#define TE_LIGHTNING3  9
#define TE_LAVASPLASH  10
#define TE_TELEPORT    11

// sound channels
#define CHAN_AUTO   0
#define CHAN_WEAPON 1
#define CHAN_VOICE  2
#define CHAN_ITEM   3
#define CHAN_BODY   4

// attenuation
#define ATTN_NONE   0
#define ATTN_NORM   1
#define ATTN_IDLE   2
#define ATTN_STATIC 3

// message destinations
#define MSG_BROADCAST 0
#define MSG_ONE       1
#define MSG_ALL       2
#define MSG_INIT      3

// entity effects
#define EF_BRIGHTFIELD 1
#define EF_MUZZLEFLASH 2
#define EF_BRIGHTLIGHT 4
#define EF_DIMLIGHT    8

// attack state (monster AI)
#define AS_STRAIGHT 1
#define AS_SLIDING  2
#define AS_MELEE    3
#define AS_MISSILE  4

// hull sizes
#define VEC_HULL_MIN_X (-16.0f)
#define VEC_HULL_MIN_Y (-16.0f)
#define VEC_HULL_MIN_Z (-24.0f)
#define VEC_HULL_MAX_X  16.0f
#define VEC_HULL_MAX_Y  16.0f
#define VEC_HULL_MAX_Z  32.0f

#define VEC_HULL2_MIN_X (-32.0f)
#define VEC_HULL2_MIN_Y (-32.0f)
#define VEC_HULL2_MIN_Z (-24.0f)
#define VEC_HULL2_MAX_X  32.0f
#define VEC_HULL2_MAX_Y  32.0f
#define VEC_HULL2_MAX_Z  64.0f

#endif // GAME_DEFS_H
```

- [ ] **Step 2: Build**

```
zig build game
```

- [ ] **Step 3: Commit**

```
git add sdlquake/game/game_defs.h
git commit -m "phase5: add game_defs.h (QC constants)"
```

---

## Task 4: Update game_main.c with full entry point stubs

**Files:**
- Modify: `sdlquake/game/game_main.c`

- [ ] **Step 1: Replace game_main.c entirely**

```c
// game_main.c -- Entry point and lifecycle for the native game DLL.

#include "../engine/game_api.h"
#include "game_types.h"
#include "game_defs.h"

engine_api_t  *eng;
game_globals_t *g;

// Forward declarations of entry points implemented across game files.
// (These stubs will be replaced as each QC file is ported.)
void world_spawn(edict_t *e, const char *classname);
void game_start_frame(void);
void game_client_connect(edict_t *client);
void game_client_disconnect(edict_t *client);
void game_put_client_in_server(edict_t *client);
void game_client_think(edict_t *client);
void game_client_kill(edict_t *client);
void game_set_new_parms(void);
void game_set_change_parms(edict_t *client);

// Declared in spawn.c
void game_entity_spawn(edict_t *e, const char *classname);

static void game_entity_think(edict_t *e)
{
    if (e->v.think)
        e->v.think(e);
}

static void game_entity_touch(edict_t *e, edict_t *other)
{
    if (e->v.touch)
        e->v.touch(e, other);
}

static void game_init(engine_api_t *engine, game_globals_t *globals)
{
    eng = engine;
    g   = globals;
}

static void game_shutdown(void) { }

static game_api_t s_api = {
    GAME_API_VERSION,
    game_init,
    game_shutdown,
    game_start_frame,
    game_entity_spawn,
    game_entity_think,
    game_entity_touch,
    game_client_connect,
    game_client_disconnect,
    game_put_client_in_server,
    game_client_think,
    game_client_kill,
    game_set_new_parms,
    game_set_change_parms,
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
game_api_t *Game_GetAPI(void) { return &s_api; }

// ---------------------------------------------------------------------------
// Stub implementations — replaced file-by-file as QC is ported
// ---------------------------------------------------------------------------
void game_start_frame(void)                              { }
void game_client_connect(edict_t *client)                { (void)client; }
void game_client_disconnect(edict_t *client)             { (void)client; }
void game_put_client_in_server(edict_t *client)          { (void)client; }
void game_client_think(edict_t *client)                  { (void)client; }
void game_client_kill(edict_t *client)                   { (void)client; }
void game_set_new_parms(void)                            { }
void game_set_change_parms(edict_t *client)              { (void)client; }
```

- [ ] **Step 2: Build**

```
zig build game
```
Expected: ERROR — `edict_t` is incomplete (we haven't updated the engine's edict_t yet).
This is expected at this stage; fix will come in Task 7.

- [ ] **Step 3: Commit the updated stub regardless**

```
git add sdlquake/game/game_main.c
git commit -m "phase5: update game_main.c with full entry point stubs"
```

---

## Task 5: Create spawn.c (spawn registry stub)

**Files:**
- Create: `sdlquake/game/spawn.c`

- [ ] **Step 1: Create the file with an empty table (entries added per porting task)**

```c
// spawn.c -- Entity classname → spawn function registry.
// Add one entry here for each entity class as its QC file is ported.

#include "../engine/game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

typedef struct { const char *classname; void (*fn)(edict_t *); } spawn_entry_t;

// Forward declarations — added as files are ported
// (none yet)

static const spawn_entry_t s_spawns[] = {
    // populated in later tasks
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
```

- [ ] **Step 2: Add spawn.c to build.zig game sources**

Open `build.zig`, find the list of game source files, add `"sdlquake/game/spawn.c"`.

- [ ] **Step 3: Build**

```
zig build game
```

- [ ] **Step 4: Commit**

```
git add sdlquake/game/spawn.c build.zig
git commit -m "phase5: add spawn registry (empty, populated per porting task)"
```

---

## Task 6: Add -Dnative_game build option

**Files:**
- Modify: `build.zig`

- [ ] **Step 1: Add the option and wire it as a compile definition**

In `build.zig`, locate where engine sources are listed. Add:

```zig
const native_game = b.option(bool, "native_game", "Route game logic through game.dll instead of VM") orelse false;

// Pass to engine C files as a preprocessor define
exe.defineCMacro("NATIVE_GAME", if (native_game) "1" else "0");
```

- [ ] **Step 2: Build with and without the flag to confirm it works**

```
zig build game
zig build game -Dnative_game=true
```
Expected: both compile (the macro value doesn't affect game.dll, only the engine).

- [ ] **Step 3: Commit**

```
git add build.zig
git commit -m "phase5: add -Dnative_game build option"
```

---

## Task 7: Add NATIVE_GAME dispatch guards in the engine

**Files:**
- Modify: `Quake-master/WinQuake/pr_edict.c` (entity spawn dispatch)
- Modify: `Quake-master/WinQuake/sv_phys.c` (think/touch dispatch)
- Modify: `Quake-master/WinQuake/sv_main.c` (client lifecycle)
- Modify: `sdlquake/engine/hotreload.c` (pass globals to init)

These are **minimal, targeted additions** — `#if NATIVE_GAME` guards around the dispatch points only. The VM path is untouched and remains the default.

- [ ] **Step 1: Find entity spawn call in pr_edict.c**

Search `Quake-master/WinQuake/pr_edict.c` for where it calls the QC `spawn` function (look for `PR_ExecuteProgram` near entity field parsing). Wrap that call:

```c
#if NATIVE_GAME
    extern game_api_t *g_game_api;
    if (g_game_api)
        g_game_api->entity_spawn(ent, classname_string);
#else
    // original PR_ExecuteProgram call
    PR_ExecuteProgram(spawn_fn);
#endif
```

The exact lines to wrap depend on the call site — read the function that iterates entities and calls their spawn function after `ED_LoadFromFile`. The pattern is: find `PR_ExecuteProgram (ed->v.classname_fn)` in the entity-loading loop and add the guard.

- [ ] **Step 2: Find think dispatch in sv_phys.c**

Search for `PR_ExecuteProgram` in `sv_phys.c`. The think dispatch looks like:
```c
PR_ExecuteProgram (ent->v.think);
```
Wrap it:
```c
#if NATIVE_GAME
    if (g_game_api) g_game_api->entity_think(ent);
#else
    PR_ExecuteProgram (ent->v.think);
#endif
```

Do the same for touch dispatch (search for `.touch` near `PR_ExecuteProgram`).

- [ ] **Step 3: Find client lifecycle calls in sv_main.c**

Search `sv_main.c` for `PR_ExecuteProgram` calls near `ClientConnect`, `ClientDisconnect`, `PutClientInServer`, `ClientKill`, `SetNewParms`, `SetChangeParms`, `PlayerPreThink`, `PlayerPostThink`, `StartFrame`. Wrap each with a corresponding `g_game_api->client_*` call.

- [ ] **Step 4: Update hotreload.c to pass globals on init**

In `sdlquake/engine/hotreload.c`, find where `game_api->init` is called and add the globals argument:

```c
// Before:
game_api->init(engine_api);
// After:
game_api->init(engine_api, &game_globals);  // game_globals is a game_globals_t allocated here
```

Declare `static game_globals_t game_globals;` in hotreload.c. The engine fills `game_globals.time`, `game_globals.frametime`, etc. before calling game entry points.

- [ ] **Step 5: Build (VM path — native_game=false)**

```
zig build run -- +map e1m1
```
Expected: game runs normally on the VM. The guards are compiled in but the NATIVE_GAME=0 branch is taken.

- [ ] **Step 6: Commit**

```
git add Quake-master/WinQuake/pr_edict.c Quake-master/WinQuake/sv_phys.c
git add Quake-master/WinQuake/sv_main.c sdlquake/engine/hotreload.c
git commit -m "phase5: add NATIVE_GAME dispatch guards in engine"
```

---

## Task 8: Port subs.qc → subs.c

**Source:** `Quake-Tools-master/qcc/v101qc/subs.qc`

**Files:**
- Create: `sdlquake/game/subs.c`

This is the **reference porting example** — subsequent tasks follow the same pattern.

- [ ] **Step 1: Create subs.c**

```c
// subs.c -- Movement and targeting helpers. Source: subs.qc

#include "../engine/game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t  *eng;
extern game_globals_t *g;

void SUB_Null(edict_t *self)  { (void)self; }
void SUB_Remove(edict_t *self) { g->self = self; eng->ED_Free(self); }

static void SetMovedir(void)
{
    vec3_t down  = {0,  0, -1};
    vec3_t up    = {0,  0,  1};
    vec3_t zero  = {0,  0,  0};
    vec3_t neg1  = {0, -1,  0};
    vec3_t neg2  = {0, -2,  0};

    if (g->self->v.angles[0] == neg1[0] && g->self->v.angles[1] == neg1[1] && g->self->v.angles[2] == neg1[2])
        memcpy(g->self->v.movedir, up, sizeof(vec3_t));
    else if (g->self->v.angles[0] == neg2[0] && g->self->v.angles[1] == neg2[1] && g->self->v.angles[2] == neg2[2])
        memcpy(g->self->v.movedir, down, sizeof(vec3_t));
    else {
        eng->MakeVectors(g->self->v.angles);
        memcpy(g->self->v.movedir, g->v_forward, sizeof(vec3_t));
    }
    memcpy(g->self->v.angles, zero, sizeof(vec3_t));
}

void InitTrigger(void)
{
    vec3_t zero = {0,0,0};
    if (g->self->v.angles[0] || g->self->v.angles[1] || g->self->v.angles[2])
        SetMovedir();
    g->self->v.solid = SOLID_TRIGGER;
    eng->SV_SetModel(g->self, g->self->v.model);
    g->self->v.movetype   = MOVETYPE_NONE;
    g->self->v.modelindex = 0;
    g->self->v.model      = "";
}

static void SUB_CalcMoveDone(edict_t *self);
static void SUB_CalcAngleMoveDone(edict_t *self);
void SUB_CalcMove(vec3_t tdest, float tspeed, thinkfn_t func);   // defined below

void SUB_CalcMoveEnt(edict_t *ent, vec3_t tdest, float tspeed, thinkfn_t func)
{
    edict_t *stemp = g->self;
    g->self = ent;
    SUB_CalcMove(tdest, tspeed, func);
    g->self = stemp;
}

void SUB_CalcMove(vec3_t tdest, float tspeed, thinkfn_t func)
{
    vec3_t vdestdelta;
    float  len, traveltime;

    if (!tspeed) { eng->Host_Error("SUB_CalcMove: no speed"); return; }

    g->self->v.think1    = func;
    memcpy(g->self->v.finaldest, tdest, sizeof(vec3_t));
    g->self->v.think     = SUB_CalcMoveDone;

    vdestdelta[0] = tdest[0] - g->self->v.origin[0];
    vdestdelta[1] = tdest[1] - g->self->v.origin[1];
    vdestdelta[2] = tdest[2] - g->self->v.origin[2];

    len = eng->VectorLength(vdestdelta);

    if (len == 0.0f || (traveltime = len / tspeed) < 0.1f) {
        g->self->v.velocity[0] = g->self->v.velocity[1] = g->self->v.velocity[2] = 0;
        g->self->v.nextthink = g->self->v.ltime + 0.1f;
        return;
    }

    g->self->v.nextthink    = g->self->v.ltime + traveltime;
    g->self->v.velocity[0]  = vdestdelta[0] / traveltime;
    g->self->v.velocity[1]  = vdestdelta[1] / traveltime;
    g->self->v.velocity[2]  = vdestdelta[2] / traveltime;
}

static void SUB_CalcMoveDone(edict_t *self)
{
    g->self = self;
    eng->SV_SetOrigin(self, self->v.finaldest);
    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0;
    self->v.nextthink = -1;
    if (self->v.think1)
        self->v.think1(self);
}

void SUB_CalcAngleMoveEnt(edict_t *ent, vec3_t destangle, float tspeed, thinkfn_t func)
{
    edict_t *stemp = g->self;
    g->self = ent;
    SUB_CalcAngleMove(destangle, tspeed, func);
    g->self = stemp;
}

void SUB_CalcAngleMove(vec3_t destangle, float tspeed, thinkfn_t func)
{
    vec3_t destdelta;
    float  len, traveltime;

    if (!tspeed) { eng->Host_Error("SUB_CalcAngleMove: no speed"); return; }

    destdelta[0] = destangle[0] - g->self->v.angles[0];
    destdelta[1] = destangle[1] - g->self->v.angles[1];
    destdelta[2] = destangle[2] - g->self->v.angles[2];
    len          = eng->VectorLength(destdelta);
    traveltime   = len / tspeed;

    g->self->v.nextthink     = g->self->v.ltime + traveltime;
    g->self->v.avelocity[0]  = destdelta[0] / traveltime;
    g->self->v.avelocity[1]  = destdelta[1] / traveltime;
    g->self->v.avelocity[2]  = destdelta[2] / traveltime;
    g->self->v.think1        = func;
    memcpy(g->self->v.finalangle, destangle, sizeof(vec3_t));
    g->self->v.think         = SUB_CalcAngleMoveDone;
}

static void SUB_CalcAngleMoveDone(edict_t *self)
{
    g->self = self;
    memcpy(self->v.angles, self->v.finalangle, sizeof(vec3_t));
    self->v.avelocity[0] = self->v.avelocity[1] = self->v.avelocity[2] = 0;
    self->v.nextthink = -1;
    if (self->v.think1)
        self->v.think1(self);
}

static void DelayThink(edict_t *self)
{
    g->self = self;
    g->activator = self->v.enemy;
    SUB_UseTargets();
    eng->ED_Free(self);
}

void SUB_UseTargets(void)
{
    edict_t *t, *stemp, *otemp, *act;

    if (g->self->v.delay) {
        t = eng->ED_Alloc();
        t->v.classname  = "DelayedUse";
        t->v.nextthink  = g->time + g->self->v.delay;
        t->v.think      = DelayThink;
        t->v.enemy      = g->activator;
        t->v.message    = g->self->v.message;
        t->v.killtarget = g->self->v.killtarget;
        t->v.target     = g->self->v.target;
        return;
    }

    if (g->activator && !strcmp(g->activator->v.classname, "player") &&
        g->self->v.message && g->self->v.message[0]) {
        eng->SV_CenterPrint(g->activator, g->self->v.message);
        if (!g->self->v.noise)
            eng->SV_StartSound(g->activator, CHAN_VOICE, "misc/talk.wav", 1, ATTN_NORM);
    }

    if (g->self->v.killtarget) {
        t = g->world;
        while ((t = eng->ED_Find(t, "targetname", g->self->v.killtarget)) != g->world)
            eng->ED_Free(t);
    }

    if (g->self->v.target) {
        act   = g->activator;
        t     = g->world;
        while ((t = eng->ED_Find(t, "targetname", g->self->v.target)) != g->world) {
            stemp = g->self;
            otemp = g->other;
            g->self  = t;
            g->other = stemp;
            if (g->self->v.use)
                g->self->v.use(g->self, act);
            g->self      = stemp;
            g->other     = otemp;
            g->activator = act;
        }
    }
}

void SUB_AttackFinished(float normal)
{
    g->self->v.cnt = 0;
    if (eng->Cvar_VariableValue("skill") != 3)
        g->self->v.attack_finished = g->time + normal;
}

int visible(edict_t *targ);   // declared in combat.c

void SUB_CheckRefire(thinkfn_t thinkst)
{
    if (eng->Cvar_VariableValue("skill") != 3) return;
    if (g->self->v.cnt == 1) return;
    if (!visible(g->self->v.enemy)) return;
    g->self->v.cnt   = 1;
    g->self->v.think = thinkst;
}
```

- [ ] **Step 2: Add to build.zig game sources**

Add `"sdlquake/game/subs.c"` to the game DLL source list.

- [ ] **Step 3: Build**

```
zig build game
```
Expected: compiles (undefined reference to `visible` is resolved after combat.c is added).

- [ ] **Step 4: Commit**

```
git add sdlquake/game/subs.c build.zig
git commit -m "phase5: port subs.qc to subs.c"
```

---

## Task 9: Port combat.qc → combat.c

**Source:** `Quake-Tools-master/qcc/v101qc/combat.qc`

**Files:**
- Create: `sdlquake/game/combat.c`

Key functions: `CanDamage`, `visible` (also used in ai.qc), `T_Damage`, `T_Heal`.

- [ ] **Step 1: Read the source**

Open `Quake-Tools-master/qcc/v101qc/combat.qc`. This file defines T_Damage (damage application, knockback, death dispatch) and CanDamage (LOS check via traceline).

- [ ] **Step 2: Create combat.c using the Translation Reference at the top of this plan**

Header block:
```c
// combat.c -- Damage and visibility. Source: combat.qc
#include "../engine/game_api.h"
#include "game_types.h"
#include "game_defs.h"
extern engine_api_t  *eng;
extern game_globals_t *g;
```

Key signatures:
```c
int  CanDamage(edict_t *targ, edict_t *inflictor);
int  visible(edict_t *targ);
void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
int  T_Heal(edict_t *e, float healamount, int ignore);
```

Translate each function body from QC to C using the Translation Reference above.

- [ ] **Step 3: Add to build.zig, build, commit**

```
zig build game
git add sdlquake/game/combat.c build.zig
git commit -m "phase5: port combat.qc to combat.c"
```

---

## Tasks 10–22: Port remaining non-monster files

Follow the exact same pattern as Tasks 8–9 for each file:
1. Read the QC source (path listed below)
2. Create the C file with correct header block
3. Apply Translation Reference for all constructs
4. Add spawn table entries to spawn.c for any entity classnames defined
5. Add to build.zig, `zig build game`, commit

| Task | QC source | C output | Spawn entries | Key functions |
|------|-----------|----------|---------------|---------------|
| 10 | `v101qc/world.qc` | `world.c` | `worldspawn`, `info_null`, `info_notnull`, `path_corner` | `worldspawn`, `StartFrame`, `changelevel triggers` |
| 11 | `v101qc/client.qc` | `client.c` | (no classnames) | `ClientConnect`, `PutClientInServer`, `ClientDisconnect`, `ClientKill`, `SetNewParms`, `SetChangeParms`, `PlayerPreThink`, `PlayerPostThink` |
| 12 | `v101qc/player.qc` | `player.c` | `player_run` frame sequences | `PlayerDie`, `DeathBubbles`, view bobbing, swimming |
| 13 | `v101qc/items.qc` | `items.c` | All `item_*` and `weapon_*` classnames | `weapon_touch`, `armor_touch`, `health_touch`, powerup touches |
| 14 | `v101qc/weapons.qc` | `weapons.c` | `spike`, `laser`, etc projectiles | `W_Attack`, `FireBullets`, all fire functions, `launch_spike` |
| 15 | `v101qc/fight.qc` | `fight.c` | (no classnames) | `ai_face`, `ai_charge`, `ai_melee`, `ai_missile` |
| 16 | `v101qc/ai.qc` | `ai.c` | (no classnames) | `ai_stand`, `ai_walk`, `ai_run`, `ai_turn`, `FindTarget`, `HuntTarget`, `SightSound` |
| 17 | `v101qc/misc.qc` | `misc.c` | `ambient_*`, `func_illusionary`, `light`, `info_*`, `misc_fireball`, `misc_explobox`, `misc_teleporttrain`, teleport logic | `teleport_touch` |
| 18 | `v101qc/doors.qc` | `doors.c` | `func_door`, `func_door_secret` | Door state machine, `door_use`, `door_blocked`, `door_touch` |
| 19 | `v101qc/buttons.qc` | `buttons.c` | `func_button` | Button state machine, `button_touch`, `button_use` |
| 20 | `v101qc/triggers.qc` | `triggers.c` | `trigger_*`, `trigger_once`, `trigger_multiple`, `trigger_secret`, `trigger_counter`, `trigger_push`, `trigger_hurt`, `trigger_relay`, `trigger_monsterjump` | `trigger_touch`, `multi_trigger` |
| 21 | `v101qc/plats.qc` | `plats.c` | `func_plat`, `func_train` | Platform/train state machines |
| 22 | `v101qc/monsters.qc` | `monsters.c` | (no classnames — shared monster helpers) | `monster_death_use`, `DropBackpack`, `WalkMove` wrappers |

**For each porting task, also update spawn.c:**
```c
// Add to s_spawns[] in spawn.c — one entry per classname
{ "worldspawn",          spawn_worldspawn       },  // Task 10
{ "func_door",           spawn_func_door        },  // Task 18
// etc.
```

And add the corresponding forward declaration at the top of spawn.c:
```c
void spawn_worldspawn(edict_t *e);
void spawn_func_door(edict_t *e);
```

---

## Tasks 23–37: Port monster files

Follow the same pattern. Source directory: `Quake-Tools-master/qcc/v101qc/`

| Task | QC source | C output | Spawn classname(s) |
|------|-----------|----------|--------------------|
| 23 | `dog.qc` | `monster_dog.c` | `monster_dog` |
| 24 | `soldier.qc` | `monster_soldier.c` | `monster_army` |
| 25 | `ogre.qc` | `monster_ogre.c` | `monster_ogre`, `monster_ogre_marksman` |
| 26 | `knight.qc` | `monster_knight.c` | `monster_knight` |
| 27 | `wizard.qc` | `monster_wizard.c` | `monster_wizard` |
| 28 | `demon.qc` | `monster_demon.c` | `monster_demon1` |
| 29 | `shambler.qc` | `monster_shambler.c` | `monster_shambler` |
| 30 | `tarbaby.qc` | `monster_tarbaby.c` | `monster_tarbaby` |
| 31 | `fish.qc` | `monster_fish.c` | `monster_fish` |
| 32 | `shalrath.qc` | `monster_shalrath.c` | `monster_shalrath` |
| 33 | `zombie.qc` | `monster_zombie.c` | `monster_zombie` |
| 34 | `hknight.qc` | `monster_hknight.c` | `monster_hell_knight` |
| 35 | `enforcer.qc` | `monster_enforcer.c` | `monster_enforcer` |
| 36 | `boss.qc` | `monster_chthon.c` | `monster_boss` |
| 37 | `oldone.qc` | `monster_shub.c` | `monster_oldone` |

Each monster file follows the same steps:
1. Read the QC source
2. Create the C file with the standard header block
3. Translate all frame macros (`$frame walk1 walk2 ...` style) to C: define each frame function as a static thinkfn_t and set `e->v.frame` to the frame number
4. Translate the spawn function and add it to spawn.c
5. Build, commit

**Frame macro pattern** (QC uses `$frame` syntax compiled by qcc; in C, write explicitly):
```c
// QC: void() dog_walk1 = [0, dog_walk2] { ai_walk(8); };
// C:
static void dog_walk2(edict_t *self);
static void dog_walk1(edict_t *self) {
    g->self = self;
    self->v.frame    = 0;
    self->v.nextthink = g->time + 0.1f;
    self->v.think    = dog_walk2;
    ai_walk(8);
}
```

---

## Task 38: Cutover — remove VM, enable native game

This task is only done after Tasks 8–37 are complete and `zig build game` succeeds.

**Files:**
- Modify: `build.zig` (exclude pr_*.c when native_game=true)
- Update: `CLAUDE.md`, `README.md`

- [ ] **Step 1: Verify all spawn entries are registered**

```
grep -c "classname" sdlquake/game/spawn.c
```
Count should match the number of entity classnames across all QC files.

- [ ] **Step 2: Build with native_game=true**

```
zig build run -Dnative_game=true -- +map e1m1
```
Expected: game loads. Monsters may behave oddly if any files have bugs — that's the regression phase.

- [ ] **Step 3: Exclude pr_*.c from build when native_game=true**

In `build.zig`, find where `Quake-master/WinQuake/pr_*.c` files are added. Wrap with:
```zig
if (!native_game) {
    exe.addCSourceFiles(.{ .files = pr_sources, .flags = engine_flags });
}
```

- [ ] **Step 4: Run all episode maps and compare behavior to VM build**

```
zig build run -Dnative_game=true -- +map e1m1
zig build run -Dnative_game=true -- +map e2m1
zig build run -Dnative_game=true -- +map e3m1
zig build run -Dnative_game=true -- +map e4m1
```

Fix any behavioral differences found. Compare against:
```
zig build run -- +map e1m1   (VM reference)
```

- [ ] **Step 5: Update README.md and CLAUDE.md**

Mark Phase 5 as ✅ done. Note in CLAUDE.md that `Quake-master/WinQuake/pr_edict.c`, `sv_phys.c`, `sv_main.c` now have minimal NATIVE_GAME guards (no longer strictly untouched upstream).

- [ ] **Step 6: Commit**

```
git add -A
git commit -m "phase5: cutover complete — VM removed, game.dll runs all logic"
```

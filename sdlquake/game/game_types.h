// game_types.h -- Native C entity and type definitions for the game DLL.
// Replaces the VM's entvars_t and progdefs.q1 for the NATIVE_GAME build.

#ifndef GAME_TYPES_H
#define GAME_TYPES_H

#include "game_api.h"

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
// Canonical VM field reference: sdlquake/engine_src/progdefs.q1
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
    // button3, button4 = Blink, Gust (Phase 8 / M3). Engine packs them
    // into bits 2 and 3 of the clc_move button byte (sv_user.c).
    float   button0, button1, button2, button3, button4, impulse, fixangle;
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
    float   flags;

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
    float   dmgtime;   // water/lava damage timer (client.qc)
    float   healamount, healtype; // item_health fields (items.qc)
    float   hit_z;                // spike projectile hit z-offset (weapons.qc)

    // Phase 6 — coexisting Wolf3D + Doom1 weapon roster. items2 holds IT2_*
    // flags. weapon2 is the active selector when nonzero (and self->v.weapon
    // is forced to 0 to keep Quake's W_BestWeapon logic out of the way).
    // ammo_bullets feeds Wolf guns + Doom pistol/chaingun. _phase6_pad keeps
    // sizeof(entvars_t) a multiple of 8 for edict_t alignment (the engine's
    // sv.edicts[] is base + i*pr_edict_size, so any non-mult-of-8 size makes
    // odd-indexed edicts misaligned and crashes UBSan-enabled DLL builds).
    float   items2;
    float   weapon2;
    float   ammo_bullets;
    // Game sets to 1.0 on gibs/heads/blood-flesh projectiles. Engine
    // SV_Physics_Toss reads it: each bounce spawns a blood splat (floor)
    // or wall drip plus a few red particles, throttled at 0.05s per edict.
    // Replaces the prior _phase6_pad slot (same offset, same size).
    float   decal_on_bounce;
} entvars_t;

// ---------------------------------------------------------------------------
// edict_t -- engine entity allocation block.
// Shell layout must match progs.h in the engine exactly.
// At NATIVE_GAME=1 cutover, the engine is recompiled against this header
// so both sides share the same struct definition.
// ---------------------------------------------------------------------------
#define MAX_ENT_LEAFS 16

// link_t may already be defined by common.h (engine context)
#ifndef LINK_T_DEFINED
#define LINK_T_DEFINED
typedef struct link_s { struct link_s *prev, *next; } link_t;
#endif

// entity_state_t may already be defined by server.h (engine context)
#ifndef ENTITY_STATE_T_DEFINED
#define ENTITY_STATE_T_DEFINED
typedef struct {
    vec3_t  origin, angles;
    int     modelindex, frame, colormap, skin, effects;
} entity_state_t;
#endif

struct edict_s {
    int            free;
    link_t         area;
    int            num_leafs;
    short          leafnums[MAX_ENT_LEAFS];
    entity_state_t baseline;
    float          freetime;
    entvars_t      v;
};

#endif // GAME_TYPES_H

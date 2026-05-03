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
    float   aflag2;
} entvars_t;

#endif // GAME_TYPES_H

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

// Phase 6 weapons live in a separate items2 bitfield + weapon2 selector,
// so the existing 8-weapon Quake roster stays untouched. See
// docs/superpowers/plans/2026-05-04-immersive-sim-m1-m2-ai-substrate.md
// (the *Phase 6* plan, despite the file name) for the full spec.
#define IT2_DOOM_FIST       (1 << 0)
#define IT2_DOOM_PISTOL     (1 << 1)
#define IT2_DOOM_SHOTGUN    (1 << 2)
#define IT2_DOOM_CHAINGUN   (1 << 3)
#define IT2_DOOM_ROCKET     (1 << 4)
#define IT2_DOOM_CHAINSAW   (1 << 5)
#define IT2_WOLF_KNIFE      (1 << 6)
#define IT2_WOLF_PISTOL     (1 << 7)
#define IT2_WOLF_MACHINEGUN (1 << 8)
#define IT2_WOLF_CHAINGUN   (1 << 9)

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

// update types (client.qc parm_update)
#define UPDATE_GENERAL 0
#define UPDATE_STATIC  1
#define UPDATE_BINARY  2
#define UPDATE_TEMP    3

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

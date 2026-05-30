// items.c -- Quake item pickups: health, armor, weapons, ammo, keys, runes,
//            powerups, backpacks (items.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// Cross-file dependencies
// ---------------------------------------------------------------------------
// Weak stub — overridden by the real W_BestWeapon in weapons.c (Task 14).
__attribute__((weak)) float W_BestWeapon(void) { return IT_SHOTGUN; }

extern void W_SetCurrentAmmo(void); // weak stub in client.c; strong in weapons.c
extern void SUB_UseTargets(void);   // subs.c
extern void SUB_Remove(edict_t *self); // subs.c

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void PlaceItem(edict_t *self);
static void health_touch(edict_t *self, edict_t *other);
static void item_megahealth_rot(edict_t *self);
static void armor_touch(edict_t *self, edict_t *other);
static void weapon_touch(edict_t *self, edict_t *other);
static void ammo_touch(edict_t *self, edict_t *other);
static void key_touch(edict_t *self, edict_t *other);
static void sigil_touch(edict_t *self, edict_t *other);
static void powerup_touch(edict_t *self, edict_t *other);
static void BackpackTouch(edict_t *self, edict_t *other);
static float RankForWeapon(float w);
static void Deathmatch_Weapon(float old_items, float new_weap);

// ---------------------------------------------------------------------------
// SUB_regen -- restore a picked-up item for deathmatch respawning
// ---------------------------------------------------------------------------
static void SUB_regen(edict_t *self) {
    g->self        = self;
    self->v.model  = self->v.mdl;
    self->v.solid  = SOLID_TRIGGER;
    eng->SV_StartSound(self, CHAN_VOICE, "items/itembk2.wav", 1, ATTN_NORM);
    eng->SV_SetOrigin(self, self->v.origin);
}

// ---------------------------------------------------------------------------
// noclass -- prints a warning and removes the entity
// ---------------------------------------------------------------------------
void spawn_noclass(edict_t *e) {
    g->self = e;
    eng->Con_DPrintf("noclass spawned at");
    eng->Con_DPrintf(eng->VToS(e->v.origin));
    eng->Con_DPrintf("\n");
    eng->ED_Free(e);
}

// ---------------------------------------------------------------------------
// PlaceItem / StartItem -- plant item on the floor after level loads
// ---------------------------------------------------------------------------
static void PlaceItem(edict_t *self) {
    g->self          = self;
    self->v.mdl      = self->v.model;
    self->v.flags    = FL_ITEM;
    self->v.solid    = SOLID_TRIGGER;
    self->v.movetype = MOVETYPE_TOSS;
    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0;
    self->v.origin[2] += 6;
    if (!eng->SV_DropToFloor(self)) {
        eng->Con_DPrintf("Bonus item fell out of level at ");
        eng->Con_DPrintf(eng->VToS(self->v.origin));
        eng->Con_DPrintf("\n");
        eng->ED_Free(self);
    }
}

void StartItem(edict_t *self) {
    self->v.nextthink = g->time + 0.2f;
    self->v.think     = PlaceItem;
}

// ---------------------------------------------------------------------------
// T_Heal -- add health to entity e, clamped by max_health (unless ignore set)
// ---------------------------------------------------------------------------
static int T_Heal(edict_t *e, float healamount, int ignore) {
    if (e->v.health <= 0) return 0;
    if (!ignore && e->v.health >= e->v.max_health) return 0;
    healamount = ceilf(healamount);
    e->v.health += healamount;
    if (!ignore && e->v.health >= e->v.max_health)
        e->v.health = e->v.max_health;
    if (e->v.health > 250)
        e->v.health = 250;
    return 1;
}

// ---------------------------------------------------------------------------
// Health box
// ---------------------------------------------------------------------------
static void health_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;

    if (self->v.healtype == 2) {
        if (other->v.health >= 250) return;
        if (!T_Heal(other, self->v.healamount, 1)) return;
    } else {
        if (!T_Heal(other, self->v.healamount, 0)) return;
    }

    eng->SV_SPrint(other, 0, "You receive ");
    eng->SV_SPrint(other, 0, eng->FToS(self->v.healamount));
    eng->SV_SPrint(other, 0, " health\n");
    eng->SV_StartSound(other, CHAN_ITEM, self->v.noise, 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");

    self->v.model = NULL;
    self->v.solid = SOLID_NOT;

    if (self->v.healtype == 2) {
        other->v.items    = (float)((int)other->v.items | IT_SUPERHEALTH);
        self->v.nextthink = g->time + 5;
        self->v.think     = item_megahealth_rot;
        self->v.owner     = other;
    } else {
        if (g->deathmatch != 2) {
            if (g->deathmatch)
                self->v.nextthink = g->time + 20;
            self->v.think = SUB_regen;
        }
    }
    g->activator = other;
    SUB_UseTargets();
}

static void item_megahealth_rot(edict_t *self) {
    g->self = self;
    edict_t *owner = self->v.owner;
    if (owner->v.health > owner->v.max_health) {
        owner->v.health  -= 1;
        self->v.nextthink = g->time + 1;
        return;
    }
    owner->v.items = (float)((int)owner->v.items & ~IT_SUPERHEALTH);
    if (g->deathmatch == 1) {
        self->v.nextthink = g->time + 20;
        self->v.think     = SUB_regen;
    }
}

void spawn_item_health(edict_t *e) {
    g->self    = e;
    e->v.touch = health_touch;
    if ((int)e->v.spawnflags & 1) {          // H_ROTTEN
        eng->PrecacheModel("maps/b_bh10.bsp");
        eng->PrecacheSound("items/r_item1.wav");
        eng->SV_SetModel(e, "maps/b_bh10.bsp");
        e->v.noise      = "items/r_item1.wav";
        e->v.healamount = 15;
        e->v.healtype   = 0;
    } else if ((int)e->v.spawnflags & 2) {   // H_MEGA
        eng->PrecacheModel("maps/b_bh100.bsp");
        eng->PrecacheSound("items/r_item2.wav");
        eng->SV_SetModel(e, "maps/b_bh100.bsp");
        e->v.noise      = "items/r_item2.wav";
        e->v.healamount = 100;
        e->v.healtype   = 2;
    } else {
        eng->PrecacheModel("maps/b_bh25.bsp");
        eng->PrecacheSound("items/health1.wav");
        eng->SV_SetModel(e, "maps/b_bh25.bsp");
        e->v.noise      = "items/health1.wav";
        e->v.healamount = 25;
        e->v.healtype   = 1;
    }
    vec3_t imin = {0,0,0}, imax = {32,32,56};
    eng->SV_SetSize(e, imin, imax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// Armor
// ---------------------------------------------------------------------------
static void armor_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other->v.health <= 0) return;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;

    float type = 0, value = 0, bit = 0;
    const char *cn = self->v.classname;
    if (cn && strcmp(cn, "item_armor1")   == 0) { type = 0.3f; value = 100; bit = IT_ARMOR1; }
    if (cn && strcmp(cn, "item_armor2")   == 0) { type = 0.6f; value = 150; bit = IT_ARMOR2; }
    if (cn && strcmp(cn, "item_armorInv") == 0) { type = 0.8f; value = 200; bit = IT_ARMOR3; }

    if (other->v.armortype * other->v.armorvalue >= type * value) return;

    other->v.armortype  = type;
    other->v.armorvalue = value;
    other->v.items = (float)(((int)other->v.items & ~(IT_ARMOR1|IT_ARMOR2|IT_ARMOR3)) | (int)bit);

    self->v.solid = SOLID_NOT;
    self->v.model = NULL;
    if (g->deathmatch == 1)
        self->v.nextthink = g->time + 20;
    self->v.think = SUB_regen;

    eng->SV_SPrint(other, 0, "You got armor\n");
    eng->SV_StartSound(other, CHAN_ITEM, "items/armor1.wav", 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");

    g->activator = other;
    SUB_UseTargets();
}

void spawn_item_armor1(edict_t *e) {
    g->self    = e;
    e->v.touch = armor_touch;
    eng->PrecacheModel("progs/armor.mdl");
    eng->SV_SetModel(e, "progs/armor.mdl");
    e->v.skin = 0;
    vec3_t amin = {-16,-16,0}, amax = {16,16,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

void spawn_item_armor2(edict_t *e) {
    g->self    = e;
    e->v.touch = armor_touch;
    eng->PrecacheModel("progs/armor.mdl");
    eng->SV_SetModel(e, "progs/armor.mdl");
    e->v.skin = 1;
    vec3_t amin = {-16,-16,0}, amax = {16,16,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

void spawn_item_armorInv(edict_t *e) {
    g->self    = e;
    e->v.touch = armor_touch;
    eng->PrecacheModel("progs/armor.mdl");
    eng->SV_SetModel(e, "progs/armor.mdl");
    e->v.skin = 2;
    vec3_t amin = {-16,-16,0}, amax = {16,16,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// Weapon pickups
// ---------------------------------------------------------------------------
static void bound_other_ammo(edict_t *other) {
    if (other->v.ammo_shells  > 100) other->v.ammo_shells  = 100;
    if (other->v.ammo_nails   > 200) other->v.ammo_nails   = 200;
    if (other->v.ammo_rockets > 100) other->v.ammo_rockets = 100;
    if (other->v.ammo_cells   > 100) other->v.ammo_cells   = 100;
}

static float RankForWeapon(float w) {
    int wi = (int)w;
    if (wi == IT_LIGHTNING)        return 1;
    if (wi == IT_ROCKET_LAUNCHER)  return 2;
    if (wi == IT_SUPER_NAILGUN)    return 3;
    if (wi == IT_GRENADE_LAUNCHER) return 4;
    if (wi == IT_SUPER_SHOTGUN)    return 5;
    if (wi == IT_NAILGUN)          return 6;
    return 7;
}

static void Deathmatch_Weapon(float old_items, float new_weap) {
    (void)old_items;
    if (RankForWeapon(new_weap) < RankForWeapon(g->self->v.weapon))
        g->self->v.weapon = new_weap;
}

static void weapon_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!((int)other->v.flags & FL_CLIENT)) return;

    edict_t *stemp = g->self;
    g->self = other;
    float best = W_BestWeapon();
    g->self = stemp;
    (void)best;

    float leave = (g->deathmatch == 2 || g->coop) ? 1.0f : 0.0f;
    float hadammo = 0, new_item = 0;
    const char *cn = self->v.classname;

    if (cn && strcmp(cn, "weapon_nailgun") == 0) {
        if (leave && ((int)other->v.items & IT_NAILGUN)) return;
        hadammo = other->v.ammo_nails; new_item = IT_NAILGUN;
        other->v.ammo_nails += 30;
    } else if (cn && strcmp(cn, "weapon_supernailgun") == 0) {
        if (leave && ((int)other->v.items & IT_SUPER_NAILGUN)) return;
        hadammo = other->v.ammo_rockets; new_item = IT_SUPER_NAILGUN;
        other->v.ammo_nails += 30;
    } else if (cn && strcmp(cn, "weapon_supershotgun") == 0) {
        if (leave && ((int)other->v.items & IT_SUPER_SHOTGUN)) return;
        hadammo = other->v.ammo_rockets; new_item = IT_SUPER_SHOTGUN;
        other->v.ammo_shells += 5;
    } else if (cn && strcmp(cn, "weapon_rocketlauncher") == 0) {
        if (leave && ((int)other->v.items & IT_ROCKET_LAUNCHER)) return;
        hadammo = other->v.ammo_rockets; new_item = IT_ROCKET_LAUNCHER;
        other->v.ammo_rockets += 5;
    } else if (cn && strcmp(cn, "weapon_grenadelauncher") == 0) {
        if (leave && ((int)other->v.items & IT_GRENADE_LAUNCHER)) return;
        hadammo = other->v.ammo_rockets; new_item = IT_GRENADE_LAUNCHER;
        other->v.ammo_rockets += 5;
    } else if (cn && strcmp(cn, "weapon_lightning") == 0) {
        if (leave && ((int)other->v.items & IT_LIGHTNING)) return;
        hadammo = other->v.ammo_rockets; new_item = IT_LIGHTNING;
        other->v.ammo_cells += 15;
    } else {
        eng->Host_Error("weapon_touch: unknown classname");
        return;
    }
    (void)hadammo;

    eng->SV_SPrint(other, 0, "You got the ");
    eng->SV_SPrint(other, 0, self->v.netname);
    eng->SV_SPrint(other, 0, "\n");
    eng->SV_StartSound(other, CHAN_ITEM, "weapons/pkup.wav", 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");
    bound_other_ammo(other);

    float old_items = other->v.items;
    other->v.items  = (float)((int)other->v.items | (int)new_item);

    stemp  = g->self;
    g->self = other;
    if (!g->deathmatch)
        g->self->v.weapon = new_item;
    else
        Deathmatch_Weapon(old_items, new_item);
    W_SetCurrentAmmo();
    g->self = stemp;

    if (leave) return;

    self->v.model = NULL;
    self->v.solid = SOLID_NOT;
    if (g->deathmatch == 1)
        self->v.nextthink = g->time + 30;
    self->v.think = SUB_regen;

    g->activator = other;
    SUB_UseTargets();
}

// M8 / F3: touch handler for the fire-weapon pickups (items2/weapon2 roster).
void weapon_touch_fire(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!((int)other->v.flags & FL_CLIENT)) return;
    if (other->v.health <= 0) return;

    int flag = 0;
    const char *cn = self->v.classname;
    if (cn && strcmp(cn, "weapon_oilgun") == 0)            flag = IT2_OILGUN;
    else if (cn && strcmp(cn, "weapon_flamethrower") == 0) flag = IT2_FLAMETHROWER;
    else { eng->Host_Error("weapon_touch_fire: unknown classname"); return; }

    other->v.items2 = (float)((int)other->v.items2 | flag);
    other->v.ammo_cells += 30;
    if (other->v.ammo_cells > 200) other->v.ammo_cells = 200;   // cell ceiling (never demote >100 holders)

    eng->SV_SPrint(other, 0, "You got the ");
    eng->SV_SPrint(other, 0, self->v.netname);
    eng->SV_SPrint(other, 0, "\n");
    eng->SV_StartSound(other, CHAN_ITEM, "weapons/pkup.wav", 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");

    edict_t *stemp = g->self;
    g->self = other;
    g->self->v.weapon  = 0;
    g->self->v.weapon2 = (float)flag;
    W_SetCurrentAmmo();
    g->self = stemp;

    self->v.model = NULL;
    self->v.solid = SOLID_NOT;
    if (g->deathmatch == 1) { self->v.nextthink = g->time + 30; self->v.think = SUB_regen; }

    g->activator = other;
    SUB_UseTargets();
}

void spawn_weapon_supershotgun(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_shot.mdl");
    eng->SV_SetModel(e, "progs/g_shot.mdl");
    e->v.weapon  = IT_SUPER_SHOTGUN;
    e->v.netname = "Double-barrelled Shotgun";
    e->v.touch   = weapon_touch;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_nailgun(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_nail.mdl");
    eng->SV_SetModel(e, "progs/g_nail.mdl");
    e->v.weapon  = IT_NAILGUN;
    e->v.netname = "nailgun";
    e->v.touch   = weapon_touch;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_supernailgun(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_nail2.mdl");
    eng->SV_SetModel(e, "progs/g_nail2.mdl");
    e->v.weapon  = IT_SUPER_NAILGUN;
    e->v.netname = "Super Nailgun";
    e->v.touch   = weapon_touch;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_grenadelauncher(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_rock.mdl");
    eng->SV_SetModel(e, "progs/g_rock.mdl");
    e->v.weapon  = 3;
    e->v.netname = "Grenade Launcher";
    e->v.touch   = weapon_touch;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_rocketlauncher(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_rock2.mdl");
    eng->SV_SetModel(e, "progs/g_rock2.mdl");
    e->v.weapon  = 3;
    e->v.netname = "Rocket Launcher";
    e->v.touch   = weapon_touch;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_lightning(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_light.mdl");
    eng->SV_SetModel(e, "progs/g_light.mdl");
    e->v.weapon  = 3;
    e->v.netname = "Thunderbolt";
    e->v.touch   = weapon_touch;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// Ammo pickups
// ---------------------------------------------------------------------------
static void ammo_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;
    if (other->v.health <= 0) return;

    edict_t *stemp = g->self;
    g->self = other;
    float best = W_BestWeapon();
    g->self = stemp;

    int wt = (int)self->v.weapon;
    if (wt == 1) {
        if (other->v.ammo_shells  >= 100) return;
        other->v.ammo_shells  += self->v.aflag;
    } else if (wt == 2) {
        if (other->v.ammo_nails   >= 200) return;
        other->v.ammo_nails   += self->v.aflag;
    } else if (wt == 3) {
        if (other->v.ammo_rockets >= 100) return;
        other->v.ammo_rockets += self->v.aflag;
    } else if (wt == 4) {
        if (other->v.ammo_cells   >= 200) return;
        other->v.ammo_cells   += self->v.aflag;
    }
    bound_other_ammo(other);

    eng->SV_SPrint(other, 0, "You got the ");
    eng->SV_SPrint(other, 0, self->v.netname);
    eng->SV_SPrint(other, 0, "\n");
    eng->SV_StartSound(other, CHAN_ITEM, "weapons/lock4.wav", 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");

    if (other->v.weapon == best) {
        stemp   = g->self;
        g->self = other;
        g->self->v.weapon = W_BestWeapon();
        W_SetCurrentAmmo();
        g->self = stemp;
    }

    stemp   = g->self;
    g->self = other;
    W_SetCurrentAmmo();
    g->self = stemp;

    self->v.model = NULL;
    self->v.solid = SOLID_NOT;
    if (g->deathmatch == 1)
        self->v.nextthink = g->time + 30;
    self->v.think = SUB_regen;

    g->activator = other;
    SUB_UseTargets();
}

void spawn_item_shells(edict_t *e) {
    g->self = e;
    e->v.touch = ammo_touch;
    if ((int)e->v.spawnflags & 1) {
        eng->PrecacheModel("maps/b_shell1.bsp");
        eng->SV_SetModel(e, "maps/b_shell1.bsp");
        e->v.aflag = 40;
    } else {
        eng->PrecacheModel("maps/b_shell0.bsp");
        eng->SV_SetModel(e, "maps/b_shell0.bsp");
        e->v.aflag = 20;
    }
    e->v.weapon  = 1;
    e->v.netname = "shells";
    vec3_t amin = {0,0,0}, amax = {32,32,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

void spawn_item_spikes(edict_t *e) {
    g->self = e;
    e->v.touch = ammo_touch;
    if ((int)e->v.spawnflags & 1) {
        eng->PrecacheModel("maps/b_nail1.bsp");
        eng->SV_SetModel(e, "maps/b_nail1.bsp");
        e->v.aflag = 50;
    } else {
        eng->PrecacheModel("maps/b_nail0.bsp");
        eng->SV_SetModel(e, "maps/b_nail0.bsp");
        e->v.aflag = 25;
    }
    e->v.weapon  = 2;
    e->v.netname = "nails";
    vec3_t amin = {0,0,0}, amax = {32,32,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

void spawn_item_rockets(edict_t *e) {
    g->self = e;
    e->v.touch = ammo_touch;
    if ((int)e->v.spawnflags & 1) {
        eng->PrecacheModel("maps/b_rock1.bsp");
        eng->SV_SetModel(e, "maps/b_rock1.bsp");
        e->v.aflag = 10;
    } else {
        eng->PrecacheModel("maps/b_rock0.bsp");
        eng->SV_SetModel(e, "maps/b_rock0.bsp");
        e->v.aflag = 5;
    }
    e->v.weapon  = 3;
    e->v.netname = "rockets";
    vec3_t amin = {0,0,0}, amax = {32,32,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

void spawn_item_cells(edict_t *e) {
    g->self = e;
    e->v.touch = ammo_touch;
    if ((int)e->v.spawnflags & 1) {
        eng->PrecacheModel("maps/b_batt1.bsp");
        eng->SV_SetModel(e, "maps/b_batt1.bsp");
        e->v.aflag = 12;
    } else {
        eng->PrecacheModel("maps/b_batt0.bsp");
        eng->SV_SetModel(e, "maps/b_batt0.bsp");
        e->v.aflag = 6;
    }
    e->v.weapon  = 4;
    e->v.netname = "cells";
    vec3_t amin = {0,0,0}, amax = {32,32,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

void spawn_item_weapon(edict_t *e) {
    g->self    = e;
    e->v.touch = ammo_touch;
    if ((int)e->v.spawnflags & 1) {            // WEAPON_SHOTGUN
        if ((int)e->v.spawnflags & 8) {         // WEAPON_BIG
            eng->PrecacheModel("maps/b_shell1.bsp");
            eng->SV_SetModel(e, "maps/b_shell1.bsp");
            e->v.aflag = 40;
        } else {
            eng->PrecacheModel("maps/b_shell0.bsp");
            eng->SV_SetModel(e, "maps/b_shell0.bsp");
            e->v.aflag = 20;
        }
        e->v.weapon = 1; e->v.netname = "shells";
    }
    if ((int)e->v.spawnflags & 4) {            // WEAPON_SPIKES
        if ((int)e->v.spawnflags & 8) {
            eng->PrecacheModel("maps/b_nail1.bsp");
            eng->SV_SetModel(e, "maps/b_nail1.bsp");
            e->v.aflag = 40;
        } else {
            eng->PrecacheModel("maps/b_nail0.bsp");
            eng->SV_SetModel(e, "maps/b_nail0.bsp");
            e->v.aflag = 20;
        }
        e->v.weapon = 2; e->v.netname = "spikes";
    }
    if ((int)e->v.spawnflags & 2) {            // WEAPON_ROCKET
        if ((int)e->v.spawnflags & 8) {
            eng->PrecacheModel("maps/b_rock1.bsp");
            eng->SV_SetModel(e, "maps/b_rock1.bsp");
            e->v.aflag = 10;
        } else {
            eng->PrecacheModel("maps/b_rock0.bsp");
            eng->SV_SetModel(e, "maps/b_rock0.bsp");
            e->v.aflag = 5;
        }
        e->v.weapon = 3; e->v.netname = "rockets";
    }
    vec3_t amin = {0,0,0}, amax = {32,32,56};
    eng->SV_SetSize(e, amin, amax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------
static void key_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;
    if (other->v.health <= 0) return;
    if ((int)other->v.items & (int)self->v.items) return;

    eng->SV_SPrint(other, 0, "You got the ");
    eng->SV_SPrint(other, 0, self->v.netname);
    eng->SV_SPrint(other, 0, "\n");
    eng->SV_StartSound(other, CHAN_ITEM, self->v.noise, 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");
    other->v.items = (float)((int)other->v.items | (int)self->v.items);

    if (!g->coop) {
        self->v.solid = SOLID_NOT;
        self->v.model = NULL;
    }
    g->activator = other;
    SUB_UseTargets();
}

static void key_setsounds(edict_t *e) {
    int wt = (int)g->world->v.worldtype;
    if (wt == 0) {
        eng->PrecacheSound("misc/medkey.wav");
        e->v.noise = "misc/medkey.wav";
    } else if (wt == 1) {
        eng->PrecacheSound("misc/runekey.wav");
        e->v.noise = "misc/runekey.wav";
    } else if (wt == 2) {
        eng->PrecacheSound("misc/basekey.wav");
        e->v.noise = "misc/basekey.wav";
    }
}

void spawn_item_key1(edict_t *e) {
    g->self = e;
    int wt  = (int)g->world->v.worldtype;
    if (wt == 0) {
        eng->PrecacheModel("progs/w_s_key.mdl");
        eng->SV_SetModel(e, "progs/w_s_key.mdl");
        e->v.netname = "silver key";
    } else if (wt == 1) {
        eng->PrecacheModel("progs/m_s_key.mdl");
        eng->SV_SetModel(e, "progs/m_s_key.mdl");
        e->v.netname = "silver runekey";
    } else if (wt == 2) {
        eng->PrecacheModel("progs/b_s_key.mdl");
        eng->SV_SetModel(e, "progs/b_s_key.mdl");
        e->v.netname = "silver keycard";
    }
    key_setsounds(e);
    e->v.touch = key_touch;
    e->v.items = IT_KEY1;
    vec3_t kmin = {-16,-16,-24}, kmax = {16,16,32};
    eng->SV_SetSize(e, kmin, kmax);
    StartItem(e);
}

void spawn_item_key2(edict_t *e) {
    g->self = e;
    int wt  = (int)g->world->v.worldtype;
    if (wt == 0) {
        eng->PrecacheModel("progs/w_g_key.mdl");
        eng->SV_SetModel(e, "progs/w_g_key.mdl");
        e->v.netname = "gold key";
    }
    if (wt == 1) {
        eng->PrecacheModel("progs/m_g_key.mdl");
        eng->SV_SetModel(e, "progs/m_g_key.mdl");
        e->v.netname = "gold runekey";
    }
    if (wt == 2) {
        eng->PrecacheModel("progs/b_g_key.mdl");
        eng->SV_SetModel(e, "progs/b_g_key.mdl");
        e->v.netname = "gold keycard";
    }
    key_setsounds(e);
    e->v.touch = key_touch;
    e->v.items = IT_KEY2;
    vec3_t kmin = {-16,-16,-24}, kmax = {16,16,32};
    eng->SV_SetSize(e, kmin, kmax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// End-of-level sigils
// ---------------------------------------------------------------------------
static void sigil_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;
    if (other->v.health <= 0) return;

    eng->SV_CenterPrint(other, "You got the rune!");
    eng->SV_StartSound(other, CHAN_ITEM, self->v.noise, 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");
    self->v.solid = SOLID_NOT;
    self->v.model = NULL;
    g->serverflags = (float)((int)g->serverflags | ((int)self->v.spawnflags & 15));
    self->v.classname = "";

    g->activator = other;
    SUB_UseTargets();
}

void spawn_item_sigil(edict_t *e) {
    g->self = e;
    if (!(int)e->v.spawnflags) {
        eng->Host_Error("item_sigil: no spawnflags");
        return;
    }
    eng->PrecacheSound("misc/runekey.wav");
    e->v.noise = "misc/runekey.wav";
    if ((int)e->v.spawnflags & 1) {
        eng->PrecacheModel("progs/end1.mdl");
        eng->SV_SetModel(e, "progs/end1.mdl");
    }
    if ((int)e->v.spawnflags & 2) {
        eng->PrecacheModel("progs/end2.mdl");
        eng->SV_SetModel(e, "progs/end2.mdl");
    }
    if ((int)e->v.spawnflags & 4) {
        eng->PrecacheModel("progs/end3.mdl");
        eng->SV_SetModel(e, "progs/end3.mdl");
    }
    if ((int)e->v.spawnflags & 8) {
        eng->PrecacheModel("progs/end4.mdl");
        eng->SV_SetModel(e, "progs/end4.mdl");
    }
    e->v.touch = sigil_touch;
    vec3_t smin = {-16,-16,-24}, smax = {16,16,32};
    eng->SV_SetSize(e, smin, smax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// Powerups
// ---------------------------------------------------------------------------
static void powerup_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;
    if (other->v.health <= 0) return;

    eng->SV_SPrint(other, 0, "You got the ");
    eng->SV_SPrint(other, 0, self->v.netname);
    eng->SV_SPrint(other, 0, "\n");

    if (g->deathmatch) {
        self->v.mdl = self->v.model;
        const char *pcn = self->v.classname;
        if (pcn && (strcmp(pcn, "item_artifact_invulnerability") == 0 ||
                    strcmp(pcn, "item_artifact_invisibility") == 0))
            self->v.nextthink = g->time + 60*5;
        else
            self->v.nextthink = g->time + 60;
        self->v.think = SUB_regen;
    }

    eng->SV_StartSound(other, CHAN_VOICE, self->v.noise, 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");
    self->v.solid  = SOLID_NOT;
    other->v.items = (float)((int)other->v.items | (int)self->v.items);
    self->v.model  = NULL;

    const char *pcn = self->v.classname;
    if (pcn && strcmp(pcn, "item_artifact_envirosuit") == 0) {
        other->v.rad_time         = 1;
        other->v.radsuit_finished = g->time + 30;
    }
    if (pcn && strcmp(pcn, "item_artifact_invulnerability") == 0) {
        other->v.invincible_time     = 1;
        other->v.invincible_finished = g->time + 30;
    }
    if (pcn && strcmp(pcn, "item_artifact_invisibility") == 0) {
        other->v.invisible_time     = 1;
        other->v.invisible_finished = g->time + 30;
    }
    if (pcn && strcmp(pcn, "item_artifact_super_damage") == 0) {
        other->v.super_time            = 1;
        other->v.super_damage_finished = g->time + 30;
    }

    g->activator = other;
    SUB_UseTargets();
}

void spawn_item_artifact_invulnerability(edict_t *e) {
    g->self    = e;
    e->v.touch = powerup_touch;
    eng->PrecacheModel("progs/invulner.mdl");
    eng->PrecacheSound("items/protect.wav");
    eng->PrecacheSound("items/protect2.wav");
    eng->PrecacheSound("items/protect3.wav");
    e->v.noise   = "items/protect.wav";
    eng->SV_SetModel(e, "progs/invulner.mdl");
    e->v.netname = "Pentagram of Protection";
    e->v.items   = IT_INVULNERABILITY;
    vec3_t pmin = {-16,-16,-24}, pmax = {16,16,32};
    eng->SV_SetSize(e, pmin, pmax);
    StartItem(e);
}

void spawn_item_artifact_envirosuit(edict_t *e) {
    g->self    = e;
    e->v.touch = powerup_touch;
    eng->PrecacheModel("progs/suit.mdl");
    eng->PrecacheSound("items/suit.wav");
    eng->PrecacheSound("items/suit2.wav");
    e->v.noise   = "items/suit.wav";
    eng->SV_SetModel(e, "progs/suit.mdl");
    e->v.netname = "Biosuit";
    e->v.items   = IT_SUIT;
    vec3_t pmin = {-16,-16,-24}, pmax = {16,16,32};
    eng->SV_SetSize(e, pmin, pmax);
    StartItem(e);
}

void spawn_item_artifact_invisibility(edict_t *e) {
    g->self    = e;
    e->v.touch = powerup_touch;
    eng->PrecacheModel("progs/invisibl.mdl");
    eng->PrecacheSound("items/inv1.wav");
    eng->PrecacheSound("items/inv2.wav");
    eng->PrecacheSound("items/inv3.wav");
    e->v.noise   = "items/inv1.wav";
    eng->SV_SetModel(e, "progs/invisibl.mdl");
    e->v.netname = "Ring of Shadows";
    e->v.items   = IT_INVISIBILITY;
    vec3_t pmin = {-16,-16,-24}, pmax = {16,16,32};
    eng->SV_SetSize(e, pmin, pmax);
    StartItem(e);
}

void spawn_item_artifact_super_damage(edict_t *e) {
    g->self    = e;
    e->v.touch = powerup_touch;
    eng->PrecacheModel("progs/quaddama.mdl");
    eng->PrecacheSound("items/damage.wav");
    eng->PrecacheSound("items/damage2.wav");
    eng->PrecacheSound("items/damage3.wav");
    e->v.noise   = "items/damage.wav";
    eng->SV_SetModel(e, "progs/quaddama.mdl");
    e->v.netname = "Quad Damage";
    e->v.items   = IT_QUAD;
    vec3_t pmin = {-16,-16,-24}, pmax = {16,16,32};
    eng->SV_SetSize(e, pmin, pmax);
    StartItem(e);
}

// ---------------------------------------------------------------------------
// Backpack (dropped on death, picked up by players)
// ---------------------------------------------------------------------------
static void BackpackTouch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;
    if (other->v.health <= 0) return;

    edict_t *stemp = g->self;
    g->self = other;
    float best = W_BestWeapon();
    g->self = stemp;

    other->v.ammo_shells  += self->v.ammo_shells;
    other->v.ammo_nails   += self->v.ammo_nails;
    other->v.ammo_rockets += self->v.ammo_rockets;
    other->v.ammo_cells   += self->v.ammo_cells;
    other->v.items = (float)((int)other->v.items | (int)self->v.items);
    bound_other_ammo(other);

    eng->SV_SPrint(other, 0, "You get ");
    if (self->v.ammo_shells)  { eng->SV_SPrint(other, 0, eng->FToS(self->v.ammo_shells));  eng->SV_SPrint(other, 0, " shells  "); }
    if (self->v.ammo_nails)   { eng->SV_SPrint(other, 0, eng->FToS(self->v.ammo_nails));   eng->SV_SPrint(other, 0, " nails "); }
    if (self->v.ammo_rockets) { eng->SV_SPrint(other, 0, eng->FToS(self->v.ammo_rockets)); eng->SV_SPrint(other, 0, " rockets  "); }
    if (self->v.ammo_cells)   { eng->SV_SPrint(other, 0, eng->FToS(self->v.ammo_cells));   eng->SV_SPrint(other, 0, " cells  "); }
    eng->SV_SPrint(other, 0, "\n");
    eng->SV_StartSound(other, CHAN_ITEM, "weapons/lock4.wav", 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");

    if (other->v.weapon == best) {
        stemp   = g->self;
        g->self = other;
        g->self->v.weapon = W_BestWeapon();
        g->self = stemp;
    }

    eng->ED_Free(self);

    g->self = other;
    W_SetCurrentAmmo();
}

void DropBackpack(void) {
    edict_t *self = g->self;
    if (!(self->v.ammo_shells + self->v.ammo_nails +
          self->v.ammo_rockets + self->v.ammo_cells))
        return;

    edict_t *item    = eng->ED_Alloc();
    item->v.origin[0] = self->v.origin[0];
    item->v.origin[1] = self->v.origin[1];
    item->v.origin[2] = self->v.origin[2] - 24;

    item->v.items         = self->v.weapon;
    item->v.ammo_shells   = self->v.ammo_shells;
    item->v.ammo_nails    = self->v.ammo_nails;
    item->v.ammo_rockets  = self->v.ammo_rockets;
    item->v.ammo_cells    = self->v.ammo_cells;

    item->v.velocity[2] = 300;
    item->v.velocity[0] = -100 + (eng->Random() * 200);
    item->v.velocity[1] = -100 + (eng->Random() * 200);

    item->v.flags    = FL_ITEM;
    item->v.solid    = SOLID_TRIGGER;
    item->v.movetype = MOVETYPE_TOSS;
    eng->SV_SetModel(item, "progs/backpack.mdl");
    vec3_t bmin = {-16,-16,0}, bmax = {16,16,56};
    eng->SV_SetSize(item, bmin, bmax);
    item->v.touch     = BackpackTouch;
    item->v.nextthink = g->time + 120;
    item->v.think     = SUB_Remove;
}

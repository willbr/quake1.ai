// misc.c -- Miscellaneous entities (misc.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// External dependencies
// ---------------------------------------------------------------------------
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);
extern void BecomeExplosion(void);
extern void SUB_Remove(edict_t *self);
extern void launch_spike(vec3_t org, vec3_t dir);
extern void superspike_touch(edict_t *self, edict_t *other);

// Weak stub — overridden when enforcer.c is ported
__attribute__((weak)) void LaunchLaser(vec3_t org, vec3_t vec) { (void)org; (void)vec; }

// ---------------------------------------------------------------------------
// SetMovedir -- convert self.angles into a direction vector
// Used by triggers, buttons, shooters.
// ---------------------------------------------------------------------------
void SetMovedir(edict_t *e) {
    if (e->v.angles[0] == 0 && e->v.angles[1] == -1 && e->v.angles[2] == 0) {
        e->v.movedir[0] = 0; e->v.movedir[1] = 0; e->v.movedir[2] = 1;
    } else if (e->v.angles[0] == 0 && e->v.angles[1] == -2 && e->v.angles[2] == 0) {
        e->v.movedir[0] = 0; e->v.movedir[1] = 0; e->v.movedir[2] = -1;
    } else {
        eng->MakeVectors(e->v.angles);
        e->v.movedir[0] = g->v_forward[0];
        e->v.movedir[1] = g->v_forward[1];
        e->v.movedir[2] = g->v_forward[2];
    }
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
}

// ---------------------------------------------------------------------------
// info_null -- positional target; remove immediately after spawn
// ---------------------------------------------------------------------------
void spawn_info_null(edict_t *e) {
    eng->ED_Free(e);
}

// ---------------------------------------------------------------------------
// info_notnull -- positional target that stays in the world
// ---------------------------------------------------------------------------
void spawn_info_notnull(edict_t *e) {
    (void)e;
}

// ---------------------------------------------------------------------------
// Light entities
// ---------------------------------------------------------------------------
static void light_use(edict_t *self, edict_t *activator) {
    (void)activator;
    if (((int)self->v.spawnflags) & 1) {  // START_OFF = 1
        eng->SV_LightStyle((int)self->v.style, "m");
        self->v.spawnflags -= 1;
    } else {
        eng->SV_LightStyle((int)self->v.style, "a");
        self->v.spawnflags += 1;
    }
}

void spawn_light(edict_t *e) {
    if (!e->v.targetname) {
        eng->ED_Free(e);
        return;
    }
    if (e->v.style >= 32) {
        e->v.use = light_use;
        if (((int)e->v.spawnflags) & 1)
            eng->SV_LightStyle((int)e->v.style, "a");
        else
            eng->SV_LightStyle((int)e->v.style, "m");
    }
}

void spawn_light_fluoro(edict_t *e) {
    if (e->v.style >= 32) {
        e->v.use = light_use;
        if (((int)e->v.spawnflags) & 1)
            eng->SV_LightStyle((int)e->v.style, "a");
        else
            eng->SV_LightStyle((int)e->v.style, "m");
    }
    eng->PrecacheSound("ambience/fl_hum1.wav");
    eng->SV_AmbientSound(e->v.origin, "ambience/fl_hum1.wav", 0.5f, ATTN_STATIC);
}

void spawn_light_fluorospark(edict_t *e) {
    if (!e->v.style) e->v.style = 10;
    eng->PrecacheSound("ambience/buzz1.wav");
    eng->SV_AmbientSound(e->v.origin, "ambience/buzz1.wav", 0.5f, ATTN_STATIC);
}

static void fire_ambient(edict_t *e) {
    eng->PrecacheSound("ambience/fire1.wav");
    eng->SV_AmbientSound(e->v.origin, "ambience/fire1.wav", 0.5f, ATTN_STATIC);
}

void spawn_light_globe(edict_t *e) {
    eng->PrecacheModel("progs/s_light.spr");
    eng->SV_SetModel(e, "progs/s_light.spr");
    eng->SV_MakeStatic(e);
}

void spawn_light_torch_small_walltorch(edict_t *e) {
    eng->PrecacheModel("progs/flame.mdl");
    eng->SV_SetModel(e, "progs/flame.mdl");
    fire_ambient(e);
    eng->SV_MakeStatic(e);
}

void spawn_light_flame_large_yellow(edict_t *e) {
    eng->PrecacheModel("progs/flame2.mdl");
    eng->SV_SetModel(e, "progs/flame2.mdl");
    e->v.frame = 1;
    fire_ambient(e);
    eng->SV_MakeStatic(e);
}

void spawn_light_flame_small_yellow(edict_t *e) {
    eng->PrecacheModel("progs/flame2.mdl");
    eng->SV_SetModel(e, "progs/flame2.mdl");
    fire_ambient(e);
    eng->SV_MakeStatic(e);
}

void spawn_light_flame_small_white(edict_t *e) {
    eng->PrecacheModel("progs/flame2.mdl");
    eng->SV_SetModel(e, "progs/flame2.mdl");
    fire_ambient(e);
    eng->SV_MakeStatic(e);
}

// ---------------------------------------------------------------------------
// misc_fireball -- lava ball spawner
// ---------------------------------------------------------------------------
static void fire_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    int bouncing_off_world =
        (other == g->world && (int)self->v.movetype == MOVETYPE_BOUNCE);

    // If the fireball hit a wall (no FL_MONSTER|FL_CLIENT), emit a burst of
    // the same orange splash particles a hitscan would throw off lava — the
    // existing TE_WATERSPLASH/kind=2 path with strength 16 (1×). The surface
    // recovery the spark version did is unused: this splash always sprays
    // upward from the impact regardless of surface orientation.
    int flesh = ((int)other->v.flags & (FL_MONSTER | FL_CLIENT)) != 0;
    if (!flesh) {
        eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte (MSG_BROADCAST, TE_WATERSPLASH);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
        eng->MSG_WriteByte (MSG_BROADCAST, 2);   // kind=2 lava
        eng->MSG_WriteByte (MSG_BROADCAST, 16);  // strength_q4 (1×)
    }
    // Gusted fireballs ricochet off world geometry instead of being consumed
    // on first wall contact — sparks already emitted above so bounces visibly
    // scuff the wall.
    if (bouncing_off_world) return;
    if (other->v.takedamage)
        T_Damage(other, self, self, 20);
    eng->ED_Free(self);
}

static void fire_fly(edict_t *self);

// Death effect: lava-splash burst in place of silent removal. Same particles
// the wall-bounce path uses, slightly stronger so the death reads larger
// than a single bounce.
static void fire_die(edict_t *self) {
    g->self = self;
    eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte (MSG_BROADCAST, TE_WATERSPLASH);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    eng->MSG_WriteByte (MSG_BROADCAST, 2);   // kind=2 lava
    eng->MSG_WriteByte (MSG_BROADCAST, 24);  // strength_q4 (1.5×)
    eng->ED_Free(self);
}

void spawn_misc_fireball(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/lavaball.mdl");
    e->v.classname = "fireball";
    e->v.nextthink = g->time + (eng->Random() * 5);
    e->v.think     = fire_fly;
    // Note: original QC has `self.speed == 1000` (comparison, not assignment) — bug.
    // Speed stays 0 if not set by the map.
}

static void fire_fly(edict_t *self) {
    g->self = self;
    edict_t *fireball = eng->ED_Alloc();
    fireball->v.solid      = SOLID_TRIGGER;
    fireball->v.movetype   = MOVETYPE_TOSS;
    fireball->v.velocity[0] = (eng->Random() * 100) - 50;
    fireball->v.velocity[1] = (eng->Random() * 100) - 50;
    fireball->v.velocity[2] = self->v.speed + (eng->Random() * 200);
    fireball->v.classname   = "fireball";
    eng->SV_SetModel(fireball, "progs/lavaball.mdl");
    vec3_t zero = {0,0,0};
    eng->SV_SetSize(fireball, zero, zero);
    eng->SV_SetOrigin(fireball, self->v.origin);
    fireball->v.nextthink = g->time + 5;
    fireball->v.think     = fire_die;
    fireball->v.touch     = fire_touch;

    self->v.nextthink = g->time + (eng->Random() * 5) + 3;
    self->v.think     = fire_fly;
}

// ---------------------------------------------------------------------------
// barrel / misc_explobox
// ---------------------------------------------------------------------------
void barrel_explode(edict_t *self) {
    g->self = self;
    self->v.takedamage = DAMAGE_NO;
    self->v.classname  = "explo_box";
    T_RadiusDamage(self, self, 160, g->world);
    eng->SV_StartSound(self, CHAN_VOICE, "weapons/r_exp3.wav", 1, ATTN_NORM);
    vec3_t zdir = {0,0,0};
    eng->SV_Particle(self->v.origin, zdir, 75, 255);
    // Broadcast TE_EXPLOSION at the base so the client spawns a floor scorch
    // (cl_tent.c maps TE_EXPLOSION -> DECAL_SCORCH). The sprite is then lifted
    // +32z below so it doesn't render half-buried in the ground.
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    vec3_t new_origin = {self->v.origin[0], self->v.origin[1], self->v.origin[2] + 32};
    eng->SV_SetOrigin(self, new_origin);
    BecomeExplosion();
}

void spawn_misc_explobox(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->SV_SetModel(e, "maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    e->v.health      = 20;
    e->v.th_die      = barrel_explode;
    e->v.takedamage  = DAMAGE_AIM;
    e->v.origin[2]  += 2;
    float oldz = e->v.origin[2];
    eng->SV_DropToFloor(e);
    if (oldz - e->v.origin[2] > 250) {
        eng->Con_DPrintf("item fell out of level\n");
        eng->ED_Free(e);
    }
}

void spawn_misc_explobox2(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_exbox2.bsp");
    eng->SV_SetModel(e, "maps/b_exbox2.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    e->v.health     = 20;
    e->v.th_die     = barrel_explode;
    e->v.takedamage = DAMAGE_AIM;
    e->v.origin[2] += 2;
    float oldz = e->v.origin[2];
    eng->SV_DropToFloor(e);
    if (oldz - e->v.origin[2] > 250) {
        eng->Con_DPrintf("item fell out of level\n");
        eng->ED_Free(e);
    }
}

// ---------------------------------------------------------------------------
// trap_spikeshooter / trap_shooter
// ---------------------------------------------------------------------------
#define SPAWNFLAG_SUPERSPIKE 1
#define SPAWNFLAG_LASER      2

static void spikeshooter_use(edict_t *self, edict_t *activator) {
    g->self = self;
    (void)activator;
    if (((int)self->v.spawnflags) & SPAWNFLAG_LASER) {
        eng->SV_StartSound(self, CHAN_VOICE, "enforcer/enfire.wav", 1, ATTN_NORM);
        LaunchLaser(self->v.origin, self->v.movedir);
    } else {
        eng->SV_StartSound(self, CHAN_VOICE, "weapons/spike2.wav", 1, ATTN_NORM);
        launch_spike(self->v.origin, self->v.movedir);
        if (g->newmis) {
            g->newmis->v.velocity[0] = self->v.movedir[0] * 500;
            g->newmis->v.velocity[1] = self->v.movedir[1] * 500;
            g->newmis->v.velocity[2] = self->v.movedir[2] * 500;
            if (((int)self->v.spawnflags) & SPAWNFLAG_SUPERSPIKE)
                g->newmis->v.touch = superspike_touch;
        }
    }
}

static void shooter_think(edict_t *self) {
    g->self = self;
    spikeshooter_use(self, NULL);
    self->v.nextthink = g->time + self->v.wait;
    if (g->newmis) {
        g->newmis->v.velocity[0] = self->v.movedir[0] * 500;
        g->newmis->v.velocity[1] = self->v.movedir[1] * 500;
        g->newmis->v.velocity[2] = self->v.movedir[2] * 500;
    }
}

void spawn_trap_spikeshooter(edict_t *e) {
    g->self = e;
    SetMovedir(e);
    e->v.use = spikeshooter_use;
    if (((int)e->v.spawnflags) & SPAWNFLAG_LASER) {
        eng->PrecacheModel("progs/laser.mdl");
        eng->PrecacheSound("enforcer/enfire.wav");
        eng->PrecacheSound("enforcer/enfstop.wav");
    } else {
        eng->PrecacheSound("weapons/spike2.wav");
    }
}

void spawn_trap_shooter(edict_t *e) {
    g->self = e;
    spawn_trap_spikeshooter(e);
    if (!e->v.wait) e->v.wait = 1;
    e->v.nextthink = e->v.nextthink + e->v.wait + e->v.ltime;
    e->v.think     = shooter_think;
}

// ---------------------------------------------------------------------------
// air_bubbles / bubble system
// ---------------------------------------------------------------------------
void bubble_bob(edict_t *self);  // non-static: overrides weak stub in player.c

static void bubble_remove(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (other->v.classname && self->v.classname &&
            strcmp(other->v.classname, self->v.classname) == 0)
        return;  // bubbles don't remove each other
    eng->ED_Free(self);
}

static void bubble_split(edict_t *self) {
    edict_t *bubble = eng->ED_Alloc();
    eng->SV_SetModel(bubble, "progs/s_bubble.spr");
    eng->SV_SetOrigin(bubble, self->v.origin);
    bubble->v.movetype  = MOVETYPE_NOCLIP;
    bubble->v.solid     = SOLID_NOT;
    bubble->v.velocity[0] = self->v.velocity[0];
    bubble->v.velocity[1] = self->v.velocity[1];
    bubble->v.velocity[2] = self->v.velocity[2];
    bubble->v.nextthink = g->time + 0.5f;
    bubble->v.think     = bubble_bob;
    bubble->v.touch     = bubble_remove;
    bubble->v.classname = "bubble";
    bubble->v.frame     = 1;
    bubble->v.cnt       = 10;
    vec3_t bmin = {-8,-8,-8}, bmax = {8,8,8};
    eng->SV_SetSize(bubble, bmin, bmax);
    self->v.frame = 1;
    self->v.cnt   = 10;
    if ((int)self->v.waterlevel != 3)
        eng->ED_Free(self);
}

void bubble_bob(edict_t *self) {
    g->self = self;
    self->v.cnt += 1;
    if ((int)self->v.cnt == 4) {
        bubble_split(self);
        if (self->free) return;  // bubble_split freed us
    }
    if ((int)self->v.cnt == 20) { eng->ED_Free(self); return; }

    float rnd1 = self->v.velocity[0] + (-10 + (eng->Random() * 20));
    float rnd2 = self->v.velocity[1] + (-10 + (eng->Random() * 20));
    float rnd3 = self->v.velocity[2] + 10 + eng->Random() * 10;

    if (rnd1 >  10) rnd1 =  5;
    if (rnd1 < -10) rnd1 = -5;
    if (rnd2 >  10) rnd2 =  5;
    if (rnd2 < -10) rnd2 = -5;
    if (rnd3 <  10) rnd3 = 15;
    if (rnd3 >  30) rnd3 = 25;

    self->v.velocity[0] = rnd1;
    self->v.velocity[1] = rnd2;
    self->v.velocity[2] = rnd3;
    self->v.nextthink   = g->time + 0.5f;
    self->v.think       = bubble_bob;
}

static void make_bubbles(edict_t *self) {
    g->self = self;
    edict_t *bubble = eng->ED_Alloc();
    eng->SV_SetModel(bubble, "progs/s_bubble.spr");
    eng->SV_SetOrigin(bubble, self->v.origin);
    bubble->v.movetype  = MOVETYPE_NOCLIP;
    bubble->v.solid     = SOLID_NOT;
    bubble->v.velocity[0] = 0; bubble->v.velocity[1] = 0; bubble->v.velocity[2] = 15;
    bubble->v.nextthink = g->time + 0.5f;
    bubble->v.think     = bubble_bob;
    bubble->v.touch     = bubble_remove;
    bubble->v.classname = "bubble";
    bubble->v.frame     = 0;
    bubble->v.cnt       = 0;
    vec3_t bmin = {-8,-8,-8}, bmax = {8,8,8};
    eng->SV_SetSize(bubble, bmin, bmax);
    self->v.nextthink = g->time + eng->Random() + 0.5f;
    self->v.think     = make_bubbles;
}

void spawn_air_bubbles(edict_t *e) {
    g->self = e;
    if ((int)g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/s_bubble.spr");
    e->v.nextthink = g->time + 1;
    e->v.think     = make_bubbles;
}

// ---------------------------------------------------------------------------
// viewthing -- debug entity
// ---------------------------------------------------------------------------
void spawn_viewthing(edict_t *e) {
    e->v.movetype = MOVETYPE_NONE;
    e->v.solid    = SOLID_NOT;
    eng->PrecacheModel("progs/player.mdl");
    eng->SV_SetModel(e, "progs/player.mdl");
}

// ---------------------------------------------------------------------------
// Simple bmodel entities
// ---------------------------------------------------------------------------
static void func_wall_use(edict_t *self, edict_t *activator) {
    (void)activator;
    self->v.frame = 1 - self->v.frame;
}

void spawn_func_wall(edict_t *e) {
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
    e->v.movetype = MOVETYPE_PUSH;
    e->v.solid    = SOLID_BSP;
    e->v.use      = func_wall_use;
    eng->SV_SetModel(e, e->v.model);
}

// func_grate -- Phase 8 / M3. Looks and collides like a func_wall, but Blink
// validity traces temporarily set it SOLID_NOT so the player can phase
// through it. The grate is registered with abilities.c so the trace pass
// can find it.
void Abilities_RegisterGrate(edict_t *e);   // abilities.h
void spawn_func_grate(edict_t *e) {
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
    e->v.movetype = MOVETYPE_PUSH;
    e->v.solid    = SOLID_BSP;
    e->v.use      = func_wall_use;
    eng->SV_SetModel(e, e->v.model);
    Abilities_RegisterGrate(e);
}

void spawn_func_illusionary(edict_t *e) {
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
    e->v.movetype = MOVETYPE_NONE;
    e->v.solid    = SOLID_NOT;
    eng->SV_SetModel(e, e->v.model);
    eng->SV_MakeStatic(e);
}

void spawn_func_episodegate(edict_t *e) {
    if (!(((int)g->serverflags) & ((int)e->v.spawnflags)))
        return;  // can still enter episode
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
    e->v.movetype = MOVETYPE_PUSH;
    e->v.solid    = SOLID_BSP;
    e->v.use      = func_wall_use;
    eng->SV_SetModel(e, e->v.model);
}

void spawn_func_bossgate(edict_t *e) {
    if ((((int)g->serverflags) & 15) == 15)
        return;  // all episodes completed
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
    e->v.movetype = MOVETYPE_PUSH;
    e->v.solid    = SOLID_BSP;
    e->v.use      = func_wall_use;
    eng->SV_SetModel(e, e->v.model);
}

// ---------------------------------------------------------------------------
// Ambient sound entities
// ---------------------------------------------------------------------------
void spawn_ambient_suck_wind(edict_t *e)   { eng->PrecacheSound("ambience/suck1.wav");    eng->SV_AmbientSound(e->v.origin, "ambience/suck1.wav",    1.0f, ATTN_STATIC); }
void spawn_ambient_drone(edict_t *e)        { eng->PrecacheSound("ambience/drone6.wav");   eng->SV_AmbientSound(e->v.origin, "ambience/drone6.wav",   0.5f, ATTN_STATIC); }
void spawn_ambient_flouro_buzz(edict_t *e)  { eng->PrecacheSound("ambience/buzz1.wav");    eng->SV_AmbientSound(e->v.origin, "ambience/buzz1.wav",    1.0f, ATTN_STATIC); }
void spawn_ambient_drip(edict_t *e)         { eng->PrecacheSound("ambience/drip1.wav");    eng->SV_AmbientSound(e->v.origin, "ambience/drip1.wav",    0.5f, ATTN_STATIC); }
void spawn_ambient_comp_hum(edict_t *e)     { eng->PrecacheSound("ambience/comp1.wav");    eng->SV_AmbientSound(e->v.origin, "ambience/comp1.wav",    1.0f, ATTN_STATIC); }
void spawn_ambient_thunder(edict_t *e)      { eng->PrecacheSound("ambience/thunder1.wav"); eng->SV_AmbientSound(e->v.origin, "ambience/thunder1.wav", 0.5f, ATTN_STATIC); }
void spawn_ambient_light_buzz(edict_t *e)   { eng->PrecacheSound("ambience/fl_hum1.wav"); eng->SV_AmbientSound(e->v.origin, "ambience/fl_hum1.wav",  0.5f, ATTN_STATIC); }
void spawn_ambient_swamp1(edict_t *e)       { eng->PrecacheSound("ambience/swamp1.wav");   eng->SV_AmbientSound(e->v.origin, "ambience/swamp1.wav",   0.5f, ATTN_STATIC); }
void spawn_ambient_swamp2(edict_t *e)       { eng->PrecacheSound("ambience/swamp2.wav");   eng->SV_AmbientSound(e->v.origin, "ambience/swamp2.wav",   0.5f, ATTN_STATIC); }

// ---------------------------------------------------------------------------
// misc_noisemaker -- sound optimization test entity
// ---------------------------------------------------------------------------
static void noise_think(edict_t *self) {
    g->self = self;
    self->v.nextthink = g->time + 0.5f;
    eng->SV_StartSound(self, 1, "enforcer/enfire.wav",  1, ATTN_NORM);
    eng->SV_StartSound(self, 2, "enforcer/enfstop.wav", 1, ATTN_NORM);
    eng->SV_StartSound(self, 3, "enforcer/sight1.wav",  1, ATTN_NORM);
    eng->SV_StartSound(self, 4, "enforcer/sight2.wav",  1, ATTN_NORM);
    eng->SV_StartSound(self, 5, "enforcer/sight3.wav",  1, ATTN_NORM);
    eng->SV_StartSound(self, 6, "enforcer/sight4.wav",  1, ATTN_NORM);
    eng->SV_StartSound(self, 7, "enforcer/pain1.wav",   1, ATTN_NORM);
}

void spawn_misc_noisemaker(edict_t *e) {
    g->self = e;
    eng->PrecacheSound("enforcer/enfire.wav");
    eng->PrecacheSound("enforcer/enfstop.wav");
    eng->PrecacheSound("enforcer/sight1.wav");
    eng->PrecacheSound("enforcer/sight2.wav");
    eng->PrecacheSound("enforcer/sight3.wav");
    eng->PrecacheSound("enforcer/sight4.wav");
    eng->PrecacheSound("enforcer/pain1.wav");
    eng->PrecacheSound("enforcer/pain2.wav");
    eng->PrecacheSound("enforcer/death1.wav");
    eng->PrecacheSound("enforcer/idle1.wav");
    e->v.nextthink = g->time + 0.1f + eng->Random();
    e->v.think     = noise_think;
}

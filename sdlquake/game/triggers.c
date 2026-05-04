// triggers.c -- Trigger entities (triggers.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <stdlib.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void InitTrigger(edict_t *e);
extern void SUB_UseTargets(void);
extern void SUB_Remove(edict_t *self);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);

#define SPAWNFLAG_NOMESSAGE  1
#define SPAWNFLAG_NOTOUCH    1
#define PLAYER_ONLY          1
#define SILENT               2
#define PUSH_ONCE            1

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void multi_trigger(edict_t *self);
static void multi_wait(edict_t *self);
static void play_teleport(edict_t *self);
void spawn_trigger_multiple(edict_t *e);

static void sub_use_targets_fn(edict_t *self, edict_t *activator) {
    g->self      = self;
    g->activator = activator;
    SUB_UseTargets();
}

// ---------------------------------------------------------------------------
// trigger_multiple / trigger_once / trigger_secret core
// ---------------------------------------------------------------------------
static void trigger_reactivate(edict_t *self) {
    self->v.solid = SOLID_TRIGGER;
}

static void multi_wait(edict_t *self) {
    if ((int)self->v.max_health) {
        self->v.health     = self->v.max_health;
        self->v.takedamage = DAMAGE_YES;
        self->v.solid      = SOLID_BBOX;
    }
}

static void multi_trigger(edict_t *self) {
    if (self->v.nextthink > g->time)
        return;  // already triggered, waiting for reset

    if (self->v.classname && strcmp(self->v.classname, "trigger_secret") == 0) {
        if (!self->v.enemy || !self->v.enemy->v.classname ||
            strcmp(self->v.enemy->v.classname, "player") != 0)
            return;
        g->found_secrets += 1;
        eng->MSG_WriteByte(MSG_ALL, SVC_FOUNDSECRET);
    }

    if (self->v.noise)
        eng->SV_StartSound(self, CHAN_VOICE, self->v.noise, 1, ATTN_NORM);

    self->v.takedamage = DAMAGE_NO;
    g->activator       = self->v.enemy;
    SUB_UseTargets();

    if (self->v.wait > 0) {
        self->v.think     = multi_wait;
        self->v.nextthink = g->time + self->v.wait;
    } else {
        self->v.touch     = NULL;  // SUB_Null — stop touching
        self->v.nextthink = g->time + 0.1f;
        self->v.think     = SUB_Remove;
    }
}

static void multi_killed(edict_t *self) {
    g->self       = self;
    self->v.enemy = g->damage_attacker;
    multi_trigger(self);
}

static void multi_use(edict_t *self, edict_t *activator) {
    g->self       = self;
    self->v.enemy = activator;
    multi_trigger(self);
}

static void multi_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    if (self->v.movedir[0] != 0.0f || self->v.movedir[1] != 0.0f || self->v.movedir[2] != 0.0f) {
        eng->MakeVectors(other->v.angles);
        float dot = g->v_forward[0]*self->v.movedir[0]
                  + g->v_forward[1]*self->v.movedir[1]
                  + g->v_forward[2]*self->v.movedir[2];
        if (dot < 0)
            return;
    }
    self->v.enemy = other;
    multi_trigger(self);
}

void spawn_trigger_multiple(edict_t *e) {
    g->self = e;
    if      ((int)e->v.sounds == 1) { eng->PrecacheSound("misc/secret.wav");   e->v.noise = "misc/secret.wav"; }
    else if ((int)e->v.sounds == 2) { eng->PrecacheSound("misc/talk.wav");     e->v.noise = "misc/talk.wav"; }
    else if ((int)e->v.sounds == 3) { eng->PrecacheSound("misc/trigger1.wav"); e->v.noise = "misc/trigger1.wav"; }

    if (!e->v.wait)
        e->v.wait = 0.2f;
    e->v.use = multi_use;

    InitTrigger(e);

    if ((int)e->v.health) {
        if ((int)e->v.spawnflags & SPAWNFLAG_NOTOUCH)
            eng->Host_Error("trigger_multiple: health and notouch don't make sense\n");
        e->v.max_health = e->v.health;
        e->v.th_die     = multi_killed;
        e->v.takedamage = DAMAGE_YES;
        e->v.solid      = SOLID_BBOX;
        vec3_t orig = { e->v.origin[0], e->v.origin[1], e->v.origin[2] };
        eng->SV_SetOrigin(e, orig);  // relink into world
    } else {
        if (!((int)e->v.spawnflags & SPAWNFLAG_NOTOUCH))
            e->v.touch = multi_touch;
    }
}

void spawn_trigger_once(edict_t *e) {
    e->v.wait = -1;
    spawn_trigger_multiple(e);
}

void spawn_trigger_relay(edict_t *e) {
    e->v.use = sub_use_targets_fn;
}

void spawn_trigger_secret(edict_t *e) {
    g->self = e;
    g->total_secrets += 1;
    e->v.wait = -1;
    if (!e->v.message)
        e->v.message = "You found a secret area!";
    if (!e->v.sounds)
        e->v.sounds = 1;
    if      ((int)e->v.sounds == 1) { eng->PrecacheSound("misc/secret.wav"); e->v.noise = "misc/secret.wav"; }
    else if ((int)e->v.sounds == 2) { eng->PrecacheSound("misc/talk.wav");   e->v.noise = "misc/talk.wav"; }
    spawn_trigger_multiple(e);
}

// ---------------------------------------------------------------------------
// trigger_counter
// ---------------------------------------------------------------------------
static void counter_use(edict_t *self, edict_t *activator) {
    g->self = self;
    self->v.count -= 1;
    if (self->v.count < 0)
        return;

    if (self->v.count != 0) {
        if (activator && activator->v.classname &&
            strcmp(activator->v.classname, "player") == 0 &&
            !((int)self->v.spawnflags & SPAWNFLAG_NOMESSAGE)) {
            if      (self->v.count >= 4) eng->SV_CenterPrint(activator, "There are more to go...");
            else if ((int)self->v.count == 3) eng->SV_CenterPrint(activator, "Only 3 more to go...");
            else if ((int)self->v.count == 2) eng->SV_CenterPrint(activator, "Only 2 more to go...");
            else                              eng->SV_CenterPrint(activator, "Only 1 more to go...");
        }
        return;
    }

    if (activator && activator->v.classname &&
        strcmp(activator->v.classname, "player") == 0 &&
        !((int)self->v.spawnflags & SPAWNFLAG_NOMESSAGE))
        eng->SV_CenterPrint(activator, "Sequence completed!");

    self->v.enemy = activator;
    multi_trigger(self);
}

void spawn_trigger_counter(edict_t *e) {
    e->v.wait = -1;
    if (!e->v.count)
        e->v.count = 2;
    e->v.use = counter_use;
}

// ---------------------------------------------------------------------------
// Teleporter helpers
// ---------------------------------------------------------------------------
static void play_teleport(edict_t *self) {
    g->self = self;
    float v = eng->Random() * 5.0f;
    const char *s;
    if      (v < 1) s = "misc/r_tele1.wav";
    else if (v < 2) s = "misc/r_tele2.wav";
    else if (v < 3) s = "misc/r_tele3.wav";
    else if (v < 4) s = "misc/r_tele4.wav";
    else            s = "misc/r_tele5.wav";
    eng->SV_StartSound(self, CHAN_VOICE, s, 1, ATTN_NORM);
    eng->ED_Free(self);
}

static void spawn_tfog(vec3_t org) {
    edict_t *s     = eng->ED_Alloc();
    s->v.origin[0] = org[0];
    s->v.origin[1] = org[1];
    s->v.origin[2] = org[2];
    s->v.nextthink = g->time + 0.2f;
    s->v.think     = play_teleport;
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_TELEPORT);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
}

static void tdeath_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (other == self->v.owner)
        return;
    if (other->v.classname && strcmp(other->v.classname, "player") == 0) {
        if (other->v.invincible_finished > g->time)
            self->v.classname = "teledeath2";
        if (!self->v.owner->v.classname ||
            strcmp(self->v.owner->v.classname, "player") != 0) {
            T_Damage(self->v.owner, self, self, 50000);
            return;
        }
    }
    if ((int)other->v.health)
        T_Damage(other, self, self, 50000);
}

static void spawn_tdeath(vec3_t org, edict_t *death_owner) {
    edict_t *death   = eng->ED_Alloc();
    death->v.classname = "teledeath";
    death->v.movetype  = MOVETYPE_NONE;
    death->v.solid     = SOLID_TRIGGER;
    death->v.angles[0] = death->v.angles[1] = death->v.angles[2] = 0.0f;
    vec3_t dmins = { death_owner->v.mins[0]-1, death_owner->v.mins[1]-1, death_owner->v.mins[2]-1 };
    vec3_t dmaxs = { death_owner->v.maxs[0]+1, death_owner->v.maxs[1]+1, death_owner->v.maxs[2]+1 };
    eng->SV_SetSize(death, dmins, dmaxs);
    eng->SV_SetOrigin(death, org);
    death->v.touch     = tdeath_touch;
    death->v.nextthink = g->time + 0.2f;
    death->v.think     = SUB_Remove;
    death->v.owner     = death_owner;
    g->force_retouch   = 2;
}

static void teleport_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;

    if (self->v.targetname) {
        if (self->v.nextthink < g->time)
            return;  // not fired yet
    }

    if ((int)self->v.spawnflags & PLAYER_ONLY) {
        if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
            return;
    }

    if (other->v.health <= 0 || (int)other->v.solid != SOLID_SLIDEBOX)
        return;

    g->activator = other;
    SUB_UseTargets();

    spawn_tfog(other->v.origin);

    edict_t *t = eng->ED_Find(g->world, "targetname", self->v.target);
    if (t == g->world)
        eng->Host_Error("trigger_teleport: couldn't find target");

    eng->MakeVectors(t->v.mangle);
    vec3_t org = {
        t->v.origin[0] + 32.0f * g->v_forward[0],
        t->v.origin[1] + 32.0f * g->v_forward[1],
        t->v.origin[2] + 32.0f * g->v_forward[2]
    };
    spawn_tfog(org);
    spawn_tdeath(t->v.origin, other);

    if (!other->v.health) {
        // no-health entity: reposition, redirect velocity along forward
        other->v.origin[0] = t->v.origin[0];
        other->v.origin[1] = t->v.origin[1];
        other->v.origin[2] = t->v.origin[2];
        float vx = other->v.velocity[0], vy = other->v.velocity[1];
        float s2 = vx + vy;
        other->v.velocity[0] = g->v_forward[0] * s2;
        other->v.velocity[1] = g->v_forward[1] * s2;
        other->v.velocity[2] = g->v_forward[2] * s2;
        return;
    }

    eng->SV_SetOrigin(other, t->v.origin);
    other->v.angles[0] = t->v.mangle[0];
    other->v.angles[1] = t->v.mangle[1];
    other->v.angles[2] = t->v.mangle[2];
    if (other->v.classname && strcmp(other->v.classname, "player") == 0) {
        other->v.fixangle      = 1;
        other->v.teleport_time = g->time + 0.7f;
        if ((int)other->v.flags & FL_ONGROUND)
            other->v.flags = (float)((int)other->v.flags - FL_ONGROUND);
        other->v.velocity[0] = g->v_forward[0] * 300.0f;
        other->v.velocity[1] = g->v_forward[1] * 300.0f;
        other->v.velocity[2] = g->v_forward[2] * 300.0f;
    }
    // QC bug: `flags - flags & FL_ONGROUND` = `(flags-flags) & FL_ONGROUND` = 0
    other->v.flags = (float)(((int)other->v.flags - (int)other->v.flags) & FL_ONGROUND);
}

void spawn_info_teleport_destination(edict_t *e) {
    g->self = e;
    e->v.mangle[0] = e->v.angles[0];
    e->v.mangle[1] = e->v.angles[1];
    e->v.mangle[2] = e->v.angles[2];
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0.0f;
    e->v.model     = "";
    e->v.origin[2] += 27.0f;
    if (!e->v.targetname)
        eng->Host_Error("info_teleport_destination: no targetname");
}

static void teleport_use(edict_t *self, edict_t *activator) {
    (void)activator;
    g->self           = self;
    self->v.nextthink = g->time + 0.2f;
    g->force_retouch  = 2;
    self->v.think     = NULL;  // SUB_Null
}

void spawn_trigger_teleport(edict_t *e) {
    g->self = e;
    InitTrigger(e);
    e->v.touch = teleport_touch;
    if (!e->v.target)
        eng->Host_Error("trigger_teleport: no target");
    e->v.use = teleport_use;
    if (!((int)e->v.spawnflags & SILENT)) {
        eng->PrecacheSound("ambience/hum1.wav");
        vec3_t o = {
            (e->v.mins[0] + e->v.maxs[0]) * 0.5f,
            (e->v.mins[1] + e->v.maxs[1]) * 0.5f,
            (e->v.mins[2] + e->v.maxs[2]) * 0.5f
        };
        eng->SV_AmbientSound(o, "ambience/hum1.wav", 0.5f, ATTN_STATIC);
    }
}

// ---------------------------------------------------------------------------
// trigger_setskill
// ---------------------------------------------------------------------------
static void trigger_skill_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    eng->Cvar_SetValue("skill", (float)atoi(self->v.message ? self->v.message : "0"));
}

void spawn_trigger_setskill(edict_t *e) {
    g->self = e;
    InitTrigger(e);
    e->v.touch = trigger_skill_touch;
}

// ---------------------------------------------------------------------------
// trigger_onlyregistered
// ---------------------------------------------------------------------------
static void trigger_onlyregistered_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    if (self->v.attack_finished > g->time)
        return;
    self->v.attack_finished = g->time + 2.0f;
    if (eng->Cvar_VariableValue("registered")) {
        self->v.message = "";
        g->activator    = other;
        SUB_UseTargets();
        eng->ED_Free(self);
    } else {
        if (self->v.message && self->v.message[0] != '\0') {
            eng->SV_CenterPrint(other, self->v.message);
            eng->SV_StartSound(other, CHAN_BODY, "misc/talk.wav", 1, ATTN_NORM);
        }
    }
}

void spawn_trigger_onlyregistered(edict_t *e) {
    g->self = e;
    eng->PrecacheSound("misc/talk.wav");
    InitTrigger(e);
    e->v.touch = trigger_onlyregistered_touch;
}

// ---------------------------------------------------------------------------
// trigger_hurt
// ---------------------------------------------------------------------------
static void hurt_on(edict_t *self) {
    self->v.solid     = SOLID_TRIGGER;
    self->v.nextthink = -1;
}

static void hurt_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if ((int)other->v.takedamage) {
        self->v.solid     = SOLID_NOT;
        T_Damage(other, self, self, self->v.dmg);
        self->v.think     = hurt_on;
        self->v.nextthink = g->time + 1.0f;
    }
}

void spawn_trigger_hurt(edict_t *e) {
    g->self = e;
    InitTrigger(e);
    e->v.touch = hurt_touch;
    if (!e->v.dmg)
        e->v.dmg = 5;
}

// ---------------------------------------------------------------------------
// trigger_push
// ---------------------------------------------------------------------------
static void trigger_push_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (other->v.classname && strcmp(other->v.classname, "grenade") == 0) {
        other->v.velocity[0] = self->v.speed * self->v.movedir[0] * 10.0f;
        other->v.velocity[1] = self->v.speed * self->v.movedir[1] * 10.0f;
        other->v.velocity[2] = self->v.speed * self->v.movedir[2] * 10.0f;
    } else if (other->v.health > 0) {
        other->v.velocity[0] = self->v.speed * self->v.movedir[0] * 10.0f;
        other->v.velocity[1] = self->v.speed * self->v.movedir[1] * 10.0f;
        other->v.velocity[2] = self->v.speed * self->v.movedir[2] * 10.0f;
        if (other->v.classname && strcmp(other->v.classname, "player") == 0) {
            if (other->v.fly_sound < g->time) {
                other->v.fly_sound = g->time + 1.5f;
                eng->SV_StartSound(other, CHAN_AUTO, "ambience/windfly.wav", 1, ATTN_NORM);
            }
        }
    }
    if ((int)self->v.spawnflags & PUSH_ONCE)
        eng->ED_Free(self);
}

void spawn_trigger_push(edict_t *e) {
    g->self = e;
    InitTrigger(e);
    eng->PrecacheSound("ambience/windfly.wav");
    e->v.touch = trigger_push_touch;
    if (!e->v.speed)
        e->v.speed = 1000;
}

// ---------------------------------------------------------------------------
// trigger_monsterjump
// ---------------------------------------------------------------------------
static void trigger_monsterjump_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    // QC bug: `flags & (FL_MONSTER|FL_FLY|FL_SWIM) != FL_MONSTER` parses as
    // `flags & ((FL_MONSTER|FL_FLY|FL_SWIM) != FL_MONSTER)` = `flags & 1` = `flags & FL_FLY`
    if (((int)other->v.flags) & FL_FLY)
        return;
    other->v.velocity[0] = self->v.movedir[0] * self->v.speed;
    other->v.velocity[1] = self->v.movedir[1] * self->v.speed;
    if (!((int)other->v.flags & FL_ONGROUND))
        return;
    other->v.flags       = (float)((int)other->v.flags - FL_ONGROUND);
    other->v.velocity[2] = self->v.height;
}

void spawn_trigger_monsterjump(edict_t *e) {
    g->self = e;
    if (!e->v.speed)  e->v.speed  = 200;
    if (!e->v.height) e->v.height = 200;
    if (e->v.angles[0] == 0.0f && e->v.angles[1] == 0.0f && e->v.angles[2] == 0.0f)
        e->v.angles[1] = 360.0f;
    InitTrigger(e);
    e->v.touch = trigger_monsterjump_touch;
}

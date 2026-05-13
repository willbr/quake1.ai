// plats.c -- Platform and train entities (plats.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void SUB_CalcMove(vec3_t tdest, float tspeed, thinkfn_t func);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);

#define PLAT_LOW_TRIGGER  1

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void plat_go_up(edict_t *self);
static void plat_go_down(edict_t *self);
static void plat_hit_top(edict_t *self);
static void plat_hit_bottom(edict_t *self);
static void plat_center_touch(edict_t *self, edict_t *other);
static void plat_outside_touch(edict_t *self, edict_t *other);
static void train_next(edict_t *self);
static void func_train_find(edict_t *self);

// ---------------------------------------------------------------------------
// func_plat
// ---------------------------------------------------------------------------
static void plat_hit_top(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise1, 1, ATTN_NORM);
    self->v.state     = STATE_TOP;
    self->v.think     = plat_go_down;
    self->v.nextthink = self->v.ltime + 3;
}

static void plat_hit_bottom(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise1, 1, ATTN_NORM);
    self->v.state = STATE_BOTTOM;
}

static void plat_go_down(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise, 1, ATTN_NORM);
    self->v.state = STATE_DOWN;
    SUB_CalcMove(self->v.pos2, self->v.speed, plat_hit_bottom);
}

static void plat_go_up(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise, 1, ATTN_NORM);
    self->v.state = STATE_UP;
    SUB_CalcMove(self->v.pos1, self->v.speed, plat_hit_top);
}

static void plat_center_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    if (other->v.health <= 0)
        return;
    edict_t *plat = self->v.enemy;  // inner trigger's enemy = the platform
    g->self = plat;
    if ((int)plat->v.state == STATE_BOTTOM)
        plat_go_up(plat);
    else if ((int)plat->v.state == STATE_TOP)
        plat->v.nextthink = plat->v.ltime + 1;
}

static void plat_outside_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    if (other->v.health <= 0)
        return;
    edict_t *plat = self->v.enemy;
    g->self = plat;
    if ((int)plat->v.state == STATE_TOP)
        plat_go_down(plat);
}

static void plat_trigger_use(edict_t *self, edict_t *activator) {
    (void)activator;
    g->self = self;
    if (self->v.think)
        return;  // already activated
    plat_go_down(self);
}

static void plat_crush(edict_t *self, edict_t *blocker) {
    g->self  = self;
    g->other = blocker;
    T_Damage(blocker, self, self, 1);
    if ((int)self->v.state == STATE_UP)
        plat_go_down(self);
    else if ((int)self->v.state == STATE_DOWN)
        plat_go_up(self);
    else
        eng->Host_Error("plat_crush: bad state\n");
}

static void plat_use(edict_t *self, edict_t *activator) {
    (void)activator;
    g->self    = self;
    self->v.use = NULL;  // SUB_Null
    if ((int)self->v.state != STATE_UP)
        eng->Host_Error("plat_use: not in up state");
    plat_go_down(self);
}

static void plat_spawn_inside_trigger(edict_t *plat) {
    edict_t *trigger      = eng->ED_Alloc();
    trigger->v.classname  = "trigger_plat"; // EDIT_CAT_TRIGGER prefix → red AABB in editor
    trigger->v.touch      = plat_center_touch;
    trigger->v.movetype   = MOVETYPE_NONE;
    trigger->v.solid      = SOLID_TRIGGER;
    trigger->v.enemy      = plat;

    float tmin[3] = { plat->v.mins[0]+25, plat->v.mins[1]+25, plat->v.mins[2] };
    float tmax[3] = { plat->v.maxs[0]-25, plat->v.maxs[1]-25, plat->v.maxs[2]+8 };

    // top of trigger spans from pos1 down to cover the travel range
    tmin[2] = tmax[2] - (plat->v.pos1[2] - plat->v.pos2[2] + 8);
    if ((int)plat->v.spawnflags & PLAT_LOW_TRIGGER)
        tmax[2] = tmin[2] + 8;

    if (plat->v.size[0] <= 50) {
        tmin[0] = (plat->v.mins[0] + plat->v.maxs[0]) * 0.5f;
        tmax[0] = tmin[0] + 1;
    }
    if (plat->v.size[1] <= 50) {
        tmin[1] = (plat->v.mins[1] + plat->v.maxs[1]) * 0.5f;
        tmax[1] = tmin[1] + 1;
    }

    eng->SV_SetSize(trigger, tmin, tmax);
}

void spawn_func_plat(edict_t *e) {
    g->self = e;

    if (!e->v.t_length) e->v.t_length = 80;
    if (!e->v.t_width)  e->v.t_width  = 10;

    if ((int)e->v.sounds == 0) e->v.sounds = 2;

    if ((int)e->v.sounds == 1) {
        eng->PrecacheSound("plats/plat1.wav");
        eng->PrecacheSound("plats/plat2.wav");
        e->v.noise  = "plats/plat1.wav";
        e->v.noise1 = "plats/plat2.wav";
    } else if ((int)e->v.sounds == 2) {
        eng->PrecacheSound("plats/medplat1.wav");
        eng->PrecacheSound("plats/medplat2.wav");
        e->v.noise  = "plats/medplat1.wav";
        e->v.noise1 = "plats/medplat2.wav";
    }

    e->v.mangle[0] = e->v.angles[0];
    e->v.mangle[1] = e->v.angles[1];
    e->v.mangle[2] = e->v.angles[2];
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0.0f;

    e->v.classname = "plat";
    e->v.solid     = SOLID_BSP;
    e->v.movetype  = MOVETYPE_PUSH;
    vec3_t orig = { e->v.origin[0], e->v.origin[1], e->v.origin[2] };
    eng->SV_SetOrigin(e, orig);
    eng->SV_SetModel(e, e->v.model);
    eng->SV_SetSize(e, e->v.mins, e->v.maxs);

    e->v.blocked = plat_crush;
    if (!e->v.speed) e->v.speed = 150;

    // pos1 = top (spawn origin), pos2 = bottom
    e->v.pos1[0] = e->v.origin[0]; e->v.pos1[1] = e->v.origin[1]; e->v.pos1[2] = e->v.origin[2];
    e->v.pos2[0] = e->v.origin[0]; e->v.pos2[1] = e->v.origin[1];
    if (e->v.height)
        e->v.pos2[2] = e->v.origin[2] - e->v.height;
    else
        e->v.pos2[2] = e->v.origin[2] - e->v.size[2] + 8;

    e->v.use = plat_trigger_use;
    plat_spawn_inside_trigger(e);

    if (e->v.targetname) {
        e->v.state = STATE_UP;
        e->v.use   = plat_use;
    } else {
        eng->SV_SetOrigin(e, e->v.pos2);
        e->v.state = STATE_BOTTOM;
    }
}

// ---------------------------------------------------------------------------
// func_train
// ---------------------------------------------------------------------------
static void train_wait(edict_t *self);

static void train_blocked(edict_t *self, edict_t *blocker) {
    g->self  = self;
    g->other = blocker;
    if (g->time < self->v.attack_finished)
        return;
    self->v.attack_finished = g->time + 0.5f;
    T_Damage(blocker, self, self, self->v.dmg);
}

static void train_use(edict_t *self, edict_t *activator) {
    (void)activator;
    g->self = self;
    if (self->v.think != func_train_find)
        return;  // already activated
    train_next(self);
}

static void train_wait(edict_t *self) {
    g->self = self;
    if (self->v.wait) {
        self->v.nextthink = self->v.ltime + self->v.wait;
        eng->SV_StartSound(self, CHAN_VOICE, self->v.noise, 1, ATTN_NORM);
    } else {
        self->v.nextthink = self->v.ltime + 0.1f;
    }
    self->v.think = train_next;
}

static void train_next(edict_t *self) {
    g->self = self;
    edict_t *targ = eng->ED_Find(g->world, "targetname", self->v.target);
    self->v.target = targ->v.target;  // advance to next waypoint
    if (!self->v.target)
        eng->Host_Error("train_next: no next target");
    self->v.wait = targ->v.wait ? targ->v.wait : 0;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise1, 1, ATTN_NORM);
    vec3_t dest = {
        targ->v.origin[0] - self->v.mins[0],
        targ->v.origin[1] - self->v.mins[1],
        targ->v.origin[2] - self->v.mins[2]
    };
    SUB_CalcMove(dest, self->v.speed, train_wait);
}

static void func_train_find(edict_t *self) {
    g->self = self;
    edict_t *targ = eng->ED_Find(g->world, "targetname", self->v.target);
    self->v.target = targ->v.target;
    vec3_t pos = {
        targ->v.origin[0] - self->v.mins[0],
        targ->v.origin[1] - self->v.mins[1],
        targ->v.origin[2] - self->v.mins[2]
    };
    eng->SV_SetOrigin(self, pos);
    if (!self->v.targetname) {
        self->v.nextthink = self->v.ltime + 0.1f;
        self->v.think     = train_next;
    }
}

void spawn_func_train(edict_t *e) {
    g->self = e;
    if (!e->v.speed)  e->v.speed = 100;
    if (!e->v.target) eng->Host_Error("func_train without a target");
    if (!e->v.dmg)    e->v.dmg   = 2;

    if ((int)e->v.sounds == 0) {
        eng->PrecacheSound("misc/null.wav");
        e->v.noise  = "misc/null.wav";
        e->v.noise1 = "misc/null.wav";
    } else if ((int)e->v.sounds == 1) {
        eng->PrecacheSound("plats/train2.wav");
        eng->PrecacheSound("plats/train1.wav");
        e->v.noise  = "plats/train2.wav";
        e->v.noise1 = "plats/train1.wav";
    }

    e->v.cnt       = 1;
    e->v.solid     = SOLID_BSP;
    e->v.movetype  = MOVETYPE_PUSH;
    e->v.blocked   = train_blocked;
    e->v.use       = train_use;
    e->v.classname = "train";

    eng->SV_SetModel(e, e->v.model);
    eng->SV_SetSize(e, e->v.mins, e->v.maxs);
    vec3_t orig = { e->v.origin[0], e->v.origin[1], e->v.origin[2] };
    eng->SV_SetOrigin(e, orig);

    // find initial position on second frame so targets have spawned
    e->v.nextthink = e->v.ltime + 0.1f;
    e->v.think     = func_train_find;
}

// ---------------------------------------------------------------------------
// misc_teleporttrain
// ---------------------------------------------------------------------------
void spawn_misc_teleporttrain(edict_t *e) {
    g->self = e;
    if (!e->v.speed)  e->v.speed = 100;
    if (!e->v.target) eng->Host_Error("misc_teleporttrain without a target");

    e->v.cnt      = 1;
    e->v.solid    = SOLID_NOT;
    e->v.movetype = MOVETYPE_PUSH;
    e->v.blocked  = train_blocked;
    e->v.use      = train_use;
    e->v.avelocity[0] = 100; e->v.avelocity[1] = 200; e->v.avelocity[2] = 300;

    eng->PrecacheSound("misc/null.wav");
    e->v.noise  = "misc/null.wav";
    e->v.noise1 = "misc/null.wav";

    eng->PrecacheModel("progs/teleport.mdl");
    eng->SV_SetModel(e, "progs/teleport.mdl");
    eng->SV_SetSize(e, e->v.mins, e->v.maxs);
    vec3_t orig = { e->v.origin[0], e->v.origin[1], e->v.origin[2] };
    eng->SV_SetOrigin(e, orig);

    e->v.nextthink = e->v.ltime + 0.1f;
    e->v.think     = func_train_find;
}

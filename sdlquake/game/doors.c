// doors.c -- Door and secret-door entities (doors.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void SUB_CalcMove(vec3_t tdest, float tspeed, thinkfn_t func);
extern void SUB_UseTargets(void);
extern void SetMovedir(edict_t *e);

// ---------------------------------------------------------------------------
// Door spawnflags
// ---------------------------------------------------------------------------
#define DOOR_START_OPEN  1
#define DOOR_DONT_LINK   4
#define DOOR_GOLD_KEY    8
#define DOOR_SILVER_KEY  16
#define DOOR_TOGGLE      32

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void door_go_down(edict_t *self);
static void door_go_up(edict_t *self);
static void door_fire(edict_t *self);
static void door_use(edict_t *self, edict_t *activator);
static void door_hit_top(edict_t *self);
static void door_hit_bottom(edict_t *self);
static void fd_secret_move1(edict_t *self);
static void fd_secret_move2(edict_t *self);
static void fd_secret_move3(edict_t *self);
static void fd_secret_move4(edict_t *self);
static void fd_secret_move5(edict_t *self);
static void fd_secret_move6(edict_t *self);
static void fd_secret_done(edict_t *self);
static void fd_secret_use_impl(edict_t *self);
static void LinkDoors(edict_t *self);

// ---------------------------------------------------------------------------
// THINK FUNCTIONS
// ---------------------------------------------------------------------------
static void door_blocked(edict_t *self, edict_t *blocker) {
    g->self  = self;
    g->other = blocker;
    T_Damage(blocker, self, self, self->v.dmg);
    if (self->v.wait >= 0) {
        if ((int)self->v.state == STATE_DOWN)
            door_go_up(self);
        else
            door_go_down(self);
    }
}

static void door_hit_top(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise1, 1, ATTN_NORM);
    self->v.state = STATE_TOP;
    if (((int)self->v.spawnflags) & DOOR_TOGGLE)
        return;
    self->v.think     = door_go_down;
    self->v.nextthink = self->v.ltime + self->v.wait;
}

static void door_hit_bottom(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise1, 1, ATTN_NORM);
    self->v.state = STATE_BOTTOM;
}

static void door_go_down(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise2, 1, ATTN_NORM);
    if (self->v.max_health) {
        self->v.takedamage = DAMAGE_YES;
        self->v.health     = self->v.max_health;
    }
    self->v.state = STATE_DOWN;
    SUB_CalcMove(self->v.pos1, self->v.speed, door_hit_bottom);
}

static void door_go_up(edict_t *self) {
    g->self = self;
    if ((int)self->v.state == STATE_UP)   return;
    if ((int)self->v.state == STATE_TOP) {
        self->v.nextthink = self->v.ltime + self->v.wait;
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise2, 1, ATTN_NORM);
    self->v.state = STATE_UP;
    SUB_CalcMove(self->v.pos2, self->v.speed, door_hit_top);
    SUB_UseTargets();
}

// ---------------------------------------------------------------------------
// ACTIVATION FUNCTIONS
// ---------------------------------------------------------------------------
static void door_fire(edict_t *self) {
    g->self = self;
    if (self->v.owner != self)
        eng->Host_Error("door_fire: self.owner != self");

    // Play use key sound
    if ((int)self->v.items)
        eng->SV_StartSound(self, CHAN_VOICE, self->v.noise4, 1, ATTN_NORM);

    self->v.message = NULL;  // no more message

    if (((int)self->v.spawnflags) & DOOR_TOGGLE) {
        if ((int)self->v.state == STATE_UP || (int)self->v.state == STATE_TOP) {
            edict_t *cur = self;
            do {
                g->self = cur;
                door_go_down(cur);
                cur = cur->v.enemy;
            } while (cur && cur != self && cur != g->world);
            g->self = self;
            return;
        }
    }

    edict_t *cur = self;
    do {
        g->self = cur;
        door_go_up(cur);
        cur = cur->v.enemy;
    } while (cur && cur != self && cur != g->world);
    g->self = self;
}

static void door_use(edict_t *self, edict_t *activator) {
    (void)activator;
    g->self = self;
    self->v.message = NULL;
    if (self->v.owner) self->v.owner->v.message = NULL;
    if (self->v.enemy) self->v.enemy->v.message = NULL;
    edict_t *owner = self->v.owner ? self->v.owner : self;
    g->self = owner;
    door_fire(owner);
    g->self = self;
}

static void door_trigger_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (other->v.health <= 0)                 return;
    if (g->time < self->v.attack_finished)    return;
    self->v.attack_finished = g->time + 1;
    g->activator = other;
    edict_t *owner = self->v.owner ? self->v.owner : self;
    door_use(owner, other);
}

static void door_killed(edict_t *self) {
    g->self = self;
    edict_t *owner = self->v.owner ? self->v.owner : self;
    owner->v.health    = owner->v.max_health;
    owner->v.takedamage = DAMAGE_NO;
    g->self = owner;
    door_use(owner, g->activator);
    g->self = self;
}

static void door_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    if (self->v.owner && self->v.owner->v.attack_finished > g->time)
        return;
    if (self->v.owner) self->v.owner->v.attack_finished = g->time + 2;

    edict_t *owner = self->v.owner ? self->v.owner : self;
    if (owner->v.message && *owner->v.message) {
        eng->SV_CenterPrint(other, owner->v.message);
        eng->SV_StartSound(other, CHAN_VOICE, "misc/talk.wav", 1, ATTN_NORM);
    }

    if (!(int)self->v.items)
        return;

    // Check if player has required key
    if (((int)self->v.items & (int)other->v.items) != (int)self->v.items) {
        int wt = (int)g->world->v.worldtype;
        if ((int)self->v.owner->v.items == IT_KEY1) {
            if      (wt == 2) eng->SV_CenterPrint(other, "You need the silver keycard");
            else if (wt == 1) eng->SV_CenterPrint(other, "You need the silver runekey");
            else              eng->SV_CenterPrint(other, "You need the silver key");
        } else {
            if      (wt == 2) eng->SV_CenterPrint(other, "You need the gold keycard");
            else if (wt == 1) eng->SV_CenterPrint(other, "You need the gold runekey");
            else              eng->SV_CenterPrint(other, "You need the gold key");
        }
        eng->SV_StartSound(self, CHAN_VOICE, self->v.noise3, 1, ATTN_NORM);
        return;
    }

    other->v.items = (int)other->v.items - (int)self->v.items;
    self->v.touch = NULL;
    if (self->v.enemy) self->v.enemy->v.touch = NULL;
    g->activator = other;
    door_use(self, other);
}

// ---------------------------------------------------------------------------
// spawn_field -- create the auto-trigger zone around a door
// ---------------------------------------------------------------------------
static edict_t *spawn_field(vec3_t fmins, vec3_t fmaxs) {
    edict_t *trigger = eng->ED_Alloc();
    trigger->v.movetype = MOVETYPE_NONE;
    trigger->v.solid    = SOLID_TRIGGER;
    trigger->v.owner    = g->self;
    trigger->v.touch    = door_trigger_touch;
    vec3_t mins = {fmins[0]-60, fmins[1]-60, fmins[2]-8};
    vec3_t maxs = {fmaxs[0]+60, fmaxs[1]+60, fmaxs[2]+8};
    eng->SV_SetSize(trigger, mins, maxs);
    return trigger;
}

static int EntitiesTouching(edict_t *e1, edict_t *e2) {
    if (e1->v.mins[0] > e2->v.maxs[0]) return 0;
    if (e1->v.mins[1] > e2->v.maxs[1]) return 0;
    if (e1->v.mins[2] > e2->v.maxs[2]) return 0;
    if (e1->v.maxs[0] < e2->v.mins[0]) return 0;
    if (e1->v.maxs[1] < e2->v.mins[1]) return 0;
    if (e1->v.maxs[2] < e2->v.mins[2]) return 0;
    return 1;
}

static void LinkDoors(edict_t *self) {
    g->self = self;
    if (self->v.enemy) return;  // already linked
    if (((int)self->v.spawnflags) & DOOR_DONT_LINK) {
        self->v.owner = self->v.enemy = self;
        return;
    }

    vec3_t cmins = {self->v.mins[0], self->v.mins[1], self->v.mins[2]};
    vec3_t cmaxs = {self->v.maxs[0], self->v.maxs[1], self->v.maxs[2]};

    edict_t *starte = self;
    edict_t *cur    = self;
    edict_t *t      = self;

    do {
        cur->v.owner = starte;  // master door

        if (cur->v.health)     starte->v.health     = cur->v.health;
        if (cur->v.targetname) starte->v.targetname = cur->v.targetname;
        if (cur->v.message && *cur->v.message) starte->v.message = cur->v.message;

        t = eng->ED_Find(t, "classname", cur->v.classname);
        if (!t) {
            // End of chain — loop it back
            cur->v.enemy = starte;
            g->self = starte;

            if (starte->v.health)     return;
            if (starte->v.targetname) return;
            if ((int)starte->v.items) return;

            starte->v.trigger_field = spawn_field(cmins, cmaxs);
            return;
        }

        if (EntitiesTouching(cur, t)) {
            if (t->v.enemy)
                eng->Host_Error("cross connected doors");

            cur->v.enemy = t;

            if (t->v.mins[0] < cmins[0]) cmins[0] = t->v.mins[0];
            if (t->v.mins[1] < cmins[1]) cmins[1] = t->v.mins[1];
            if (t->v.mins[2] < cmins[2]) cmins[2] = t->v.mins[2];
            if (t->v.maxs[0] > cmaxs[0]) cmaxs[0] = t->v.maxs[0];
            if (t->v.maxs[1] > cmaxs[1]) cmaxs[1] = t->v.maxs[1];
            if (t->v.maxs[2] > cmaxs[2]) cmaxs[2] = t->v.maxs[2];

            cur = t;
        }
    } while (1);
}

// ---------------------------------------------------------------------------
// func_door spawn
// ---------------------------------------------------------------------------
void spawn_func_door(edict_t *e) {
    g->self = e;

    // Key sound set by world type
    int wt = (int)g->world->v.worldtype;
    if (wt == 0) {
        eng->PrecacheSound("doors/medtry.wav"); eng->PrecacheSound("doors/meduse.wav");
        e->v.noise3 = "doors/medtry.wav";      e->v.noise4 = "doors/meduse.wav";
    } else if (wt == 1) {
        eng->PrecacheSound("doors/runetry.wav"); eng->PrecacheSound("doors/runeuse.wav");
        e->v.noise3 = "doors/runetry.wav";       e->v.noise4 = "doors/runeuse.wav";
    } else if (wt == 2) {
        eng->PrecacheSound("doors/basetry.wav"); eng->PrecacheSound("doors/baseuse.wav");
        e->v.noise3 = "doors/basetry.wav";       e->v.noise4 = "doors/baseuse.wav";
    }

    // Movement sounds by sounds field
    if ((int)e->v.sounds == 0) {
        eng->PrecacheSound("misc/null.wav");
        e->v.noise1 = "misc/null.wav"; e->v.noise2 = "misc/null.wav";
    } else if ((int)e->v.sounds == 1) {
        eng->PrecacheSound("doors/drclos4.wav"); eng->PrecacheSound("doors/doormv1.wav");
        e->v.noise1 = "doors/drclos4.wav";       e->v.noise2 = "doors/doormv1.wav";
    } else if ((int)e->v.sounds == 2) {
        eng->PrecacheSound("doors/hydro1.wav");  eng->PrecacheSound("doors/hydro2.wav");
        e->v.noise2 = "doors/hydro1.wav";        e->v.noise1 = "doors/hydro2.wav";
    } else if ((int)e->v.sounds == 3) {
        eng->PrecacheSound("doors/stndr1.wav");  eng->PrecacheSound("doors/stndr2.wav");
        e->v.noise2 = "doors/stndr1.wav";        e->v.noise1 = "doors/stndr2.wav";
    } else if ((int)e->v.sounds == 4) {
        eng->PrecacheSound("doors/ddoor1.wav");  eng->PrecacheSound("doors/ddoor2.wav");
        e->v.noise1 = "doors/ddoor2.wav";        e->v.noise2 = "doors/ddoor1.wav";
    }

    SetMovedir(e);

    e->v.max_health = e->v.health;
    e->v.solid      = SOLID_BSP;
    e->v.movetype   = MOVETYPE_PUSH;
    eng->SV_SetOrigin(e, e->v.origin);
    eng->SV_SetModel(e, e->v.model);
    e->v.classname  = "door";
    e->v.blocked    = door_blocked;
    e->v.use        = door_use;

    if (((int)e->v.spawnflags) & DOOR_SILVER_KEY) e->v.items = IT_KEY1;
    if (((int)e->v.spawnflags) & DOOR_GOLD_KEY)   e->v.items = IT_KEY2;

    if (!e->v.speed) e->v.speed = 100;
    if (!e->v.wait)  e->v.wait  = 3;
    if (!e->v.lip)   e->v.lip   = 8;
    if (!e->v.dmg)   e->v.dmg   = 2;

    e->v.pos1[0] = e->v.origin[0];
    e->v.pos1[1] = e->v.origin[1];
    e->v.pos1[2] = e->v.origin[2];

    float dot = fabsf(
        e->v.movedir[0]*e->v.size[0] +
        e->v.movedir[1]*e->v.size[1] +
        e->v.movedir[2]*e->v.size[2]);
    float scale = dot - e->v.lip;
    e->v.pos2[0] = e->v.pos1[0] + e->v.movedir[0] * scale;
    e->v.pos2[1] = e->v.pos1[1] + e->v.movedir[1] * scale;
    e->v.pos2[2] = e->v.pos1[2] + e->v.movedir[2] * scale;

    if (((int)e->v.spawnflags) & DOOR_START_OPEN) {
        eng->SV_SetOrigin(e, e->v.pos2);
        e->v.pos2[0] = e->v.pos1[0]; e->v.pos2[1] = e->v.pos1[1]; e->v.pos2[2] = e->v.pos1[2];
        e->v.pos1[0] = e->v.origin[0]; e->v.pos1[1] = e->v.origin[1]; e->v.pos1[2] = e->v.origin[2];
    }

    e->v.state = STATE_BOTTOM;

    if (e->v.health) {
        e->v.takedamage = DAMAGE_YES;
        e->v.th_die     = door_killed;
    }
    if ((int)e->v.items) e->v.wait = -1;

    e->v.touch    = door_touch;
    e->v.think    = LinkDoors;
    e->v.nextthink = e->v.ltime + 0.1f;
}

// ---------------------------------------------------------------------------
// SECRET DOORS
// ---------------------------------------------------------------------------
#define SECRET_OPEN_ONCE  1
#define SECRET_1ST_LEFT   2
#define SECRET_1ST_DOWN   4
#define SECRET_NO_SHOOT   8
#define SECRET_YES_SHOOT  16

static void fd_secret_use_impl(edict_t *self) {
    g->self = self;
    self->v.health = 10000;

    // Exit if still moving
    if (self->v.origin[0] != self->v.oldorigin[0] ||
        self->v.origin[1] != self->v.oldorigin[1] ||
        self->v.origin[2] != self->v.oldorigin[2])
        return;

    self->v.message = NULL;
    SUB_UseTargets();

    if (!(((int)self->v.spawnflags) & SECRET_NO_SHOOT)) {
        self->v.th_pain    = NULL;
        self->v.takedamage = DAMAGE_NO;
    }
    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0;

    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise1, 1, ATTN_NORM);
    self->v.nextthink = self->v.ltime + 0.1f;

    int temp = 1 - (((int)self->v.spawnflags) & SECRET_1ST_LEFT);  // 1 or -1
    eng->MakeVectors(self->v.mangle);

    if (!self->v.t_width) {
        if (((int)self->v.spawnflags) & SECRET_1ST_DOWN)
            self->v.t_width = fabsf(g->v_up[0]*self->v.size[0] + g->v_up[1]*self->v.size[1] + g->v_up[2]*self->v.size[2]);
        else
            self->v.t_width = fabsf(g->v_right[0]*self->v.size[0] + g->v_right[1]*self->v.size[1] + g->v_right[2]*self->v.size[2]);
    }
    if (!self->v.t_length)
        self->v.t_length = fabsf(g->v_forward[0]*self->v.size[0] + g->v_forward[1]*self->v.size[1] + g->v_forward[2]*self->v.size[2]);

    if (((int)self->v.spawnflags) & SECRET_1ST_DOWN) {
        self->v.dest1[0] = self->v.origin[0] - g->v_up[0] * self->v.t_width;
        self->v.dest1[1] = self->v.origin[1] - g->v_up[1] * self->v.t_width;
        self->v.dest1[2] = self->v.origin[2] - g->v_up[2] * self->v.t_width;
    } else {
        self->v.dest1[0] = self->v.origin[0] + g->v_right[0] * (self->v.t_width * temp);
        self->v.dest1[1] = self->v.origin[1] + g->v_right[1] * (self->v.t_width * temp);
        self->v.dest1[2] = self->v.origin[2] + g->v_right[2] * (self->v.t_width * temp);
    }
    self->v.dest2[0] = self->v.dest1[0] + g->v_forward[0] * self->v.t_length;
    self->v.dest2[1] = self->v.dest1[1] + g->v_forward[1] * self->v.t_length;
    self->v.dest2[2] = self->v.dest1[2] + g->v_forward[2] * self->v.t_length;

    SUB_CalcMove(self->v.dest1, self->v.speed, fd_secret_move1);
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise2, 1, ATTN_NORM);
}

// usefn_t wrapper
static void fd_secret_use_fn(edict_t *self, edict_t *activator) {
    (void)activator;
    fd_secret_use_impl(self);
}
// painfn_t wrapper
static void fd_secret_pain_fn(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    fd_secret_use_impl(self);
}

static void fd_secret_move1(edict_t *self) {
    g->self = self;
    self->v.nextthink = self->v.ltime + 1.0f;
    self->v.think     = fd_secret_move2;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise3, 1, ATTN_NORM);
}

static void fd_secret_move2(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise2, 1, ATTN_NORM);
    SUB_CalcMove(self->v.dest2, self->v.speed, fd_secret_move3);
}

static void fd_secret_move3(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise3, 1, ATTN_NORM);
    if (!(((int)self->v.spawnflags) & SECRET_OPEN_ONCE)) {
        self->v.nextthink = self->v.ltime + self->v.wait;
        self->v.think     = fd_secret_move4;
    }
}

static void fd_secret_move4(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise2, 1, ATTN_NORM);
    SUB_CalcMove(self->v.dest1, self->v.speed, fd_secret_move5);
}

static void fd_secret_move5(edict_t *self) {
    g->self = self;
    self->v.nextthink = self->v.ltime + 1.0f;
    self->v.think     = fd_secret_move6;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise3, 1, ATTN_NORM);
}

static void fd_secret_move6(edict_t *self) {
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise2, 1, ATTN_NORM);
    SUB_CalcMove(self->v.oldorigin, self->v.speed, fd_secret_done);
}

static void fd_secret_done(edict_t *self) {
    g->self = self;
    if (!self->v.targetname ||
            (((int)self->v.spawnflags) & SECRET_YES_SHOOT)) {
        self->v.health     = 10000;
        self->v.takedamage = DAMAGE_YES;
        self->v.th_pain    = fd_secret_pain_fn;
    }
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise3, 1, ATTN_NORM);
}

static void secret_blocked(edict_t *self, edict_t *blocker) {
    g->self  = self;
    g->other = blocker;
    if (g->time < self->v.attack_finished) return;
    self->v.attack_finished = g->time + 0.5f;
    T_Damage(blocker, self, self, self->v.dmg);
}

static void secret_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0) return;
    if (self->v.attack_finished > g->time) return;
    self->v.attack_finished = g->time + 2;
    if (self->v.message) {
        eng->SV_CenterPrint(other, self->v.message);
        eng->SV_StartSound(other, CHAN_BODY, "misc/talk.wav", 1, ATTN_NORM);
    }
}

void spawn_func_door_secret(edict_t *e) {
    g->self = e;
    if (!(int)e->v.sounds) e->v.sounds = 3;

    if ((int)e->v.sounds == 1) {
        eng->PrecacheSound("doors/latch2.wav");
        eng->PrecacheSound("doors/winch2.wav");
        eng->PrecacheSound("doors/drclos4.wav");
        e->v.noise1 = "doors/latch2.wav";
        e->v.noise2 = "doors/winch2.wav";
        e->v.noise3 = "doors/drclos4.wav";
    } else if ((int)e->v.sounds == 2) {
        eng->PrecacheSound("doors/airdoor1.wav");
        eng->PrecacheSound("doors/airdoor2.wav");
        e->v.noise2 = "doors/airdoor1.wav";
        e->v.noise1 = "doors/airdoor2.wav";
        e->v.noise3 = "doors/airdoor2.wav";
    } else if ((int)e->v.sounds == 3) {
        eng->PrecacheSound("doors/basesec1.wav");
        eng->PrecacheSound("doors/basesec2.wav");
        e->v.noise2 = "doors/basesec1.wav";
        e->v.noise1 = "doors/basesec2.wav";
        e->v.noise3 = "doors/basesec2.wav";
    }

    if (!e->v.dmg)  e->v.dmg  = 2;
    if (!e->v.wait) e->v.wait = 5;
    e->v.speed = 50;  // QC sets this unconditionally

    e->v.mangle[0] = e->v.angles[0];  // QC: self.mangle = self.angles
    e->v.mangle[1] = e->v.angles[1];
    e->v.mangle[2] = e->v.angles[2];
    e->v.angles[0] = e->v.angles[1] = e->v.angles[2] = 0;
    e->v.solid    = SOLID_BSP;
    e->v.movetype = MOVETYPE_PUSH;
    e->v.classname = "door";
    eng->SV_SetModel(e, e->v.model);
    eng->SV_SetOrigin(e, e->v.origin);
    e->v.touch    = secret_touch;
    e->v.blocked  = secret_blocked;
    e->v.use      = fd_secret_use_fn;

    if (!e->v.targetname || (((int)e->v.spawnflags) & SECRET_YES_SHOOT)) {
        e->v.health    = 10000;
        e->v.takedamage = DAMAGE_YES;
        e->v.th_pain   = fd_secret_pain_fn;
    }

    e->v.oldorigin[0] = e->v.origin[0];
    e->v.oldorigin[1] = e->v.origin[1];
    e->v.oldorigin[2] = e->v.origin[2];
}

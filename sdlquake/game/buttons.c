// buttons.c -- Button entities (buttons.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void SUB_CalcMove(vec3_t tdest, float tspeed, thinkfn_t func);
extern void SUB_UseTargets(void);
extern void SetMovedir(edict_t *e);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void button_fire(edict_t *self);
static void button_wait(edict_t *self);
static void button_return(edict_t *self);
static void button_done(edict_t *self);

static void button_done(edict_t *self) {
    g->self = self;
    self->v.state = STATE_BOTTOM;
}

static void button_wait(edict_t *self) {
    g->self = self;
    self->v.state     = STATE_TOP;
    self->v.nextthink = self->v.ltime + self->v.wait;
    self->v.think     = button_return;
    g->activator      = self->v.enemy;
    SUB_UseTargets();
    self->v.frame     = 1;  // alternate textures
}

static void button_return(edict_t *self) {
    g->self = self;
    self->v.state = STATE_DOWN;
    SUB_CalcMove(self->v.pos1, self->v.speed, button_done);
    self->v.frame = 0;
    if (self->v.health)
        self->v.takedamage = DAMAGE_YES;
}

static void button_blocked(edict_t *self, edict_t *blocker) {
    (void)self; (void)blocker;  // do nothing
}

static void button_fire(edict_t *self) {
    if ((int)self->v.state == STATE_UP || (int)self->v.state == STATE_TOP)
        return;
    eng->SV_StartSound(self, CHAN_VOICE, self->v.noise, 1, ATTN_NORM);
    self->v.state = STATE_UP;
    SUB_CalcMove(self->v.pos2, self->v.speed, button_wait);
}

static void button_use(edict_t *self, edict_t *activator) {
    g->self = self;
    self->v.enemy = activator;
    button_fire(self);
}

static void button_touch(edict_t *self, edict_t *other) {
    g->self  = self;
    g->other = other;
    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;
    self->v.enemy = other;
    button_fire(self);
}

static void button_killed(edict_t *self) {
    g->self = self;
    self->v.enemy      = g->damage_attacker;
    self->v.health     = self->v.max_health;
    self->v.takedamage = DAMAGE_NO;
    button_fire(self);
}

// ---------------------------------------------------------------------------
// func_button spawn
// ---------------------------------------------------------------------------
void spawn_func_button(edict_t *e) {
    g->self = e;

    if      ((int)e->v.sounds == 0) { eng->PrecacheSound("buttons/airbut1.wav");  e->v.noise = "buttons/airbut1.wav"; }
    else if ((int)e->v.sounds == 1) { eng->PrecacheSound("buttons/switch21.wav"); e->v.noise = "buttons/switch21.wav"; }
    else if ((int)e->v.sounds == 2) { eng->PrecacheSound("buttons/switch02.wav"); e->v.noise = "buttons/switch02.wav"; }
    else if ((int)e->v.sounds == 3) { eng->PrecacheSound("buttons/switch04.wav"); e->v.noise = "buttons/switch04.wav"; }

    SetMovedir(e);

    e->v.movetype = MOVETYPE_PUSH;
    e->v.solid    = SOLID_BSP;
    eng->SV_SetModel(e, e->v.model);

    e->v.blocked = button_blocked;
    e->v.use     = button_use;

    if (e->v.health) {
        e->v.max_health = e->v.health;
        e->v.th_die     = button_killed;
        e->v.takedamage = DAMAGE_YES;
    } else {
        e->v.touch = button_touch;
    }

    if (!e->v.speed) e->v.speed = 40;
    if (!e->v.wait)  e->v.wait  = 1;
    if (!e->v.lip)   e->v.lip   = 4;

    e->v.state    = STATE_BOTTOM;
    e->v.pos1[0]  = e->v.origin[0];
    e->v.pos1[1]  = e->v.origin[1];
    e->v.pos1[2]  = e->v.origin[2];

    float dot = fabsf(
        e->v.movedir[0]*e->v.size[0] +
        e->v.movedir[1]*e->v.size[1] +
        e->v.movedir[2]*e->v.size[2]);
    float scale = dot - e->v.lip;
    e->v.pos2[0] = e->v.pos1[0] + e->v.movedir[0] * scale;
    e->v.pos2[1] = e->v.pos1[1] + e->v.movedir[1] * scale;
    e->v.pos2[2] = e->v.pos1[2] + e->v.movedir[2] * scale;
}

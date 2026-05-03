// subs.c -- Movement and targeting helpers. Source: subs.qc

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

void SUB_Null(edict_t *self)   { (void)self; }
void SUB_Remove(edict_t *self) { g->self = self; eng->ED_Free(self); }

static void SetMovedir(void)
{
    vec3_t down = {0,  0, -1};
    vec3_t up   = {0,  0,  1};
    vec3_t zero = {0,  0,  0};
    vec3_t neg1 = {0, -1,  0};
    vec3_t neg2 = {0, -2,  0};

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
    if (g->self->v.angles[0] || g->self->v.angles[1] || g->self->v.angles[2])
        SetMovedir();
    g->self->v.solid     = SOLID_TRIGGER;
    eng->SV_SetModel(g->self, g->self->v.model);
    g->self->v.movetype  = MOVETYPE_NONE;
    g->self->v.modelindex = 0;
    g->self->v.model     = "";
}

static void SUB_CalcMoveDone(edict_t *self);
static void SUB_CalcAngleMoveDone(edict_t *self);
void SUB_CalcMove(vec3_t tdest, float tspeed, thinkfn_t func);
void SUB_CalcAngleMove(vec3_t destangle, float tspeed, thinkfn_t func);

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

    g->self->v.think1   = func;
    memcpy(g->self->v.finaldest, tdest, sizeof(vec3_t));
    g->self->v.think    = SUB_CalcMoveDone;

    vdestdelta[0] = tdest[0] - g->self->v.origin[0];
    vdestdelta[1] = tdest[1] - g->self->v.origin[1];
    vdestdelta[2] = tdest[2] - g->self->v.origin[2];

    len = eng->VectorLength(vdestdelta);

    if (len == 0.0f || (traveltime = len / tspeed) < 0.1f) {
        g->self->v.velocity[0] = g->self->v.velocity[1] = g->self->v.velocity[2] = 0;
        g->self->v.nextthink   = g->self->v.ltime + 0.1f;
        return;
    }

    g->self->v.nextthink   = g->self->v.ltime + traveltime;
    g->self->v.velocity[0] = vdestdelta[0] / traveltime;
    g->self->v.velocity[1] = vdestdelta[1] / traveltime;
    g->self->v.velocity[2] = vdestdelta[2] / traveltime;
}

static void SUB_CalcMoveDone(edict_t *self)
{
    g->self = self;
    eng->SV_SetOrigin(self, self->v.finaldest);
    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0;
    self->v.nextthink   = -1;
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

    g->self->v.nextthink      = g->self->v.ltime + traveltime;
    g->self->v.avelocity[0]   = destdelta[0] / traveltime;
    g->self->v.avelocity[1]   = destdelta[1] / traveltime;
    g->self->v.avelocity[2]   = destdelta[2] / traveltime;
    g->self->v.think1         = func;
    memcpy(g->self->v.finalangle, destangle, sizeof(vec3_t));
    g->self->v.think          = SUB_CalcAngleMoveDone;
}

static void SUB_CalcAngleMoveDone(edict_t *self)
{
    g->self = self;
    memcpy(self->v.angles, self->v.finalangle, sizeof(vec3_t));
    self->v.avelocity[0] = self->v.avelocity[1] = self->v.avelocity[2] = 0;
    self->v.nextthink    = -1;
    if (self->v.think1)
        self->v.think1(self);
}

void SUB_UseTargets(void);   // defined below

static void DelayThink(edict_t *self)
{
    g->self     = self;
    g->activator = self->v.enemy;
    SUB_UseTargets();
    eng->ED_Free(self);
}

void SUB_UseTargets(void)
{
    edict_t *t, *stemp, *otemp, *act;

    if (g->self->v.delay) {
        t               = eng->ED_Alloc();
        t->v.classname  = "DelayedUse";
        t->v.nextthink  = g->time + g->self->v.delay;
        t->v.think      = DelayThink;
        t->v.enemy      = g->activator;
        t->v.message    = g->self->v.message;
        t->v.killtarget = g->self->v.killtarget;
        t->v.target     = g->self->v.target;
        return;
    }

    if (g->activator && g->activator->v.classname &&
        !strcmp(g->activator->v.classname, "player") &&
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
        act = g->activator;
        t   = g->world;
        while ((t = eng->ED_Find(t, "targetname", g->self->v.target)) != g->world) {
            stemp    = g->self;
            otemp    = g->other;
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

// Weak stub — overridden by the real visible() in combat.c (Task 9).
__attribute__((weak)) int visible(edict_t *targ) { (void)targ; return 1; }

void SUB_CheckRefire(thinkfn_t thinkst)
{
    if (eng->Cvar_VariableValue("skill") != 3) return;
    if (g->self->v.cnt == 1) return;
    if (!visible(g->self->v.enemy)) return;
    g->self->v.cnt   = 1;
    g->self->v.think = thinkst;
}

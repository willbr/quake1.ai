// monster_wizard.c -- Scrag/Wizard (wizard.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_face(void);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void Corpse_LayProne(edict_t *self);
extern void flymonster_start(edict_t *self);
extern void SUB_AttackFinished(float t);
extern void SUB_Remove(edict_t *e);
extern void launch_spike(vec3_t org, vec3_t dir);

// fight.c globals
extern int enemy_vis;
extern float enemy_range;
extern float enemy_yaw;

// Frame indices
// hover1-15: 0-14
// fly1-14: 15-28
// magatt1-13: 29-41
// pain1-4: 42-45
// death1-8: 46-53
enum {
    WZ_HOVER1=0,WZ_HOVER2,WZ_HOVER3,WZ_HOVER4,WZ_HOVER5,WZ_HOVER6,WZ_HOVER7,
    WZ_HOVER8,WZ_HOVER9,WZ_HOVER10,WZ_HOVER11,WZ_HOVER12,WZ_HOVER13,WZ_HOVER14,WZ_HOVER15,
    WZ_FLY1,WZ_FLY2,WZ_FLY3,WZ_FLY4,WZ_FLY5,WZ_FLY6,WZ_FLY7,
    WZ_FLY8,WZ_FLY9,WZ_FLY10,WZ_FLY11,WZ_FLY12,WZ_FLY13,WZ_FLY14,
    WZ_MAGATT1,WZ_MAGATT2,WZ_MAGATT3,WZ_MAGATT4,WZ_MAGATT5,WZ_MAGATT6,
    WZ_MAGATT7,WZ_MAGATT8,WZ_MAGATT9,WZ_MAGATT10,WZ_MAGATT11,WZ_MAGATT12,WZ_MAGATT13,
    WZ_PAIN1,WZ_PAIN2,WZ_PAIN3,WZ_PAIN4,
    WZ_DEATH1,WZ_DEATH2,WZ_DEATH3,WZ_DEATH4,WZ_DEATH5,WZ_DEATH6,WZ_DEATH7,WZ_DEATH8
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Forward declarations
static void wz_run1(edict_t *e),wz_side1(edict_t *e),wz_fast1(edict_t *e);
static void wz_pain1(edict_t *e),wz_death1(edict_t *e),wz_death2(edict_t *e);

// Idle sound
static void Wiz_idlesound(edict_t *e) {
    float wr = eng->Random() * 5;
    if (e->v.waitmin < g->time) {
        e->v.waitmin = g->time + 2;
        if (wr > 4.5f) eng->SV_StartSound(e, CHAN_VOICE, "wizard/widle1.wav", 1, ATTN_IDLE);
        if (wr < 1.5f) eng->SV_StartSound(e, CHAN_VOICE, "wizard/widle2.wav", 1, ATTN_IDLE);
    }
}

// --- WizardCheckAttack and WizardAttackFinished ---
static void WizardAttackFinished(void) {
    edict_t *e = g->self;
    if (enemy_range >= RANGE_MID || !enemy_vis) {
        e->v.attack_state = AS_STRAIGHT;
        e->v.think = wz_run1;
    } else {
        e->v.attack_state = AS_SLIDING;
        e->v.think = wz_side1;
    }
}

// Attack decision -- ported from wizard.qc:107 WizardCheckAttack.
// Overrides the weak stub in ai.c so CheckAnyAttack dispatches here for monster_wizard.
int WizardCheckAttack(void) {
    edict_t *self = g->self;
    edict_t *targ = self->v.enemy;

    if (g->time < self->v.attack_finished) return 0;
    if (!enemy_vis)                        return 0;

    if ((int)enemy_range == RANGE_FAR) {
        if ((int)self->v.attack_state != AS_STRAIGHT) {
            self->v.attack_state = AS_STRAIGHT;
            wz_run1(self);
        }
        return 0;
    }

    // Line-of-sight check from eyes to enemy eyes.
    vec3_t spot1 = {
        self->v.origin[0] + self->v.view_ofs[0],
        self->v.origin[1] + self->v.view_ofs[1],
        self->v.origin[2] + self->v.view_ofs[2]
    };
    vec3_t spot2 = {
        targ->v.origin[0] + targ->v.view_ofs[0],
        targ->v.origin[1] + targ->v.view_ofs[1],
        targ->v.origin[2] + targ->v.view_ofs[2]
    };
    eng->SV_Traceline(spot1, spot2, 0, self);

    if (g->trace_ent != targ) {
        // No clear shot — straight-line approach.
        if ((int)self->v.attack_state != AS_STRAIGHT) {
            self->v.attack_state = AS_STRAIGHT;
            wz_run1(self);
        }
        return 0;
    }

    float chance = 0;
    if      ((int)enemy_range == RANGE_MELEE) chance = 0.9f;
    else if ((int)enemy_range == RANGE_NEAR)  chance = 0.6f;
    else if ((int)enemy_range == RANGE_MID)   chance = 0.2f;

    if (eng->Random() < chance) {
        self->v.attack_state = AS_MISSILE;
        return 1;
    }

    if ((int)enemy_range == RANGE_MID) {
        if ((int)self->v.attack_state != AS_STRAIGHT) {
            self->v.attack_state = AS_STRAIGHT;
            wz_run1(self);
        }
    } else {
        if ((int)self->v.attack_state != AS_SLIDING) {
            self->v.attack_state = AS_SLIDING;
            wz_side1(self);
        }
    }

    return 0;
}

// Stand (hover in place)
static void wz_stand2(edict_t *e),wz_stand3(edict_t *e),wz_stand4(edict_t *e),
            wz_stand5(edict_t *e),wz_stand6(edict_t *e),wz_stand7(edict_t *e),
            wz_stand8(edict_t *e);
static void wz_stand1(edict_t *e) { FRAME(e,WZ_HOVER1,wz_stand2); ai_stand(e); }
static void wz_stand2(edict_t *e) { FRAME(e,WZ_HOVER2,wz_stand3); ai_stand(e); }
static void wz_stand3(edict_t *e) { FRAME(e,WZ_HOVER3,wz_stand4); ai_stand(e); }
static void wz_stand4(edict_t *e) { FRAME(e,WZ_HOVER4,wz_stand5); ai_stand(e); }
static void wz_stand5(edict_t *e) { FRAME(e,WZ_HOVER5,wz_stand6); ai_stand(e); }
static void wz_stand6(edict_t *e) { FRAME(e,WZ_HOVER6,wz_stand7); ai_stand(e); }
static void wz_stand7(edict_t *e) { FRAME(e,WZ_HOVER7,wz_stand8); ai_stand(e); }
static void wz_stand8(edict_t *e) { FRAME(e,WZ_HOVER8,wz_stand1); ai_stand(e); }

// Walk (hover, slow)
static void wz_walk2(edict_t *e),wz_walk3(edict_t *e),wz_walk4(edict_t *e),
            wz_walk5(edict_t *e),wz_walk6(edict_t *e),wz_walk7(edict_t *e),
            wz_walk8(edict_t *e);
static void wz_walk1(edict_t *e) {
    FRAME(e,WZ_HOVER1,wz_walk2); ai_walk(8); Wiz_idlesound(e);
}
static void wz_walk2(edict_t *e) { FRAME(e,WZ_HOVER2,wz_walk3); ai_walk(8); }
static void wz_walk3(edict_t *e) { FRAME(e,WZ_HOVER3,wz_walk4); ai_walk(8); }
static void wz_walk4(edict_t *e) { FRAME(e,WZ_HOVER4,wz_walk5); ai_walk(8); }
static void wz_walk5(edict_t *e) { FRAME(e,WZ_HOVER5,wz_walk6); ai_walk(8); }
static void wz_walk6(edict_t *e) { FRAME(e,WZ_HOVER6,wz_walk7); ai_walk(8); }
static void wz_walk7(edict_t *e) { FRAME(e,WZ_HOVER7,wz_walk8); ai_walk(8); }
static void wz_walk8(edict_t *e) { FRAME(e,WZ_HOVER8,wz_walk1); ai_walk(8); }

// Side (hover sliding)
static void wz_side2(edict_t *e),wz_side3(edict_t *e),wz_side4(edict_t *e),
            wz_side5(edict_t *e),wz_side6(edict_t *e),wz_side7(edict_t *e),
            wz_side8(edict_t *e);
static void wz_side1(edict_t *e) {
    FRAME(e,WZ_HOVER1,wz_side2); ai_run(8); Wiz_idlesound(e);
}
static void wz_side2(edict_t *e) { FRAME(e,WZ_HOVER2,wz_side3); ai_run(8); }
static void wz_side3(edict_t *e) { FRAME(e,WZ_HOVER3,wz_side4); ai_run(8); }
static void wz_side4(edict_t *e) { FRAME(e,WZ_HOVER4,wz_side5); ai_run(8); }
static void wz_side5(edict_t *e) { FRAME(e,WZ_HOVER5,wz_side6); ai_run(8); }
static void wz_side6(edict_t *e) { FRAME(e,WZ_HOVER6,wz_side7); ai_run(8); }
static void wz_side7(edict_t *e) { FRAME(e,WZ_HOVER7,wz_side8); ai_run(8); }
static void wz_side8(edict_t *e) { FRAME(e,WZ_HOVER8,wz_side1); ai_run(8); }

// Run (fly frames)
static void wz_run2(edict_t *e),wz_run3(edict_t *e),wz_run4(edict_t *e),wz_run5(edict_t *e),
            wz_run6(edict_t *e),wz_run7(edict_t *e),wz_run8(edict_t *e),wz_run9(edict_t *e),
            wz_run10(edict_t *e),wz_run11(edict_t *e),wz_run12(edict_t *e),wz_run13(edict_t *e),
            wz_run14(edict_t *e);
static void wz_run1(edict_t *e) {
    FRAME(e,WZ_FLY1,wz_run2); ai_run(16); Wiz_idlesound(e);
}
static void wz_run2(edict_t *e)  { FRAME(e,WZ_FLY2, wz_run3);  ai_run(16); }
static void wz_run3(edict_t *e)  { FRAME(e,WZ_FLY3, wz_run4);  ai_run(16); }
static void wz_run4(edict_t *e)  { FRAME(e,WZ_FLY4, wz_run5);  ai_run(16); }
static void wz_run5(edict_t *e)  { FRAME(e,WZ_FLY5, wz_run6);  ai_run(16); }
static void wz_run6(edict_t *e)  { FRAME(e,WZ_FLY6, wz_run7);  ai_run(16); }
static void wz_run7(edict_t *e)  { FRAME(e,WZ_FLY7, wz_run8);  ai_run(16); }
static void wz_run8(edict_t *e)  { FRAME(e,WZ_FLY8, wz_run9);  ai_run(16); }
static void wz_run9(edict_t *e)  { FRAME(e,WZ_FLY9, wz_run10); ai_run(16); }
static void wz_run10(edict_t *e) { FRAME(e,WZ_FLY10,wz_run11); ai_run(16); }
static void wz_run11(edict_t *e) { FRAME(e,WZ_FLY11,wz_run12); ai_run(16); }
static void wz_run12(edict_t *e) { FRAME(e,WZ_FLY12,wz_run13); ai_run(16); }
static void wz_run13(edict_t *e) { FRAME(e,WZ_FLY13,wz_run14); ai_run(16); }
static void wz_run14(edict_t *e) { FRAME(e,WZ_FLY14,wz_run1);  ai_run(16); }

// --- Wiz_FastFire helper entity callback ---
static void Wiz_FastFire(edict_t *self) {
    g->self = self;
    if (self->v.owner->v.health > 0) {
        self->v.owner->v.effects = (float)((int)self->v.owner->v.effects | EF_MUZZLEFLASH);
        eng->MakeVectors(self->v.enemy->v.angles);
        // dst = enemy.origin - 13 * movedir
        vec3_t dst = {
            self->v.enemy->v.origin[0] - 13*self->v.movedir[0],
            self->v.enemy->v.origin[1] - 13*self->v.movedir[1],
            self->v.enemy->v.origin[2] - 13*self->v.movedir[2]
        };
        float dx = dst[0]-self->v.origin[0], dy = dst[1]-self->v.origin[1], dz = dst[2]-self->v.origin[2];
        float len = (float)sqrt((double)(dx*dx+dy*dy+dz*dz));
        if (len < 0.001f) len = 0.001f;
        vec3_t vec = {dx/len, dy/len, dz/len};
        eng->SV_StartSound(self, CHAN_WEAPON, "wizard/wattack.wav", 1, ATTN_NORM);
        launch_spike(self->v.origin, vec);
        // g->newmis is set by launch_spike
        g->newmis->v.velocity[0] = vec[0]*600;
        g->newmis->v.velocity[1] = vec[1]*600;
        g->newmis->v.velocity[2] = vec[2]*600;
        g->newmis->v.owner = self->v.owner;
        // set classname and model
        eng->SV_SetModel(g->newmis, "progs/w_spike.mdl");
        vec3_t z = {0,0,0};
        eng->SV_SetSize(g->newmis, z, z);
    }
    eng->ED_Free(self);
}

static void Wiz_StartFast(edict_t *self) {
    eng->SV_StartSound(self, CHAN_WEAPON, "wizard/wattack.wav", 1, ATTN_NORM);
    eng->MakeVectors(self->v.angles);

    edict_t *m1 = eng->ED_Alloc();
    m1->v.owner = self;
    vec3_t z = {0,0,0};
    eng->SV_SetSize(m1, z, z);
    vec3_t o1 = {
        self->v.origin[0] + 14*g->v_forward[0] + 14*g->v_right[0],
        self->v.origin[1] + 14*g->v_forward[1] + 14*g->v_right[1],
        self->v.origin[2] + 30 + 14*g->v_forward[2] + 14*g->v_right[2]
    };
    eng->SV_SetOrigin(m1, o1);
    m1->v.enemy    = self->v.enemy;
    m1->v.nextthink = g->time + 0.8f;
    m1->v.think    = Wiz_FastFire;
    m1->v.movedir[0] = g->v_right[0];
    m1->v.movedir[1] = g->v_right[1];
    m1->v.movedir[2] = g->v_right[2];

    edict_t *m2 = eng->ED_Alloc();
    m2->v.owner = self;
    eng->SV_SetSize(m2, z, z);
    vec3_t o2 = {
        self->v.origin[0] + 14*g->v_forward[0] - 14*g->v_right[0],
        self->v.origin[1] + 14*g->v_forward[1] - 14*g->v_right[1],
        self->v.origin[2] + 30 + 14*g->v_forward[2] - 14*g->v_right[2]
    };
    eng->SV_SetOrigin(m2, o2);
    m2->v.enemy    = self->v.enemy;
    m2->v.nextthink = g->time + 0.3f;
    m2->v.think    = Wiz_FastFire;
    m2->v.movedir[0] = -g->v_right[0];
    m2->v.movedir[1] = -g->v_right[1];
    m2->v.movedir[2] = -g->v_right[2];
}

// Attack
static void wz_fast2(edict_t *e),wz_fast3(edict_t *e),wz_fast4(edict_t *e),wz_fast5(edict_t *e),
            wz_fast6(edict_t *e),wz_fast7(edict_t *e),wz_fast8(edict_t *e),wz_fast9(edict_t *e),
            wz_fast10(edict_t *e);
static void wz_fast1(edict_t *e) {
    FRAME(e,WZ_MAGATT1,wz_fast2); ai_face(); Wiz_StartFast(e);
}
static void wz_fast2(edict_t *e)  { FRAME(e,WZ_MAGATT2, wz_fast3);  ai_face(); }
static void wz_fast3(edict_t *e)  { FRAME(e,WZ_MAGATT3, wz_fast4);  ai_face(); }
static void wz_fast4(edict_t *e)  { FRAME(e,WZ_MAGATT4, wz_fast5);  ai_face(); }
static void wz_fast5(edict_t *e)  { FRAME(e,WZ_MAGATT5, wz_fast6);  ai_face(); }
static void wz_fast6(edict_t *e)  { FRAME(e,WZ_MAGATT6, wz_fast7);  ai_face(); }
static void wz_fast7(edict_t *e)  { FRAME(e,WZ_MAGATT5, wz_fast8);  ai_face(); } // magatt5 again
static void wz_fast8(edict_t *e)  { FRAME(e,WZ_MAGATT4, wz_fast9);  ai_face(); }
static void wz_fast9(edict_t *e)  { FRAME(e,WZ_MAGATT3, wz_fast10); ai_face(); }
static void wz_fast10(edict_t *e) {
    FRAME(e,WZ_MAGATT2,wz_run1); ai_face();
    SUB_AttackFinished(2);
    WizardAttackFinished();
}

// Pain
static void wz_pain2(edict_t *e),wz_pain3(edict_t *e),wz_pain4(edict_t *e);
static void wz_pain1(edict_t *e) { FRAME(e,WZ_PAIN1,wz_pain2); }
static void wz_pain2(edict_t *e) { FRAME(e,WZ_PAIN2,wz_pain3); }
static void wz_pain3(edict_t *e) { FRAME(e,WZ_PAIN3,wz_pain4); }
static void wz_pain4(edict_t *e) { FRAME(e,WZ_PAIN4,wz_run1);  }

static void wiz_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker;
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, "wizard/wpain.wav", 1, ATTN_NORM);
    if (eng->Random() * 70 > damage) return;
    wz_pain1(self);
}

// Death
static void wz_death2(edict_t *e),wz_death3(edict_t *e),wz_death4(edict_t *e),
            wz_death5(edict_t *e),wz_death6(edict_t *e),wz_death7(edict_t *e),
            wz_death8(edict_t *e);
static void wz_death1(edict_t *e) {
    FRAME(e,WZ_DEATH1,wz_death2);
    e->v.velocity[0] = -200 + 400*eng->Random();
    e->v.velocity[1] = -200 + 400*eng->Random();
    e->v.velocity[2] =  100 + 100*eng->Random();
    e->v.flags = (float)((int)e->v.flags & ~FL_ONGROUND);
    eng->SV_StartSound(e, CHAN_VOICE, "wizard/wdeath.wav", 1, ATTN_NORM);
}
static void wz_death2(edict_t *e) { FRAME(e,WZ_DEATH2,wz_death3); }
static void wz_death3(edict_t *e) { FRAME(e,WZ_DEATH3,wz_death4); Corpse_LayProne(e); }
static void wz_death4(edict_t *e) { FRAME(e,WZ_DEATH4,wz_death5); }
static void wz_death5(edict_t *e) { FRAME(e,WZ_DEATH5,wz_death6); }
static void wz_death6(edict_t *e) { FRAME(e,WZ_DEATH6,wz_death7); }
static void wz_death7(edict_t *e) { FRAME(e,WZ_DEATH7,wz_death8); }
static void wz_death8(edict_t *e) { FRAME(e,WZ_DEATH8,wz_death8); }

static void wiz_die(edict_t *self) {
    g->self = self;
    if (self->v.health < -40) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_wizard.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        return;
    }
    wz_death1(self);
}

static void Wiz_Missile(edict_t *e) { g->self = e; wz_fast1(e); }

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_wizard(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/wizard.mdl");
    eng->PrecacheModel("progs/h_wizard.mdl");
    eng->PrecacheModel("progs/w_spike.mdl");
    eng->PrecacheSound("wizard/hit.wav");
    eng->PrecacheSound("wizard/wattack.wav");
    eng->PrecacheSound("wizard/wdeath.wav");
    eng->PrecacheSound("wizard/widle1.wav");
    eng->PrecacheSound("wizard/widle2.wav");
    eng->PrecacheSound("wizard/wpain.wav");
    eng->PrecacheSound("wizard/wsight.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/wizard.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health    = 80;
    e->v.th_stand  = wz_stand1;
    e->v.th_walk   = wz_walk1;
    e->v.th_run    = wz_run1;
    e->v.th_missile = Wiz_Missile;
    e->v.th_pain   = wiz_pain;
    e->v.th_die    = wiz_die;
    flymonster_start(e);
}

// monster_dog.c -- Rottweiler (dog.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>
#include <stddef.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_face(void);
extern void ai_charge(float dist);
extern void ai_pain(float dist);
extern float CanDamage(edict_t *targ, edict_t *inflictor);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *gibname, float dm);
extern void ThrowGib(const char *gibname, float dm);
extern void walkmonster_start(edict_t *self);

// Frame indices
// attack1-8: 0-7
// death1-9: 8-16
// deathb1-9: 17-25
// pain1-6: 26-31
// painb1-16: 32-47
// run1-12: 48-59
// leap1-9: 60-68
// stand1-9: 69-77
// walk1-8: 78-85
enum {
    DOG_ATTACK1=0,DOG_ATTACK2,DOG_ATTACK3,DOG_ATTACK4,DOG_ATTACK5,DOG_ATTACK6,DOG_ATTACK7,DOG_ATTACK8,
    DOG_DEATH1,DOG_DEATH2,DOG_DEATH3,DOG_DEATH4,DOG_DEATH5,DOG_DEATH6,DOG_DEATH7,DOG_DEATH8,DOG_DEATH9,
    DOG_DEATHB1,DOG_DEATHB2,DOG_DEATHB3,DOG_DEATHB4,DOG_DEATHB5,DOG_DEATHB6,DOG_DEATHB7,DOG_DEATHB8,DOG_DEATHB9,
    DOG_PAIN1,DOG_PAIN2,DOG_PAIN3,DOG_PAIN4,DOG_PAIN5,DOG_PAIN6,
    DOG_PAINB1,DOG_PAINB2,DOG_PAINB3,DOG_PAINB4,DOG_PAINB5,DOG_PAINB6,DOG_PAINB7,DOG_PAINB8,
    DOG_PAINB9,DOG_PAINB10,DOG_PAINB11,DOG_PAINB12,DOG_PAINB13,DOG_PAINB14,DOG_PAINB15,DOG_PAINB16,
    DOG_RUN1,DOG_RUN2,DOG_RUN3,DOG_RUN4,DOG_RUN5,DOG_RUN6,DOG_RUN7,DOG_RUN8,DOG_RUN9,DOG_RUN10,DOG_RUN11,DOG_RUN12,
    DOG_LEAP1,DOG_LEAP2,DOG_LEAP3,DOG_LEAP4,DOG_LEAP5,DOG_LEAP6,DOG_LEAP7,DOG_LEAP8,DOG_LEAP9,
    DOG_STAND1,DOG_STAND2,DOG_STAND3,DOG_STAND4,DOG_STAND5,DOG_STAND6,DOG_STAND7,DOG_STAND8,DOG_STAND9,
    DOG_WALK1,DOG_WALK2,DOG_WALK3,DOG_WALK4,DOG_WALK5,DOG_WALK6,DOG_WALK7,DOG_WALK8
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

static void dog_stand1(edict_t *e),dog_stand2(edict_t *e),dog_stand3(edict_t *e),dog_stand4(edict_t *e),dog_stand5(edict_t *e),dog_stand6(edict_t *e),dog_stand7(edict_t *e),dog_stand8(edict_t *e),dog_stand9(edict_t *e);
static void dog_walk1(edict_t *e),dog_walk2(edict_t *e),dog_walk3(edict_t *e),dog_walk4(edict_t *e),dog_walk5(edict_t *e),dog_walk6(edict_t *e),dog_walk7(edict_t *e),dog_walk8(edict_t *e);
static void dog_run1(edict_t *e),dog_run2(edict_t *e),dog_run3(edict_t *e),dog_run4(edict_t *e),dog_run5(edict_t *e),dog_run6(edict_t *e),dog_run7(edict_t *e),dog_run8(edict_t *e),dog_run9(edict_t *e),dog_run10(edict_t *e),dog_run11(edict_t *e),dog_run12(edict_t *e);
static void dog_atta1(edict_t *e),dog_atta2(edict_t *e),dog_atta3(edict_t *e),dog_atta4(edict_t *e),dog_atta5(edict_t *e),dog_atta6(edict_t *e),dog_atta7(edict_t *e),dog_atta8(edict_t *e);
static void dog_leap1(edict_t *e),dog_leap2(edict_t *e),dog_leap3(edict_t *e),dog_leap4(edict_t *e),dog_leap5(edict_t *e),dog_leap6(edict_t *e),dog_leap7(edict_t *e),dog_leap8(edict_t *e),dog_leap9(edict_t *e);
static void dog_pain1(edict_t *e),dog_pain2(edict_t *e),dog_pain3(edict_t *e),dog_pain4(edict_t *e),dog_pain5(edict_t *e),dog_pain6(edict_t *e);
static void dog_painb1(edict_t *e),dog_painb2(edict_t *e),dog_painb3(edict_t *e),dog_painb4(edict_t *e),dog_painb5(edict_t *e),dog_painb6(edict_t *e),dog_painb7(edict_t *e),dog_painb8(edict_t *e),dog_painb9(edict_t *e),dog_painb10(edict_t *e),dog_painb11(edict_t *e),dog_painb12(edict_t *e),dog_painb13(edict_t *e),dog_painb14(edict_t *e),dog_painb15(edict_t *e),dog_painb16(edict_t *e);
static void dog_die1(edict_t *e),dog_die2(edict_t *e),dog_die3(edict_t *e),dog_die4(edict_t *e),dog_die5(edict_t *e),dog_die6(edict_t *e),dog_die7(edict_t *e),dog_die8(edict_t *e),dog_die9(edict_t *e);
static void dog_dieb1(edict_t *e),dog_dieb2(edict_t *e),dog_dieb3(edict_t *e),dog_dieb4(edict_t *e),dog_dieb5(edict_t *e),dog_dieb6(edict_t *e),dog_dieb7(edict_t *e),dog_dieb8(edict_t *e),dog_dieb9(edict_t *e);
static void Dog_JumpTouch(edict_t *self, edict_t *other);

// Stand
static void dog_stand1(edict_t *e) { FRAME(e,DOG_STAND1,dog_stand2); ai_stand(e); }
static void dog_stand2(edict_t *e) { FRAME(e,DOG_STAND2,dog_stand3); ai_stand(e); }
static void dog_stand3(edict_t *e) { FRAME(e,DOG_STAND3,dog_stand4); ai_stand(e); }
static void dog_stand4(edict_t *e) { FRAME(e,DOG_STAND4,dog_stand5); ai_stand(e); }
static void dog_stand5(edict_t *e) { FRAME(e,DOG_STAND5,dog_stand6); ai_stand(e); }
static void dog_stand6(edict_t *e) { FRAME(e,DOG_STAND6,dog_stand7); ai_stand(e); }
static void dog_stand7(edict_t *e) { FRAME(e,DOG_STAND7,dog_stand8); ai_stand(e); }
static void dog_stand8(edict_t *e) { FRAME(e,DOG_STAND8,dog_stand9); ai_stand(e); }
static void dog_stand9(edict_t *e) { FRAME(e,DOG_STAND9,dog_stand1); ai_stand(e); }

// Walk
static void dog_walk1(edict_t *e) { FRAME(e,DOG_WALK1,dog_walk2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e,CHAN_VOICE,"dog/idle.wav",1,ATTN_IDLE);
    ai_walk(8); }
static void dog_walk2(edict_t *e) { FRAME(e,DOG_WALK2,dog_walk3); ai_walk(8); }
static void dog_walk3(edict_t *e) { FRAME(e,DOG_WALK3,dog_walk4); ai_walk(8); }
static void dog_walk4(edict_t *e) { FRAME(e,DOG_WALK4,dog_walk5); ai_walk(8); }
static void dog_walk5(edict_t *e) { FRAME(e,DOG_WALK5,dog_walk6); ai_walk(8); }
static void dog_walk6(edict_t *e) { FRAME(e,DOG_WALK6,dog_walk7); ai_walk(8); }
static void dog_walk7(edict_t *e) { FRAME(e,DOG_WALK7,dog_walk8); ai_walk(8); }
static void dog_walk8(edict_t *e) { FRAME(e,DOG_WALK8,dog_walk1); ai_walk(8); }

// Run
static void dog_run1(edict_t *e)  { FRAME(e,DOG_RUN1,dog_run2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e,CHAN_VOICE,"dog/idle.wav",1,ATTN_IDLE);
    ai_run(16); }
static void dog_run2(edict_t *e)  { FRAME(e,DOG_RUN2,dog_run3);  ai_run(32); }
static void dog_run3(edict_t *e)  { FRAME(e,DOG_RUN3,dog_run4);  ai_run(32); }
static void dog_run4(edict_t *e)  { FRAME(e,DOG_RUN4,dog_run5);  ai_run(20); }
static void dog_run5(edict_t *e)  { FRAME(e,DOG_RUN5,dog_run6);  ai_run(64); }
static void dog_run6(edict_t *e)  { FRAME(e,DOG_RUN6,dog_run7);  ai_run(32); }
static void dog_run7(edict_t *e)  { FRAME(e,DOG_RUN7,dog_run8);  ai_run(16); }
static void dog_run8(edict_t *e)  { FRAME(e,DOG_RUN8,dog_run9);  ai_run(32); }
static void dog_run9(edict_t *e)  { FRAME(e,DOG_RUN9,dog_run10); ai_run(32); }
static void dog_run10(edict_t *e) { FRAME(e,DOG_RUN10,dog_run11); ai_run(20); }
static void dog_run11(edict_t *e) { FRAME(e,DOG_RUN11,dog_run12); ai_run(64); }
static void dog_run12(edict_t *e) { FRAME(e,DOG_RUN12,dog_run1);  ai_run(32); }

// Bite
static void dog_bite(edict_t *e) {
    if (!e->v.enemy) return;
    ai_charge(10);
    if (!CanDamage(e->v.enemy, e)) return;
    float dx=e->v.enemy->v.origin[0]-e->v.origin[0];
    float dy=e->v.enemy->v.origin[1]-e->v.origin[1];
    float dz=e->v.enemy->v.origin[2]-e->v.origin[2];
    if (dx*dx+dy*dy+dz*dz > 100.0f*100.0f) return;
    float ldmg = (eng->Random()+eng->Random()+eng->Random())*8;
    T_Damage(e->v.enemy, e, e, ldmg);
}

// Attack
static void dog_atta1(edict_t *e) { FRAME(e,DOG_ATTACK1,dog_atta2); ai_charge(10); }
static void dog_atta2(edict_t *e) { FRAME(e,DOG_ATTACK2,dog_atta3); ai_charge(10); }
static void dog_atta3(edict_t *e) { FRAME(e,DOG_ATTACK3,dog_atta4); ai_charge(10); }
static void dog_atta4(edict_t *e) { FRAME(e,DOG_ATTACK4,dog_atta5);
    eng->SV_StartSound(e,CHAN_VOICE,"dog/dattack1.wav",1,ATTN_NORM);
    dog_bite(e); }
static void dog_atta5(edict_t *e) { FRAME(e,DOG_ATTACK5,dog_atta6); ai_charge(10); }
static void dog_atta6(edict_t *e) { FRAME(e,DOG_ATTACK6,dog_atta7); ai_charge(10); }
static void dog_atta7(edict_t *e) { FRAME(e,DOG_ATTACK7,dog_atta8); ai_charge(10); }
static void dog_atta8(edict_t *e) { FRAME(e,DOG_ATTACK8,dog_run1);  ai_charge(10); }

// Leap
static void Dog_JumpTouch(edict_t *self, edict_t *other) {
    g->self = self;
    if (self->v.health <= 0) return;
    if ((int)other->v.takedamage) {
        float *vel = self->v.velocity;
        float spd = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
        if (spd > 300) {
            float ldmg = 10 + 10*eng->Random();
            T_Damage(other, self, self, ldmg);
        }
    }
    if (!eng->SV_CheckBottom(self)) {
        if ((int)self->v.flags & FL_ONGROUND) {
            self->v.touch = NULL;
            self->v.think = dog_leap1;
            self->v.nextthink = g->time + 0.1f;
        }
        return;
    }
    self->v.touch = NULL;
    self->v.think = dog_run1;
    self->v.nextthink = g->time + 0.1f;
}

static void dog_leap1(edict_t *e) { FRAME(e,DOG_LEAP1,dog_leap2); ai_face(); }
static void dog_leap2(edict_t *e) {
    FRAME(e,DOG_LEAP2,dog_leap3);
    ai_face();
    e->v.touch = Dog_JumpTouch;
    eng->MakeVectors(e->v.angles);
    e->v.origin[2] += 1;
    float *vf = g->v_forward;
    e->v.velocity[0] = vf[0]*300;
    e->v.velocity[1] = vf[1]*300;
    e->v.velocity[2] = vf[2]*300 + 200;
    if ((int)e->v.flags & FL_ONGROUND)
        e->v.flags = (float)((int)e->v.flags - FL_ONGROUND);
}
static void dog_leap3(edict_t *e) { FRAME(e,DOG_LEAP3,dog_leap4); }
static void dog_leap4(edict_t *e) { FRAME(e,DOG_LEAP4,dog_leap5); }
static void dog_leap5(edict_t *e) { FRAME(e,DOG_LEAP5,dog_leap6); }
static void dog_leap6(edict_t *e) { FRAME(e,DOG_LEAP6,dog_leap7); }
static void dog_leap7(edict_t *e) { FRAME(e,DOG_LEAP7,dog_leap8); }
static void dog_leap8(edict_t *e) { FRAME(e,DOG_LEAP8,dog_leap9); }
static void dog_leap9(edict_t *e) { FRAME(e,DOG_LEAP9,dog_leap9); }

// Pain
static void dog_pain1(edict_t *e) { FRAME(e,DOG_PAIN1,dog_pain2); }
static void dog_pain2(edict_t *e) { FRAME(e,DOG_PAIN2,dog_pain3); }
static void dog_pain3(edict_t *e) { FRAME(e,DOG_PAIN3,dog_pain4); }
static void dog_pain4(edict_t *e) { FRAME(e,DOG_PAIN4,dog_pain5); }
static void dog_pain5(edict_t *e) { FRAME(e,DOG_PAIN5,dog_pain6); }
static void dog_pain6(edict_t *e) { FRAME(e,DOG_PAIN6,dog_run1); }

static void dog_painb1(edict_t *e)  { FRAME(e,DOG_PAINB1,dog_painb2); }
static void dog_painb2(edict_t *e)  { FRAME(e,DOG_PAINB2,dog_painb3); }
static void dog_painb3(edict_t *e)  { FRAME(e,DOG_PAINB3,dog_painb4); ai_pain(4); }
static void dog_painb4(edict_t *e)  { FRAME(e,DOG_PAINB4,dog_painb5); ai_pain(12); }
static void dog_painb5(edict_t *e)  { FRAME(e,DOG_PAINB5,dog_painb6); ai_pain(12); }
static void dog_painb6(edict_t *e)  { FRAME(e,DOG_PAINB6,dog_painb7); ai_pain(2); }
static void dog_painb7(edict_t *e)  { FRAME(e,DOG_PAINB7,dog_painb8); }
static void dog_painb8(edict_t *e)  { FRAME(e,DOG_PAINB8,dog_painb9); ai_pain(4); }
static void dog_painb9(edict_t *e)  { FRAME(e,DOG_PAINB9,dog_painb10); }
static void dog_painb10(edict_t *e) { FRAME(e,DOG_PAINB10,dog_painb11); ai_pain(10); }
static void dog_painb11(edict_t *e) { FRAME(e,DOG_PAINB11,dog_painb12); }
static void dog_painb12(edict_t *e) { FRAME(e,DOG_PAINB12,dog_painb13); }
static void dog_painb13(edict_t *e) { FRAME(e,DOG_PAINB13,dog_painb14); }
static void dog_painb14(edict_t *e) { FRAME(e,DOG_PAINB14,dog_painb15); }
static void dog_painb15(edict_t *e) { FRAME(e,DOG_PAINB15,dog_painb16); }
static void dog_painb16(edict_t *e) { FRAME(e,DOG_PAINB16,dog_run1); }

static void dog_pain_cb(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, "dog/dpain1.wav", 1, ATTN_NORM);
    if (eng->Random() > 0.5f)
        dog_pain1(self);
    else
        dog_painb1(self);
}

// Death
static void dog_die1(edict_t *e) { FRAME(e,DOG_DEATH1,dog_die2); }
static void dog_die2(edict_t *e) { FRAME(e,DOG_DEATH2,dog_die3); }
static void dog_die3(edict_t *e) { FRAME(e,DOG_DEATH3,dog_die4); }
static void dog_die4(edict_t *e) { FRAME(e,DOG_DEATH4,dog_die5); }
static void dog_die5(edict_t *e) { FRAME(e,DOG_DEATH5,dog_die6); }
static void dog_die6(edict_t *e) { FRAME(e,DOG_DEATH6,dog_die7); }
static void dog_die7(edict_t *e) { FRAME(e,DOG_DEATH7,dog_die8); }
static void dog_die8(edict_t *e) { FRAME(e,DOG_DEATH8,dog_die9); }
static void dog_die9(edict_t *e) { FRAME(e,DOG_DEATH9,dog_die9); }

static void dog_dieb1(edict_t *e) { FRAME(e,DOG_DEATHB1,dog_dieb2); }
static void dog_dieb2(edict_t *e) { FRAME(e,DOG_DEATHB2,dog_dieb3); }
static void dog_dieb3(edict_t *e) { FRAME(e,DOG_DEATHB3,dog_dieb4); }
static void dog_dieb4(edict_t *e) { FRAME(e,DOG_DEATHB4,dog_dieb5); }
static void dog_dieb5(edict_t *e) { FRAME(e,DOG_DEATHB5,dog_dieb6); }
static void dog_dieb6(edict_t *e) { FRAME(e,DOG_DEATHB6,dog_dieb7); }
static void dog_dieb7(edict_t *e) { FRAME(e,DOG_DEATHB7,dog_dieb8); }
static void dog_dieb8(edict_t *e) { FRAME(e,DOG_DEATHB8,dog_dieb9); }
static void dog_dieb9(edict_t *e) { FRAME(e,DOG_DEATHB9,dog_dieb9); }

static void dog_die_cb(edict_t *self) {
    g->self = self;
    if (self->v.health < -35) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowGib("progs/gib3.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        ThrowHead("progs/h_dog.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "dog/ddeath.wav", 1, ATTN_NORM);
    self->v.solid = SOLID_NOT;
    if (eng->Random() > 0.5f)
        dog_die1(self);
    else
        dog_dieb1(self);
}

void spawn_monster_dog(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/h_dog.mdl");
    eng->PrecacheModel("progs/dog.mdl");
    eng->PrecacheSound("dog/dattack1.wav");
    eng->PrecacheSound("dog/ddeath.wav");
    eng->PrecacheSound("dog/dpain1.wav");
    eng->PrecacheSound("dog/dsight.wav");
    eng->PrecacheSound("dog/idle.wav");
    eng->PrecacheSound("player/udeath.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/dog.mdl");
    vec3_t mins = {-32,-32,-24}, maxs = {32,32,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health     = 25;
    e->v.th_stand   = dog_stand1;
    e->v.th_walk    = dog_walk1;
    e->v.th_run     = dog_run1;
    e->v.th_pain    = dog_pain_cb;
    e->v.th_die     = dog_die_cb;
    e->v.th_melee   = dog_atta1;
    e->v.th_missile = dog_leap1;
    walkmonster_start(e);
}

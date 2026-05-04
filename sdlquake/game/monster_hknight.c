// monster_hknight.c -- Hell Knight (hknight.qc port).

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
extern void ai_charge(float dist);
extern void ai_melee(void);
extern void ai_forward(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void walkmonster_start(edict_t *self);
extern void SUB_AttackFinished(float t);
extern void launch_spike(vec3_t org, vec3_t dir);

// fight.c
extern int enemy_vis;
extern float attack_finished;  // not available -- use e->v.attack_finished

// Frame indices
// stand1-9: 0-8
// walk1-20: 9-28
// run1-8: 29-36
// pain1-5: 37-41
// death1-12: 42-53
// deathb1-9: 54-62
// char_a1-16: 63-78
// magica1-14: 79-92
// magicb1-13: 93-105
// char_b1-6: 106-111
// slice1-10: 112-121
// smash1-11: 122-132
// w_attack1-22: 133-154
// magicc1-11: 155-165
enum {
    HK_STAND1=0,HK_STAND2,HK_STAND3,HK_STAND4,HK_STAND5,
    HK_STAND6,HK_STAND7,HK_STAND8,HK_STAND9,
    HK_WALK1,HK_WALK2,HK_WALK3,HK_WALK4,HK_WALK5,HK_WALK6,HK_WALK7,
    HK_WALK8,HK_WALK9,HK_WALK10,HK_WALK11,HK_WALK12,HK_WALK13,HK_WALK14,
    HK_WALK15,HK_WALK16,HK_WALK17,HK_WALK18,HK_WALK19,HK_WALK20,
    HK_RUN1,HK_RUN2,HK_RUN3,HK_RUN4,HK_RUN5,HK_RUN6,HK_RUN7,HK_RUN8,
    HK_PAIN1,HK_PAIN2,HK_PAIN3,HK_PAIN4,HK_PAIN5,
    HK_DEATH1,HK_DEATH2,HK_DEATH3,HK_DEATH4,HK_DEATH5,HK_DEATH6,
    HK_DEATH7,HK_DEATH8,HK_DEATH9,HK_DEATH10,HK_DEATH11,HK_DEATH12,
    HK_DEATHB1,HK_DEATHB2,HK_DEATHB3,HK_DEATHB4,HK_DEATHB5,
    HK_DEATHB6,HK_DEATHB7,HK_DEATHB8,HK_DEATHB9,
    HK_CHARA1,HK_CHARA2,HK_CHARA3,HK_CHARA4,HK_CHARA5,HK_CHARA6,
    HK_CHARA7,HK_CHARA8,HK_CHARA9,HK_CHARA10,HK_CHARA11,HK_CHARA12,
    HK_CHARA13,HK_CHARA14,HK_CHARA15,HK_CHARA16,
    HK_MAGICA1,HK_MAGICA2,HK_MAGICA3,HK_MAGICA4,HK_MAGICA5,HK_MAGICA6,
    HK_MAGICA7,HK_MAGICA8,HK_MAGICA9,HK_MAGICA10,HK_MAGICA11,HK_MAGICA12,
    HK_MAGICA13,HK_MAGICA14,
    HK_MAGICB1,HK_MAGICB2,HK_MAGICB3,HK_MAGICB4,HK_MAGICB5,HK_MAGICB6,
    HK_MAGICB7,HK_MAGICB8,HK_MAGICB9,HK_MAGICB10,HK_MAGICB11,HK_MAGICB12,HK_MAGICB13,
    HK_CHARB1,HK_CHARB2,HK_CHARB3,HK_CHARB4,HK_CHARB5,HK_CHARB6,
    HK_SLICE1,HK_SLICE2,HK_SLICE3,HK_SLICE4,HK_SLICE5,
    HK_SLICE6,HK_SLICE7,HK_SLICE8,HK_SLICE9,HK_SLICE10,
    HK_SMASH1,HK_SMASH2,HK_SMASH3,HK_SMASH4,HK_SMASH5,HK_SMASH6,
    HK_SMASH7,HK_SMASH8,HK_SMASH9,HK_SMASH10,HK_SMASH11,
    HK_WATK1,HK_WATK2,HK_WATK3,HK_WATK4,HK_WATK5,HK_WATK6,HK_WATK7,
    HK_WATK8,HK_WATK9,HK_WATK10,HK_WATK11,HK_WATK12,HK_WATK13,HK_WATK14,
    HK_WATK15,HK_WATK16,HK_WATK17,HK_WATK18,HK_WATK19,HK_WATK20,
    HK_WATK21,HK_WATK22,
    HK_MAGICC1,HK_MAGICC2,HK_MAGICC3,HK_MAGICC4,HK_MAGICC5,HK_MAGICC6,
    HK_MAGICC7,HK_MAGICC8,HK_MAGICC9,HK_MAGICC10,HK_MAGICC11
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

static int hknight_type = 0;

// Forward declarations
static void hk_run1(edict_t *e);
static void hk_char_a1(edict_t *e),hk_char_b1(edict_t *e);
static void hk_slice1(edict_t *e),hk_smash1(edict_t *e),hk_watk1(edict_t *e);
static void hk_pain1(edict_t *e);

static void hk_idle_sound(edict_t *e) {
    if (eng->Random() < 0.2f)
        eng->SV_StartSound(e, CHAN_VOICE, "hknight/idle.wav", 1, ATTN_NORM);
}

static void hknight_shot(edict_t *e, float offset) {
    g->self = e;
    // offang = vectoangles(enemy.origin - origin), then offset y by offset*6
    float dx = e->v.enemy->v.origin[0]-e->v.origin[0];
    float dy = e->v.enemy->v.origin[1]-e->v.origin[1];
    float dz = e->v.enemy->v.origin[2]-e->v.origin[2];
    float yaw = eng->VectorToYaw((vec3_t){dx,dy,dz}); // approximate
    float offang_y = yaw + offset * 6;
    vec3_t offang = {0, offang_y, 0};
    eng->MakeVectors(offang);
    // org = origin + mins + size*0.5 + v_forward*20
    float sx = e->v.maxs[0]-e->v.mins[0], sy = e->v.maxs[1]-e->v.mins[1], sz = e->v.maxs[2]-e->v.mins[2];
    vec3_t org = {
        e->v.origin[0] + e->v.mins[0] + sx*0.5f + g->v_forward[0]*20,
        e->v.origin[1] + e->v.mins[1] + sy*0.5f + g->v_forward[1]*20,
        e->v.origin[2] + e->v.mins[2] + sz*0.5f + g->v_forward[2]*20
    };
    float fx = g->v_forward[0], fy = g->v_forward[1], fz = g->v_forward[2];
    float flen = (float)sqrt((double)(fx*fx+fy*fy+fz*fz));
    if (flen < 0.001f) flen = 0.001f;
    vec3_t vec = {fx/flen, fy/flen, -fz/flen + (eng->Random()-0.5f)*0.1f};
    float vlen = (float)sqrt((double)(vec[0]*vec[0]+vec[1]*vec[1]+vec[2]*vec[2]));
    if (vlen < 0.001f) vlen = 0.001f;
    vec[0]/=vlen; vec[1]/=vlen; vec[2]/=vlen;
    launch_spike(org, vec);
    g->newmis->v.velocity[0] = vec[0]*300;
    g->newmis->v.velocity[1] = vec[1]*300;
    g->newmis->v.velocity[2] = vec[2]*300;
    eng->SV_SetModel(g->newmis, "progs/k_spike.mdl");
    vec3_t z = {0,0,0};
    eng->SV_SetSize(g->newmis, z, z);
    eng->SV_StartSound(e, CHAN_WEAPON, "hknight/attack1.wav", 1, ATTN_NORM);
}

static void CheckForCharge(edict_t *e) {
    if (!e->v.enemy) return;
    // Use a simple approximation of enemy_vis (assume visible if we have an enemy)
    if (g->time < e->v.attack_finished) return;
    float dz = e->v.origin[2] - e->v.enemy->v.origin[2];
    if (dz > 20 || dz < -20) return;
    float dx = e->v.origin[0]-e->v.enemy->v.origin[0];
    float dy = e->v.origin[1]-e->v.enemy->v.origin[1];
    float d2 = dx*dx+dy*dy;
    if (d2 < 80*80) return;
    SUB_AttackFinished(2);
    hk_char_a1(e);
}

static void CheckContinueCharge(edict_t *e) {
    if (g->time > e->v.attack_finished) {
        SUB_AttackFinished(3);
        hk_run1(e);
        return;
    }
    if (eng->Random() > 0.5f)
        eng->SV_StartSound(e, CHAN_WEAPON, "knight/sword2.wav", 1, ATTN_NORM);
    else
        eng->SV_StartSound(e, CHAN_WEAPON, "knight/sword1.wav", 1, ATTN_NORM);
}

// Stand
static void hk_stand2(edict_t *e),hk_stand3(edict_t *e),hk_stand4(edict_t *e),
            hk_stand5(edict_t *e),hk_stand6(edict_t *e),hk_stand7(edict_t *e),
            hk_stand8(edict_t *e),hk_stand9(edict_t *e);
static void hk_stand1(edict_t *e) { FRAME(e,HK_STAND1,hk_stand2); ai_stand(e); }
static void hk_stand2(edict_t *e) { FRAME(e,HK_STAND2,hk_stand3); ai_stand(e); }
static void hk_stand3(edict_t *e) { FRAME(e,HK_STAND3,hk_stand4); ai_stand(e); }
static void hk_stand4(edict_t *e) { FRAME(e,HK_STAND4,hk_stand5); ai_stand(e); }
static void hk_stand5(edict_t *e) { FRAME(e,HK_STAND5,hk_stand6); ai_stand(e); }
static void hk_stand6(edict_t *e) { FRAME(e,HK_STAND6,hk_stand7); ai_stand(e); }
static void hk_stand7(edict_t *e) { FRAME(e,HK_STAND7,hk_stand8); ai_stand(e); }
static void hk_stand8(edict_t *e) { FRAME(e,HK_STAND8,hk_stand9); ai_stand(e); }
static void hk_stand9(edict_t *e) { FRAME(e,HK_STAND9,hk_stand1); ai_stand(e); }

// Walk
static void hk_walk2(edict_t *e),hk_walk3(edict_t *e),hk_walk4(edict_t *e),hk_walk5(edict_t *e),
            hk_walk6(edict_t *e),hk_walk7(edict_t *e),hk_walk8(edict_t *e),hk_walk9(edict_t *e),
            hk_walk10(edict_t *e),hk_walk11(edict_t *e),hk_walk12(edict_t *e),hk_walk13(edict_t *e),
            hk_walk14(edict_t *e),hk_walk15(edict_t *e),hk_walk16(edict_t *e),hk_walk17(edict_t *e),
            hk_walk18(edict_t *e),hk_walk19(edict_t *e),hk_walk20(edict_t *e);
static void hk_walk1(edict_t *e) { FRAME(e,HK_WALK1,hk_walk2); hk_idle_sound(e); ai_walk(2); }
static void hk_walk2(edict_t *e)  { FRAME(e,HK_WALK2, hk_walk3);  ai_walk(5); }
static void hk_walk3(edict_t *e)  { FRAME(e,HK_WALK3, hk_walk4);  ai_walk(5); }
static void hk_walk4(edict_t *e)  { FRAME(e,HK_WALK4, hk_walk5);  ai_walk(4); }
static void hk_walk5(edict_t *e)  { FRAME(e,HK_WALK5, hk_walk6);  ai_walk(4); }
static void hk_walk6(edict_t *e)  { FRAME(e,HK_WALK6, hk_walk7);  ai_walk(2); }
static void hk_walk7(edict_t *e)  { FRAME(e,HK_WALK7, hk_walk8);  ai_walk(2); }
static void hk_walk8(edict_t *e)  { FRAME(e,HK_WALK8, hk_walk9);  ai_walk(3); }
static void hk_walk9(edict_t *e)  { FRAME(e,HK_WALK9, hk_walk10); ai_walk(3); }
static void hk_walk10(edict_t *e) { FRAME(e,HK_WALK10,hk_walk11); ai_walk(4); }
static void hk_walk11(edict_t *e) { FRAME(e,HK_WALK11,hk_walk12); ai_walk(3); }
static void hk_walk12(edict_t *e) { FRAME(e,HK_WALK12,hk_walk13); ai_walk(4); }
static void hk_walk13(edict_t *e) { FRAME(e,HK_WALK13,hk_walk14); ai_walk(6); }
static void hk_walk14(edict_t *e) { FRAME(e,HK_WALK14,hk_walk15); ai_walk(2); }
static void hk_walk15(edict_t *e) { FRAME(e,HK_WALK15,hk_walk16); ai_walk(2); }
static void hk_walk16(edict_t *e) { FRAME(e,HK_WALK16,hk_walk17); ai_walk(4); }
static void hk_walk17(edict_t *e) { FRAME(e,HK_WALK17,hk_walk18); ai_walk(3); }
static void hk_walk18(edict_t *e) { FRAME(e,HK_WALK18,hk_walk19); ai_walk(3); }
static void hk_walk19(edict_t *e) { FRAME(e,HK_WALK19,hk_walk20); ai_walk(3); }
static void hk_walk20(edict_t *e) { FRAME(e,HK_WALK20,hk_walk1);  ai_walk(2); }

// Run
static void hk_run2(edict_t *e),hk_run3(edict_t *e),hk_run4(edict_t *e),hk_run5(edict_t *e),
            hk_run6(edict_t *e),hk_run7(edict_t *e),hk_run8(edict_t *e);
static void hk_run1(edict_t *e) {
    FRAME(e,HK_RUN1,hk_run2); hk_idle_sound(e); ai_run(20); CheckForCharge(e);
}
static void hk_run2(edict_t *e) { FRAME(e,HK_RUN2,hk_run3); ai_run(25); }
static void hk_run3(edict_t *e) { FRAME(e,HK_RUN3,hk_run4); ai_run(18); }
static void hk_run4(edict_t *e) { FRAME(e,HK_RUN4,hk_run5); ai_run(16); }
static void hk_run5(edict_t *e) { FRAME(e,HK_RUN5,hk_run6); ai_run(14); }
static void hk_run6(edict_t *e) { FRAME(e,HK_RUN6,hk_run7); ai_run(25); }
static void hk_run7(edict_t *e) { FRAME(e,HK_RUN7,hk_run8); ai_run(21); }
static void hk_run8(edict_t *e) { FRAME(e,HK_RUN8,hk_run1); ai_run(13); }

// Pain
static void hk_pain2(edict_t *e),hk_pain3(edict_t *e),hk_pain4(edict_t *e),hk_pain5(edict_t *e);
static void hk_pain1(edict_t *e) {
    FRAME(e,HK_PAIN1,hk_pain2);
    eng->SV_StartSound(e, CHAN_VOICE, "hknight/pain1.wav", 1, ATTN_NORM);
}
static void hk_pain2(edict_t *e) { FRAME(e,HK_PAIN2,hk_pain3); }
static void hk_pain3(edict_t *e) { FRAME(e,HK_PAIN3,hk_pain4); }
static void hk_pain4(edict_t *e) { FRAME(e,HK_PAIN4,hk_pain5); }
static void hk_pain5(edict_t *e) { FRAME(e,HK_PAIN5,hk_run1);  }

static void hknight_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker;
    g->self = self;
    if (self->v.pain_finished > g->time) return;
    eng->SV_StartSound(self, CHAN_VOICE, "hknight/pain1.wav", 1, ATTN_NORM);
    if (g->time - self->v.pain_finished > 5) {
        hk_pain1(self);
        self->v.pain_finished = g->time + 1;
        return;
    }
    if (eng->Random() * 30 > damage) return;
    self->v.pain_finished = g->time + 1;
    hk_pain1(self);
}

// Death
static void hk_die2(edict_t *e),hk_die3(edict_t *e),hk_die4(edict_t *e),hk_die5(edict_t *e),
            hk_die6(edict_t *e),hk_die7(edict_t *e),hk_die8(edict_t *e),hk_die9(edict_t *e),
            hk_die10(edict_t *e),hk_die11(edict_t *e),hk_die12(edict_t *e);
static void hk_die1(edict_t *e) { FRAME(e,HK_DEATH1,hk_die2); ai_forward(10); }
static void hk_die2(edict_t *e) { FRAME(e,HK_DEATH2,hk_die3); ai_forward(8);  }
static void hk_die3(edict_t *e) { FRAME(e,HK_DEATH3,hk_die4); e->v.solid = SOLID_NOT; ai_forward(7); }
static void hk_die4(edict_t *e) { FRAME(e,HK_DEATH4,hk_die5); }
static void hk_die5(edict_t *e) { FRAME(e,HK_DEATH5,hk_die6); }
static void hk_die6(edict_t *e) { FRAME(e,HK_DEATH6,hk_die7); }
static void hk_die7(edict_t *e) { FRAME(e,HK_DEATH7,hk_die8); }
static void hk_die8(edict_t *e) { FRAME(e,HK_DEATH8,hk_die9);  ai_forward(10); }
static void hk_die9(edict_t *e) { FRAME(e,HK_DEATH9,hk_die10); ai_forward(11); }
static void hk_die10(edict_t *e){ FRAME(e,HK_DEATH10,hk_die11);}
static void hk_die11(edict_t *e){ FRAME(e,HK_DEATH11,hk_die12);}
static void hk_die12(edict_t *e){ FRAME(e,HK_DEATH12,hk_die12);}

static void hk_dieb2(edict_t *e),hk_dieb3(edict_t *e),hk_dieb4(edict_t *e),hk_dieb5(edict_t *e),
            hk_dieb6(edict_t *e),hk_dieb7(edict_t *e),hk_dieb8(edict_t *e),hk_dieb9(edict_t *e);
static void hk_dieb1(edict_t *e) { FRAME(e,HK_DEATHB1,hk_dieb2); }
static void hk_dieb2(edict_t *e) { FRAME(e,HK_DEATHB2,hk_dieb3); }
static void hk_dieb3(edict_t *e) { FRAME(e,HK_DEATHB3,hk_dieb4); e->v.solid = SOLID_NOT; }
static void hk_dieb4(edict_t *e) { FRAME(e,HK_DEATHB4,hk_dieb5); }
static void hk_dieb5(edict_t *e) { FRAME(e,HK_DEATHB5,hk_dieb6); }
static void hk_dieb6(edict_t *e) { FRAME(e,HK_DEATHB6,hk_dieb7); }
static void hk_dieb7(edict_t *e) { FRAME(e,HK_DEATHB7,hk_dieb8); }
static void hk_dieb8(edict_t *e) { FRAME(e,HK_DEATHB8,hk_dieb9); }
static void hk_dieb9(edict_t *e) { FRAME(e,HK_DEATHB9,hk_dieb9); }

static void hknight_die(edict_t *self) {
    g->self = self;
    if (self->v.health < -40) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_hellkn.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "hknight/death1.wav", 1, ATTN_NORM);
    if (eng->Random() > 0.5f) hk_die1(self);
    else hk_dieb1(self);
}

// Charge A
static void hk_char_a2(edict_t *e),hk_char_a3(edict_t *e),hk_char_a4(edict_t *e),
            hk_char_a5(edict_t *e),hk_char_a6(edict_t *e),hk_char_a7(edict_t *e),
            hk_char_a8(edict_t *e),hk_char_a9(edict_t *e),hk_char_a10(edict_t *e),
            hk_char_a11(edict_t *e),hk_char_a12(edict_t *e),hk_char_a13(edict_t *e),
            hk_char_a14(edict_t *e),hk_char_a15(edict_t *e),hk_char_a16(edict_t *e);
static void hk_char_a1(edict_t *e) { FRAME(e,HK_CHARA1, hk_char_a2);  ai_charge(20); }
static void hk_char_a2(edict_t *e)  { FRAME(e,HK_CHARA2, hk_char_a3);  ai_charge(25); }
static void hk_char_a3(edict_t *e)  { FRAME(e,HK_CHARA3, hk_char_a4);  ai_charge(18); }
static void hk_char_a4(edict_t *e)  { FRAME(e,HK_CHARA4, hk_char_a5);  ai_charge(16); }
static void hk_char_a5(edict_t *e)  { FRAME(e,HK_CHARA5, hk_char_a6);  ai_charge(14); }
static void hk_char_a6(edict_t *e)  { FRAME(e,HK_CHARA6, hk_char_a7);  ai_charge(20); ai_melee(); }
static void hk_char_a7(edict_t *e)  { FRAME(e,HK_CHARA7, hk_char_a8);  ai_charge(21); ai_melee(); }
static void hk_char_a8(edict_t *e)  { FRAME(e,HK_CHARA8, hk_char_a9);  ai_charge(13); ai_melee(); }
static void hk_char_a9(edict_t *e)  { FRAME(e,HK_CHARA9, hk_char_a10); ai_charge(20); ai_melee(); }
static void hk_char_a10(edict_t *e) { FRAME(e,HK_CHARA10,hk_char_a11); ai_charge(20); ai_melee(); }
static void hk_char_a11(edict_t *e) { FRAME(e,HK_CHARA11,hk_char_a12); ai_charge(18); ai_melee(); }
static void hk_char_a12(edict_t *e) { FRAME(e,HK_CHARA12,hk_char_a13); ai_charge(16); }
static void hk_char_a13(edict_t *e) { FRAME(e,HK_CHARA13,hk_char_a14); ai_charge(14); }
static void hk_char_a14(edict_t *e) { FRAME(e,HK_CHARA14,hk_char_a15); ai_charge(25); }
static void hk_char_a15(edict_t *e) { FRAME(e,HK_CHARA15,hk_char_a16); ai_charge(21); }
static void hk_char_a16(edict_t *e) { FRAME(e,HK_CHARA16,hk_run1);     ai_charge(13); }

// Charge B
static void hk_char_b2(edict_t *e),hk_char_b3(edict_t *e),hk_char_b4(edict_t *e),
            hk_char_b5(edict_t *e),hk_char_b6(edict_t *e);
static void hk_char_b1(edict_t *e) {
    FRAME(e,HK_CHARB1,hk_char_b2); CheckContinueCharge(e); ai_charge(23); ai_melee();
}
static void hk_char_b2(edict_t *e) { FRAME(e,HK_CHARB2,hk_char_b3); ai_charge(17); ai_melee(); }
static void hk_char_b3(edict_t *e) { FRAME(e,HK_CHARB3,hk_char_b4); ai_charge(12); ai_melee(); }
static void hk_char_b4(edict_t *e) { FRAME(e,HK_CHARB4,hk_char_b5); ai_charge(22); ai_melee(); }
static void hk_char_b5(edict_t *e) { FRAME(e,HK_CHARB5,hk_char_b6); ai_charge(18); ai_melee(); }
static void hk_char_b6(edict_t *e) { FRAME(e,HK_CHARB6,hk_char_b1); ai_charge(8);  ai_melee(); }

// Slice
static void hk_slice2(edict_t *e),hk_slice3(edict_t *e),hk_slice4(edict_t *e),hk_slice5(edict_t *e),
            hk_slice6(edict_t *e),hk_slice7(edict_t *e),hk_slice8(edict_t *e),hk_slice9(edict_t *e),
            hk_slice10(edict_t *e);
static void hk_slice1(edict_t *e)  { FRAME(e,HK_SLICE1, hk_slice2);  ai_charge(9);  }
static void hk_slice2(edict_t *e)  { FRAME(e,HK_SLICE2, hk_slice3);  ai_charge(6);  }
static void hk_slice3(edict_t *e)  { FRAME(e,HK_SLICE3, hk_slice4);  ai_charge(13); }
static void hk_slice4(edict_t *e)  { FRAME(e,HK_SLICE4, hk_slice5);  ai_charge(4);  }
static void hk_slice5(edict_t *e)  { FRAME(e,HK_SLICE5, hk_slice6);  ai_charge(7);  ai_melee(); }
static void hk_slice6(edict_t *e)  { FRAME(e,HK_SLICE6, hk_slice7);  ai_charge(15); ai_melee(); }
static void hk_slice7(edict_t *e)  { FRAME(e,HK_SLICE7, hk_slice8);  ai_charge(8);  ai_melee(); }
static void hk_slice8(edict_t *e)  { FRAME(e,HK_SLICE8, hk_slice9);  ai_charge(2);  ai_melee(); }
static void hk_slice9(edict_t *e)  { FRAME(e,HK_SLICE9, hk_slice10); ai_melee(); }
static void hk_slice10(edict_t *e) { FRAME(e,HK_SLICE10,hk_run1);    ai_charge(3);  }

// Smash
static void hk_smash2(edict_t *e),hk_smash3(edict_t *e),hk_smash4(edict_t *e),hk_smash5(edict_t *e),
            hk_smash6(edict_t *e),hk_smash7(edict_t *e),hk_smash8(edict_t *e),hk_smash9(edict_t *e),
            hk_smash10(edict_t *e),hk_smash11(edict_t *e);
static void hk_smash1(edict_t *e)  { FRAME(e,HK_SMASH1, hk_smash2);  ai_charge(1);  }
static void hk_smash2(edict_t *e)  { FRAME(e,HK_SMASH2, hk_smash3);  ai_charge(13); }
static void hk_smash3(edict_t *e)  { FRAME(e,HK_SMASH3, hk_smash4);  ai_charge(9);  }
static void hk_smash4(edict_t *e)  { FRAME(e,HK_SMASH4, hk_smash5);  ai_charge(11); }
static void hk_smash5(edict_t *e)  { FRAME(e,HK_SMASH5, hk_smash6);  ai_charge(10); ai_melee(); }
static void hk_smash6(edict_t *e)  { FRAME(e,HK_SMASH6, hk_smash7);  ai_charge(7);  ai_melee(); }
static void hk_smash7(edict_t *e)  { FRAME(e,HK_SMASH7, hk_smash8);  ai_charge(12); ai_melee(); }
static void hk_smash8(edict_t *e)  { FRAME(e,HK_SMASH8, hk_smash9);  ai_charge(2);  ai_melee(); }
static void hk_smash9(edict_t *e)  { FRAME(e,HK_SMASH9, hk_smash10); ai_charge(3);  ai_melee(); }
static void hk_smash10(edict_t *e) { FRAME(e,HK_SMASH10,hk_smash11); ai_charge(0);  }
static void hk_smash11(edict_t *e) { FRAME(e,HK_SMASH11,hk_run1);    ai_charge(0);  }

// Wide attack
static void hk_watk2(edict_t *e),hk_watk3(edict_t *e),hk_watk4(edict_t *e),hk_watk5(edict_t *e),
            hk_watk6(edict_t *e),hk_watk7(edict_t *e),hk_watk8(edict_t *e),hk_watk9(edict_t *e),
            hk_watk10(edict_t *e),hk_watk11(edict_t *e),hk_watk12(edict_t *e),hk_watk13(edict_t *e),
            hk_watk14(edict_t *e),hk_watk15(edict_t *e),hk_watk16(edict_t *e),hk_watk17(edict_t *e),
            hk_watk18(edict_t *e),hk_watk19(edict_t *e),hk_watk20(edict_t *e),hk_watk21(edict_t *e),
            hk_watk22(edict_t *e);
static void hk_watk1(edict_t *e)  { FRAME(e,HK_WATK1, hk_watk2);  ai_charge(2);  }
static void hk_watk2(edict_t *e)  { FRAME(e,HK_WATK2, hk_watk3);  ai_charge(0);  }
static void hk_watk3(edict_t *e)  { FRAME(e,HK_WATK3, hk_watk4);  ai_charge(0);  }
static void hk_watk4(edict_t *e)  { FRAME(e,HK_WATK4, hk_watk5);  ai_melee(); }
static void hk_watk5(edict_t *e)  { FRAME(e,HK_WATK5, hk_watk6);  ai_melee(); }
static void hk_watk6(edict_t *e)  { FRAME(e,HK_WATK6, hk_watk7);  ai_melee(); }
static void hk_watk7(edict_t *e)  { FRAME(e,HK_WATK7, hk_watk8);  ai_charge(1);  }
static void hk_watk8(edict_t *e)  { FRAME(e,HK_WATK8, hk_watk9);  ai_charge(4);  }
static void hk_watk9(edict_t *e)  { FRAME(e,HK_WATK9, hk_watk10); ai_charge(5);  }
static void hk_watk10(edict_t *e) { FRAME(e,HK_WATK10,hk_watk11); ai_charge(3);  ai_melee(); }
static void hk_watk11(edict_t *e) { FRAME(e,HK_WATK11,hk_watk12); ai_charge(2);  ai_melee(); }
static void hk_watk12(edict_t *e) { FRAME(e,HK_WATK12,hk_watk13); ai_charge(2);  ai_melee(); }
static void hk_watk13(edict_t *e) { FRAME(e,HK_WATK13,hk_watk14); ai_charge(0);  }
static void hk_watk14(edict_t *e) { FRAME(e,HK_WATK14,hk_watk15); ai_charge(0);  }
static void hk_watk15(edict_t *e) { FRAME(e,HK_WATK15,hk_watk16); ai_charge(0);  }
static void hk_watk16(edict_t *e) { FRAME(e,HK_WATK16,hk_watk17); ai_charge(1);  }
static void hk_watk17(edict_t *e) { FRAME(e,HK_WATK17,hk_watk18); ai_charge(1);  ai_melee(); }
static void hk_watk18(edict_t *e) { FRAME(e,HK_WATK18,hk_watk19); ai_charge(3);  ai_melee(); }
static void hk_watk19(edict_t *e) { FRAME(e,HK_WATK19,hk_watk20); ai_charge(4);  ai_melee(); }
static void hk_watk20(edict_t *e) { FRAME(e,HK_WATK20,hk_watk21); ai_charge(6);  }
static void hk_watk21(edict_t *e) { FRAME(e,HK_WATK21,hk_watk22); ai_charge(7);  }
static void hk_watk22(edict_t *e) { FRAME(e,HK_WATK22,hk_run1);   ai_charge(3);  }

// Magic attacks
static void hk_magica2(edict_t *e),hk_magica3(edict_t *e),hk_magica4(edict_t *e),
            hk_magica5(edict_t *e),hk_magica6(edict_t *e),hk_magica7(edict_t *e),
            hk_magica8(edict_t *e),hk_magica9(edict_t *e),hk_magica10(edict_t *e),
            hk_magica11(edict_t *e),hk_magica12(edict_t *e),hk_magica13(edict_t *e),
            hk_magica14(edict_t *e);
static void hk_magica1(edict_t *e)  { FRAME(e,HK_MAGICA1, hk_magica2);  ai_face(); }
static void hk_magica2(edict_t *e)  { FRAME(e,HK_MAGICA2, hk_magica3);  ai_face(); }
static void hk_magica3(edict_t *e)  { FRAME(e,HK_MAGICA3, hk_magica4);  ai_face(); }
static void hk_magica4(edict_t *e)  { FRAME(e,HK_MAGICA4, hk_magica5);  ai_face(); }
static void hk_magica5(edict_t *e)  { FRAME(e,HK_MAGICA5, hk_magica6);  ai_face(); }
static void hk_magica6(edict_t *e)  { FRAME(e,HK_MAGICA6, hk_magica7);  ai_face(); }
static void hk_magica7(edict_t *e)  { FRAME(e,HK_MAGICA7, hk_magica8);  hknight_shot(e,-2); }
static void hk_magica8(edict_t *e)  { FRAME(e,HK_MAGICA8, hk_magica9);  hknight_shot(e,-1); }
static void hk_magica9(edict_t *e)  { FRAME(e,HK_MAGICA9, hk_magica10); hknight_shot(e, 0); }
static void hk_magica10(edict_t *e) { FRAME(e,HK_MAGICA10,hk_magica11); hknight_shot(e, 1); }
static void hk_magica11(edict_t *e) { FRAME(e,HK_MAGICA11,hk_magica12); hknight_shot(e, 2); }
static void hk_magica12(edict_t *e) { FRAME(e,HK_MAGICA12,hk_magica13); hknight_shot(e, 3); }
static void hk_magica13(edict_t *e) { FRAME(e,HK_MAGICA13,hk_magica14); ai_face(); }
static void hk_magica14(edict_t *e) { FRAME(e,HK_MAGICA14,hk_run1);     ai_face(); }

static void hk_magicb2(edict_t *e),hk_magicb3(edict_t *e),hk_magicb4(edict_t *e),
            hk_magicb5(edict_t *e),hk_magicb6(edict_t *e),hk_magicb7(edict_t *e),
            hk_magicb8(edict_t *e),hk_magicb9(edict_t *e),hk_magicb10(edict_t *e),
            hk_magicb11(edict_t *e),hk_magicb12(edict_t *e),hk_magicb13(edict_t *e);
static void hk_magicb1(edict_t *e)  { FRAME(e,HK_MAGICB1, hk_magicb2);  ai_face(); }
static void hk_magicb2(edict_t *e)  { FRAME(e,HK_MAGICB2, hk_magicb3);  ai_face(); }
static void hk_magicb3(edict_t *e)  { FRAME(e,HK_MAGICB3, hk_magicb4);  ai_face(); }
static void hk_magicb4(edict_t *e)  { FRAME(e,HK_MAGICB4, hk_magicb5);  ai_face(); }
static void hk_magicb5(edict_t *e)  { FRAME(e,HK_MAGICB5, hk_magicb6);  ai_face(); }
static void hk_magicb6(edict_t *e)  { FRAME(e,HK_MAGICB6, hk_magicb7);  ai_face(); }
static void hk_magicb7(edict_t *e)  { FRAME(e,HK_MAGICB7, hk_magicb8);  hknight_shot(e,-2); }
static void hk_magicb8(edict_t *e)  { FRAME(e,HK_MAGICB8, hk_magicb9);  hknight_shot(e,-1); }
static void hk_magicb9(edict_t *e)  { FRAME(e,HK_MAGICB9, hk_magicb10); hknight_shot(e, 0); }
static void hk_magicb10(edict_t *e) { FRAME(e,HK_MAGICB10,hk_magicb11); hknight_shot(e, 1); }
static void hk_magicb11(edict_t *e) { FRAME(e,HK_MAGICB11,hk_magicb12); hknight_shot(e, 2); }
static void hk_magicb12(edict_t *e) { FRAME(e,HK_MAGICB12,hk_magicb13); hknight_shot(e, 3); }
static void hk_magicb13(edict_t *e) { FRAME(e,HK_MAGICB13,hk_run1);     ai_face(); }

static void hk_magicc2(edict_t *e),hk_magicc3(edict_t *e),hk_magicc4(edict_t *e),
            hk_magicc5(edict_t *e),hk_magicc6(edict_t *e),hk_magicc7(edict_t *e),
            hk_magicc8(edict_t *e),hk_magicc9(edict_t *e),hk_magicc10(edict_t *e),
            hk_magicc11(edict_t *e);
static void hk_magicc1(edict_t *e)  { FRAME(e,HK_MAGICC1, hk_magicc2);  ai_face(); }
static void hk_magicc2(edict_t *e)  { FRAME(e,HK_MAGICC2, hk_magicc3);  ai_face(); }
static void hk_magicc3(edict_t *e)  { FRAME(e,HK_MAGICC3, hk_magicc4);  ai_face(); }
static void hk_magicc4(edict_t *e)  { FRAME(e,HK_MAGICC4, hk_magicc5);  ai_face(); }
static void hk_magicc5(edict_t *e)  { FRAME(e,HK_MAGICC5, hk_magicc6);  ai_face(); }
static void hk_magicc6(edict_t *e)  { FRAME(e,HK_MAGICC6, hk_magicc7);  hknight_shot(e,-2); }
static void hk_magicc7(edict_t *e)  { FRAME(e,HK_MAGICC7, hk_magicc8);  hknight_shot(e,-1); }
static void hk_magicc8(edict_t *e)  { FRAME(e,HK_MAGICC8, hk_magicc9);  hknight_shot(e, 0); }
static void hk_magicc9(edict_t *e)  { FRAME(e,HK_MAGICC9, hk_magicc10); hknight_shot(e, 1); }
static void hk_magicc10(edict_t *e) { FRAME(e,HK_MAGICC10,hk_magicc11); hknight_shot(e, 2); }
static void hk_magicc11(edict_t *e) { FRAME(e,HK_MAGICC11,hk_run1);     hknight_shot(e, 3); }

static void hknight_melee(edict_t *e) {
    g->self = e;
    hknight_type++;
    eng->SV_StartSound(e, CHAN_WEAPON, "hknight/slash1.wav", 1, ATTN_NORM);
    if (hknight_type == 1) hk_slice1(e);
    else if (hknight_type == 2) hk_smash1(e);
    else { hk_watk1(e); hknight_type = 0; }
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_hell_knight(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/hknight.mdl");
    eng->PrecacheModel("progs/k_spike.mdl");
    eng->PrecacheModel("progs/h_hellkn.mdl");
    eng->PrecacheSound("hknight/attack1.wav");
    eng->PrecacheSound("hknight/death1.wav");
    eng->PrecacheSound("hknight/pain1.wav");
    eng->PrecacheSound("hknight/sight1.wav");
    eng->PrecacheSound("hknight/hit.wav");
    eng->PrecacheSound("hknight/slash1.wav");
    eng->PrecacheSound("hknight/idle.wav");
    eng->PrecacheSound("hknight/grunt.wav");
    eng->PrecacheSound("knight/sword1.wav");
    eng->PrecacheSound("knight/sword2.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/hknight.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health    = 250;
    e->v.th_stand  = hk_stand1;
    e->v.th_walk   = hk_walk1;
    e->v.th_run    = hk_run1;
    e->v.th_melee  = hknight_melee;
    e->v.th_missile = hk_magicc1;
    e->v.th_pain   = hknight_pain;
    e->v.th_die    = hknight_die;
    walkmonster_start(e);
}

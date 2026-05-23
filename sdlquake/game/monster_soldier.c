// monster_soldier.c -- Grunt / Soldier (soldier.qc / monster_army port).

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
extern void ai_pain(float dist);
extern void ai_painforward(float dist);
extern void ai_back(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *gibname, float dm);
extern void ThrowGib(const char *gibname, float dm);
extern void Corpse_LayProne(edict_t *self);
extern void DropBackpack(void);
extern void walkmonster_start(edict_t *self);
extern void SUB_CheckRefire(thinkfn_t fn);

// FireBullets is static in weapons.c; re-declare a local stub using traceline approach
// We expose a simplified version: use SV_Aim for direction then apply spread
// Actually for soldier we replicate the QC: normalize(enemy.origin - enemy.velocity*0.2 - self.origin)
// and call FireBullets with 4 bullets, spread 0.1 0.1
// We forward-declare from weapons.c
extern void FireBullets_extern(float cnt, float *dir, float sp0, float sp1);

// Frame indices
// stand1-8: 0-7
// death1-10: 8-17
// deathc1-11: 18-28
// load1-11: 29-39
// pain1-6: 40-45
// painb1-14: 46-59
// painc1-13: 60-72
// run1-8: 73-80
// shoot1-9: 81-89
// prowl_1-24: 90-113
enum {
    SLD_STAND1=0,SLD_STAND2,SLD_STAND3,SLD_STAND4,SLD_STAND5,SLD_STAND6,SLD_STAND7,SLD_STAND8,
    SLD_DEATH1,SLD_DEATH2,SLD_DEATH3,SLD_DEATH4,SLD_DEATH5,SLD_DEATH6,SLD_DEATH7,SLD_DEATH8,SLD_DEATH9,SLD_DEATH10,
    SLD_DEATHC1,SLD_DEATHC2,SLD_DEATHC3,SLD_DEATHC4,SLD_DEATHC5,SLD_DEATHC6,SLD_DEATHC7,SLD_DEATHC8,SLD_DEATHC9,SLD_DEATHC10,SLD_DEATHC11,
    SLD_LOAD1,SLD_LOAD2,SLD_LOAD3,SLD_LOAD4,SLD_LOAD5,SLD_LOAD6,SLD_LOAD7,SLD_LOAD8,SLD_LOAD9,SLD_LOAD10,SLD_LOAD11,
    SLD_PAIN1,SLD_PAIN2,SLD_PAIN3,SLD_PAIN4,SLD_PAIN5,SLD_PAIN6,
    SLD_PAINB1,SLD_PAINB2,SLD_PAINB3,SLD_PAINB4,SLD_PAINB5,SLD_PAINB6,SLD_PAINB7,SLD_PAINB8,SLD_PAINB9,SLD_PAINB10,SLD_PAINB11,SLD_PAINB12,SLD_PAINB13,SLD_PAINB14,
    SLD_PAINC1,SLD_PAINC2,SLD_PAINC3,SLD_PAINC4,SLD_PAINC5,SLD_PAINC6,SLD_PAINC7,SLD_PAINC8,SLD_PAINC9,SLD_PAINC10,SLD_PAINC11,SLD_PAINC12,SLD_PAINC13,
    SLD_RUN1,SLD_RUN2,SLD_RUN3,SLD_RUN4,SLD_RUN5,SLD_RUN6,SLD_RUN7,SLD_RUN8,
    SLD_SHOOT1,SLD_SHOOT2,SLD_SHOOT3,SLD_SHOOT4,SLD_SHOOT5,SLD_SHOOT6,SLD_SHOOT7,SLD_SHOOT8,SLD_SHOOT9,
    SLD_PROWL1,SLD_PROWL2,SLD_PROWL3,SLD_PROWL4,SLD_PROWL5,SLD_PROWL6,SLD_PROWL7,SLD_PROWL8,
    SLD_PROWL9,SLD_PROWL10,SLD_PROWL11,SLD_PROWL12,SLD_PROWL13,SLD_PROWL14,SLD_PROWL15,SLD_PROWL16,
    SLD_PROWL17,SLD_PROWL18,SLD_PROWL19,SLD_PROWL20,SLD_PROWL21,SLD_PROWL22,SLD_PROWL23,SLD_PROWL24
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Forward declarations
static void army_stand1(edict_t *e),army_stand2(edict_t *e),army_stand3(edict_t *e),army_stand4(edict_t *e),
            army_stand5(edict_t *e),army_stand6(edict_t *e),army_stand7(edict_t *e),army_stand8(edict_t *e);
static void army_walk1(edict_t *e),army_walk2(edict_t *e),army_walk3(edict_t *e),army_walk4(edict_t *e),
            army_walk5(edict_t *e),army_walk6(edict_t *e),army_walk7(edict_t *e),army_walk8(edict_t *e),
            army_walk9(edict_t *e),army_walk10(edict_t *e),army_walk11(edict_t *e),army_walk12(edict_t *e),
            army_walk13(edict_t *e),army_walk14(edict_t *e),army_walk15(edict_t *e),army_walk16(edict_t *e),
            army_walk17(edict_t *e),army_walk18(edict_t *e),army_walk19(edict_t *e),army_walk20(edict_t *e),
            army_walk21(edict_t *e),army_walk22(edict_t *e),army_walk23(edict_t *e),army_walk24(edict_t *e);
static void army_run1(edict_t *e),army_run2(edict_t *e),army_run3(edict_t *e),army_run4(edict_t *e),
            army_run5(edict_t *e),army_run6(edict_t *e),army_run7(edict_t *e),army_run8(edict_t *e);
static void army_atk1(edict_t *e),army_atk2(edict_t *e),army_atk3(edict_t *e),army_atk4(edict_t *e),
            army_atk5(edict_t *e),army_atk6(edict_t *e),army_atk7(edict_t *e),army_atk8(edict_t *e),army_atk9(edict_t *e);
static void army_pain1(edict_t *e),army_pain2(edict_t *e),army_pain3(edict_t *e),army_pain4(edict_t *e),army_pain5(edict_t *e),army_pain6(edict_t *e);
static void army_painb1(edict_t *e),army_painb2(edict_t *e),army_painb3(edict_t *e),army_painb4(edict_t *e),army_painb5(edict_t *e),army_painb6(edict_t *e),army_painb7(edict_t *e),army_painb8(edict_t *e),army_painb9(edict_t *e),army_painb10(edict_t *e),army_painb11(edict_t *e),army_painb12(edict_t *e),army_painb13(edict_t *e),army_painb14(edict_t *e);
static void army_painc1(edict_t *e),army_painc2(edict_t *e),army_painc3(edict_t *e),army_painc4(edict_t *e),army_painc5(edict_t *e),army_painc6(edict_t *e),army_painc7(edict_t *e),army_painc8(edict_t *e),army_painc9(edict_t *e),army_painc10(edict_t *e),army_painc11(edict_t *e),army_painc12(edict_t *e),army_painc13(edict_t *e);
static void army_die1(edict_t *e),army_die2(edict_t *e),army_die3(edict_t *e),army_die4(edict_t *e),army_die5(edict_t *e),army_die6(edict_t *e),army_die7(edict_t *e),army_die8(edict_t *e),army_die9(edict_t *e),army_die10(edict_t *e);
static void army_cdie1(edict_t *e),army_cdie2(edict_t *e),army_cdie3(edict_t *e),army_cdie4(edict_t *e),army_cdie5(edict_t *e),army_cdie6(edict_t *e),army_cdie7(edict_t *e),army_cdie8(edict_t *e),army_cdie9(edict_t *e),army_cdie10(edict_t *e),army_cdie11(edict_t *e);

// Stand
static void army_stand1(edict_t *e) { FRAME(e,SLD_STAND1,army_stand2); ai_stand(e); }
static void army_stand2(edict_t *e) { FRAME(e,SLD_STAND2,army_stand3); ai_stand(e); }
static void army_stand3(edict_t *e) { FRAME(e,SLD_STAND3,army_stand4); ai_stand(e); }
static void army_stand4(edict_t *e) { FRAME(e,SLD_STAND4,army_stand5); ai_stand(e); }
static void army_stand5(edict_t *e) { FRAME(e,SLD_STAND5,army_stand6); ai_stand(e); }
static void army_stand6(edict_t *e) { FRAME(e,SLD_STAND6,army_stand7); ai_stand(e); }
static void army_stand7(edict_t *e) { FRAME(e,SLD_STAND7,army_stand8); ai_stand(e); }
static void army_stand8(edict_t *e) { FRAME(e,SLD_STAND8,army_stand1); ai_stand(e); }

// Walk
static void army_walk1(edict_t *e)  { FRAME(e,SLD_PROWL1,army_walk2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e,CHAN_VOICE,"soldier/idle.wav",1,ATTN_IDLE);
    ai_walk(1); }
static void army_walk2(edict_t *e)  { FRAME(e,SLD_PROWL2,army_walk3);  ai_walk(1); }
static void army_walk3(edict_t *e)  { FRAME(e,SLD_PROWL3,army_walk4);  ai_walk(1); }
static void army_walk4(edict_t *e)  { FRAME(e,SLD_PROWL4,army_walk5);  ai_walk(1); }
static void army_walk5(edict_t *e)  { FRAME(e,SLD_PROWL5,army_walk6);  ai_walk(2); }
static void army_walk6(edict_t *e)  { FRAME(e,SLD_PROWL6,army_walk7);  ai_walk(3); }
static void army_walk7(edict_t *e)  { FRAME(e,SLD_PROWL7,army_walk8);  ai_walk(4); }
static void army_walk8(edict_t *e)  { FRAME(e,SLD_PROWL8,army_walk9);  ai_walk(4); }
static void army_walk9(edict_t *e)  { FRAME(e,SLD_PROWL9,army_walk10); ai_walk(2); }
static void army_walk10(edict_t *e) { FRAME(e,SLD_PROWL10,army_walk11); ai_walk(2); }
static void army_walk11(edict_t *e) { FRAME(e,SLD_PROWL11,army_walk12); ai_walk(2); }
static void army_walk12(edict_t *e) { FRAME(e,SLD_PROWL12,army_walk13); ai_walk(1); }
static void army_walk13(edict_t *e) { FRAME(e,SLD_PROWL13,army_walk14); ai_walk(0); }
static void army_walk14(edict_t *e) { FRAME(e,SLD_PROWL14,army_walk15); ai_walk(1); }
static void army_walk15(edict_t *e) { FRAME(e,SLD_PROWL15,army_walk16); ai_walk(1); }
static void army_walk16(edict_t *e) { FRAME(e,SLD_PROWL16,army_walk17); ai_walk(1); }
static void army_walk17(edict_t *e) { FRAME(e,SLD_PROWL17,army_walk18); ai_walk(3); }
static void army_walk18(edict_t *e) { FRAME(e,SLD_PROWL18,army_walk19); ai_walk(3); }
static void army_walk19(edict_t *e) { FRAME(e,SLD_PROWL19,army_walk20); ai_walk(3); }
static void army_walk20(edict_t *e) { FRAME(e,SLD_PROWL20,army_walk21); ai_walk(3); }
static void army_walk21(edict_t *e) { FRAME(e,SLD_PROWL21,army_walk22); ai_walk(2); }
static void army_walk22(edict_t *e) { FRAME(e,SLD_PROWL22,army_walk23); ai_walk(1); }
static void army_walk23(edict_t *e) { FRAME(e,SLD_PROWL23,army_walk24); ai_walk(1); }
static void army_walk24(edict_t *e) { FRAME(e,SLD_PROWL24,army_walk1);  ai_walk(1); }

// Run
static void army_run1(edict_t *e) { FRAME(e,SLD_RUN1,army_run2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e,CHAN_VOICE,"soldier/idle.wav",1,ATTN_IDLE);
    ai_run(11); }
static void army_run2(edict_t *e) { FRAME(e,SLD_RUN2,army_run3); ai_run(15); }
static void army_run3(edict_t *e) { FRAME(e,SLD_RUN3,army_run4); ai_run(10); }
static void army_run4(edict_t *e) { FRAME(e,SLD_RUN4,army_run5); ai_run(10); }
static void army_run5(edict_t *e) { FRAME(e,SLD_RUN5,army_run6); ai_run(8); }
static void army_run6(edict_t *e) { FRAME(e,SLD_RUN6,army_run7); ai_run(15); }
static void army_run7(edict_t *e) { FRAME(e,SLD_RUN7,army_run8); ai_run(10); }
static void army_run8(edict_t *e) { FRAME(e,SLD_RUN8,army_run1); ai_run(8); }

// Fire
static void army_fire(edict_t *e) {
    ai_face();
    eng->SV_StartSound(e, CHAN_WEAPON, "soldier/sattck1.wav", 1, ATTN_NORM);
    edict_t *en = e->v.enemy;

    // Eject one brass shell to the grunt's right. MakeVectors on its yaw
    // gives us the right-vector; spawn at approximate muzzle height (~0.7
    // of bbox) and fling outward.
    {
        vec3_t ang = { 0, e->v.angles[1], 0 };
        eng->MakeVectors(ang);
        vec3_t sorg, svel;
        sorg[0] = e->v.origin[0] + g->v_forward[0]*4.0f + g->v_right[0]*10.0f;
        sorg[1] = e->v.origin[1] + g->v_forward[1]*4.0f + g->v_right[1]*10.0f;
        sorg[2] = e->v.absmin[2] + e->v.size[2]*0.7f;
        float rspeed = 90.0f + eng->Random()*40.0f;
        svel[0] = g->v_right[0]*rspeed + (eng->Random()-0.5f)*20.0f;
        svel[1] = g->v_right[1]*rspeed + (eng->Random()-0.5f)*20.0f;
        svel[2] = 40.0f + eng->Random()*30.0f;
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_SHELLEJECT);
        eng->MSG_WriteCoord(MSG_BROADCAST, sorg[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, sorg[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, sorg[2]);
        eng->MSG_WriteCoord(MSG_BROADCAST, svel[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, svel[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, svel[2]);
    }

    // dir = normalize(enemy.origin - enemy.velocity*0.2 - self.origin)
    vec3_t dir;
    dir[0] = en->v.origin[0] - en->v.velocity[0]*0.2f - e->v.origin[0];
    dir[1] = en->v.origin[1] - en->v.velocity[1]*0.2f - e->v.origin[1];
    dir[2] = en->v.origin[2] - en->v.velocity[2]*0.2f - e->v.origin[2];
    float len = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    if (len > 0) { dir[0]/=len; dir[1]/=len; dir[2]/=len; }
    // Approximate FireBullets: SV_Aim then apply
    // Use SV_Aim for actual wall-hit detection, then add spread manually
    vec3_t aimed;
    eng->SV_Aim(e, 1000, aimed);
    // blend: use computed dir as basis (QC does normalize of lead shot)
    // Simple: fire 4 shots with 0.1 spread using traceline
    for (int i = 0; i < 4; i++) {
        float s0 = (eng->Random()-0.5f)*0.2f;
        float s1 = (eng->Random()-0.5f)*0.2f;
        vec3_t d2 = { dir[0]+s0, dir[1]+s1, dir[2] };
        float l2 = sqrtf(d2[0]*d2[0]+d2[1]*d2[1]+d2[2]*d2[2]);
        if (l2 > 0) { d2[0]/=l2; d2[1]/=l2; d2[2]/=l2; }
        vec3_t end = { e->v.origin[0]+d2[0]*2048, e->v.origin[1]+d2[1]*2048, e->v.origin[2]+d2[2]*2048 };
        eng->SV_Traceline(e->v.origin, end, 0, e);
        if (g->trace_ent && g->trace_ent->v.takedamage)
            T_Damage(g->trace_ent, e, e, 4 + eng->Random()*4);
    }
}

// Attack
static void army_atk1(edict_t *e) { FRAME(e,SLD_SHOOT1,army_atk2); ai_face(); }
static void army_atk2(edict_t *e) { FRAME(e,SLD_SHOOT2,army_atk3); ai_face(); }
static void army_atk3(edict_t *e) { FRAME(e,SLD_SHOOT3,army_atk4); ai_face(); }
static void army_atk4(edict_t *e) { FRAME(e,SLD_SHOOT4,army_atk5); ai_face(); }
static void army_atk5(edict_t *e) {
    FRAME(e,SLD_SHOOT5,army_atk6);
    ai_face();
    army_fire(e);
    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
}
static void army_atk6(edict_t *e) { FRAME(e,SLD_SHOOT6,army_atk7); ai_face(); }
static void army_atk7(edict_t *e) { FRAME(e,SLD_SHOOT7,army_atk8); ai_face(); SUB_CheckRefire(army_atk1); }
static void army_atk8(edict_t *e) { FRAME(e,SLD_SHOOT8,army_atk9); ai_face(); }
static void army_atk9(edict_t *e) { FRAME(e,SLD_SHOOT9,army_run1); ai_face(); }

// Pain
static void army_pain1(edict_t *e)  { FRAME(e,SLD_PAIN1,army_pain2); }
static void army_pain2(edict_t *e)  { FRAME(e,SLD_PAIN2,army_pain3); }
static void army_pain3(edict_t *e)  { FRAME(e,SLD_PAIN3,army_pain4); }
static void army_pain4(edict_t *e)  { FRAME(e,SLD_PAIN4,army_pain5); }
static void army_pain5(edict_t *e)  { FRAME(e,SLD_PAIN5,army_pain6); }
static void army_pain6(edict_t *e)  { FRAME(e,SLD_PAIN6,army_run1); ai_pain(1); }

static void army_painb1(edict_t *e)  { FRAME(e,SLD_PAINB1,army_painb2); }
static void army_painb2(edict_t *e)  { FRAME(e,SLD_PAINB2,army_painb3);  ai_painforward(13); }
static void army_painb3(edict_t *e)  { FRAME(e,SLD_PAINB3,army_painb4);  ai_painforward(9); }
static void army_painb4(edict_t *e)  { FRAME(e,SLD_PAINB4,army_painb5); }
static void army_painb5(edict_t *e)  { FRAME(e,SLD_PAINB5,army_painb6); }
static void army_painb6(edict_t *e)  { FRAME(e,SLD_PAINB6,army_painb7); }
static void army_painb7(edict_t *e)  { FRAME(e,SLD_PAINB7,army_painb8); }
static void army_painb8(edict_t *e)  { FRAME(e,SLD_PAINB8,army_painb9); }
static void army_painb9(edict_t *e)  { FRAME(e,SLD_PAINB9,army_painb10); }
static void army_painb10(edict_t *e) { FRAME(e,SLD_PAINB10,army_painb11); }
static void army_painb11(edict_t *e) { FRAME(e,SLD_PAINB11,army_painb12); }
static void army_painb12(edict_t *e) { FRAME(e,SLD_PAINB12,army_painb13); ai_pain(2); }
static void army_painb13(edict_t *e) { FRAME(e,SLD_PAINB13,army_painb14); }
static void army_painb14(edict_t *e) { FRAME(e,SLD_PAINB14,army_run1); }

static void army_painc1(edict_t *e)  { FRAME(e,SLD_PAINC1,army_painc2); }
static void army_painc2(edict_t *e)  { FRAME(e,SLD_PAINC2,army_painc3);  ai_pain(1); }
static void army_painc3(edict_t *e)  { FRAME(e,SLD_PAINC3,army_painc4); }
static void army_painc4(edict_t *e)  { FRAME(e,SLD_PAINC4,army_painc5); }
static void army_painc5(edict_t *e)  { FRAME(e,SLD_PAINC5,army_painc6);  ai_painforward(1); }
static void army_painc6(edict_t *e)  { FRAME(e,SLD_PAINC6,army_painc7);  ai_painforward(1); }
static void army_painc7(edict_t *e)  { FRAME(e,SLD_PAINC7,army_painc8); }
static void army_painc8(edict_t *e)  { FRAME(e,SLD_PAINC8,army_painc9);  ai_pain(1); }
static void army_painc9(edict_t *e)  { FRAME(e,SLD_PAINC9,army_painc10); ai_painforward(4); }
static void army_painc10(edict_t *e) { FRAME(e,SLD_PAINC10,army_painc11); ai_painforward(3); }
static void army_painc11(edict_t *e) { FRAME(e,SLD_PAINC11,army_painc12); ai_painforward(6); }
static void army_painc12(edict_t *e) { FRAME(e,SLD_PAINC12,army_painc13); ai_painforward(8); }
static void army_painc13(edict_t *e) { FRAME(e,SLD_PAINC13,army_run1); }

static void army_pain_cb(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    g->self = self;
    if (self->v.pain_finished > g->time) return;
    float r = eng->Random();
    if (r < 0.2f) {
        self->v.pain_finished = g->time + 0.6f;
        army_pain1(self);
        eng->SV_StartSound(self, CHAN_VOICE, "soldier/pain1.wav", 1, ATTN_NORM);
    } else if (r < 0.6f) {
        self->v.pain_finished = g->time + 1.1f;
        army_painb1(self);
        eng->SV_StartSound(self, CHAN_VOICE, "soldier/pain2.wav", 1, ATTN_NORM);
    } else {
        self->v.pain_finished = g->time + 1.1f;
        army_painc1(self);
        eng->SV_StartSound(self, CHAN_VOICE, "soldier/pain2.wav", 1, ATTN_NORM);
    }
}

// Death
static void army_die1(edict_t *e)  { FRAME(e,SLD_DEATH1,army_die2); }
static void army_die2(edict_t *e)  { FRAME(e,SLD_DEATH2,army_die3); }
static void army_die3(edict_t *e)  { FRAME(e,SLD_DEATH3,army_die4); Corpse_LayProne(e); e->v.ammo_shells=5; DropBackpack(); }
static void army_die4(edict_t *e)  { FRAME(e,SLD_DEATH4,army_die5); }
static void army_die5(edict_t *e)  { FRAME(e,SLD_DEATH5,army_die6); }
static void army_die6(edict_t *e)  { FRAME(e,SLD_DEATH6,army_die7); }
static void army_die7(edict_t *e)  { FRAME(e,SLD_DEATH7,army_die8); }
static void army_die8(edict_t *e)  { FRAME(e,SLD_DEATH8,army_die9); }
static void army_die9(edict_t *e)  { FRAME(e,SLD_DEATH9,army_die10); }
static void army_die10(edict_t *e) { FRAME(e,SLD_DEATH10,army_die10); }

static void army_cdie1(edict_t *e)  { FRAME(e,SLD_DEATHC1,army_cdie2); }
static void army_cdie2(edict_t *e)  { FRAME(e,SLD_DEATHC2,army_cdie3); ai_back(5); }
static void army_cdie3(edict_t *e)  { FRAME(e,SLD_DEATHC3,army_cdie4); Corpse_LayProne(e); e->v.ammo_shells=5; DropBackpack(); ai_back(4); }
static void army_cdie4(edict_t *e)  { FRAME(e,SLD_DEATHC4,army_cdie5); ai_back(13); }
static void army_cdie5(edict_t *e)  { FRAME(e,SLD_DEATHC5,army_cdie6); ai_back(3); }
static void army_cdie6(edict_t *e)  { FRAME(e,SLD_DEATHC6,army_cdie7); ai_back(4); }
static void army_cdie7(edict_t *e)  { FRAME(e,SLD_DEATHC7,army_cdie8); }
static void army_cdie8(edict_t *e)  { FRAME(e,SLD_DEATHC8,army_cdie9); }
static void army_cdie9(edict_t *e)  { FRAME(e,SLD_DEATHC9,army_cdie10); }
static void army_cdie10(edict_t *e) { FRAME(e,SLD_DEATHC10,army_cdie11); }
static void army_cdie11(edict_t *e) { FRAME(e,SLD_DEATHC11,army_cdie11); }

static void army_die_cb(edict_t *self) {
    g->self = self;
    if (self->v.health < -35) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_guard.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "soldier/death1.wav", 1, ATTN_NORM);
    if (eng->Random() < 0.5f)
        army_die1(self);
    else
        army_cdie1(self);
}

void spawn_monster_soldier(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/soldier.mdl");
    eng->PrecacheModel("progs/h_guard.mdl");
    eng->PrecacheModel("progs/gib1.mdl");
    eng->PrecacheModel("progs/gib2.mdl");
    eng->PrecacheModel("progs/gib3.mdl");
    eng->PrecacheSound("soldier/death1.wav");
    eng->PrecacheSound("soldier/idle.wav");
    eng->PrecacheSound("soldier/pain1.wav");
    eng->PrecacheSound("soldier/pain2.wav");
    eng->PrecacheSound("soldier/sattck1.wav");
    eng->PrecacheSound("soldier/sight1.wav");
    eng->PrecacheSound("player/udeath.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/soldier.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health     = 30;
    e->v.th_stand   = army_stand1;
    e->v.th_walk    = army_walk1;
    e->v.th_run     = army_run1;
    e->v.th_missile = army_atk1;
    e->v.th_pain    = army_pain_cb;
    e->v.th_die     = army_die_cb;
    walkmonster_start(e);
}

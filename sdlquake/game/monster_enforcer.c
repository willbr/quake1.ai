// monster_enforcer.c -- Enforcer (enforcer.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_face(void);
extern void ai_forward(float dist);
extern void ai_painforward(float dist);
extern void ai_pain(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *gibname, float dm);
extern void ThrowGib(const char *gibname, float dm);
extern void Corpse_LayProne(edict_t *self);
extern void DropBackpack(void);
extern void SUB_Remove(edict_t *self);
extern void SUB_CheckRefire(thinkfn_t fn);
extern void walkmonster_start(edict_t *self);

// Frame indices
// stand1-7: 0-6
// walk1-16: 7-22
// run1-8: 23-30
// attack1-10: 31-40
// death1-14: 41-54
// fdeath1-11: 55-65
// paina1-4: 66-69
// painb1-5: 70-74
// painc1-8: 75-82
// paind1-19: 83-101
enum {
    ENF_STAND1=0,ENF_STAND2,ENF_STAND3,ENF_STAND4,ENF_STAND5,ENF_STAND6,ENF_STAND7,
    ENF_WALK1,ENF_WALK2,ENF_WALK3,ENF_WALK4,ENF_WALK5,ENF_WALK6,ENF_WALK7,ENF_WALK8,
    ENF_WALK9,ENF_WALK10,ENF_WALK11,ENF_WALK12,ENF_WALK13,ENF_WALK14,ENF_WALK15,ENF_WALK16,
    ENF_RUN1,ENF_RUN2,ENF_RUN3,ENF_RUN4,ENF_RUN5,ENF_RUN6,ENF_RUN7,ENF_RUN8,
    ENF_ATK1,ENF_ATK2,ENF_ATK3,ENF_ATK4,ENF_ATK5,ENF_ATK6,ENF_ATK7,ENF_ATK8,ENF_ATK9,ENF_ATK10,
    ENF_DEATH1,ENF_DEATH2,ENF_DEATH3,ENF_DEATH4,ENF_DEATH5,ENF_DEATH6,ENF_DEATH7,ENF_DEATH8,ENF_DEATH9,ENF_DEATH10,ENF_DEATH11,ENF_DEATH12,ENF_DEATH13,ENF_DEATH14,
    ENF_FDEATH1,ENF_FDEATH2,ENF_FDEATH3,ENF_FDEATH4,ENF_FDEATH5,ENF_FDEATH6,ENF_FDEATH7,ENF_FDEATH8,ENF_FDEATH9,ENF_FDEATH10,ENF_FDEATH11,
    ENF_PAINA1,ENF_PAINA2,ENF_PAINA3,ENF_PAINA4,
    ENF_PAINB1,ENF_PAINB2,ENF_PAINB3,ENF_PAINB4,ENF_PAINB5,
    ENF_PAINC1,ENF_PAINC2,ENF_PAINC3,ENF_PAINC4,ENF_PAINC5,ENF_PAINC6,ENF_PAINC7,ENF_PAINC8,
    ENF_PAIND1,ENF_PAIND2,ENF_PAIND3,ENF_PAIND4,ENF_PAIND5,ENF_PAIND6,ENF_PAIND7,ENF_PAIND8,ENF_PAIND9,ENF_PAIND10,ENF_PAIND11,ENF_PAIND12,ENF_PAIND13,ENF_PAIND14,ENF_PAIND15,ENF_PAIND16,ENF_PAIND17,ENF_PAIND18,ENF_PAIND19
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Laser touch handler
static void Laser_Touch(edict_t *self, edict_t *other) {
    g->self = self;
    if (other == self->v.owner) return;
    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) { eng->ED_Free(self); return; }
    eng->SV_StartSound(self, CHAN_WEAPON, "enforcer/enfstop.wav", 1, ATTN_STATIC);
    float *vel = self->v.velocity;
    float len = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
    vec3_t org = {self->v.origin[0]-8*(len>0?vel[0]/len:0),
                  self->v.origin[1]-8*(len>0?vel[1]/len:0),
                  self->v.origin[2]-8*(len>0?vel[2]/len:0)};
    if (other->v.health) {
        T_Damage(other, self, self->v.owner, 15);
    } else {
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_GUNSHOT);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    }
    eng->ED_Free(self);
}

// Non-static, 2-arg signature matches QC. Uses g->self as owner so trap_shooter
// (misc.c spikeshooter_use) and enforcer_fire share the same implementation.
// Overrides the weak stub in misc.c.
void LaunchLaser(vec3_t org, vec3_t vec) {
    edict_t *owner = g->self;
    eng->SV_StartSound(owner, CHAN_WEAPON, "enforcer/enfire.wav", 1, ATTN_NORM);
    // normalize vec
    float len = sqrtf(vec[0]*vec[0]+vec[1]*vec[1]+vec[2]*vec[2]);
    if (len > 0) { vec[0]/=len; vec[1]/=len; vec[2]/=len; }
    edict_t *mis = eng->ED_Alloc();
    mis->v.owner    = owner;
    mis->v.movetype = MOVETYPE_FLY;
    mis->v.solid    = SOLID_BBOX;
    mis->v.effects  = EF_DIMLIGHT;
    eng->SV_SetModel(mis, "progs/laser.mdl");
    vec3_t z0 = {0,0,0};
    eng->SV_SetSize(mis, z0, z0);
    eng->SV_SetOrigin(mis, org);
    mis->v.velocity[0] = vec[0]*600;
    mis->v.velocity[1] = vec[1]*600;
    mis->v.velocity[2] = vec[2]*600;
    // set angles from velocity
    mis->v.angles[1] = eng->VectorToYaw(mis->v.velocity);
    mis->v.nextthink = g->time + 5;
    mis->v.think = SUB_Remove;
    mis->v.touch = Laser_Touch;
}

static void enforcer_fire(edict_t *e) {
    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
    eng->MakeVectors(e->v.angles);
    float *vf = g->v_forward;
    float *vr = g->v_right;
    vec3_t org = {
        e->v.origin[0] + vf[0]*30 + vr[0]*8.5f,
        e->v.origin[1] + vf[1]*30 + vr[1]*8.5f,
        e->v.origin[2] + vf[2]*30 + vr[2]*8.5f + 16
    };
    vec3_t vec = {
        e->v.enemy->v.origin[0] - e->v.origin[0],
        e->v.enemy->v.origin[1] - e->v.origin[1],
        e->v.enemy->v.origin[2] - e->v.origin[2]
    };
    g->self = e;  // ensure g->self is the enforcer, since LaunchLaser uses it as owner
    LaunchLaser(org, vec);
}

// Forward declarations
static void enf_stand1(edict_t *e),enf_stand2(edict_t *e),enf_stand3(edict_t *e),enf_stand4(edict_t *e),enf_stand5(edict_t *e),enf_stand6(edict_t *e),enf_stand7(edict_t *e);
static void enf_walk1(edict_t *e),enf_walk2(edict_t *e),enf_walk3(edict_t *e),enf_walk4(edict_t *e),enf_walk5(edict_t *e),enf_walk6(edict_t *e),enf_walk7(edict_t *e),enf_walk8(edict_t *e),enf_walk9(edict_t *e),enf_walk10(edict_t *e),enf_walk11(edict_t *e),enf_walk12(edict_t *e),enf_walk13(edict_t *e),enf_walk14(edict_t *e),enf_walk15(edict_t *e),enf_walk16(edict_t *e);
static void enf_run1(edict_t *e),enf_run2(edict_t *e),enf_run3(edict_t *e),enf_run4(edict_t *e),enf_run5(edict_t *e),enf_run6(edict_t *e),enf_run7(edict_t *e),enf_run8(edict_t *e);
static void enf_atk1(edict_t *e),enf_atk2(edict_t *e),enf_atk3(edict_t *e),enf_atk4(edict_t *e),enf_atk5(edict_t *e),enf_atk6(edict_t *e),enf_atk7(edict_t *e),enf_atk8(edict_t *e),enf_atk9(edict_t *e),enf_atk10(edict_t *e),enf_atk11(edict_t *e),enf_atk12(edict_t *e),enf_atk13(edict_t *e),enf_atk14(edict_t *e);
static void enf_paina1(edict_t *e),enf_paina2(edict_t *e),enf_paina3(edict_t *e),enf_paina4(edict_t *e);
static void enf_painb1(edict_t *e),enf_painb2(edict_t *e),enf_painb3(edict_t *e),enf_painb4(edict_t *e),enf_painb5(edict_t *e);
static void enf_painc1(edict_t *e),enf_painc2(edict_t *e),enf_painc3(edict_t *e),enf_painc4(edict_t *e),enf_painc5(edict_t *e),enf_painc6(edict_t *e),enf_painc7(edict_t *e),enf_painc8(edict_t *e);
static void enf_paind1(edict_t *e),enf_paind2(edict_t *e),enf_paind3(edict_t *e),enf_paind4(edict_t *e),enf_paind5(edict_t *e),enf_paind6(edict_t *e),enf_paind7(edict_t *e),enf_paind8(edict_t *e),enf_paind9(edict_t *e),enf_paind10(edict_t *e),enf_paind11(edict_t *e),enf_paind12(edict_t *e),enf_paind13(edict_t *e),enf_paind14(edict_t *e),enf_paind15(edict_t *e),enf_paind16(edict_t *e),enf_paind17(edict_t *e),enf_paind18(edict_t *e),enf_paind19(edict_t *e);
static void enf_die1(edict_t *e),enf_die2(edict_t *e),enf_die3(edict_t *e),enf_die4(edict_t *e),enf_die5(edict_t *e),enf_die6(edict_t *e),enf_die7(edict_t *e),enf_die8(edict_t *e),enf_die9(edict_t *e),enf_die10(edict_t *e),enf_die11(edict_t *e),enf_die12(edict_t *e),enf_die13(edict_t *e),enf_die14(edict_t *e);
static void enf_fdie1(edict_t *e),enf_fdie2(edict_t *e),enf_fdie3(edict_t *e),enf_fdie4(edict_t *e),enf_fdie5(edict_t *e),enf_fdie6(edict_t *e),enf_fdie7(edict_t *e),enf_fdie8(edict_t *e),enf_fdie9(edict_t *e),enf_fdie10(edict_t *e),enf_fdie11(edict_t *e);

// Stand
static void enf_stand1(edict_t *e) { FRAME(e,ENF_STAND1,enf_stand2); ai_stand(e); }
static void enf_stand2(edict_t *e) { FRAME(e,ENF_STAND2,enf_stand3); ai_stand(e); }
static void enf_stand3(edict_t *e) { FRAME(e,ENF_STAND3,enf_stand4); ai_stand(e); }
static void enf_stand4(edict_t *e) { FRAME(e,ENF_STAND4,enf_stand5); ai_stand(e); }
static void enf_stand5(edict_t *e) { FRAME(e,ENF_STAND5,enf_stand6); ai_stand(e); }
static void enf_stand6(edict_t *e) { FRAME(e,ENF_STAND6,enf_stand7); ai_stand(e); }
static void enf_stand7(edict_t *e) { FRAME(e,ENF_STAND7,enf_stand1); ai_stand(e); }

// Walk
static void enf_walk1(edict_t *e)  { FRAME(e,ENF_WALK1,enf_walk2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e,CHAN_VOICE,"enforcer/idle1.wav",1,ATTN_IDLE);
    ai_walk(2); }
static void enf_walk2(edict_t *e)  { FRAME(e,ENF_WALK2,enf_walk3);  ai_walk(4); }
static void enf_walk3(edict_t *e)  { FRAME(e,ENF_WALK3,enf_walk4);  ai_walk(4); }
static void enf_walk4(edict_t *e)  { FRAME(e,ENF_WALK4,enf_walk5);  ai_walk(3); }
static void enf_walk5(edict_t *e)  { FRAME(e,ENF_WALK5,enf_walk6);  ai_walk(1); }
static void enf_walk6(edict_t *e)  { FRAME(e,ENF_WALK6,enf_walk7);  ai_walk(2); }
static void enf_walk7(edict_t *e)  { FRAME(e,ENF_WALK7,enf_walk8);  ai_walk(2); }
static void enf_walk8(edict_t *e)  { FRAME(e,ENF_WALK8,enf_walk9);  ai_walk(1); }
static void enf_walk9(edict_t *e)  { FRAME(e,ENF_WALK9,enf_walk10); ai_walk(2); }
static void enf_walk10(edict_t *e) { FRAME(e,ENF_WALK10,enf_walk11); ai_walk(4); }
static void enf_walk11(edict_t *e) { FRAME(e,ENF_WALK11,enf_walk12); ai_walk(4); }
static void enf_walk12(edict_t *e) { FRAME(e,ENF_WALK12,enf_walk13); ai_walk(1); }
static void enf_walk13(edict_t *e) { FRAME(e,ENF_WALK13,enf_walk14); ai_walk(2); }
static void enf_walk14(edict_t *e) { FRAME(e,ENF_WALK14,enf_walk15); ai_walk(3); }
static void enf_walk15(edict_t *e) { FRAME(e,ENF_WALK15,enf_walk16); ai_walk(4); }
static void enf_walk16(edict_t *e) { FRAME(e,ENF_WALK16,enf_walk1);  ai_walk(2); }

// Run
static void enf_run1(edict_t *e) { FRAME(e,ENF_RUN1,enf_run2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e,CHAN_VOICE,"enforcer/idle1.wav",1,ATTN_IDLE);
    ai_run(18); }
static void enf_run2(edict_t *e) { FRAME(e,ENF_RUN2,enf_run3); ai_run(14); }
static void enf_run3(edict_t *e) { FRAME(e,ENF_RUN3,enf_run4); ai_run(7); }
static void enf_run4(edict_t *e) { FRAME(e,ENF_RUN4,enf_run5); ai_run(12); }
static void enf_run5(edict_t *e) { FRAME(e,ENF_RUN5,enf_run6); ai_run(14); }
static void enf_run6(edict_t *e) { FRAME(e,ENF_RUN6,enf_run7); ai_run(14); }
static void enf_run7(edict_t *e) { FRAME(e,ENF_RUN7,enf_run8); ai_run(7); }
static void enf_run8(edict_t *e) { FRAME(e,ENF_RUN8,enf_run1); ai_run(11); }

// Attack
static void enf_atk1(edict_t *e)  { FRAME(e,ENF_ATK1,enf_atk2);  ai_face(); }
static void enf_atk2(edict_t *e)  { FRAME(e,ENF_ATK2,enf_atk3);  ai_face(); }
static void enf_atk3(edict_t *e)  { FRAME(e,ENF_ATK3,enf_atk4);  ai_face(); }
static void enf_atk4(edict_t *e)  { FRAME(e,ENF_ATK4,enf_atk5);  ai_face(); }
static void enf_atk5(edict_t *e)  { FRAME(e,ENF_ATK5,enf_atk6);  ai_face(); }
static void enf_atk6(edict_t *e)  { FRAME(e,ENF_ATK6,enf_atk7);  enforcer_fire(e); }
static void enf_atk7(edict_t *e)  { FRAME(e,ENF_ATK7,enf_atk8);  ai_face(); }
static void enf_atk8(edict_t *e)  { FRAME(e,ENF_ATK8,enf_atk9);  ai_face(); }
static void enf_atk9(edict_t *e)  { FRAME(e,ENF_ATK5,enf_atk10); ai_face(); }
static void enf_atk10(edict_t *e) { FRAME(e,ENF_ATK6,enf_atk11); enforcer_fire(e); }
static void enf_atk11(edict_t *e) { FRAME(e,ENF_ATK7,enf_atk12); ai_face(); }
static void enf_atk12(edict_t *e) { FRAME(e,ENF_ATK8,enf_atk13); ai_face(); }
static void enf_atk13(edict_t *e) { FRAME(e,ENF_ATK9,enf_atk14); ai_face(); }
static void enf_atk14(edict_t *e) { FRAME(e,ENF_ATK10,enf_run1); ai_face(); SUB_CheckRefire(enf_atk1); }

// Pain
static void enf_paina1(edict_t *e) { FRAME(e,ENF_PAINA1,enf_paina2); }
static void enf_paina2(edict_t *e) { FRAME(e,ENF_PAINA2,enf_paina3); }
static void enf_paina3(edict_t *e) { FRAME(e,ENF_PAINA3,enf_paina4); }
static void enf_paina4(edict_t *e) { FRAME(e,ENF_PAINA4,enf_run1); }

static void enf_painb1(edict_t *e) { FRAME(e,ENF_PAINB1,enf_painb2); }
static void enf_painb2(edict_t *e) { FRAME(e,ENF_PAINB2,enf_painb3); }
static void enf_painb3(edict_t *e) { FRAME(e,ENF_PAINB3,enf_painb4); }
static void enf_painb4(edict_t *e) { FRAME(e,ENF_PAINB4,enf_painb5); }
static void enf_painb5(edict_t *e) { FRAME(e,ENF_PAINB5,enf_run1); }

static void enf_painc1(edict_t *e) { FRAME(e,ENF_PAINC1,enf_painc2); }
static void enf_painc2(edict_t *e) { FRAME(e,ENF_PAINC2,enf_painc3); }
static void enf_painc3(edict_t *e) { FRAME(e,ENF_PAINC3,enf_painc4); }
static void enf_painc4(edict_t *e) { FRAME(e,ENF_PAINC4,enf_painc5); }
static void enf_painc5(edict_t *e) { FRAME(e,ENF_PAINC5,enf_painc6); }
static void enf_painc6(edict_t *e) { FRAME(e,ENF_PAINC6,enf_painc7); }
static void enf_painc7(edict_t *e) { FRAME(e,ENF_PAINC7,enf_painc8); }
static void enf_painc8(edict_t *e) { FRAME(e,ENF_PAINC8,enf_run1); }

static void enf_paind1(edict_t *e)  { FRAME(e,ENF_PAIND1,enf_paind2); }
static void enf_paind2(edict_t *e)  { FRAME(e,ENF_PAIND2,enf_paind3); }
static void enf_paind3(edict_t *e)  { FRAME(e,ENF_PAIND3,enf_paind4); }
static void enf_paind4(edict_t *e)  { FRAME(e,ENF_PAIND4,enf_paind5); ai_painforward(2); }
static void enf_paind5(edict_t *e)  { FRAME(e,ENF_PAIND5,enf_paind6); ai_painforward(1); }
static void enf_paind6(edict_t *e)  { FRAME(e,ENF_PAIND6,enf_paind7); }
static void enf_paind7(edict_t *e)  { FRAME(e,ENF_PAIND7,enf_paind8); }
static void enf_paind8(edict_t *e)  { FRAME(e,ENF_PAIND8,enf_paind9); }
static void enf_paind9(edict_t *e)  { FRAME(e,ENF_PAIND9,enf_paind10); }
static void enf_paind10(edict_t *e) { FRAME(e,ENF_PAIND10,enf_paind11); }
static void enf_paind11(edict_t *e) { FRAME(e,ENF_PAIND11,enf_paind12); ai_painforward(1); }
static void enf_paind12(edict_t *e) { FRAME(e,ENF_PAIND12,enf_paind13); ai_painforward(1); }
static void enf_paind13(edict_t *e) { FRAME(e,ENF_PAIND13,enf_paind14); ai_painforward(1); }
static void enf_paind14(edict_t *e) { FRAME(e,ENF_PAIND14,enf_paind15); }
static void enf_paind15(edict_t *e) { FRAME(e,ENF_PAIND15,enf_paind16); }
static void enf_paind16(edict_t *e) { FRAME(e,ENF_PAIND16,enf_paind17); ai_pain(1); }
static void enf_paind17(edict_t *e) { FRAME(e,ENF_PAIND17,enf_paind18); ai_pain(1); }
static void enf_paind18(edict_t *e) { FRAME(e,ENF_PAIND18,enf_paind19); }
static void enf_paind19(edict_t *e) { FRAME(e,ENF_PAIND19,enf_run1); }

static void enf_pain_cb(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    g->self = self;
    float r = eng->Random();
    if (self->v.pain_finished > g->time) return;
    if (r < 0.5f)
        eng->SV_StartSound(self, CHAN_VOICE, "enforcer/pain1.wav", 1, ATTN_NORM);
    else
        eng->SV_StartSound(self, CHAN_VOICE, "enforcer/pain2.wav", 1, ATTN_NORM);
    if (r < 0.2f) { self->v.pain_finished = g->time + 1; enf_paina1(self); }
    else if (r < 0.4f) { self->v.pain_finished = g->time + 1; enf_painb1(self); }
    else if (r < 0.7f) { self->v.pain_finished = g->time + 1; enf_painc1(self); }
    else { self->v.pain_finished = g->time + 2; enf_paind1(self); }
}

// Death
static void enf_die1(edict_t *e)  { FRAME(e,ENF_DEATH1,enf_die2); }
static void enf_die2(edict_t *e)  { FRAME(e,ENF_DEATH2,enf_die3); }
static void enf_die3(edict_t *e)  { FRAME(e,ENF_DEATH3,enf_die4); Corpse_LayProne(e); e->v.ammo_cells=5; DropBackpack(); }
static void enf_die4(edict_t *e)  { FRAME(e,ENF_DEATH4,enf_die5);  ai_forward(14); }
static void enf_die5(edict_t *e)  { FRAME(e,ENF_DEATH5,enf_die6);  ai_forward(2); }
static void enf_die6(edict_t *e)  { FRAME(e,ENF_DEATH6,enf_die7); }
static void enf_die7(edict_t *e)  { FRAME(e,ENF_DEATH7,enf_die8); }
static void enf_die8(edict_t *e)  { FRAME(e,ENF_DEATH8,enf_die9); }
static void enf_die9(edict_t *e)  { FRAME(e,ENF_DEATH9,enf_die10); ai_forward(3); }
static void enf_die10(edict_t *e) { FRAME(e,ENF_DEATH10,enf_die11); ai_forward(5); }
static void enf_die11(edict_t *e) { FRAME(e,ENF_DEATH11,enf_die12); ai_forward(5); }
static void enf_die12(edict_t *e) { FRAME(e,ENF_DEATH12,enf_die13); ai_forward(5); }
static void enf_die13(edict_t *e) { FRAME(e,ENF_DEATH13,enf_die14); }
static void enf_die14(edict_t *e) { FRAME(e,ENF_DEATH14,enf_die14); }

static void enf_fdie1(edict_t *e)  { FRAME(e,ENF_FDEATH1,enf_fdie2); }
static void enf_fdie2(edict_t *e)  { FRAME(e,ENF_FDEATH2,enf_fdie3); }
static void enf_fdie3(edict_t *e)  { FRAME(e,ENF_FDEATH3,enf_fdie4); Corpse_LayProne(e); e->v.ammo_cells=5; DropBackpack(); }
static void enf_fdie4(edict_t *e)  { FRAME(e,ENF_FDEATH4,enf_fdie5); }
static void enf_fdie5(edict_t *e)  { FRAME(e,ENF_FDEATH5,enf_fdie6); }
static void enf_fdie6(edict_t *e)  { FRAME(e,ENF_FDEATH6,enf_fdie7); }
static void enf_fdie7(edict_t *e)  { FRAME(e,ENF_FDEATH7,enf_fdie8); }
static void enf_fdie8(edict_t *e)  { FRAME(e,ENF_FDEATH8,enf_fdie9); }
static void enf_fdie9(edict_t *e)  { FRAME(e,ENF_FDEATH9,enf_fdie10); }
static void enf_fdie10(edict_t *e) { FRAME(e,ENF_FDEATH10,enf_fdie11); }
static void enf_fdie11(edict_t *e) { FRAME(e,ENF_FDEATH11,enf_fdie11); }

static void enf_die_cb(edict_t *self) {
    g->self = self;
    if (self->v.health < -35) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_mega.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "enforcer/death1.wav", 1, ATTN_NORM);
    if (eng->Random() > 0.5f)
        enf_die1(self);
    else
        enf_fdie1(self);
}

void spawn_monster_enforcer(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/enforcer.mdl");
    eng->PrecacheModel("progs/h_mega.mdl");
    eng->PrecacheModel("progs/laser.mdl");
    eng->PrecacheSound("enforcer/death1.wav");
    eng->PrecacheSound("enforcer/enfire.wav");
    eng->PrecacheSound("enforcer/enfstop.wav");
    eng->PrecacheSound("enforcer/idle1.wav");
    eng->PrecacheSound("enforcer/pain1.wav");
    eng->PrecacheSound("enforcer/pain2.wav");
    eng->PrecacheSound("enforcer/sight1.wav");
    eng->PrecacheSound("enforcer/sight2.wav");
    eng->PrecacheSound("enforcer/sight3.wav");
    eng->PrecacheSound("enforcer/sight4.wav");
    eng->PrecacheSound("player/udeath.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/enforcer.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health     = 80;
    e->v.th_stand   = enf_stand1;
    e->v.th_walk    = enf_walk1;
    e->v.th_run     = enf_run1;
    e->v.th_pain    = enf_pain_cb;
    e->v.th_die     = enf_die_cb;
    e->v.th_missile = enf_atk1;
    walkmonster_start(e);
}

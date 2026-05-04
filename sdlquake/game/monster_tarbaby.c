// monster_tarbaby.c -- Tar Baby / Blob (tarbaby.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_turn(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_face(void);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);
extern void BecomeExplosion(void);
extern void walkmonster_start(edict_t *self);

// Frame indices
enum {
    TB_WALK1=0,TB_WALK2,TB_WALK3,TB_WALK4,TB_WALK5,TB_WALK6,TB_WALK7,TB_WALK8,TB_WALK9,TB_WALK10,
    TB_WALK11,TB_WALK12,TB_WALK13,TB_WALK14,TB_WALK15,TB_WALK16,TB_WALK17,TB_WALK18,TB_WALK19,TB_WALK20,
    TB_WALK21,TB_WALK22,TB_WALK23,TB_WALK24,TB_WALK25,
    TB_RUN1,TB_RUN2,TB_RUN3,TB_RUN4,TB_RUN5,TB_RUN6,TB_RUN7,TB_RUN8,TB_RUN9,TB_RUN10,
    TB_RUN11,TB_RUN12,TB_RUN13,TB_RUN14,TB_RUN15,TB_RUN16,TB_RUN17,TB_RUN18,TB_RUN19,TB_RUN20,
    TB_RUN21,TB_RUN22,TB_RUN23,TB_RUN24,TB_RUN25,
    TB_JUMP1,TB_JUMP2,TB_JUMP3,TB_JUMP4,TB_JUMP5,TB_JUMP6,
    TB_FLY1,TB_FLY2,TB_FLY3,TB_FLY4,
    TB_EXP
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Forward declarations
static void tbaby_stand1(edict_t *e);
static void tbaby_walk1(edict_t *e),tbaby_walk2(edict_t *e),tbaby_walk3(edict_t *e),tbaby_walk4(edict_t *e),
            tbaby_walk5(edict_t *e),tbaby_walk6(edict_t *e),tbaby_walk7(edict_t *e),tbaby_walk8(edict_t *e),
            tbaby_walk9(edict_t *e),tbaby_walk10(edict_t *e),tbaby_walk11(edict_t *e),tbaby_walk12(edict_t *e),
            tbaby_walk13(edict_t *e),tbaby_walk14(edict_t *e),tbaby_walk15(edict_t *e),tbaby_walk16(edict_t *e),
            tbaby_walk17(edict_t *e),tbaby_walk18(edict_t *e),tbaby_walk19(edict_t *e),tbaby_walk20(edict_t *e),
            tbaby_walk21(edict_t *e),tbaby_walk22(edict_t *e),tbaby_walk23(edict_t *e),tbaby_walk24(edict_t *e),
            tbaby_walk25(edict_t *e);
static void tbaby_run1(edict_t *e),tbaby_run2(edict_t *e),tbaby_run3(edict_t *e),tbaby_run4(edict_t *e),
            tbaby_run5(edict_t *e),tbaby_run6(edict_t *e),tbaby_run7(edict_t *e),tbaby_run8(edict_t *e),
            tbaby_run9(edict_t *e),tbaby_run10(edict_t *e),tbaby_run11(edict_t *e),tbaby_run12(edict_t *e),
            tbaby_run13(edict_t *e),tbaby_run14(edict_t *e),tbaby_run15(edict_t *e),tbaby_run16(edict_t *e),
            tbaby_run17(edict_t *e),tbaby_run18(edict_t *e),tbaby_run19(edict_t *e),tbaby_run20(edict_t *e),
            tbaby_run21(edict_t *e),tbaby_run22(edict_t *e),tbaby_run23(edict_t *e),tbaby_run24(edict_t *e),
            tbaby_run25(edict_t *e);
static void tbaby_jump1(edict_t *e),tbaby_jump2(edict_t *e),tbaby_jump3(edict_t *e),tbaby_jump4(edict_t *e),
            tbaby_jump5(edict_t *e),tbaby_jump6(edict_t *e);
static void tbaby_fly1(edict_t *e),tbaby_fly2(edict_t *e),tbaby_fly3(edict_t *e),tbaby_fly4(edict_t *e);
static void tbaby_die1(edict_t *e),tbaby_die2(edict_t *e);
static void Tar_JumpTouch(edict_t *self, edict_t *other);

// Stand
static void tbaby_stand1(edict_t *e) { FRAME(e,TB_WALK1,tbaby_stand1); ai_stand(e); }

// Walk
static void tbaby_walk1(edict_t *e)  { FRAME(e,TB_WALK1,tbaby_walk2);  ai_turn(e); }
static void tbaby_walk2(edict_t *e)  { FRAME(e,TB_WALK2,tbaby_walk3);  ai_turn(e); }
static void tbaby_walk3(edict_t *e)  { FRAME(e,TB_WALK3,tbaby_walk4);  ai_turn(e); }
static void tbaby_walk4(edict_t *e)  { FRAME(e,TB_WALK4,tbaby_walk5);  ai_turn(e); }
static void tbaby_walk5(edict_t *e)  { FRAME(e,TB_WALK5,tbaby_walk6);  ai_turn(e); }
static void tbaby_walk6(edict_t *e)  { FRAME(e,TB_WALK6,tbaby_walk7);  ai_turn(e); }
static void tbaby_walk7(edict_t *e)  { FRAME(e,TB_WALK7,tbaby_walk8);  ai_turn(e); }
static void tbaby_walk8(edict_t *e)  { FRAME(e,TB_WALK8,tbaby_walk9);  ai_turn(e); }
static void tbaby_walk9(edict_t *e)  { FRAME(e,TB_WALK9,tbaby_walk10); ai_turn(e); }
static void tbaby_walk10(edict_t *e) { FRAME(e,TB_WALK10,tbaby_walk11); ai_turn(e); }
static void tbaby_walk11(edict_t *e) { FRAME(e,TB_WALK11,tbaby_walk12); ai_walk(2); }
static void tbaby_walk12(edict_t *e) { FRAME(e,TB_WALK12,tbaby_walk13); ai_walk(2); }
static void tbaby_walk13(edict_t *e) { FRAME(e,TB_WALK13,tbaby_walk14); ai_walk(2); }
static void tbaby_walk14(edict_t *e) { FRAME(e,TB_WALK14,tbaby_walk15); ai_walk(2); }
static void tbaby_walk15(edict_t *e) { FRAME(e,TB_WALK15,tbaby_walk16); ai_walk(2); }
static void tbaby_walk16(edict_t *e) { FRAME(e,TB_WALK16,tbaby_walk17); ai_walk(2); }
static void tbaby_walk17(edict_t *e) { FRAME(e,TB_WALK17,tbaby_walk18); ai_walk(2); }
static void tbaby_walk18(edict_t *e) { FRAME(e,TB_WALK18,tbaby_walk19); ai_walk(2); }
static void tbaby_walk19(edict_t *e) { FRAME(e,TB_WALK19,tbaby_walk20); ai_walk(2); }
static void tbaby_walk20(edict_t *e) { FRAME(e,TB_WALK20,tbaby_walk21); ai_walk(2); }
static void tbaby_walk21(edict_t *e) { FRAME(e,TB_WALK21,tbaby_walk22); ai_walk(2); }
static void tbaby_walk22(edict_t *e) { FRAME(e,TB_WALK22,tbaby_walk23); ai_walk(2); }
static void tbaby_walk23(edict_t *e) { FRAME(e,TB_WALK23,tbaby_walk24); ai_walk(2); }
static void tbaby_walk24(edict_t *e) { FRAME(e,TB_WALK24,tbaby_walk25); ai_walk(2); }
static void tbaby_walk25(edict_t *e) { FRAME(e,TB_WALK25,tbaby_walk1);  ai_walk(2); }

// Run
static void tbaby_run1(edict_t *e)  { FRAME(e,TB_RUN1,tbaby_run2);  ai_face(); }
static void tbaby_run2(edict_t *e)  { FRAME(e,TB_RUN2,tbaby_run3);  ai_face(); }
static void tbaby_run3(edict_t *e)  { FRAME(e,TB_RUN3,tbaby_run4);  ai_face(); }
static void tbaby_run4(edict_t *e)  { FRAME(e,TB_RUN4,tbaby_run5);  ai_face(); }
static void tbaby_run5(edict_t *e)  { FRAME(e,TB_RUN5,tbaby_run6);  ai_face(); }
static void tbaby_run6(edict_t *e)  { FRAME(e,TB_RUN6,tbaby_run7);  ai_face(); }
static void tbaby_run7(edict_t *e)  { FRAME(e,TB_RUN7,tbaby_run8);  ai_face(); }
static void tbaby_run8(edict_t *e)  { FRAME(e,TB_RUN8,tbaby_run9);  ai_face(); }
static void tbaby_run9(edict_t *e)  { FRAME(e,TB_RUN9,tbaby_run10); ai_face(); }
static void tbaby_run10(edict_t *e) { FRAME(e,TB_RUN10,tbaby_run11); ai_face(); }
static void tbaby_run11(edict_t *e) { FRAME(e,TB_RUN11,tbaby_run12); ai_run(2); }
static void tbaby_run12(edict_t *e) { FRAME(e,TB_RUN12,tbaby_run13); ai_run(2); }
static void tbaby_run13(edict_t *e) { FRAME(e,TB_RUN13,tbaby_run14); ai_run(2); }
static void tbaby_run14(edict_t *e) { FRAME(e,TB_RUN14,tbaby_run15); ai_run(2); }
static void tbaby_run15(edict_t *e) { FRAME(e,TB_RUN15,tbaby_run16); ai_run(2); }
static void tbaby_run16(edict_t *e) { FRAME(e,TB_RUN16,tbaby_run17); ai_run(2); }
static void tbaby_run17(edict_t *e) { FRAME(e,TB_RUN17,tbaby_run18); ai_run(2); }
static void tbaby_run18(edict_t *e) { FRAME(e,TB_RUN18,tbaby_run19); ai_run(2); }
static void tbaby_run19(edict_t *e) { FRAME(e,TB_RUN19,tbaby_run20); ai_run(2); }
static void tbaby_run20(edict_t *e) { FRAME(e,TB_RUN20,tbaby_run21); ai_run(2); }
static void tbaby_run21(edict_t *e) { FRAME(e,TB_RUN21,tbaby_run22); ai_run(2); }
static void tbaby_run22(edict_t *e) { FRAME(e,TB_RUN22,tbaby_run23); ai_run(2); }
static void tbaby_run23(edict_t *e) { FRAME(e,TB_RUN23,tbaby_run24); ai_run(2); }
static void tbaby_run24(edict_t *e) { FRAME(e,TB_RUN24,tbaby_run25); ai_run(2); }
static void tbaby_run25(edict_t *e) { FRAME(e,TB_RUN25,tbaby_run1);  ai_run(2); }

// Fly (in-air cycling)
static void tbaby_fly1(edict_t *e) { FRAME(e,TB_FLY1,tbaby_fly2); }
static void tbaby_fly2(edict_t *e) { FRAME(e,TB_FLY2,tbaby_fly3); }
static void tbaby_fly3(edict_t *e) { FRAME(e,TB_FLY3,tbaby_fly4); }
static void tbaby_fly4(edict_t *e) {
    FRAME(e,TB_FLY4,tbaby_fly1);
    e->v.cnt += 1;
    if (e->v.cnt == 4)
        tbaby_jump5(e);
}

// Jump
static void tbaby_jump1(edict_t *e) { FRAME(e,TB_JUMP1,tbaby_jump2); ai_face(); }
static void tbaby_jump2(edict_t *e) { FRAME(e,TB_JUMP2,tbaby_jump3); ai_face(); }
static void tbaby_jump3(edict_t *e) { FRAME(e,TB_JUMP3,tbaby_jump4); ai_face(); }
static void tbaby_jump4(edict_t *e) { FRAME(e,TB_JUMP4,tbaby_jump5); ai_face(); }
static void tbaby_jump5(edict_t *e) {
    FRAME(e,TB_JUMP5,tbaby_jump6);
    e->v.movetype = MOVETYPE_BOUNCE;
    e->v.touch = Tar_JumpTouch;
    eng->MakeVectors(e->v.angles);
    e->v.origin[2] += 1;
    float *vf = g->v_forward;
    e->v.velocity[0] = vf[0]*600;
    e->v.velocity[1] = vf[1]*600;
    e->v.velocity[2] = vf[2]*600 + 200 + eng->Random()*150;
    if ((int)e->v.flags & FL_ONGROUND)
        e->v.flags = (float)((int)e->v.flags - FL_ONGROUND);
    e->v.cnt = 0;
}
static void tbaby_jump6(edict_t *e) { FRAME(e,TB_JUMP6,tbaby_fly1); }

// Touch
static void Tar_JumpTouch(edict_t *self, edict_t *other) {
    g->self = self;
    if ((int)other->v.takedamage &&
        (!other->v.classname || !self->v.classname ||
         strcmp(other->v.classname, self->v.classname) != 0))
    {
        float *vel = self->v.velocity;
        float spd = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
        if (spd > 400) {
            float ldmg = 10 + 10*eng->Random();
            T_Damage(other, self, self, ldmg);
            eng->SV_StartSound(self, CHAN_WEAPON, "blob/hit1.wav", 1, ATTN_NORM);
        }
    } else {
        eng->SV_StartSound(self, CHAN_WEAPON, "blob/land1.wav", 1, ATTN_NORM);
    }

    if (!eng->SV_CheckBottom(self)) {
        if ((int)self->v.flags & FL_ONGROUND) {
            self->v.touch = NULL;
            self->v.think = tbaby_run1;
            self->v.movetype = MOVETYPE_STEP;
            self->v.nextthink = g->time + 0.1f;
        }
        return;
    }

    self->v.touch = NULL;
    self->v.think = tbaby_jump1;
    self->v.nextthink = g->time + 0.1f;
}

// Death
static void tbaby_die1(edict_t *e) {
    FRAME(e,TB_EXP,tbaby_die2);
    e->v.takedamage = DAMAGE_NO;
}
static void tbaby_die2(edict_t *e) {
    FRAME(e,TB_EXP,tbaby_run1);
    T_RadiusDamage(e, e, 120, g->world);
    eng->SV_StartSound(e, CHAN_VOICE, "blob/death1.wav", 1, ATTN_NORM);
    float *vel = e->v.velocity;
    float len = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]+vel[2]*vel[2]);
    if (len > 0) {
        vec3_t neworg = {e->v.origin[0]-8*vel[0]/len,
                         e->v.origin[1]-8*vel[1]/len,
                         e->v.origin[2]-8*vel[2]/len};
        eng->SV_SetOrigin(e, neworg);
    }
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_TAREXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, e->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, e->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, e->v.origin[2]);
    BecomeExplosion();
}

void spawn_monster_tarbaby(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/tarbaby.mdl");
    eng->PrecacheSound("blob/death1.wav");
    eng->PrecacheSound("blob/hit1.wav");
    eng->PrecacheSound("blob/land1.wav");
    eng->PrecacheSound("blob/sight1.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/tarbaby.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health     = 80;
    e->v.th_stand   = tbaby_stand1;
    e->v.th_walk    = tbaby_walk1;
    e->v.th_run     = tbaby_run1;
    e->v.th_missile = tbaby_jump1;
    e->v.th_melee   = tbaby_jump1;
    e->v.th_die     = tbaby_die1;
    walkmonster_start(e);
}

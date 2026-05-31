// monster_zombie.c -- Zombie (zombie.qc port).

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
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void walkmonster_start(edict_t *self);
extern void SUB_Remove(edict_t *e);
static void zb_grenade_touch_remove(edict_t *self, edict_t *other) { (void)other; SUB_Remove(self); }

// Frame indices
// stand1-15: 0-14
// walk1-19: 15-33
// run1-18: 34-51
// atta1-13: 52-64
// attb1-14: 65-78  (attb14 reuses attb13 frame)
// attc1-12: 79-90
// paina1-12: 91-102
// painb1-28: 103-130
// painc1-18: 131-148
// paind1-13: 149-161
// paine1-30: 162-191
// cruc_1-6: 192-197
enum {
    ZB_STAND1=0,ZB_STAND2,ZB_STAND3,ZB_STAND4,ZB_STAND5,
    ZB_STAND6,ZB_STAND7,ZB_STAND8,ZB_STAND9,ZB_STAND10,
    ZB_STAND11,ZB_STAND12,ZB_STAND13,ZB_STAND14,ZB_STAND15,
    ZB_WALK1,ZB_WALK2,ZB_WALK3,ZB_WALK4,ZB_WALK5,ZB_WALK6,ZB_WALK7,
    ZB_WALK8,ZB_WALK9,ZB_WALK10,ZB_WALK11,ZB_WALK12,ZB_WALK13,ZB_WALK14,
    ZB_WALK15,ZB_WALK16,ZB_WALK17,ZB_WALK18,ZB_WALK19,
    ZB_RUN1,ZB_RUN2,ZB_RUN3,ZB_RUN4,ZB_RUN5,ZB_RUN6,ZB_RUN7,ZB_RUN8,
    ZB_RUN9,ZB_RUN10,ZB_RUN11,ZB_RUN12,ZB_RUN13,ZB_RUN14,ZB_RUN15,
    ZB_RUN16,ZB_RUN17,ZB_RUN18,
    ZB_ATTA1,ZB_ATTA2,ZB_ATTA3,ZB_ATTA4,ZB_ATTA5,ZB_ATTA6,ZB_ATTA7,
    ZB_ATTA8,ZB_ATTA9,ZB_ATTA10,ZB_ATTA11,ZB_ATTA12,ZB_ATTA13,
    ZB_ATTB1,ZB_ATTB2,ZB_ATTB3,ZB_ATTB4,ZB_ATTB5,ZB_ATTB6,ZB_ATTB7,
    ZB_ATTB8,ZB_ATTB9,ZB_ATTB10,ZB_ATTB11,ZB_ATTB12,ZB_ATTB13,ZB_ATTB14,
    ZB_ATTC1,ZB_ATTC2,ZB_ATTC3,ZB_ATTC4,ZB_ATTC5,ZB_ATTC6,ZB_ATTC7,
    ZB_ATTC8,ZB_ATTC9,ZB_ATTC10,ZB_ATTC11,ZB_ATTC12,
    ZB_PAINA1,ZB_PAINA2,ZB_PAINA3,ZB_PAINA4,ZB_PAINA5,ZB_PAINA6,
    ZB_PAINA7,ZB_PAINA8,ZB_PAINA9,ZB_PAINA10,ZB_PAINA11,ZB_PAINA12,
    ZB_PAINB1,ZB_PAINB2,ZB_PAINB3,ZB_PAINB4,ZB_PAINB5,ZB_PAINB6,
    ZB_PAINB7,ZB_PAINB8,ZB_PAINB9,ZB_PAINB10,ZB_PAINB11,ZB_PAINB12,
    ZB_PAINB13,ZB_PAINB14,ZB_PAINB15,ZB_PAINB16,ZB_PAINB17,ZB_PAINB18,
    ZB_PAINB19,ZB_PAINB20,ZB_PAINB21,ZB_PAINB22,ZB_PAINB23,ZB_PAINB24,
    ZB_PAINB25,ZB_PAINB26,ZB_PAINB27,ZB_PAINB28,
    ZB_PAINC1,ZB_PAINC2,ZB_PAINC3,ZB_PAINC4,ZB_PAINC5,ZB_PAINC6,
    ZB_PAINC7,ZB_PAINC8,ZB_PAINC9,ZB_PAINC10,ZB_PAINC11,ZB_PAINC12,
    ZB_PAINC13,ZB_PAINC14,ZB_PAINC15,ZB_PAINC16,ZB_PAINC17,ZB_PAINC18,
    ZB_PAIND1,ZB_PAIND2,ZB_PAIND3,ZB_PAIND4,ZB_PAIND5,ZB_PAIND6,
    ZB_PAIND7,ZB_PAIND8,ZB_PAIND9,ZB_PAIND10,ZB_PAIND11,ZB_PAIND12,ZB_PAIND13,
    ZB_PAINE1,ZB_PAINE2,ZB_PAINE3,ZB_PAINE4,ZB_PAINE5,ZB_PAINE6,
    ZB_PAINE7,ZB_PAINE8,ZB_PAINE9,ZB_PAINE10,ZB_PAINE11,ZB_PAINE12,
    ZB_PAINE13,ZB_PAINE14,ZB_PAINE15,ZB_PAINE16,ZB_PAINE17,ZB_PAINE18,
    ZB_PAINE19,ZB_PAINE20,ZB_PAINE21,ZB_PAINE22,ZB_PAINE23,ZB_PAINE24,
    ZB_PAINE25,ZB_PAINE26,ZB_PAINE27,ZB_PAINE28,ZB_PAINE29,ZB_PAINE30,
    ZB_CRUC1,ZB_CRUC2,ZB_CRUC3,ZB_CRUC4,ZB_CRUC5,ZB_CRUC6
};

#define SPAWN_CRUCIFIED 1

// Use e->v.cnt to store the "inpain" state (QC custom field .float inpain)
#define INPAIN(e) ((e)->v.cnt)

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Forward declarations
static void zb_stand1(edict_t *e),zb_stand2(edict_t *e),zb_stand3(edict_t *e),
            zb_stand4(edict_t *e),zb_stand5(edict_t *e),zb_stand6(edict_t *e),
            zb_stand7(edict_t *e),zb_stand8(edict_t *e),zb_stand9(edict_t *e),
            zb_stand10(edict_t *e),zb_stand11(edict_t *e),zb_stand12(edict_t *e),
            zb_stand13(edict_t *e),zb_stand14(edict_t *e),zb_stand15(edict_t *e);
static void zb_run1(edict_t *e);
static void zb_paine1(edict_t *e),zb_paine11(edict_t *e),zb_paine12(edict_t *e);
static void zb_cruc1(edict_t *e);

static void zb_stand1(edict_t *e)  { FRAME(e,ZB_STAND1, zb_stand2);  ai_stand(e); }
// Stand sequence — inline all 15
// just define them directly:
static void zb_stand2(edict_t *e)  { FRAME(e,ZB_STAND2, zb_stand3);  ai_stand(e); }
static void zb_stand3(edict_t *e)  { FRAME(e,ZB_STAND3, zb_stand4);  ai_stand(e); }
static void zb_stand4(edict_t *e)  { FRAME(e,ZB_STAND4, zb_stand5);  ai_stand(e); }
static void zb_stand5(edict_t *e)  { FRAME(e,ZB_STAND5, zb_stand6);  ai_stand(e); }
static void zb_stand6(edict_t *e)  { FRAME(e,ZB_STAND6, zb_stand7);  ai_stand(e); }
static void zb_stand7(edict_t *e)  { FRAME(e,ZB_STAND7, zb_stand8);  ai_stand(e); }
static void zb_stand8(edict_t *e)  { FRAME(e,ZB_STAND8, zb_stand9);  ai_stand(e); }
static void zb_stand9(edict_t *e)  { FRAME(e,ZB_STAND9, zb_stand10); ai_stand(e); }
static void zb_stand10(edict_t *e) { FRAME(e,ZB_STAND10,zb_stand11); ai_stand(e); }
static void zb_stand11(edict_t *e) { FRAME(e,ZB_STAND11,zb_stand12); ai_stand(e); }
static void zb_stand12(edict_t *e) { FRAME(e,ZB_STAND12,zb_stand13); ai_stand(e); }
static void zb_stand13(edict_t *e) { FRAME(e,ZB_STAND13,zb_stand14); ai_stand(e); }
static void zb_stand14(edict_t *e) { FRAME(e,ZB_STAND14,zb_stand15); ai_stand(e); }
static void zb_stand15(edict_t *e) { FRAME(e,ZB_STAND15,zb_stand1);  ai_stand(e); }

// Crucified
static void zb_cruc2(edict_t *e),zb_cruc3(edict_t *e),zb_cruc4(edict_t *e),
            zb_cruc5(edict_t *e),zb_cruc6(edict_t *e);
static void zb_cruc1(edict_t *e) {
    FRAME(e,ZB_CRUC1,zb_cruc2);
    if (eng->Random() < 0.1f)
        eng->SV_StartSound(e, CHAN_VOICE, "zombie/idle_w2.wav", 1, ATTN_STATIC);
}
static void zb_cruc2(edict_t *e) { FRAME(e,ZB_CRUC2,zb_cruc3); e->v.nextthink += eng->Random()*0.1f; }
static void zb_cruc3(edict_t *e) { FRAME(e,ZB_CRUC3,zb_cruc4); e->v.nextthink += eng->Random()*0.1f; }
static void zb_cruc4(edict_t *e) { FRAME(e,ZB_CRUC4,zb_cruc5); e->v.nextthink += eng->Random()*0.1f; }
static void zb_cruc5(edict_t *e) { FRAME(e,ZB_CRUC5,zb_cruc6); e->v.nextthink += eng->Random()*0.1f; }
static void zb_cruc6(edict_t *e) { FRAME(e,ZB_CRUC6,zb_cruc1); e->v.nextthink += eng->Random()*0.1f; }

// Walk
static void zb_walk2(edict_t *e),zb_walk3(edict_t *e),zb_walk4(edict_t *e),
            zb_walk5(edict_t *e),zb_walk6(edict_t *e),zb_walk7(edict_t *e),
            zb_walk8(edict_t *e),zb_walk9(edict_t *e),zb_walk10(edict_t *e),
            zb_walk11(edict_t *e),zb_walk12(edict_t *e),zb_walk13(edict_t *e),
            zb_walk14(edict_t *e),zb_walk15(edict_t *e),zb_walk16(edict_t *e),
            zb_walk17(edict_t *e),zb_walk18(edict_t *e),zb_walk19(edict_t *e);
static void zb_walk1(edict_t *e)  { FRAME(e,ZB_WALK1, zb_walk2);  ai_walk(0); }
static void zb_walk2(edict_t *e)  { FRAME(e,ZB_WALK2, zb_walk3);  ai_walk(2); }
static void zb_walk3(edict_t *e)  { FRAME(e,ZB_WALK3, zb_walk4);  ai_walk(3); }
static void zb_walk4(edict_t *e)  { FRAME(e,ZB_WALK4, zb_walk5);  ai_walk(2); }
static void zb_walk5(edict_t *e)  { FRAME(e,ZB_WALK5, zb_walk6);  ai_walk(1); }
static void zb_walk6(edict_t *e)  { FRAME(e,ZB_WALK6, zb_walk7);  ai_walk(0); }
static void zb_walk7(edict_t *e)  { FRAME(e,ZB_WALK7, zb_walk8);  ai_walk(0); }
static void zb_walk8(edict_t *e)  { FRAME(e,ZB_WALK8, zb_walk9);  ai_walk(0); }
static void zb_walk9(edict_t *e)  { FRAME(e,ZB_WALK9, zb_walk10); ai_walk(0); }
static void zb_walk10(edict_t *e) { FRAME(e,ZB_WALK10,zb_walk11); ai_walk(0); }
static void zb_walk11(edict_t *e) { FRAME(e,ZB_WALK11,zb_walk12); ai_walk(2); }
static void zb_walk12(edict_t *e) { FRAME(e,ZB_WALK12,zb_walk13); ai_walk(2); }
static void zb_walk13(edict_t *e) { FRAME(e,ZB_WALK13,zb_walk14); ai_walk(1); }
static void zb_walk14(edict_t *e) { FRAME(e,ZB_WALK14,zb_walk15); ai_walk(0); }
static void zb_walk15(edict_t *e) { FRAME(e,ZB_WALK15,zb_walk16); ai_walk(0); }
static void zb_walk16(edict_t *e) { FRAME(e,ZB_WALK16,zb_walk17); ai_walk(0); }
static void zb_walk17(edict_t *e) { FRAME(e,ZB_WALK17,zb_walk18); ai_walk(0); }
static void zb_walk18(edict_t *e) { FRAME(e,ZB_WALK18,zb_walk19); ai_walk(0); }
static void zb_walk19(edict_t *e) {
    FRAME(e,ZB_WALK19,zb_walk1); ai_walk(0);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_idle.wav", 1, ATTN_IDLE);
}

// Run
static void zb_run2(edict_t *e),zb_run3(edict_t *e),zb_run4(edict_t *e),
            zb_run5(edict_t *e),zb_run6(edict_t *e),zb_run7(edict_t *e),
            zb_run8(edict_t *e),zb_run9(edict_t *e),zb_run10(edict_t *e),
            zb_run11(edict_t *e),zb_run12(edict_t *e),zb_run13(edict_t *e),
            zb_run14(edict_t *e),zb_run15(edict_t *e),zb_run16(edict_t *e),
            zb_run17(edict_t *e),zb_run18(edict_t *e);
static void zb_run1(edict_t *e)  { FRAME(e,ZB_RUN1, zb_run2);  ai_run(1); INPAIN(e) = 0; }
static void zb_run2(edict_t *e)  { FRAME(e,ZB_RUN2, zb_run3);  ai_run(1); }
static void zb_run3(edict_t *e)  { FRAME(e,ZB_RUN3, zb_run4);  ai_run(0); }
static void zb_run4(edict_t *e)  { FRAME(e,ZB_RUN4, zb_run5);  ai_run(1); }
static void zb_run5(edict_t *e)  { FRAME(e,ZB_RUN5, zb_run6);  ai_run(2); }
static void zb_run6(edict_t *e)  { FRAME(e,ZB_RUN6, zb_run7);  ai_run(3); }
static void zb_run7(edict_t *e)  { FRAME(e,ZB_RUN7, zb_run8);  ai_run(4); }
static void zb_run8(edict_t *e)  { FRAME(e,ZB_RUN8, zb_run9);  ai_run(4); }
static void zb_run9(edict_t *e)  { FRAME(e,ZB_RUN9, zb_run10); ai_run(2); }
static void zb_run10(edict_t *e) { FRAME(e,ZB_RUN10,zb_run11); ai_run(0); }
static void zb_run11(edict_t *e) { FRAME(e,ZB_RUN11,zb_run12); ai_run(0); }
static void zb_run12(edict_t *e) { FRAME(e,ZB_RUN12,zb_run13); ai_run(0); }
static void zb_run13(edict_t *e) { FRAME(e,ZB_RUN13,zb_run14); ai_run(2); }
static void zb_run14(edict_t *e) { FRAME(e,ZB_RUN14,zb_run15); ai_run(4); }
static void zb_run15(edict_t *e) { FRAME(e,ZB_RUN15,zb_run16); ai_run(6); }
static void zb_run16(edict_t *e) { FRAME(e,ZB_RUN16,zb_run17); ai_run(7); }
static void zb_run17(edict_t *e) { FRAME(e,ZB_RUN17,zb_run18); ai_run(3); }
static void zb_run18(edict_t *e) {
    FRAME(e,ZB_RUN18,zb_run1); ai_run(8);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_idle.wav", 1, ATTN_IDLE);
    if (eng->Random() > 0.8f) eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_idle1.wav", 1, ATTN_IDLE);
}

// Grenade touch
static void ZombieGrenadeTouch(edict_t *self, edict_t *other) {
    g->self = self;
    if (other == self->v.owner) return;
    if (other->v.takedamage) {
        T_Damage(other, self, self->v.owner, 10);
        eng->SV_StartSound(self, CHAN_WEAPON, "zombie/z_hit.wav", 1, ATTN_NORM);
        eng->ED_Free(self);
        return;
    }
    eng->SV_StartSound(self, CHAN_WEAPON, "zombie/z_miss.wav", 1, ATTN_NORM);
    self->v.velocity[0] = 0; self->v.velocity[1] = 0; self->v.velocity[2] = 0;
    self->v.avelocity[0] = 0; self->v.avelocity[1] = 0; self->v.avelocity[2] = 0;
    self->v.touch = zb_grenade_touch_remove;
}

static void ZombieFireGrenade(vec3_t st) {
    edict_t *self = g->self;
    eng->SV_StartSound(self, CHAN_WEAPON, "zombie/z_shot1.wav", 1, ATTN_NORM);
    eng->MakeVectors(self->v.angles);
    // org = self.origin + st_x*v_forward + st_y*v_right + (st_z-24)*v_up
    vec3_t org = {
        self->v.origin[0] + st[0]*g->v_forward[0] + st[1]*g->v_right[0] + (st[2]-24)*g->v_up[0],
        self->v.origin[1] + st[0]*g->v_forward[1] + st[1]*g->v_right[1] + (st[2]-24)*g->v_up[1],
        self->v.origin[2] + st[0]*g->v_forward[2] + st[1]*g->v_right[2] + (st[2]-24)*g->v_up[2]
    };
    edict_t *missile = eng->ED_Alloc();
    missile->v.classname = "zombie_gib";
    missile->v.owner     = self;
    missile->v.movetype  = MOVETYPE_BOUNCE;
    missile->v.solid     = SOLID_BBOX;
    float dx = self->v.enemy->v.origin[0] - org[0];
    float dy = self->v.enemy->v.origin[1] - org[1];
    float dz = self->v.enemy->v.origin[2] - org[2];
    float len = (float)sqrt((double)(dx*dx+dy*dy+dz*dz));
    if (len < 0.001f) len = 0.001f;
    missile->v.velocity[0] = dx/len * 600;
    missile->v.velocity[1] = dy/len * 600;
    missile->v.velocity[2] = dz/len * 600 + 200;
    missile->v.avelocity[0] = 3000; missile->v.avelocity[1] = 1000; missile->v.avelocity[2] = 2000;
    missile->v.touch        = ZombieGrenadeTouch;
    missile->v.nextthink    = g->time + 2.5f;
    missile->v.think        = SUB_Remove;
    missile->v.decal_on_bounce = 1.0f;
    eng->SV_SetModel(missile, "progs/zom_gib.mdl");
    vec3_t z = {0,0,0};
    eng->SV_SetSize(missile, z, z);
    eng->SV_SetOrigin(missile, org);
}

// Attack sequences
static void zb_atta2(edict_t *e),zb_atta3(edict_t *e),zb_atta4(edict_t *e),
            zb_atta5(edict_t *e),zb_atta6(edict_t *e),zb_atta7(edict_t *e),
            zb_atta8(edict_t *e),zb_atta9(edict_t *e),zb_atta10(edict_t *e),
            zb_atta11(edict_t *e),zb_atta12(edict_t *e),zb_atta13(edict_t *e);
static void zb_atta1(edict_t *e)  { FRAME(e,ZB_ATTA1, zb_atta2);  ai_face(); }
static void zb_atta2(edict_t *e)  { FRAME(e,ZB_ATTA2, zb_atta3);  ai_face(); }
static void zb_atta3(edict_t *e)  { FRAME(e,ZB_ATTA3, zb_atta4);  ai_face(); }
static void zb_atta4(edict_t *e)  { FRAME(e,ZB_ATTA4, zb_atta5);  ai_face(); }
static void zb_atta5(edict_t *e)  { FRAME(e,ZB_ATTA5, zb_atta6);  ai_face(); }
static void zb_atta6(edict_t *e)  { FRAME(e,ZB_ATTA6, zb_atta7);  ai_face(); }
static void zb_atta7(edict_t *e)  { FRAME(e,ZB_ATTA7, zb_atta8);  ai_face(); }
static void zb_atta8(edict_t *e)  { FRAME(e,ZB_ATTA8, zb_atta9);  ai_face(); }
static void zb_atta9(edict_t *e)  { FRAME(e,ZB_ATTA9, zb_atta10); ai_face(); }
static void zb_atta10(edict_t *e) { FRAME(e,ZB_ATTA10,zb_atta11); ai_face(); }
static void zb_atta11(edict_t *e) { FRAME(e,ZB_ATTA11,zb_atta12); ai_face(); }
static void zb_atta12(edict_t *e) { FRAME(e,ZB_ATTA12,zb_atta13); ai_face(); }
static void zb_atta13(edict_t *e) {
    FRAME(e,ZB_ATTA13,zb_run1); ai_face();
    vec3_t st = {-10,-22,30}; ZombieFireGrenade(st);
}

static void zb_attb2(edict_t *e),zb_attb3(edict_t *e),zb_attb4(edict_t *e),
            zb_attb5(edict_t *e),zb_attb6(edict_t *e),zb_attb7(edict_t *e),
            zb_attb8(edict_t *e),zb_attb9(edict_t *e),zb_attb10(edict_t *e),
            zb_attb11(edict_t *e),zb_attb12(edict_t *e),zb_attb13(edict_t *e),
            zb_attb14(edict_t *e);
static void zb_attb1(edict_t *e)  { FRAME(e,ZB_ATTB1, zb_attb2);  ai_face(); }
static void zb_attb2(edict_t *e)  { FRAME(e,ZB_ATTB2, zb_attb3);  ai_face(); }
static void zb_attb3(edict_t *e)  { FRAME(e,ZB_ATTB3, zb_attb4);  ai_face(); }
static void zb_attb4(edict_t *e)  { FRAME(e,ZB_ATTB4, zb_attb5);  ai_face(); }
static void zb_attb5(edict_t *e)  { FRAME(e,ZB_ATTB5, zb_attb6);  ai_face(); }
static void zb_attb6(edict_t *e)  { FRAME(e,ZB_ATTB6, zb_attb7);  ai_face(); }
static void zb_attb7(edict_t *e)  { FRAME(e,ZB_ATTB7, zb_attb8);  ai_face(); }
static void zb_attb8(edict_t *e)  { FRAME(e,ZB_ATTB8, zb_attb9);  ai_face(); }
static void zb_attb9(edict_t *e)  { FRAME(e,ZB_ATTB9, zb_attb10); ai_face(); }
static void zb_attb10(edict_t *e) { FRAME(e,ZB_ATTB10,zb_attb11); ai_face(); }
static void zb_attb11(edict_t *e) { FRAME(e,ZB_ATTB11,zb_attb12); ai_face(); }
static void zb_attb12(edict_t *e) { FRAME(e,ZB_ATTB12,zb_attb13); ai_face(); }
static void zb_attb13(edict_t *e) { FRAME(e,ZB_ATTB13,zb_attb14); ai_face(); }
static void zb_attb14(edict_t *e) {
    FRAME(e,ZB_ATTB13,zb_run1); ai_face(); // uses attb13 frame (QC duplicate)
    vec3_t st = {-10,-24,29}; ZombieFireGrenade(st);
}

static void zb_attc2(edict_t *e),zb_attc3(edict_t *e),zb_attc4(edict_t *e),
            zb_attc5(edict_t *e),zb_attc6(edict_t *e),zb_attc7(edict_t *e),
            zb_attc8(edict_t *e),zb_attc9(edict_t *e),zb_attc10(edict_t *e),
            zb_attc11(edict_t *e),zb_attc12(edict_t *e);
static void zb_attc1(edict_t *e)  { FRAME(e,ZB_ATTC1, zb_attc2);  ai_face(); }
static void zb_attc2(edict_t *e)  { FRAME(e,ZB_ATTC2, zb_attc3);  ai_face(); }
static void zb_attc3(edict_t *e)  { FRAME(e,ZB_ATTC3, zb_attc4);  ai_face(); }
static void zb_attc4(edict_t *e)  { FRAME(e,ZB_ATTC4, zb_attc5);  ai_face(); }
static void zb_attc5(edict_t *e)  { FRAME(e,ZB_ATTC5, zb_attc6);  ai_face(); }
static void zb_attc6(edict_t *e)  { FRAME(e,ZB_ATTC6, zb_attc7);  ai_face(); }
static void zb_attc7(edict_t *e)  { FRAME(e,ZB_ATTC7, zb_attc8);  ai_face(); }
static void zb_attc8(edict_t *e)  { FRAME(e,ZB_ATTC8, zb_attc9);  ai_face(); }
static void zb_attc9(edict_t *e)  { FRAME(e,ZB_ATTC9, zb_attc10); ai_face(); }
static void zb_attc10(edict_t *e) { FRAME(e,ZB_ATTC10,zb_attc11); ai_face(); }
static void zb_attc11(edict_t *e) { FRAME(e,ZB_ATTC11,zb_attc12); ai_face(); }
static void zb_attc12(edict_t *e) {
    FRAME(e,ZB_ATTC12,zb_run1); ai_face();
    vec3_t st = {-12,-19,29}; ZombieFireGrenade(st);
}

static void zombie_missile(edict_t *e) {
    g->self = e;
    float r = eng->Random();
    if (r < 0.3f) zb_atta1(e);
    else if (r < 0.6f) zb_attb1(e);
    else zb_attc1(e);
}

// Pain sequences
static void zb_paina2(edict_t *e),zb_paina3(edict_t *e),zb_paina4(edict_t *e),
            zb_paina5(edict_t *e),zb_paina6(edict_t *e),zb_paina7(edict_t *e),
            zb_paina8(edict_t *e),zb_paina9(edict_t *e),zb_paina10(edict_t *e),
            zb_paina11(edict_t *e),zb_paina12(edict_t *e);
static void zb_paina1(edict_t *e) {
    FRAME(e,ZB_PAINA1,zb_paina2);
    eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_pain.wav", 1, ATTN_NORM);
}
static void zb_paina2(edict_t *e)  { FRAME(e,ZB_PAINA2, zb_paina3);  ai_painforward(3); }
static void zb_paina3(edict_t *e)  { FRAME(e,ZB_PAINA3, zb_paina4);  ai_painforward(1); }
static void zb_paina4(edict_t *e)  { FRAME(e,ZB_PAINA4, zb_paina5);  ai_pain(1); }
static void zb_paina5(edict_t *e)  { FRAME(e,ZB_PAINA5, zb_paina6);  ai_pain(3); }
static void zb_paina6(edict_t *e)  { FRAME(e,ZB_PAINA6, zb_paina7);  ai_pain(1); }
static void zb_paina7(edict_t *e)  { FRAME(e,ZB_PAINA7, zb_paina8);  }
static void zb_paina8(edict_t *e)  { FRAME(e,ZB_PAINA8, zb_paina9);  }
static void zb_paina9(edict_t *e)  { FRAME(e,ZB_PAINA9, zb_paina10); }
static void zb_paina10(edict_t *e) { FRAME(e,ZB_PAINA10,zb_paina11); }
static void zb_paina11(edict_t *e) { FRAME(e,ZB_PAINA11,zb_paina12); }
static void zb_paina12(edict_t *e) { FRAME(e,ZB_PAINA12,zb_run1);    }

// painb
static void zb_painb2(edict_t *e),zb_painb3(edict_t *e),zb_painb4(edict_t *e),
            zb_painb5(edict_t *e),zb_painb6(edict_t *e),zb_painb7(edict_t *e),
            zb_painb8(edict_t *e),zb_painb9(edict_t *e),zb_painb10(edict_t *e),
            zb_painb11(edict_t *e),zb_painb12(edict_t *e),zb_painb13(edict_t *e),
            zb_painb14(edict_t *e),zb_painb15(edict_t *e),zb_painb16(edict_t *e),
            zb_painb17(edict_t *e),zb_painb18(edict_t *e),zb_painb19(edict_t *e),
            zb_painb20(edict_t *e),zb_painb21(edict_t *e),zb_painb22(edict_t *e),
            zb_painb23(edict_t *e),zb_painb24(edict_t *e),zb_painb25(edict_t *e),
            zb_painb26(edict_t *e),zb_painb27(edict_t *e),zb_painb28(edict_t *e);
static void zb_painb1(edict_t *e) {
    FRAME(e,ZB_PAINB1,zb_painb2);
    eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_pain1.wav", 1, ATTN_NORM);
}
static void zb_painb2(edict_t *e)  { FRAME(e,ZB_PAINB2, zb_painb3);  ai_pain(2); }
static void zb_painb3(edict_t *e)  { FRAME(e,ZB_PAINB3, zb_painb4);  ai_pain(8); }
static void zb_painb4(edict_t *e)  { FRAME(e,ZB_PAINB4, zb_painb5);  ai_pain(6); }
static void zb_painb5(edict_t *e)  { FRAME(e,ZB_PAINB5, zb_painb6);  ai_pain(2); }
static void zb_painb6(edict_t *e)  { FRAME(e,ZB_PAINB6, zb_painb7);  }
static void zb_painb7(edict_t *e)  { FRAME(e,ZB_PAINB7, zb_painb8);  }
static void zb_painb8(edict_t *e)  { FRAME(e,ZB_PAINB8, zb_painb9);  }
static void zb_painb9(edict_t *e)  {
    FRAME(e,ZB_PAINB9,zb_painb10);
    eng->SV_StartSound(e, CHAN_BODY, "zombie/z_fall.wav", 1, ATTN_NORM);
}
static void zb_painb10(edict_t *e) { FRAME(e,ZB_PAINB10,zb_painb11); }
static void zb_painb11(edict_t *e) { FRAME(e,ZB_PAINB11,zb_painb12); }
static void zb_painb12(edict_t *e) { FRAME(e,ZB_PAINB12,zb_painb13); }
static void zb_painb13(edict_t *e) { FRAME(e,ZB_PAINB13,zb_painb14); }
static void zb_painb14(edict_t *e) { FRAME(e,ZB_PAINB14,zb_painb15); }
static void zb_painb15(edict_t *e) { FRAME(e,ZB_PAINB15,zb_painb16); }
static void zb_painb16(edict_t *e) { FRAME(e,ZB_PAINB16,zb_painb17); }
static void zb_painb17(edict_t *e) { FRAME(e,ZB_PAINB17,zb_painb18); }
static void zb_painb18(edict_t *e) { FRAME(e,ZB_PAINB18,zb_painb19); }
static void zb_painb19(edict_t *e) { FRAME(e,ZB_PAINB19,zb_painb20); }
static void zb_painb20(edict_t *e) { FRAME(e,ZB_PAINB20,zb_painb21); }
static void zb_painb21(edict_t *e) { FRAME(e,ZB_PAINB21,zb_painb22); }
static void zb_painb22(edict_t *e) { FRAME(e,ZB_PAINB22,zb_painb23); }
static void zb_painb23(edict_t *e) { FRAME(e,ZB_PAINB23,zb_painb24); }
static void zb_painb24(edict_t *e) { FRAME(e,ZB_PAINB24,zb_painb25); }
static void zb_painb25(edict_t *e) { FRAME(e,ZB_PAINB25,zb_painb26); ai_painforward(1); }
static void zb_painb26(edict_t *e) { FRAME(e,ZB_PAINB26,zb_painb27); }
static void zb_painb27(edict_t *e) { FRAME(e,ZB_PAINB27,zb_painb28); }
static void zb_painb28(edict_t *e) { FRAME(e,ZB_PAINB28,zb_run1);    }

// painc
static void zb_painc2(edict_t *e),zb_painc3(edict_t *e),zb_painc4(edict_t *e),
            zb_painc5(edict_t *e),zb_painc6(edict_t *e),zb_painc7(edict_t *e),
            zb_painc8(edict_t *e),zb_painc9(edict_t *e),zb_painc10(edict_t *e),
            zb_painc11(edict_t *e),zb_painc12(edict_t *e),zb_painc13(edict_t *e),
            zb_painc14(edict_t *e),zb_painc15(edict_t *e),zb_painc16(edict_t *e),
            zb_painc17(edict_t *e),zb_painc18(edict_t *e);
static void zb_painc1(edict_t *e) {
    FRAME(e,ZB_PAINC1,zb_painc2);
    eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_pain1.wav", 1, ATTN_NORM);
}
static void zb_painc2(edict_t *e)  { FRAME(e,ZB_PAINC2, zb_painc3);  }
static void zb_painc3(edict_t *e)  { FRAME(e,ZB_PAINC3, zb_painc4);  ai_pain(3); }
static void zb_painc4(edict_t *e)  { FRAME(e,ZB_PAINC4, zb_painc5);  ai_pain(1); }
static void zb_painc5(edict_t *e)  { FRAME(e,ZB_PAINC5, zb_painc6);  }
static void zb_painc6(edict_t *e)  { FRAME(e,ZB_PAINC6, zb_painc7);  }
static void zb_painc7(edict_t *e)  { FRAME(e,ZB_PAINC7, zb_painc8);  }
static void zb_painc8(edict_t *e)  { FRAME(e,ZB_PAINC8, zb_painc9);  }
static void zb_painc9(edict_t *e)  { FRAME(e,ZB_PAINC9, zb_painc10); }
static void zb_painc10(edict_t *e) { FRAME(e,ZB_PAINC10,zb_painc11); }
static void zb_painc11(edict_t *e) { FRAME(e,ZB_PAINC11,zb_painc12); ai_painforward(1); }
static void zb_painc12(edict_t *e) { FRAME(e,ZB_PAINC12,zb_painc13); ai_painforward(1); }
static void zb_painc13(edict_t *e) { FRAME(e,ZB_PAINC13,zb_painc14); }
static void zb_painc14(edict_t *e) { FRAME(e,ZB_PAINC14,zb_painc15); }
static void zb_painc15(edict_t *e) { FRAME(e,ZB_PAINC15,zb_painc16); }
static void zb_painc16(edict_t *e) { FRAME(e,ZB_PAINC16,zb_painc17); }
static void zb_painc17(edict_t *e) { FRAME(e,ZB_PAINC17,zb_painc18); }
static void zb_painc18(edict_t *e) { FRAME(e,ZB_PAINC18,zb_run1);    }

// paind
static void zb_paind2(edict_t *e),zb_paind3(edict_t *e),zb_paind4(edict_t *e),
            zb_paind5(edict_t *e),zb_paind6(edict_t *e),zb_paind7(edict_t *e),
            zb_paind8(edict_t *e),zb_paind9(edict_t *e),zb_paind10(edict_t *e),
            zb_paind11(edict_t *e),zb_paind12(edict_t *e),zb_paind13(edict_t *e);
static void zb_paind1(edict_t *e) {
    FRAME(e,ZB_PAIND1,zb_paind2);
    eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_pain.wav", 1, ATTN_NORM);
}
static void zb_paind2(edict_t *e)  { FRAME(e,ZB_PAIND2, zb_paind3);  }
static void zb_paind3(edict_t *e)  { FRAME(e,ZB_PAIND3, zb_paind4);  }
static void zb_paind4(edict_t *e)  { FRAME(e,ZB_PAIND4, zb_paind5);  }
static void zb_paind5(edict_t *e)  { FRAME(e,ZB_PAIND5, zb_paind6);  }
static void zb_paind6(edict_t *e)  { FRAME(e,ZB_PAIND6, zb_paind7);  }
static void zb_paind7(edict_t *e)  { FRAME(e,ZB_PAIND7, zb_paind8);  }
static void zb_paind8(edict_t *e)  { FRAME(e,ZB_PAIND8, zb_paind9);  }
static void zb_paind9(edict_t *e)  { FRAME(e,ZB_PAIND9, zb_paind10); ai_pain(1); }
static void zb_paind10(edict_t *e) { FRAME(e,ZB_PAIND10,zb_paind11); }
static void zb_paind11(edict_t *e) { FRAME(e,ZB_PAIND11,zb_paind12); }
static void zb_paind12(edict_t *e) { FRAME(e,ZB_PAIND12,zb_paind13); }
static void zb_paind13(edict_t *e) { FRAME(e,ZB_PAIND13,zb_run1);    }

// paine
static void zb_paine2(edict_t *e),zb_paine3(edict_t *e),zb_paine4(edict_t *e),
            zb_paine5(edict_t *e),zb_paine6(edict_t *e),zb_paine7(edict_t *e),
            zb_paine8(edict_t *e),zb_paine9(edict_t *e),zb_paine10(edict_t *e),
            zb_paine13(edict_t *e),zb_paine14(edict_t *e),zb_paine15(edict_t *e),
            zb_paine16(edict_t *e),zb_paine17(edict_t *e),zb_paine18(edict_t *e),
            zb_paine19(edict_t *e),zb_paine20(edict_t *e),zb_paine21(edict_t *e),
            zb_paine22(edict_t *e),zb_paine23(edict_t *e),zb_paine24(edict_t *e),
            zb_paine25(edict_t *e),zb_paine26(edict_t *e),zb_paine27(edict_t *e),
            zb_paine28(edict_t *e),zb_paine29(edict_t *e),zb_paine30(edict_t *e);
static void zb_paine1(edict_t *e) {
    FRAME(e,ZB_PAINE1,zb_paine2);
    eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_pain.wav", 1, ATTN_NORM);
    e->v.health = 60;
}
static void zb_paine2(edict_t *e)  { FRAME(e,ZB_PAINE2, zb_paine3);  ai_pain(8); }
static void zb_paine3(edict_t *e)  { FRAME(e,ZB_PAINE3, zb_paine4);  ai_pain(5); }
static void zb_paine4(edict_t *e)  { FRAME(e,ZB_PAINE4, zb_paine5);  ai_pain(3); }
static void zb_paine5(edict_t *e)  { FRAME(e,ZB_PAINE5, zb_paine6);  ai_pain(1); }
static void zb_paine6(edict_t *e)  { FRAME(e,ZB_PAINE6, zb_paine7);  ai_pain(2); }
static void zb_paine7(edict_t *e)  { FRAME(e,ZB_PAINE7, zb_paine8);  ai_pain(1); }
static void zb_paine8(edict_t *e)  { FRAME(e,ZB_PAINE8, zb_paine9);  ai_pain(1); }
static void zb_paine9(edict_t *e)  { FRAME(e,ZB_PAINE9, zb_paine10); ai_pain(2); }
static void zb_paine10(edict_t *e) {
    FRAME(e,ZB_PAINE10,zb_paine11);
    eng->SV_StartSound(e, CHAN_BODY, "zombie/z_fall.wav", 1, ATTN_NORM);
    e->v.solid = SOLID_NOT;
}
static void zb_paine11(edict_t *e) {
    e->v.frame = ZB_PAINE11;
    e->v.nextthink = g->time + 0.1f + 5.0f;
    e->v.think = zb_paine12;
    e->v.health = 60;
}
static void zb_paine12(edict_t *e) {
    e->v.health = 60;
    eng->SV_StartSound(e, CHAN_VOICE, "zombie/z_idle.wav", 1, ATTN_IDLE);
    e->v.solid = SOLID_SLIDEBOX;
    if (!eng->SV_WalkMove(e, 0, 0)) {
        e->v.think  = zb_paine11;
        e->v.solid  = SOLID_NOT;
        e->v.nextthink = g->time + 0.1f;
        return;
    }
    FRAME(e,ZB_PAINE12,zb_paine13);
}
static void zb_paine13(edict_t *e) { FRAME(e,ZB_PAINE13,zb_paine14); }
static void zb_paine14(edict_t *e) { FRAME(e,ZB_PAINE14,zb_paine15); }
static void zb_paine15(edict_t *e) { FRAME(e,ZB_PAINE15,zb_paine16); }
static void zb_paine16(edict_t *e) { FRAME(e,ZB_PAINE16,zb_paine17); }
static void zb_paine17(edict_t *e) { FRAME(e,ZB_PAINE17,zb_paine18); }
static void zb_paine18(edict_t *e) { FRAME(e,ZB_PAINE18,zb_paine19); }
static void zb_paine19(edict_t *e) { FRAME(e,ZB_PAINE19,zb_paine20); }
static void zb_paine20(edict_t *e) { FRAME(e,ZB_PAINE20,zb_paine21); }
static void zb_paine21(edict_t *e) { FRAME(e,ZB_PAINE21,zb_paine22); }
static void zb_paine22(edict_t *e) { FRAME(e,ZB_PAINE22,zb_paine23); }
static void zb_paine23(edict_t *e) { FRAME(e,ZB_PAINE23,zb_paine24); }
static void zb_paine24(edict_t *e) { FRAME(e,ZB_PAINE24,zb_paine25); }
static void zb_paine25(edict_t *e) { FRAME(e,ZB_PAINE25,zb_paine26); ai_painforward(5); }
static void zb_paine26(edict_t *e) { FRAME(e,ZB_PAINE26,zb_paine27); ai_painforward(3); }
static void zb_paine27(edict_t *e) { FRAME(e,ZB_PAINE27,zb_paine28); ai_painforward(1); }
static void zb_paine28(edict_t *e) { FRAME(e,ZB_PAINE28,zb_paine29); ai_pain(1); }
static void zb_paine29(edict_t *e) { FRAME(e,ZB_PAINE29,zb_paine30); }
static void zb_paine30(edict_t *e) { FRAME(e,ZB_PAINE30,zb_run1);    }

static void zombie_pain(edict_t *self, edict_t *attacker, float take) {
    (void)attacker;
    g->self = self;
    self->v.health = 60;
    if (take < 9) return;
    if (INPAIN(self) == 2) return;
    if (take >= 25) {
        INPAIN(self) = 2;
        zb_paine1(self);
        return;
    }
    if (INPAIN(self)) {
        self->v.pain_finished = g->time + 3;
        return;
    }
    if (self->v.pain_finished > g->time) {
        INPAIN(self) = 2;
        zb_paine1(self);
        return;
    }
    INPAIN(self) = 1;
    float r = eng->Random();
    if (r < 0.25f) zb_paina1(self);
    else if (r < 0.5f) zb_painb1(self);
    else if (r < 0.75f) zb_painc1(self);
    else zb_paind1(self);
}

static void zombie_die(edict_t *self) {
    g->self = self;
    // Zombie always gibs
    eng->SV_StartSound(self, CHAN_VOICE, "zombie/z_gib.wav", 1, ATTN_NORM);
    ThrowHead("progs/h_zombie.mdl", self->v.health);
    ThrowGib("progs/gib1.mdl", self->v.health);
    ThrowGib("progs/gib2.mdl", self->v.health);
    ThrowGib("progs/gib3.mdl", self->v.health);
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_zombie(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/zombie.mdl");
    eng->PrecacheModel("progs/h_zombie.mdl");
    eng->PrecacheModel("progs/zom_gib.mdl");
    eng->PrecacheSound("zombie/z_idle.wav");
    eng->PrecacheSound("zombie/z_idle1.wav");
    eng->PrecacheSound("zombie/z_shot1.wav");
    eng->PrecacheSound("zombie/z_gib.wav");
    eng->PrecacheSound("zombie/z_pain.wav");
    eng->PrecacheSound("zombie/z_pain1.wav");
    eng->PrecacheSound("zombie/z_fall.wav");
    eng->PrecacheSound("zombie/z_miss.wav");
    eng->PrecacheSound("zombie/z_hit.wav");
    eng->PrecacheSound("zombie/idle_w2.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/zombie.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health    = 60;
    e->v.th_stand  = zb_stand1;
    e->v.th_walk   = zb_walk1;
    e->v.th_run    = zb_run1;
    e->v.th_pain   = zombie_pain;
    e->v.th_die    = zombie_die;
    e->v.th_missile = zombie_missile;
    if ((int)e->v.spawnflags & SPAWN_CRUCIFIED) {
        e->v.movetype = MOVETYPE_NONE;
        zb_cruc1(e);
    } else {
        walkmonster_start(e);
    }
}

// monster_ogre.c -- Ogre (ogre.qc port).

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
extern void ai_pain(float dist);
extern void ai_forward(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void Corpse_LayProne(edict_t *self);
extern void walkmonster_start(edict_t *self);
extern int CanDamage(edict_t *targ, edict_t *inflictor);
extern void SpawnMeatSpray(vec3_t org, vec3_t vel);
extern void DropBackpack(edict_t *e);
extern void SUB_Remove(edict_t *e);

static float crandom(void) { return eng->Random() * 2.0f - 1.0f; }

// Frame indices
// stand1-9: 0-8
// walk1-16: 9-24
// run1-8: 25-32
// swing1-14: 33-46
// smash1-14: 47-60
// shoot1-6: 61-66
// pain1-5: 67-71
// painb1-3: 72-74
// painc1-6: 75-80
// paind1-16: 81-96
// paine1-15: 97-111
// death1-14: 112-125
// bdeath1-10: 126-135
// pull1-11: 136-146
enum {
    OG_STAND1=0,OG_STAND2,OG_STAND3,OG_STAND4,OG_STAND5,
    OG_STAND6,OG_STAND7,OG_STAND8,OG_STAND9,
    OG_WALK1,OG_WALK2,OG_WALK3,OG_WALK4,OG_WALK5,OG_WALK6,OG_WALK7,
    OG_WALK8,OG_WALK9,OG_WALK10,OG_WALK11,OG_WALK12,OG_WALK13,OG_WALK14,
    OG_WALK15,OG_WALK16,
    OG_RUN1,OG_RUN2,OG_RUN3,OG_RUN4,OG_RUN5,OG_RUN6,OG_RUN7,OG_RUN8,
    OG_SWING1,OG_SWING2,OG_SWING3,OG_SWING4,OG_SWING5,OG_SWING6,OG_SWING7,
    OG_SWING8,OG_SWING9,OG_SWING10,OG_SWING11,OG_SWING12,OG_SWING13,OG_SWING14,
    OG_SMASH1,OG_SMASH2,OG_SMASH3,OG_SMASH4,OG_SMASH5,OG_SMASH6,OG_SMASH7,
    OG_SMASH8,OG_SMASH9,OG_SMASH10,OG_SMASH11,OG_SMASH12,OG_SMASH13,OG_SMASH14,
    OG_SHOOT1,OG_SHOOT2,OG_SHOOT3,OG_SHOOT4,OG_SHOOT5,OG_SHOOT6,
    OG_PAIN1,OG_PAIN2,OG_PAIN3,OG_PAIN4,OG_PAIN5,
    OG_PAINB1,OG_PAINB2,OG_PAINB3,
    OG_PAINC1,OG_PAINC2,OG_PAINC3,OG_PAINC4,OG_PAINC5,OG_PAINC6,
    OG_PAIND1,OG_PAIND2,OG_PAIND3,OG_PAIND4,OG_PAIND5,OG_PAIND6,
    OG_PAIND7,OG_PAIND8,OG_PAIND9,OG_PAIND10,OG_PAIND11,OG_PAIND12,
    OG_PAIND13,OG_PAIND14,OG_PAIND15,OG_PAIND16,
    OG_PAINE1,OG_PAINE2,OG_PAINE3,OG_PAINE4,OG_PAINE5,OG_PAINE6,
    OG_PAINE7,OG_PAINE8,OG_PAINE9,OG_PAINE10,OG_PAINE11,OG_PAINE12,
    OG_PAINE13,OG_PAINE14,OG_PAINE15,
    OG_DEATH1,OG_DEATH2,OG_DEATH3,OG_DEATH4,OG_DEATH5,OG_DEATH6,
    OG_DEATH7,OG_DEATH8,OG_DEATH9,OG_DEATH10,OG_DEATH11,OG_DEATH12,
    OG_DEATH13,OG_DEATH14,
    OG_BDEATH1,OG_BDEATH2,OG_BDEATH3,OG_BDEATH4,OG_BDEATH5,
    OG_BDEATH6,OG_BDEATH7,OG_BDEATH8,OG_BDEATH9,OG_BDEATH10
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Forward declarations
static void og_run1(edict_t *e);
static void og_die1(edict_t *e),og_bdie1(edict_t *e);
static void og_swing1(edict_t *e),og_smash1(edict_t *e),og_nail1(edict_t *e);

// --- Explosion sequence for grenade ---
static void og_s_explode2(edict_t *e),og_s_explode3(edict_t *e),og_s_explode4(edict_t *e),
            og_s_explode5(edict_t *e),og_s_explode6(edict_t *e);
static void og_s_explode1(edict_t *e) { FRAME(e,0,og_s_explode2); }
static void og_s_explode2(edict_t *e) { FRAME(e,1,og_s_explode3); }
static void og_s_explode3(edict_t *e) { FRAME(e,2,og_s_explode4); }
static void og_s_explode4(edict_t *e) { FRAME(e,3,og_s_explode5); }
static void og_s_explode5(edict_t *e) { FRAME(e,4,og_s_explode6); }
static void og_s_explode6(edict_t *e) { FRAME(e,5,SUB_Remove); }

static void OgreGrenadeExplode(edict_t *self) {
    g->self = self;
    T_RadiusDamage(self, self->v.owner, 40, g->world);
    eng->SV_StartSound(self, CHAN_VOICE, "weapons/r_exp3.wav", 1, ATTN_NORM);
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    self->v.velocity[0] = 0; self->v.velocity[1] = 0; self->v.velocity[2] = 0;
    self->v.touch = NULL;
    eng->SV_SetModel(self, "progs/s_explod.spr");
    self->v.solid = SOLID_NOT;
    og_s_explode1(self);
}

static void OgreGrenadeTouch(edict_t *self, edict_t *other) {
    g->self = self;
    if (other == self->v.owner) return;
    if (other->v.takedamage == DAMAGE_AIM) {
        OgreGrenadeExplode(self);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "weapons/bounce.wav", 1, ATTN_NORM);
    float vx = self->v.velocity[0], vy = self->v.velocity[1], vz = self->v.velocity[2];
    if (vx == 0 && vy == 0 && vz == 0) {
        self->v.avelocity[0] = 0; self->v.avelocity[1] = 0; self->v.avelocity[2] = 0;
    }
}

static void OgreFireGrenade(void) {
    edict_t *self = g->self;
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/grenade.wav", 1, ATTN_NORM);
    eng->MakeVectors(self->v.angles);
    edict_t *missile = eng->ED_Alloc();
    missile->v.classname = "ogre_grenade";
    missile->v.owner    = self;
    missile->v.movetype = MOVETYPE_BOUNCE;
    missile->v.solid    = SOLID_BBOX;
    float dx = self->v.enemy->v.origin[0] - self->v.origin[0];
    float dy = self->v.enemy->v.origin[1] - self->v.origin[1];
    float dz = self->v.enemy->v.origin[2] - self->v.origin[2];
    float len = (float)sqrt((double)(dx*dx+dy*dy+dz*dz));
    if (len < 0.001f) len = 0.001f;
    missile->v.velocity[0] = dx/len * 600;
    missile->v.velocity[1] = dy/len * 600;
    missile->v.velocity[2] = dz/len * 600 + 200;
    missile->v.avelocity[0] = 300; missile->v.avelocity[1] = 300; missile->v.avelocity[2] = 300;
    // angles = vectoangles(velocity)
    float yaw = eng->VectorToYaw(missile->v.velocity);
    missile->v.angles[0] = 0; missile->v.angles[1] = yaw; missile->v.angles[2] = 0;
    missile->v.touch      = OgreGrenadeTouch;
    missile->v.nextthink  = g->time + 2.5f;
    missile->v.think      = OgreGrenadeExplode;
    eng->SV_SetModel(missile, "progs/grenade.mdl");
    vec3_t z = {0,0,0};
    eng->SV_SetSize(missile, z, z);
    eng->SV_SetOrigin(missile, self->v.origin);
}

static void chainsaw(float side) {
    edict_t *e = g->self;
    if (!e->v.enemy) return;
    if (!CanDamage(e->v.enemy, e)) return;
    ai_charge(10);
    float dx = e->v.enemy->v.origin[0]-e->v.origin[0];
    float dy = e->v.enemy->v.origin[1]-e->v.origin[1];
    float dz = e->v.enemy->v.origin[2]-e->v.origin[2];
    float dist = (float)sqrt((double)(dx*dx+dy*dy+dz*dz));
    if (dist > 100) return;
    float ldmg = (eng->Random() + eng->Random() + eng->Random()) * 4;
    T_Damage(e->v.enemy, e, e, ldmg);
    if (side) {
        eng->MakeVectors(e->v.angles);
        if (side == 1) {
            float cr = crandom() * 100;
            vec3_t vel = { cr*g->v_right[0], cr*g->v_right[1], cr*g->v_right[2] };
            vec3_t org = { e->v.origin[0]+g->v_forward[0]*16,
                           e->v.origin[1]+g->v_forward[1]*16,
                           e->v.origin[2]+g->v_forward[2]*16 };
            SpawnMeatSpray(org, vel);
        } else {
            vec3_t vel = { side*g->v_right[0], side*g->v_right[1], side*g->v_right[2] };
            vec3_t org = { e->v.origin[0]+g->v_forward[0]*16,
                           e->v.origin[1]+g->v_forward[1]*16,
                           e->v.origin[2]+g->v_forward[2]*16 };
            SpawnMeatSpray(org, vel);
        }
    }
}

// Stand
static void og_stand2(edict_t *e),og_stand3(edict_t *e),og_stand4(edict_t *e),
            og_stand5(edict_t *e),og_stand6(edict_t *e),og_stand7(edict_t *e),
            og_stand8(edict_t *e),og_stand9(edict_t *e);
static void og_stand1(edict_t *e) { FRAME(e,OG_STAND1,og_stand2); ai_stand(e); }
static void og_stand2(edict_t *e) { FRAME(e,OG_STAND2,og_stand3); ai_stand(e); }
static void og_stand3(edict_t *e) { FRAME(e,OG_STAND3,og_stand4); ai_stand(e); }
static void og_stand4(edict_t *e) { FRAME(e,OG_STAND4,og_stand5); ai_stand(e); }
static void og_stand5(edict_t *e) {
    FRAME(e,OG_STAND5,og_stand6);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "ogre/ogidle.wav", 1, ATTN_IDLE);
    ai_stand(e);
}
static void og_stand6(edict_t *e) { FRAME(e,OG_STAND6,og_stand7); ai_stand(e); }
static void og_stand7(edict_t *e) { FRAME(e,OG_STAND7,og_stand8); ai_stand(e); }
static void og_stand8(edict_t *e) { FRAME(e,OG_STAND8,og_stand9); ai_stand(e); }
static void og_stand9(edict_t *e) { FRAME(e,OG_STAND9,og_stand1); ai_stand(e); }

// Walk
static void og_walk2(edict_t *e),og_walk3(edict_t *e),og_walk4(edict_t *e),og_walk5(edict_t *e),
            og_walk6(edict_t *e),og_walk7(edict_t *e),og_walk8(edict_t *e),og_walk9(edict_t *e),
            og_walk10(edict_t *e),og_walk11(edict_t *e),og_walk12(edict_t *e),og_walk13(edict_t *e),
            og_walk14(edict_t *e),og_walk15(edict_t *e),og_walk16(edict_t *e);
static void og_walk1(edict_t *e)  { FRAME(e,OG_WALK1, og_walk2);  ai_walk(3); }
static void og_walk2(edict_t *e)  { FRAME(e,OG_WALK2, og_walk3);  ai_walk(2); }
static void og_walk3(edict_t *e)  {
    FRAME(e,OG_WALK3,og_walk4); ai_walk(2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "ogre/ogidle.wav", 1, ATTN_IDLE);
}
static void og_walk4(edict_t *e)  { FRAME(e,OG_WALK4, og_walk5);  ai_walk(2); }
static void og_walk5(edict_t *e)  { FRAME(e,OG_WALK5, og_walk6);  ai_walk(2); }
static void og_walk6(edict_t *e)  {
    FRAME(e,OG_WALK6,og_walk7); ai_walk(5);
    if (eng->Random() < 0.1f) eng->SV_StartSound(e, CHAN_VOICE, "ogre/ogdrag.wav", 1, ATTN_IDLE);
}
static void og_walk7(edict_t *e)  { FRAME(e,OG_WALK7, og_walk8);  ai_walk(3); }
static void og_walk8(edict_t *e)  { FRAME(e,OG_WALK8, og_walk9);  ai_walk(2); }
static void og_walk9(edict_t *e)  { FRAME(e,OG_WALK9, og_walk10); ai_walk(3); }
static void og_walk10(edict_t *e) { FRAME(e,OG_WALK10,og_walk11); ai_walk(1); }
static void og_walk11(edict_t *e) { FRAME(e,OG_WALK11,og_walk12); ai_walk(2); }
static void og_walk12(edict_t *e) { FRAME(e,OG_WALK12,og_walk13); ai_walk(3); }
static void og_walk13(edict_t *e) { FRAME(e,OG_WALK13,og_walk14); ai_walk(3); }
static void og_walk14(edict_t *e) { FRAME(e,OG_WALK14,og_walk15); ai_walk(3); }
static void og_walk15(edict_t *e) { FRAME(e,OG_WALK15,og_walk16); ai_walk(3); }
static void og_walk16(edict_t *e) { FRAME(e,OG_WALK16,og_walk1);  ai_walk(4); }

// Run
static void og_run2(edict_t *e),og_run3(edict_t *e),og_run4(edict_t *e),og_run5(edict_t *e),
            og_run6(edict_t *e),og_run7(edict_t *e),og_run8(edict_t *e);
static void og_run1(edict_t *e) {
    FRAME(e,OG_RUN1,og_run2); ai_run(9);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "ogre/ogidle2.wav", 1, ATTN_IDLE);
}
static void og_run2(edict_t *e) { FRAME(e,OG_RUN2,og_run3); ai_run(12); }
static void og_run3(edict_t *e) { FRAME(e,OG_RUN3,og_run4); ai_run(8);  }
static void og_run4(edict_t *e) { FRAME(e,OG_RUN4,og_run5); ai_run(22); }
static void og_run5(edict_t *e) { FRAME(e,OG_RUN5,og_run6); ai_run(16); }
static void og_run6(edict_t *e) { FRAME(e,OG_RUN6,og_run7); ai_run(4);  }
static void og_run7(edict_t *e) { FRAME(e,OG_RUN7,og_run8); ai_run(13); }
static void og_run8(edict_t *e) { FRAME(e,OG_RUN8,og_run1); ai_run(24); }

// Swing melee
static void og_swing2(edict_t *e),og_swing3(edict_t *e),og_swing4(edict_t *e),og_swing5(edict_t *e),
            og_swing6(edict_t *e),og_swing7(edict_t *e),og_swing8(edict_t *e),og_swing9(edict_t *e),
            og_swing10(edict_t *e),og_swing11(edict_t *e),og_swing12(edict_t *e),og_swing13(edict_t *e),
            og_swing14(edict_t *e);
static void og_swing1(edict_t *e) {
    FRAME(e,OG_SWING1,og_swing2); ai_charge(11);
    eng->SV_StartSound(e, CHAN_WEAPON, "ogre/ogsawatk.wav", 1, ATTN_NORM);
}
static void og_swing2(edict_t *e)  { FRAME(e,OG_SWING2, og_swing3);  ai_charge(1); }
static void og_swing3(edict_t *e)  { FRAME(e,OG_SWING3, og_swing4);  ai_charge(4); }
static void og_swing4(edict_t *e)  { FRAME(e,OG_SWING4, og_swing5);  ai_charge(13);}
static void og_swing5(edict_t *e)  {
    FRAME(e,OG_SWING5,og_swing6); ai_charge(9); chainsaw(0);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing6(edict_t *e)  {
    FRAME(e,OG_SWING6,og_swing7); chainsaw(200);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing7(edict_t *e)  {
    FRAME(e,OG_SWING7,og_swing8); chainsaw(0);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing8(edict_t *e)  {
    FRAME(e,OG_SWING8,og_swing9); chainsaw(0);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing9(edict_t *e)  {
    FRAME(e,OG_SWING9,og_swing10); chainsaw(0);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing10(edict_t *e) {
    FRAME(e,OG_SWING10,og_swing11); chainsaw(-200);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing11(edict_t *e) {
    FRAME(e,OG_SWING11,og_swing12); chainsaw(0);
    e->v.angles[1] += eng->Random() * 25;
}
static void og_swing12(edict_t *e) { FRAME(e,OG_SWING12,og_swing13); ai_charge(3); }
static void og_swing13(edict_t *e) { FRAME(e,OG_SWING13,og_swing14); ai_charge(8); }
static void og_swing14(edict_t *e) { FRAME(e,OG_SWING14,og_run1);    ai_charge(9); }

// Smash melee
static void og_smash2(edict_t *e),og_smash3(edict_t *e),og_smash4(edict_t *e),og_smash5(edict_t *e),
            og_smash6(edict_t *e),og_smash7(edict_t *e),og_smash8(edict_t *e),og_smash9(edict_t *e),
            og_smash10(edict_t *e),og_smash11(edict_t *e),og_smash12(edict_t *e),og_smash13(edict_t *e),
            og_smash14(edict_t *e);
static void og_smash1(edict_t *e) {
    FRAME(e,OG_SMASH1,og_smash2); ai_charge(6);
    eng->SV_StartSound(e, CHAN_WEAPON, "ogre/ogsawatk.wav", 1, ATTN_NORM);
}
static void og_smash2(edict_t *e)  { FRAME(e,OG_SMASH2, og_smash3);  ai_charge(0); }
static void og_smash3(edict_t *e)  { FRAME(e,OG_SMASH3, og_smash4);  ai_charge(0); }
static void og_smash4(edict_t *e)  { FRAME(e,OG_SMASH4, og_smash5);  ai_charge(1); }
static void og_smash5(edict_t *e)  { FRAME(e,OG_SMASH5, og_smash6);  ai_charge(4); }
static void og_smash6(edict_t *e)  { FRAME(e,OG_SMASH6, og_smash7);  ai_charge(4);  chainsaw(0); }
static void og_smash7(edict_t *e)  { FRAME(e,OG_SMASH7, og_smash8);  ai_charge(4);  chainsaw(0); }
static void og_smash8(edict_t *e)  { FRAME(e,OG_SMASH8, og_smash9);  ai_charge(10); chainsaw(0); }
static void og_smash9(edict_t *e)  { FRAME(e,OG_SMASH9, og_smash10); ai_charge(13); chainsaw(0); }
static void og_smash10(edict_t *e) { FRAME(e,OG_SMASH10,og_smash11); chainsaw(1); }
static void og_smash11(edict_t *e) {
    FRAME(e,OG_SMASH11,og_smash12); ai_charge(2); chainsaw(0);
    e->v.nextthink += eng->Random() * 0.2f;
}
static void og_smash12(edict_t *e) { FRAME(e,OG_SMASH12,og_smash13); ai_charge(0); }
static void og_smash13(edict_t *e) { FRAME(e,OG_SMASH13,og_smash14); ai_charge(4); }
static void og_smash14(edict_t *e) { FRAME(e,OG_SMASH14,og_run1);    ai_charge(12);}

// Grenade attack
static void og_nail2(edict_t *e),og_nail3(edict_t *e),og_nail4(edict_t *e),
            og_nail5(edict_t *e),og_nail6(edict_t *e),og_nail7(edict_t *e);
static void og_nail1(edict_t *e)  { FRAME(e,OG_SHOOT1,og_nail2); ai_face(); }
static void og_nail2(edict_t *e)  { FRAME(e,OG_SHOOT2,og_nail3); ai_face(); }
static void og_nail3(edict_t *e)  { FRAME(e,OG_SHOOT2,og_nail4); ai_face(); } // shoot2 repeated
static void og_nail4(edict_t *e)  { FRAME(e,OG_SHOOT3,og_nail5); ai_face(); OgreFireGrenade(); }
static void og_nail5(edict_t *e)  { FRAME(e,OG_SHOOT4,og_nail6); ai_face(); }
static void og_nail6(edict_t *e)  { FRAME(e,OG_SHOOT5,og_nail7); ai_face(); }
static void og_nail7(edict_t *e)  { FRAME(e,OG_SHOOT6,og_run1);  ai_face(); }

// Pain
static void og_pain2(edict_t *e),og_pain3(edict_t *e),og_pain4(edict_t *e),og_pain5(edict_t *e);
static void og_pain1(edict_t *e)  { FRAME(e,OG_PAIN1,og_pain2); }
static void og_pain2(edict_t *e)  { FRAME(e,OG_PAIN2,og_pain3); }
static void og_pain3(edict_t *e)  { FRAME(e,OG_PAIN3,og_pain4); }
static void og_pain4(edict_t *e)  { FRAME(e,OG_PAIN4,og_pain5); }
static void og_pain5(edict_t *e)  { FRAME(e,OG_PAIN5,og_run1);  }

static void og_painb2(edict_t *e),og_painb3(edict_t *e);
static void og_painb1(edict_t *e) { FRAME(e,OG_PAINB1,og_painb2); }
static void og_painb2(edict_t *e) { FRAME(e,OG_PAINB2,og_painb3); }
static void og_painb3(edict_t *e) { FRAME(e,OG_PAINB3,og_run1);   }

static void og_painc2(edict_t *e),og_painc3(edict_t *e),og_painc4(edict_t *e),
            og_painc5(edict_t *e),og_painc6(edict_t *e);
static void og_painc1(edict_t *e) { FRAME(e,OG_PAINC1,og_painc2); }
static void og_painc2(edict_t *e) { FRAME(e,OG_PAINC2,og_painc3); }
static void og_painc3(edict_t *e) { FRAME(e,OG_PAINC3,og_painc4); }
static void og_painc4(edict_t *e) { FRAME(e,OG_PAINC4,og_painc5); }
static void og_painc5(edict_t *e) { FRAME(e,OG_PAINC5,og_painc6); }
static void og_painc6(edict_t *e) { FRAME(e,OG_PAINC6,og_run1);   }

static void og_paind2(edict_t *e),og_paind3(edict_t *e),og_paind4(edict_t *e),og_paind5(edict_t *e),
            og_paind6(edict_t *e),og_paind7(edict_t *e),og_paind8(edict_t *e),og_paind9(edict_t *e),
            og_paind10(edict_t *e),og_paind11(edict_t *e),og_paind12(edict_t *e),og_paind13(edict_t *e),
            og_paind14(edict_t *e),og_paind15(edict_t *e),og_paind16(edict_t *e);
static void og_paind1(edict_t *e)  { FRAME(e,OG_PAIND1, og_paind2);  }
static void og_paind2(edict_t *e)  { FRAME(e,OG_PAIND2, og_paind3);  ai_pain(10); }
static void og_paind3(edict_t *e)  { FRAME(e,OG_PAIND3, og_paind4);  ai_pain(9);  }
static void og_paind4(edict_t *e)  { FRAME(e,OG_PAIND4, og_paind5);  ai_pain(4);  }
static void og_paind5(edict_t *e)  { FRAME(e,OG_PAIND5, og_paind6);  }
static void og_paind6(edict_t *e)  { FRAME(e,OG_PAIND6, og_paind7);  }
static void og_paind7(edict_t *e)  { FRAME(e,OG_PAIND7, og_paind8);  }
static void og_paind8(edict_t *e)  { FRAME(e,OG_PAIND8, og_paind9);  }
static void og_paind9(edict_t *e)  { FRAME(e,OG_PAIND9, og_paind10); }
static void og_paind10(edict_t *e) { FRAME(e,OG_PAIND10,og_paind11); }
static void og_paind11(edict_t *e) { FRAME(e,OG_PAIND11,og_paind12); }
static void og_paind12(edict_t *e) { FRAME(e,OG_PAIND12,og_paind13); }
static void og_paind13(edict_t *e) { FRAME(e,OG_PAIND13,og_paind14); }
static void og_paind14(edict_t *e) { FRAME(e,OG_PAIND14,og_paind15); }
static void og_paind15(edict_t *e) { FRAME(e,OG_PAIND15,og_paind16); }
static void og_paind16(edict_t *e) { FRAME(e,OG_PAIND16,og_run1);    }

static void og_paine2(edict_t *e),og_paine3(edict_t *e),og_paine4(edict_t *e),og_paine5(edict_t *e),
            og_paine6(edict_t *e),og_paine7(edict_t *e),og_paine8(edict_t *e),og_paine9(edict_t *e),
            og_paine10(edict_t *e),og_paine11(edict_t *e),og_paine12(edict_t *e),og_paine13(edict_t *e),
            og_paine14(edict_t *e),og_paine15(edict_t *e);
static void og_paine1(edict_t *e)  { FRAME(e,OG_PAINE1, og_paine2);  }
static void og_paine2(edict_t *e)  { FRAME(e,OG_PAINE2, og_paine3);  ai_pain(10); }
static void og_paine3(edict_t *e)  { FRAME(e,OG_PAINE3, og_paine4);  ai_pain(9);  }
static void og_paine4(edict_t *e)  { FRAME(e,OG_PAINE4, og_paine5);  ai_pain(4);  }
static void og_paine5(edict_t *e)  { FRAME(e,OG_PAINE5, og_paine6);  }
static void og_paine6(edict_t *e)  { FRAME(e,OG_PAINE6, og_paine7);  }
static void og_paine7(edict_t *e)  { FRAME(e,OG_PAINE7, og_paine8);  }
static void og_paine8(edict_t *e)  { FRAME(e,OG_PAINE8, og_paine9);  }
static void og_paine9(edict_t *e)  { FRAME(e,OG_PAINE9, og_paine10); }
static void og_paine10(edict_t *e) { FRAME(e,OG_PAINE10,og_paine11); }
static void og_paine11(edict_t *e) { FRAME(e,OG_PAINE11,og_paine12); }
static void og_paine12(edict_t *e) { FRAME(e,OG_PAINE12,og_paine13); }
static void og_paine13(edict_t *e) { FRAME(e,OG_PAINE13,og_paine14); }
static void og_paine14(edict_t *e) { FRAME(e,OG_PAINE14,og_paine15); }
static void og_paine15(edict_t *e) { FRAME(e,OG_PAINE15,og_run1);    }

static void ogre_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    g->self = self;
    if (self->v.pain_finished > g->time) return;
    eng->SV_StartSound(self, CHAN_VOICE, "ogre/ogpain1.wav", 1, ATTN_NORM);
    float r = eng->Random();
    if (r < 0.25f)       { og_pain1(self);  self->v.pain_finished = g->time + 1; }
    else if (r < 0.5f)   { og_painb1(self); self->v.pain_finished = g->time + 1; }
    else if (r < 0.75f)  { og_painc1(self); self->v.pain_finished = g->time + 1; }
    else if (r < 0.88f)  { og_paind1(self); self->v.pain_finished = g->time + 2; }
    else                 { og_paine1(self); self->v.pain_finished = g->time + 2; }
}

// Death
static void og_die2(edict_t *e),og_die3(edict_t *e),og_die4(edict_t *e),og_die5(edict_t *e),
            og_die6(edict_t *e),og_die7(edict_t *e),og_die8(edict_t *e),og_die9(edict_t *e),
            og_die10(edict_t *e),og_die11(edict_t *e),og_die12(edict_t *e),og_die13(edict_t *e),
            og_die14(edict_t *e);
static void og_die1(edict_t *e)  { FRAME(e,OG_DEATH1, og_die2);  }
static void og_die2(edict_t *e)  { FRAME(e,OG_DEATH2, og_die3);  }
static void og_die3(edict_t *e)  {
    FRAME(e,OG_DEATH3,og_die4); Corpse_LayProne(e);
    e->v.ammo_rockets = 2; DropBackpack(e);
}
static void og_die4(edict_t *e)  { FRAME(e,OG_DEATH4, og_die5);  }
static void og_die5(edict_t *e)  { FRAME(e,OG_DEATH5, og_die6);  }
static void og_die6(edict_t *e)  { FRAME(e,OG_DEATH6, og_die7);  }
static void og_die7(edict_t *e)  { FRAME(e,OG_DEATH7, og_die8);  }
static void og_die8(edict_t *e)  { FRAME(e,OG_DEATH8, og_die9);  }
static void og_die9(edict_t *e)  { FRAME(e,OG_DEATH9, og_die10); }
static void og_die10(edict_t *e) { FRAME(e,OG_DEATH10,og_die11); }
static void og_die11(edict_t *e) { FRAME(e,OG_DEATH11,og_die12); }
static void og_die12(edict_t *e) { FRAME(e,OG_DEATH12,og_die13); }
static void og_die13(edict_t *e) { FRAME(e,OG_DEATH13,og_die14); }
static void og_die14(edict_t *e) { FRAME(e,OG_DEATH14,og_die14); }

static void og_bdie2(edict_t *e),og_bdie3(edict_t *e),og_bdie4(edict_t *e),og_bdie5(edict_t *e),
            og_bdie6(edict_t *e),og_bdie7(edict_t *e),og_bdie8(edict_t *e),og_bdie9(edict_t *e),
            og_bdie10(edict_t *e);
static void og_bdie1(edict_t *e)  { FRAME(e,OG_BDEATH1,og_bdie2);  }
static void og_bdie2(edict_t *e)  { FRAME(e,OG_BDEATH2,og_bdie3);  ai_forward(5); }
static void og_bdie3(edict_t *e)  {
    FRAME(e,OG_BDEATH3,og_bdie4); Corpse_LayProne(e);
    e->v.ammo_rockets = 2; DropBackpack(e);
}
static void og_bdie4(edict_t *e)  { FRAME(e,OG_BDEATH4,og_bdie5);  ai_forward(1); }
static void og_bdie5(edict_t *e)  { FRAME(e,OG_BDEATH5,og_bdie6);  ai_forward(3); }
static void og_bdie6(edict_t *e)  { FRAME(e,OG_BDEATH6,og_bdie7);  ai_forward(7); }
static void og_bdie7(edict_t *e)  { FRAME(e,OG_BDEATH7,og_bdie8);  ai_forward(25);}
static void og_bdie8(edict_t *e)  { FRAME(e,OG_BDEATH8,og_bdie9);  }
static void og_bdie9(edict_t *e)  { FRAME(e,OG_BDEATH9,og_bdie10); }
static void og_bdie10(edict_t *e) { FRAME(e,OG_BDEATH10,og_bdie10);}

static void ogre_die(edict_t *self) {
    g->self = self;
    if (self->v.health < -80) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_ogre.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "ogre/ogdth.wav", 1, ATTN_NORM);
    if (eng->Random() < 0.5f) og_die1(self);
    else og_bdie1(self);
}

static void ogre_melee(edict_t *e) {
    g->self = e;
    if (eng->Random() > 0.5f) og_smash1(e);
    else og_swing1(e);
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_ogre(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/ogre.mdl");
    eng->PrecacheModel("progs/h_ogre.mdl");
    eng->PrecacheModel("progs/grenade.mdl");
    eng->PrecacheSound("ogre/ogdrag.wav");
    eng->PrecacheSound("ogre/ogdth.wav");
    eng->PrecacheSound("ogre/ogidle.wav");
    eng->PrecacheSound("ogre/ogidle2.wav");
    eng->PrecacheSound("ogre/ogpain1.wav");
    eng->PrecacheSound("ogre/ogsawatk.wav");
    eng->PrecacheSound("ogre/ogwake.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/ogre.mdl");
    vec3_t mins = {VEC_HULL2_MIN_X,VEC_HULL2_MIN_Y,VEC_HULL2_MIN_Z};
    vec3_t maxs = {VEC_HULL2_MAX_X,VEC_HULL2_MAX_Y,VEC_HULL2_MAX_Z};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health    = 200;
    e->v.th_stand  = og_stand1;
    e->v.th_walk   = og_walk1;
    e->v.th_run    = og_run1;
    e->v.th_die    = ogre_die;
    e->v.th_melee  = ogre_melee;
    e->v.th_missile = og_nail1;
    e->v.th_pain   = ogre_pain;
    walkmonster_start(e);
}

// marksman variant is identical
void spawn_monster_ogre_marksman(edict_t *e) { spawn_monster_ogre(e); }

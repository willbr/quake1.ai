// monster_shalrath.c -- Vore/Shalrath (shalrath.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_face(void);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void Corpse_LayProne(edict_t *self);
extern void walkmonster_start(edict_t *self);
extern void SUB_Remove(edict_t *e);

// Frame indices (sequential from $frame directives)
// attack1-11 (0-10), pain1-5 (11-15), death1-7 (16-22), walk1-12 (23-34)
enum {
    SH_ATTACK1=0,SH_ATTACK2,SH_ATTACK3,SH_ATTACK4,SH_ATTACK5,
    SH_ATTACK6,SH_ATTACK7,SH_ATTACK8,SH_ATTACK9,SH_ATTACK10,SH_ATTACK11,
    SH_PAIN1,SH_PAIN2,SH_PAIN3,SH_PAIN4,SH_PAIN5,
    SH_DEATH1,SH_DEATH2,SH_DEATH3,SH_DEATH4,SH_DEATH5,SH_DEATH6,SH_DEATH7,
    SH_WALK1,SH_WALK2,SH_WALK3,SH_WALK4,SH_WALK5,SH_WALK6,
    SH_WALK7,SH_WALK8,SH_WALK9,SH_WALK10,SH_WALK11,SH_WALK12
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void shal_stand(edict_t *e);
static void shal_walk1(edict_t *e),shal_walk2(edict_t *e),shal_walk3(edict_t *e),
            shal_walk4(edict_t *e),shal_walk5(edict_t *e),shal_walk6(edict_t *e),
            shal_walk7(edict_t *e),shal_walk8(edict_t *e),shal_walk9(edict_t *e),
            shal_walk10(edict_t *e),shal_walk11(edict_t *e),shal_walk12(edict_t *e);
static void shal_run1(edict_t *e),shal_run2(edict_t *e),shal_run3(edict_t *e),
            shal_run4(edict_t *e),shal_run5(edict_t *e),shal_run6(edict_t *e),
            shal_run7(edict_t *e),shal_run8(edict_t *e),shal_run9(edict_t *e),
            shal_run10(edict_t *e),shal_run11(edict_t *e),shal_run12(edict_t *e);
static void shal_attack1(edict_t *e),shal_attack2(edict_t *e),shal_attack3(edict_t *e),
            shal_attack4(edict_t *e),shal_attack5(edict_t *e),shal_attack6(edict_t *e),
            shal_attack7(edict_t *e),shal_attack8(edict_t *e),shal_attack9(edict_t *e),
            shal_attack10(edict_t *e),shal_attack11(edict_t *e);
static void shal_pain1(edict_t *e),shal_pain2(edict_t *e),shal_pain3(edict_t *e),
            shal_pain4(edict_t *e),shal_pain5(edict_t *e);
static void shal_death1(edict_t *e),shal_death2(edict_t *e),shal_death3(edict_t *e),
            shal_death4(edict_t *e),shal_death5(edict_t *e),shal_death6(edict_t *e),
            shal_death7(edict_t *e);

// ---------------------------------------------------------------------------
// Local explosion animation (mirrors s_explode1-6 from weapons.c)
// ---------------------------------------------------------------------------
static void sh_s_explode6(edict_t *e) { FRAME(e,5,sh_s_explode6); SUB_Remove(e); }
static void sh_s_explode5(edict_t *e) { FRAME(e,4,sh_s_explode6); }
static void sh_s_explode4(edict_t *e) { FRAME(e,3,sh_s_explode5); }
static void sh_s_explode3(edict_t *e) { FRAME(e,2,sh_s_explode4); }
static void sh_s_explode2(edict_t *e) { FRAME(e,1,sh_s_explode3); }
static void sh_s_explode1(edict_t *e) { FRAME(e,0,sh_s_explode2); }

// ---------------------------------------------------------------------------
// Missile homing think
// ---------------------------------------------------------------------------
static void ShalHome(edict_t *e);
static void ShalMissileTouch(edict_t *self, edict_t *other);

static void ShalHome(edict_t *e) {
    if (!e->v.enemy || e->v.enemy->v.health < 1) {
        eng->ED_Free(e);
        return;
    }
    vec3_t dir;
    dir[0] = e->v.enemy->v.origin[0] - e->v.origin[0];
    dir[1] = e->v.enemy->v.origin[1] - e->v.origin[1];
    dir[2] = (e->v.enemy->v.origin[2] + 10.0f) - e->v.origin[2];
    vec3_t ndir;
    eng->VectorNormalize(dir, ndir);
    float spd = (eng->Cvar_VariableValue("skill") >= 3.0f) ? 350.0f : 250.0f;
    e->v.velocity[0] = ndir[0]*spd;
    e->v.velocity[1] = ndir[1]*spd;
    e->v.velocity[2] = ndir[2]*spd;
    e->v.nextthink = g->time + 0.2f;
    e->v.think = ShalHome;
}

static void ShalMissileTouch(edict_t *self, edict_t *other) {
    if (other == self->v.owner) return;

    if (strcmp(other->v.classname, "monster_zombie") == 0)
        T_Damage(other, self, self->v.owner, 110.0f);

    T_RadiusDamage(self, self->v.owner, 40.0f, g->world);

    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/r_exp3.wav", 1, ATTN_NORM);

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);

    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0.0f;
    self->v.touch = NULL;
    eng->SV_SetModel(self, "progs/s_explod.spr");
    self->v.solid = SOLID_NOT;
    sh_s_explode1(self);
}

static void ShalMissile(edict_t *e) {
    if (!e->v.enemy) return;

    float dx = e->v.enemy->v.origin[0] - e->v.origin[0];
    float dy = e->v.enemy->v.origin[1] - e->v.origin[1];
    float dz = (e->v.enemy->v.origin[2]+10.0f) - e->v.origin[2];
    vec3_t dir; dir[0]=dx; dir[1]=dy; dir[2]=dz;
    float dist = eng->VectorLength(dir);
    float flytime = dist * 0.002f;
    vec3_t ndir;
    eng->VectorNormalize(dir, ndir);
    if (flytime < 0.1f) flytime = 0.1f;

    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
    eng->SV_StartSound(e, CHAN_WEAPON, "shalrath/attack2.wav", 1, ATTN_NORM);

    edict_t *missile = eng->ED_Alloc();
    missile->v.owner = e;
    missile->v.solid = SOLID_BBOX;
    missile->v.movetype = MOVETYPE_FLYMISSILE;
    eng->SV_SetModel(missile, "progs/v_spike.mdl");
    vec3_t zero = {0,0,0};
    eng->SV_SetSize(missile, zero, zero);

    missile->v.origin[0] = e->v.origin[0];
    missile->v.origin[1] = e->v.origin[1];
    missile->v.origin[2] = e->v.origin[2] + 10.0f;

    missile->v.velocity[0] = ndir[0]*400.0f;
    missile->v.velocity[1] = ndir[1]*400.0f;
    missile->v.velocity[2] = ndir[2]*400.0f;

    missile->v.avelocity[0] = 300.0f;
    missile->v.avelocity[1] = 300.0f;
    missile->v.avelocity[2] = 300.0f;

    missile->v.nextthink = g->time + flytime;
    missile->v.think = ShalHome;
    missile->v.enemy = e->v.enemy;
    missile->v.touch = ShalMissileTouch;
}

// ---------------------------------------------------------------------------
// Stand
// ---------------------------------------------------------------------------
static void shal_stand(edict_t *e) { FRAME(e,SH_WALK1,shal_stand); ai_stand(e); }

// ---------------------------------------------------------------------------
// Walk
// ---------------------------------------------------------------------------
static void shal_walk1(edict_t *e) {
    FRAME(e,SH_WALK2,shal_walk2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "shalrath/idle.wav", 1, ATTN_IDLE);
    ai_walk(6);
}
static void shal_walk2(edict_t *e)  { FRAME(e,SH_WALK3, shal_walk3);  ai_walk(4); }
static void shal_walk3(edict_t *e)  { FRAME(e,SH_WALK4, shal_walk4);  ai_walk(0); }
static void shal_walk4(edict_t *e)  { FRAME(e,SH_WALK5, shal_walk5);  ai_walk(0); }
static void shal_walk5(edict_t *e)  { FRAME(e,SH_WALK6, shal_walk6);  ai_walk(0); }
static void shal_walk6(edict_t *e)  { FRAME(e,SH_WALK7, shal_walk7);  ai_walk(0); }
static void shal_walk7(edict_t *e)  { FRAME(e,SH_WALK8, shal_walk8);  ai_walk(5); }
static void shal_walk8(edict_t *e)  { FRAME(e,SH_WALK9, shal_walk9);  ai_walk(6); }
static void shal_walk9(edict_t *e)  { FRAME(e,SH_WALK10,shal_walk10); ai_walk(5); }
static void shal_walk10(edict_t *e) { FRAME(e,SH_WALK11,shal_walk11); ai_walk(0); }
static void shal_walk11(edict_t *e) { FRAME(e,SH_WALK12,shal_walk12); ai_walk(4); }
static void shal_walk12(edict_t *e) { FRAME(e,SH_WALK1, shal_walk1);  ai_walk(5); }

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
static void shal_run1(edict_t *e) {
    FRAME(e,SH_WALK2,shal_run2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "shalrath/idle.wav", 1, ATTN_IDLE);
    ai_run(6);
}
static void shal_run2(edict_t *e)  { FRAME(e,SH_WALK3, shal_run3);  ai_run(4); }
static void shal_run3(edict_t *e)  { FRAME(e,SH_WALK4, shal_run4);  ai_run(0); }
static void shal_run4(edict_t *e)  { FRAME(e,SH_WALK5, shal_run5);  ai_run(0); }
static void shal_run5(edict_t *e)  { FRAME(e,SH_WALK6, shal_run6);  ai_run(0); }
static void shal_run6(edict_t *e)  { FRAME(e,SH_WALK7, shal_run7);  ai_run(0); }
static void shal_run7(edict_t *e)  { FRAME(e,SH_WALK8, shal_run8);  ai_run(5); }
static void shal_run8(edict_t *e)  { FRAME(e,SH_WALK9, shal_run9);  ai_run(6); }
static void shal_run9(edict_t *e)  { FRAME(e,SH_WALK10,shal_run10); ai_run(5); }
static void shal_run10(edict_t *e) { FRAME(e,SH_WALK11,shal_run11); ai_run(0); }
static void shal_run11(edict_t *e) { FRAME(e,SH_WALK12,shal_run12); ai_run(4); }
static void shal_run12(edict_t *e) { FRAME(e,SH_WALK1, shal_run1);  ai_run(5); }

// ---------------------------------------------------------------------------
// Attack
// ---------------------------------------------------------------------------
static void shal_attack1(edict_t *e) {
    FRAME(e,SH_ATTACK1,shal_attack2);
    eng->SV_StartSound(e, CHAN_VOICE, "shalrath/attack.wav", 1, ATTN_NORM);
    ai_face();
}
static void shal_attack2(edict_t *e)  { FRAME(e,SH_ATTACK2, shal_attack3);  ai_face(); }
static void shal_attack3(edict_t *e)  { FRAME(e,SH_ATTACK3, shal_attack4);  ai_face(); }
static void shal_attack4(edict_t *e)  { FRAME(e,SH_ATTACK4, shal_attack5);  ai_face(); }
static void shal_attack5(edict_t *e)  { FRAME(e,SH_ATTACK5, shal_attack6);  ai_face(); }
static void shal_attack6(edict_t *e)  { FRAME(e,SH_ATTACK6, shal_attack7);  ai_face(); }
static void shal_attack7(edict_t *e)  { FRAME(e,SH_ATTACK7, shal_attack8);  ai_face(); }
static void shal_attack8(edict_t *e)  { FRAME(e,SH_ATTACK8, shal_attack9);  ai_face(); }
static void shal_attack9(edict_t *e)  { FRAME(e,SH_ATTACK9, shal_attack10); ShalMissile(e); }
static void shal_attack10(edict_t *e) { FRAME(e,SH_ATTACK10,shal_attack11); ai_face(); }
static void shal_attack11(edict_t *e) { FRAME(e,SH_ATTACK11,shal_run1);     (void)e; }

// ---------------------------------------------------------------------------
// Pain
// ---------------------------------------------------------------------------
static void shal_pain1(edict_t *e) { FRAME(e,SH_PAIN1,shal_pain2); }
static void shal_pain2(edict_t *e) { FRAME(e,SH_PAIN2,shal_pain3); }
static void shal_pain3(edict_t *e) { FRAME(e,SH_PAIN3,shal_pain4); }
static void shal_pain4(edict_t *e) { FRAME(e,SH_PAIN4,shal_pain5); }
static void shal_pain5(edict_t *e) { FRAME(e,SH_PAIN5,shal_run1);  }

static void shalrath_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    if (self->v.pain_finished > g->time) return;
    eng->SV_StartSound(self, CHAN_VOICE, "shalrath/pain.wav", 1, ATTN_NORM);
    shal_pain1(self);
    self->v.pain_finished = g->time + 3.0f;
}

// ---------------------------------------------------------------------------
// Death
// ---------------------------------------------------------------------------
static void shal_death1(edict_t *e) { FRAME(e,SH_DEATH1,shal_death2); }
static void shal_death2(edict_t *e) { FRAME(e,SH_DEATH2,shal_death3); }
static void shal_death3(edict_t *e) { FRAME(e,SH_DEATH3,shal_death4); }
static void shal_death4(edict_t *e) { FRAME(e,SH_DEATH4,shal_death5); }
static void shal_death5(edict_t *e) { FRAME(e,SH_DEATH5,shal_death6); }
static void shal_death6(edict_t *e) { FRAME(e,SH_DEATH6,shal_death7); }
static void shal_death7(edict_t *e) { FRAME(e,SH_DEATH7,shal_death7); } /* loop */

static void shalrath_die(edict_t *self) {
    if (self->v.health < -90.0f) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_shal.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "shalrath/death.wav", 1, ATTN_NORM);
    Corpse_LayProne(self);
    shal_death1(self);
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_shalrath(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }

    eng->PrecacheModel("progs/shalrath.mdl");
    eng->PrecacheModel("progs/h_shal.mdl");
    eng->PrecacheModel("progs/v_spike.mdl");
    eng->PrecacheModel("progs/s_explod.spr");

    eng->PrecacheSound("shalrath/attack.wav");
    eng->PrecacheSound("shalrath/attack2.wav");
    eng->PrecacheSound("shalrath/death.wav");
    eng->PrecacheSound("shalrath/idle.wav");
    eng->PrecacheSound("shalrath/pain.wav");
    eng->PrecacheSound("shalrath/sight.wav");
    eng->PrecacheSound("weapons/r_exp3.wav");

    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/shalrath.mdl");
    vec3_t mins = {VEC_HULL2_MIN_X, VEC_HULL2_MIN_Y, VEC_HULL2_MIN_Z};
    vec3_t maxs = {VEC_HULL2_MAX_X, VEC_HULL2_MAX_Y, VEC_HULL2_MAX_Z};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health   = 400;

    e->v.th_stand   = shal_stand;
    e->v.th_walk    = shal_walk1;
    e->v.th_run     = shal_run1;
    e->v.th_die     = shalrath_die;
    e->v.th_pain    = shalrath_pain;
    e->v.th_missile = shal_attack1;

    /* Defer walkmonster_start (matching QC: nextthink = time + 0.1 + random*0.1) */
    e->v.think     = walkmonster_start;
    e->v.nextthink = g->time + 0.1f + eng->Random() * 0.1f;
}

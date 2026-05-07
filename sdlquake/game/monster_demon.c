// monster_demon.c -- Demon/Fiend (demon.qc port).

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
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void walkmonster_start(edict_t *self);
extern int CanDamage(edict_t *targ, edict_t *inflictor);
extern void SpawnMeatSpray(vec3_t org, vec3_t vel);
extern void SUB_Remove(edict_t *e);

// fight.c
extern float enemy_range;
extern int   enemy_vis;

// Frame indices
// stand1-13: 0-12
// walk1-8: 13-20
// run1-6: 21-26
// leap1-12: 27-38
// pain1-6: 39-44
// death1-9: 45-53
// attacka1-15: 54-68
enum {
    DM_STAND1=0,DM_STAND2,DM_STAND3,DM_STAND4,DM_STAND5,DM_STAND6,
    DM_STAND7,DM_STAND8,DM_STAND9,DM_STAND10,DM_STAND11,DM_STAND12,DM_STAND13,
    DM_WALK1,DM_WALK2,DM_WALK3,DM_WALK4,DM_WALK5,DM_WALK6,DM_WALK7,DM_WALK8,
    DM_RUN1,DM_RUN2,DM_RUN3,DM_RUN4,DM_RUN5,DM_RUN6,
    DM_LEAP1,DM_LEAP2,DM_LEAP3,DM_LEAP4,DM_LEAP5,DM_LEAP6,
    DM_LEAP7,DM_LEAP8,DM_LEAP9,DM_LEAP10,DM_LEAP11,DM_LEAP12,
    DM_PAIN1,DM_PAIN2,DM_PAIN3,DM_PAIN4,DM_PAIN5,DM_PAIN6,
    DM_DEATH1,DM_DEATH2,DM_DEATH3,DM_DEATH4,DM_DEATH5,
    DM_DEATH6,DM_DEATH7,DM_DEATH8,DM_DEATH9,
    DM_ATTA1,DM_ATTA2,DM_ATTA3,DM_ATTA4,DM_ATTA5,DM_ATTA6,
    DM_ATTA7,DM_ATTA8,DM_ATTA9,DM_ATTA10,DM_ATTA11,DM_ATTA12,
    DM_ATTA13,DM_ATTA14,DM_ATTA15
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// Forward declarations
static void dm_stand1(edict_t *e),dm_stand2(edict_t *e),dm_stand3(edict_t *e),
            dm_stand4(edict_t *e),dm_stand5(edict_t *e),dm_stand6(edict_t *e),
            dm_stand7(edict_t *e),dm_stand8(edict_t *e),dm_stand9(edict_t *e),
            dm_stand10(edict_t *e),dm_stand11(edict_t *e),dm_stand12(edict_t *e),
            dm_stand13(edict_t *e);
static void dm_walk1(edict_t *e),dm_walk2(edict_t *e),dm_walk3(edict_t *e),
            dm_walk4(edict_t *e),dm_walk5(edict_t *e),dm_walk6(edict_t *e),
            dm_walk7(edict_t *e),dm_walk8(edict_t *e);
static void dm_run1(edict_t *e),dm_run2(edict_t *e),dm_run3(edict_t *e),
            dm_run4(edict_t *e),dm_run5(edict_t *e),dm_run6(edict_t *e);
static void dm_jump1(edict_t *e),dm_jump2(edict_t *e),dm_jump3(edict_t *e),
            dm_jump4(edict_t *e),dm_jump5(edict_t *e),dm_jump6(edict_t *e),
            dm_jump7(edict_t *e),dm_jump8(edict_t *e),dm_jump9(edict_t *e),
            dm_jump10(edict_t *e),dm_jump11(edict_t *e),dm_jump12(edict_t *e);
static void dm_atta1(edict_t *e),dm_atta2(edict_t *e),dm_atta3(edict_t *e),
            dm_atta4(edict_t *e),dm_atta5(edict_t *e),dm_atta6(edict_t *e),
            dm_atta7(edict_t *e),dm_atta8(edict_t *e),dm_atta9(edict_t *e),
            dm_atta10(edict_t *e),dm_atta11(edict_t *e),dm_atta12(edict_t *e),
            dm_atta13(edict_t *e),dm_atta14(edict_t *e),dm_atta15(edict_t *e);
static void dm_pain1(edict_t *e),dm_pain2(edict_t *e),dm_pain3(edict_t *e),
            dm_pain4(edict_t *e),dm_pain5(edict_t *e),dm_pain6(edict_t *e);
static void dm_die1(edict_t *e),dm_die2(edict_t *e),dm_die3(edict_t *e),
            dm_die4(edict_t *e),dm_die5(edict_t *e),dm_die6(edict_t *e),
            dm_die7(edict_t *e),dm_die8(edict_t *e),dm_die9(edict_t *e);
static void Demon_JumpTouch(edict_t *self, edict_t *other);

// Stand
static void dm_stand1(edict_t *e)  { FRAME(e,DM_STAND1, dm_stand2);  ai_stand(e); }
static void dm_stand2(edict_t *e)  { FRAME(e,DM_STAND2, dm_stand3);  ai_stand(e); }
static void dm_stand3(edict_t *e)  { FRAME(e,DM_STAND3, dm_stand4);  ai_stand(e); }
static void dm_stand4(edict_t *e)  { FRAME(e,DM_STAND4, dm_stand5);  ai_stand(e); }
static void dm_stand5(edict_t *e)  { FRAME(e,DM_STAND5, dm_stand6);  ai_stand(e); }
static void dm_stand6(edict_t *e)  { FRAME(e,DM_STAND6, dm_stand7);  ai_stand(e); }
static void dm_stand7(edict_t *e)  { FRAME(e,DM_STAND7, dm_stand8);  ai_stand(e); }
static void dm_stand8(edict_t *e)  { FRAME(e,DM_STAND8, dm_stand9);  ai_stand(e); }
static void dm_stand9(edict_t *e)  { FRAME(e,DM_STAND9, dm_stand10); ai_stand(e); }
static void dm_stand10(edict_t *e) { FRAME(e,DM_STAND10,dm_stand11); ai_stand(e); }
static void dm_stand11(edict_t *e) { FRAME(e,DM_STAND11,dm_stand12); ai_stand(e); }
static void dm_stand12(edict_t *e) { FRAME(e,DM_STAND12,dm_stand13); ai_stand(e); }
static void dm_stand13(edict_t *e) { FRAME(e,DM_STAND13,dm_stand1);  ai_stand(e); }

// Walk
static void dm_walk1(edict_t *e) {
    FRAME(e,DM_WALK1,dm_walk2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "demon/idle1.wav", 1, ATTN_IDLE);
    ai_walk(8);
}
static void dm_walk2(edict_t *e) { FRAME(e,DM_WALK2,dm_walk3); ai_walk(6); }
static void dm_walk3(edict_t *e) { FRAME(e,DM_WALK3,dm_walk4); ai_walk(6); }
static void dm_walk4(edict_t *e) { FRAME(e,DM_WALK4,dm_walk5); ai_walk(7); }
static void dm_walk5(edict_t *e) { FRAME(e,DM_WALK5,dm_walk6); ai_walk(4); }
static void dm_walk6(edict_t *e) { FRAME(e,DM_WALK6,dm_walk7); ai_walk(6); }
static void dm_walk7(edict_t *e) { FRAME(e,DM_WALK7,dm_walk8); ai_walk(10);}
static void dm_walk8(edict_t *e) { FRAME(e,DM_WALK8,dm_walk1); ai_walk(10);}

// Run
static void dm_run1(edict_t *e) {
    FRAME(e,DM_RUN1,dm_run2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "demon/idle1.wav", 1, ATTN_IDLE);
    ai_run(20);
}
static void dm_run2(edict_t *e) { FRAME(e,DM_RUN2,dm_run3); ai_run(15); }
static void dm_run3(edict_t *e) { FRAME(e,DM_RUN3,dm_run4); ai_run(36); }
static void dm_run4(edict_t *e) { FRAME(e,DM_RUN4,dm_run5); ai_run(20); }
static void dm_run5(edict_t *e) { FRAME(e,DM_RUN5,dm_run6); ai_run(15); }
static void dm_run6(edict_t *e) { FRAME(e,DM_RUN6,dm_run1); ai_run(36); }

// Jump attack
static void dm_jump1(edict_t *e)  { FRAME(e,DM_LEAP1, dm_jump2);  ai_face(); }
static void dm_jump2(edict_t *e)  { FRAME(e,DM_LEAP2, dm_jump3);  ai_face(); }
static void dm_jump3(edict_t *e)  { FRAME(e,DM_LEAP3, dm_jump4);  ai_face(); }
static void dm_jump4(edict_t *e)  {
    FRAME(e,DM_LEAP4, dm_jump5);
    ai_face();
    e->v.touch = Demon_JumpTouch;
    eng->MakeVectors(e->v.angles);
    e->v.origin[2] += 1;
    e->v.velocity[0] = g->v_forward[0] * 600;
    e->v.velocity[1] = g->v_forward[1] * 600;
    e->v.velocity[2] = g->v_forward[2] * 600 + 250;
    e->v.flags = (float)((int)e->v.flags & ~FL_ONGROUND);
}
static void dm_jump5(edict_t *e)  { FRAME(e,DM_LEAP5, dm_jump6);  }
static void dm_jump6(edict_t *e)  { FRAME(e,DM_LEAP6, dm_jump7);  }
static void dm_jump7(edict_t *e)  { FRAME(e,DM_LEAP7, dm_jump8);  }
static void dm_jump8(edict_t *e)  { FRAME(e,DM_LEAP8, dm_jump9);  }
static void dm_jump9(edict_t *e)  { FRAME(e,DM_LEAP9, dm_jump10); }
static void dm_jump10(edict_t *e) {
    // anti-stuck: extend nextthink 3 seconds; if not touched by then, jump again
    e->v.frame    = DM_LEAP10;
    e->v.nextthink = g->time + 3.0f;
    e->v.think    = dm_jump1;
}
static void dm_jump11(edict_t *e) { FRAME(e,DM_LEAP11,dm_jump12); }
static void dm_jump12(edict_t *e) { FRAME(e,DM_LEAP12,dm_run1);   }

// Melee attack
static void Demon_Melee(float side) {
    edict_t *e = g->self;
    float dx, dy, dz, dist;
    ai_face();
    eng->SV_WalkMove(e, e->v.ideal_yaw, 12);
    dx = e->v.enemy->v.origin[0] - e->v.origin[0];
    dy = e->v.enemy->v.origin[1] - e->v.origin[1];
    dz = e->v.enemy->v.origin[2] - e->v.origin[2];
    dist = (float)sqrt((double)(dx*dx + dy*dy + dz*dz));
    if (dist > 100) return;
    if (!CanDamage(e->v.enemy, e)) return;
    eng->SV_StartSound(e, CHAN_WEAPON, "demon/dhit2.wav", 1, ATTN_NORM);
    float ldmg = 10 + 5 * eng->Random();
    T_Damage(e->v.enemy, e, e, ldmg);
    eng->MakeVectors(e->v.angles);
    vec3_t spray_vel = { side * g->v_right[0], side * g->v_right[1], side * g->v_right[2] };
    vec3_t spray_org = { e->v.origin[0] + g->v_forward[0]*16,
                         e->v.origin[1] + g->v_forward[1]*16,
                         e->v.origin[2] + g->v_forward[2]*16 };
    SpawnMeatSpray(spray_org, spray_vel);
}

static void dm_atta1(edict_t *e)  { FRAME(e,DM_ATTA1, dm_atta2);  ai_charge(4); }
static void dm_atta2(edict_t *e)  { FRAME(e,DM_ATTA2, dm_atta3);  ai_charge(0); }
static void dm_atta3(edict_t *e)  { FRAME(e,DM_ATTA3, dm_atta4);  ai_charge(0); }
static void dm_atta4(edict_t *e)  { FRAME(e,DM_ATTA4, dm_atta5);  ai_charge(1); }
static void dm_atta5(edict_t *e)  { FRAME(e,DM_ATTA5, dm_atta6);  ai_charge(2); Demon_Melee(200); }
static void dm_atta6(edict_t *e)  { FRAME(e,DM_ATTA6, dm_atta7);  ai_charge(1); }
static void dm_atta7(edict_t *e)  { FRAME(e,DM_ATTA7, dm_atta8);  ai_charge(6); }
static void dm_atta8(edict_t *e)  { FRAME(e,DM_ATTA8, dm_atta9);  ai_charge(8); }
static void dm_atta9(edict_t *e)  { FRAME(e,DM_ATTA9, dm_atta10); ai_charge(4); }
static void dm_atta10(edict_t *e) { FRAME(e,DM_ATTA10,dm_atta11); ai_charge(2); }
static void dm_atta11(edict_t *e) { FRAME(e,DM_ATTA11,dm_atta12); Demon_Melee(-200); }
static void dm_atta12(edict_t *e) { FRAME(e,DM_ATTA12,dm_atta13); ai_charge(5); }
static void dm_atta13(edict_t *e) { FRAME(e,DM_ATTA13,dm_atta14); ai_charge(8); }
static void dm_atta14(edict_t *e) { FRAME(e,DM_ATTA14,dm_atta15); ai_charge(4); }
static void dm_atta15(edict_t *e) { FRAME(e,DM_ATTA15,dm_run1);   ai_charge(4); }

static void Demon_MeleeAttack(edict_t *e) { g->self = e; dm_atta1(e); }

// Pain
static void dm_pain1(edict_t *e) { FRAME(e,DM_PAIN1,dm_pain2); }
static void dm_pain2(edict_t *e) { FRAME(e,DM_PAIN2,dm_pain3); }
static void dm_pain3(edict_t *e) { FRAME(e,DM_PAIN3,dm_pain4); }
static void dm_pain4(edict_t *e) { FRAME(e,DM_PAIN4,dm_pain5); }
static void dm_pain5(edict_t *e) { FRAME(e,DM_PAIN5,dm_pain6); }
static void dm_pain6(edict_t *e) { FRAME(e,DM_PAIN6,dm_run1);  }

static void demon_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker;
    g->self = self;
    if (self->v.touch == (touchfn_t)Demon_JumpTouch) return;
    if (self->v.pain_finished > g->time) return;
    self->v.pain_finished = g->time + 1;
    eng->SV_StartSound(self, CHAN_VOICE, "demon/dpain1.wav", 1, ATTN_NORM);
    if (eng->Random() * 200 > damage) return;
    dm_pain1(self);
}

// Death
static void dm_die1(edict_t *e) {
    FRAME(e,DM_DEATH1,dm_die2);
    eng->SV_StartSound(e, CHAN_VOICE, "demon/ddeath.wav", 1, ATTN_NORM);
}
static void dm_die2(edict_t *e) { FRAME(e,DM_DEATH2,dm_die3); }
static void dm_die3(edict_t *e) { FRAME(e,DM_DEATH3,dm_die4); }
static void dm_die4(edict_t *e) { FRAME(e,DM_DEATH4,dm_die5); }
static void dm_die5(edict_t *e) { FRAME(e,DM_DEATH5,dm_die6); }
static void dm_die6(edict_t *e) { FRAME(e,DM_DEATH6,dm_die7); e->v.solid = SOLID_NOT; }
static void dm_die7(edict_t *e) { FRAME(e,DM_DEATH7,dm_die8); }
static void dm_die8(edict_t *e) { FRAME(e,DM_DEATH8,dm_die9); }
static void dm_die9(edict_t *e) { FRAME(e,DM_DEATH9,dm_die9); }

static void demon_die(edict_t *self) {
    g->self = self;
    if (self->v.health < -80) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_demon.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        return;
    }
    dm_die1(self);
}

// Attack decision (ports CheckDemonMelee, CheckDemonJump, DemonCheckAttack from demon.qc).
// Overrides the weak stub in fight.c so CheckAnyAttack dispatches here for monster_demon1.
static int CheckDemonMelee(void) {
    if ((int)enemy_range == RANGE_MELEE) {
        g->self->v.attack_state = AS_MELEE;
        return 1;
    }
    return 0;
}

static int CheckDemonJump(void) {
    edict_t *self = g->self;
    edict_t *en   = self->v.enemy;

    if (self->v.origin[2] + self->v.mins[2] >
        en->v.origin[2] + en->v.mins[2] + 0.75f * en->v.size[2])
        return 0;
    if (self->v.origin[2] + self->v.maxs[2] <
        en->v.origin[2] + en->v.mins[2] + 0.25f * en->v.size[2])
        return 0;

    float dx = en->v.origin[0] - self->v.origin[0];
    float dy = en->v.origin[1] - self->v.origin[1];
    float d  = sqrtf(dx*dx + dy*dy);

    if (d < 100) return 0;
    if (d > 200 && eng->Random() < 0.9f) return 0;
    return 1;
}

int DemonCheckAttack(void) {
    edict_t *self = g->self;
    if (CheckDemonMelee()) {
        self->v.attack_state = AS_MELEE;
        return 1;
    }
    if (CheckDemonJump()) {
        self->v.attack_state = AS_MISSILE;
        eng->SV_StartSound(self, CHAN_VOICE, "demon/djump.wav", 1, ATTN_NORM);
        return 1;
    }
    return 0;
}

// Jump touch handler
static void Demon_JumpTouch(edict_t *self, edict_t *other) {
    g->self = self;
    if (self->v.health <= 0) return;
    if (other->v.takedamage) {
        float vx = self->v.velocity[0], vy = self->v.velocity[1], vz = self->v.velocity[2];
        float speed = (float)sqrt((double)(vx*vx + vy*vy + vz*vz));
        if (speed > 400) {
            float ldmg = 40 + 10 * eng->Random();
            T_Damage(other, self, self, ldmg);
        }
    }
    if (!eng->SV_CheckBottom(self)) {
        if ((int)self->v.flags & FL_ONGROUND) {
            self->v.touch    = NULL;
            self->v.think    = dm_jump1;
            self->v.nextthink = g->time + 0.1f;
        }
        return;
    }
    self->v.touch    = NULL;
    self->v.think    = dm_jump11;
    self->v.nextthink = g->time + 0.1f;
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_demon(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/demon.mdl");
    eng->PrecacheModel("progs/h_demon.mdl");
    eng->PrecacheSound("demon/ddeath.wav");
    eng->PrecacheSound("demon/dhit2.wav");
    eng->PrecacheSound("demon/djump.wav");
    eng->PrecacheSound("demon/dpain1.wav");
    eng->PrecacheSound("demon/idle1.wav");
    eng->PrecacheSound("demon/sight2.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/demon.mdl");
    vec3_t mins = {VEC_HULL2_MIN_X,VEC_HULL2_MIN_Y,VEC_HULL2_MIN_Z};
    vec3_t maxs = {VEC_HULL2_MAX_X,VEC_HULL2_MAX_Y,VEC_HULL2_MAX_Z};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health    = 300;
    e->v.th_stand  = dm_stand1;
    e->v.th_walk   = dm_walk1;
    e->v.th_run    = dm_run1;
    e->v.th_die    = demon_die;
    e->v.th_melee  = Demon_MeleeAttack;
    e->v.th_missile = dm_jump1;
    e->v.th_pain   = demon_pain;
    walkmonster_start(e);
}

// Also register as "monster_demon1" (original QC classname)
void spawn_monster_demon1(edict_t *e) { spawn_monster_demon(e); }

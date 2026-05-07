// monster_knight.c -- Knight (knight.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_turn(edict_t *self);
extern void ai_face(void);
extern void ai_charge(float dist);
extern void ai_charge_side(void);
extern void ai_melee(void);
extern void ai_melee_side(void);
extern void ai_painforward(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void walkmonster_start(edict_t *self);

// Frame indices (sequential from $frame directives)
// stand1-9: 0-8
// runb1-8: 9-16
// runattack1-11: 17-27
// pain1-3: 28-30
// painb1-11: 31-41
// attackb1 attackb1 attackb2-10: 42-52 (11 entries, attackb1 duplicated)
// walk1-14: 53-66
// kneel1-5: 67-71
// standing2-5: 72-75
// death1-10: 76-85
// deathb1-11: 86-96
enum {
    KN_STAND1=0,KN_STAND2,KN_STAND3,KN_STAND4,KN_STAND5,
    KN_STAND6,KN_STAND7,KN_STAND8,KN_STAND9,
    KN_RUNB1,KN_RUNB2,KN_RUNB3,KN_RUNB4,KN_RUNB5,
    KN_RUNB6,KN_RUNB7,KN_RUNB8,
    KN_RUNATK1,KN_RUNATK2,KN_RUNATK3,KN_RUNATK4,KN_RUNATK5,
    KN_RUNATK6,KN_RUNATK7,KN_RUNATK8,KN_RUNATK9,KN_RUNATK10,KN_RUNATK11,
    KN_PAIN1,KN_PAIN2,KN_PAIN3,
    KN_PAINB1,KN_PAINB2,KN_PAINB3,KN_PAINB4,KN_PAINB5,
    KN_PAINB6,KN_PAINB7,KN_PAINB8,KN_PAINB9,KN_PAINB10,KN_PAINB11,
    KN_ATKB1,KN_ATKB1b,KN_ATKB2,KN_ATKB3,KN_ATKB4,KN_ATKB5,
    KN_ATKB6,KN_ATKB7,KN_ATKB8,KN_ATKB9,KN_ATKB10,
    KN_WALK1,KN_WALK2,KN_WALK3,KN_WALK4,KN_WALK5,KN_WALK6,KN_WALK7,
    KN_WALK8,KN_WALK9,KN_WALK10,KN_WALK11,KN_WALK12,KN_WALK13,KN_WALK14,
    KN_KNEEL1,KN_KNEEL2,KN_KNEEL3,KN_KNEEL4,KN_KNEEL5,
    KN_STANDING2,KN_STANDING3,KN_STANDING4,KN_STANDING5,
    KN_DEATH1,KN_DEATH2,KN_DEATH3,KN_DEATH4,KN_DEATH5,
    KN_DEATH6,KN_DEATH7,KN_DEATH8,KN_DEATH9,KN_DEATH10,
    KN_DEATHB1,KN_DEATHB2,KN_DEATHB3,KN_DEATHB4,KN_DEATHB5,
    KN_DEATHB6,KN_DEATHB7,KN_DEATHB8,KN_DEATHB9,KN_DEATHB10,KN_DEATHB11
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void kn_stand1(edict_t *e),kn_stand2(edict_t *e),kn_stand3(edict_t *e),
            kn_stand4(edict_t *e),kn_stand5(edict_t *e),kn_stand6(edict_t *e),
            kn_stand7(edict_t *e),kn_stand8(edict_t *e),kn_stand9(edict_t *e);
static void kn_walk1(edict_t *e),kn_walk2(edict_t *e),kn_walk3(edict_t *e),
            kn_walk4(edict_t *e),kn_walk5(edict_t *e),kn_walk6(edict_t *e),
            kn_walk7(edict_t *e),kn_walk8(edict_t *e),kn_walk9(edict_t *e),
            kn_walk10(edict_t *e),kn_walk11(edict_t *e),kn_walk12(edict_t *e),
            kn_walk13(edict_t *e),kn_walk14(edict_t *e);
static void kn_run1(edict_t *e),kn_run2(edict_t *e),kn_run3(edict_t *e),
            kn_run4(edict_t *e),kn_run5(edict_t *e),kn_run6(edict_t *e),
            kn_run7(edict_t *e),kn_run8(edict_t *e);
static void kn_runatk1(edict_t *e),kn_runatk2(edict_t *e),kn_runatk3(edict_t *e),
            kn_runatk4(edict_t *e),kn_runatk5(edict_t *e),kn_runatk6(edict_t *e),
            kn_runatk7(edict_t *e),kn_runatk8(edict_t *e),kn_runatk9(edict_t *e),
            kn_runatk10(edict_t *e),kn_runatk11(edict_t *e);
static void kn_atk1(edict_t *e),kn_atk2(edict_t *e),kn_atk3(edict_t *e),
            kn_atk4(edict_t *e),kn_atk5(edict_t *e),kn_atk6(edict_t *e),
            kn_atk7(edict_t *e),kn_atk8(edict_t *e),kn_atk9(edict_t *e),
            kn_atk10(edict_t *e);
static void kn_pain1(edict_t *e),kn_pain2(edict_t *e),kn_pain3(edict_t *e);
static void kn_painb1(edict_t *e),kn_painb2(edict_t *e),kn_painb3(edict_t *e),
            kn_painb4(edict_t *e),kn_painb5(edict_t *e),kn_painb6(edict_t *e),
            kn_painb7(edict_t *e),kn_painb8(edict_t *e),kn_painb9(edict_t *e),
            kn_painb10(edict_t *e),kn_painb11(edict_t *e);
static void kn_die1(edict_t *e),kn_die2(edict_t *e),kn_die3(edict_t *e),
            kn_die4(edict_t *e),kn_die5(edict_t *e),kn_die6(edict_t *e),
            kn_die7(edict_t *e),kn_die8(edict_t *e),kn_die9(edict_t *e),
            kn_die10(edict_t *e);
static void kn_dieb1(edict_t *e),kn_dieb2(edict_t *e),kn_dieb3(edict_t *e),
            kn_dieb4(edict_t *e),kn_dieb5(edict_t *e),kn_dieb6(edict_t *e),
            kn_dieb7(edict_t *e),kn_dieb8(edict_t *e),kn_dieb9(edict_t *e),
            kn_dieb10(edict_t *e),kn_dieb11(edict_t *e);

// ---------------------------------------------------------------------------
// Stand
// ---------------------------------------------------------------------------
static void kn_stand1(edict_t *e) { FRAME(e,KN_STAND1,kn_stand2); ai_stand(e); }
static void kn_stand2(edict_t *e) { FRAME(e,KN_STAND2,kn_stand3); ai_stand(e); }
static void kn_stand3(edict_t *e) { FRAME(e,KN_STAND3,kn_stand4); ai_stand(e); }
static void kn_stand4(edict_t *e) { FRAME(e,KN_STAND4,kn_stand5); ai_stand(e); }
static void kn_stand5(edict_t *e) { FRAME(e,KN_STAND5,kn_stand6); ai_stand(e); }
static void kn_stand6(edict_t *e) { FRAME(e,KN_STAND6,kn_stand7); ai_stand(e); }
static void kn_stand7(edict_t *e) { FRAME(e,KN_STAND7,kn_stand8); ai_stand(e); }
static void kn_stand8(edict_t *e) { FRAME(e,KN_STAND8,kn_stand9); ai_stand(e); }
static void kn_stand9(edict_t *e) { FRAME(e,KN_STAND9,kn_stand1); ai_stand(e); }

// ---------------------------------------------------------------------------
// Walk
// ---------------------------------------------------------------------------
static void kn_walk1(edict_t *e) {
    FRAME(e,KN_WALK1,kn_walk2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "knight/idle.wav", 1, ATTN_IDLE);
    ai_walk(3);
}
static void kn_walk2(edict_t *e)  { FRAME(e,KN_WALK2, kn_walk3);  ai_walk(2); }
static void kn_walk3(edict_t *e)  { FRAME(e,KN_WALK3, kn_walk4);  ai_walk(3); }
static void kn_walk4(edict_t *e)  { FRAME(e,KN_WALK4, kn_walk5);  ai_walk(4); }
static void kn_walk5(edict_t *e)  { FRAME(e,KN_WALK5, kn_walk6);  ai_walk(3); }
static void kn_walk6(edict_t *e)  { FRAME(e,KN_WALK6, kn_walk7);  ai_walk(3); }
static void kn_walk7(edict_t *e)  { FRAME(e,KN_WALK7, kn_walk8);  ai_walk(3); }
static void kn_walk8(edict_t *e)  { FRAME(e,KN_WALK8, kn_walk9);  ai_walk(4); }
static void kn_walk9(edict_t *e)  { FRAME(e,KN_WALK9, kn_walk10); ai_walk(3); }
static void kn_walk10(edict_t *e) { FRAME(e,KN_WALK10,kn_walk11); ai_walk(3); }
static void kn_walk11(edict_t *e) { FRAME(e,KN_WALK11,kn_walk12); ai_walk(2); }
static void kn_walk12(edict_t *e) { FRAME(e,KN_WALK12,kn_walk13); ai_walk(3); }
static void kn_walk13(edict_t *e) { FRAME(e,KN_WALK13,kn_walk14); ai_walk(4); }
static void kn_walk14(edict_t *e) { FRAME(e,KN_WALK14,kn_walk1);  ai_walk(3); }

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
static void kn_run1(edict_t *e) {
    FRAME(e,KN_RUNB1,kn_run2);
    if (eng->Random() < 0.2f) eng->SV_StartSound(e, CHAN_VOICE, "knight/idle.wav", 1, ATTN_IDLE);
    ai_run(16);
}
static void kn_run2(edict_t *e) { FRAME(e,KN_RUNB2,kn_run3); ai_run(20); }
static void kn_run3(edict_t *e) { FRAME(e,KN_RUNB3,kn_run4); ai_run(13); }
static void kn_run4(edict_t *e) { FRAME(e,KN_RUNB4,kn_run5); ai_run(7);  }
static void kn_run5(edict_t *e) { FRAME(e,KN_RUNB5,kn_run6); ai_run(16); }
static void kn_run6(edict_t *e) { FRAME(e,KN_RUNB6,kn_run7); ai_run(20); }
static void kn_run7(edict_t *e) { FRAME(e,KN_RUNB7,kn_run8); ai_run(14); }
static void kn_run8(edict_t *e) { FRAME(e,KN_RUNB8,kn_run1); ai_run(6);  }

// ---------------------------------------------------------------------------
// Run attack (runatk uses extern weak stubs from fight.c)
// ---------------------------------------------------------------------------
static void kn_runatk1(edict_t *e) {
    FRAME(e,KN_RUNATK1,kn_runatk2);
    if (eng->Random() > 0.5f)
        eng->SV_StartSound(e, CHAN_WEAPON, "knight/sword2.wav", 1, ATTN_NORM);
    else
        eng->SV_StartSound(e, CHAN_WEAPON, "knight/sword1.wav", 1, ATTN_NORM);
    ai_charge(20);
}
static void kn_runatk2(edict_t *e)  { FRAME(e,KN_RUNATK2, kn_runatk3);  ai_charge_side(); }
static void kn_runatk3(edict_t *e)  { FRAME(e,KN_RUNATK3, kn_runatk4);  ai_charge_side(); }
static void kn_runatk4(edict_t *e)  { FRAME(e,KN_RUNATK4, kn_runatk5);  ai_charge_side(); }
static void kn_runatk5(edict_t *e)  { FRAME(e,KN_RUNATK5, kn_runatk6);  ai_melee_side(); }
static void kn_runatk6(edict_t *e)  { FRAME(e,KN_RUNATK6, kn_runatk7);  ai_melee_side(); }
static void kn_runatk7(edict_t *e)  { FRAME(e,KN_RUNATK7, kn_runatk8);  ai_melee_side(); }
static void kn_runatk8(edict_t *e)  { FRAME(e,KN_RUNATK8, kn_runatk9);  ai_melee_side(); }
static void kn_runatk9(edict_t *e)  { FRAME(e,KN_RUNATK9, kn_runatk10); ai_melee_side(); }
static void kn_runatk10(edict_t *e) { FRAME(e,KN_RUNATK10,kn_runatk11); ai_charge_side(); }
static void kn_runatk11(edict_t *e) { FRAME(e,KN_RUNATK11,kn_run1);     ai_charge(10); }

// ---------------------------------------------------------------------------
// Attack (attackb sequence — attackb1 is duplicated at index 0)
// ---------------------------------------------------------------------------
static void kn_atk1(edict_t *e) {
    FRAME(e,KN_ATKB1,kn_atk2);
    eng->SV_StartSound(e, CHAN_WEAPON, "knight/sword1.wav", 1, ATTN_NORM);
    ai_charge(0);
}
static void kn_atk2(edict_t *e)  { FRAME(e,KN_ATKB2, kn_atk3);  ai_charge(7); }
static void kn_atk3(edict_t *e)  { FRAME(e,KN_ATKB3, kn_atk4);  ai_charge(4); }
static void kn_atk4(edict_t *e)  { FRAME(e,KN_ATKB4, kn_atk5);  ai_charge(0); }
static void kn_atk5(edict_t *e)  { FRAME(e,KN_ATKB5, kn_atk6);  ai_charge(3); }
static void kn_atk6(edict_t *e)  { FRAME(e,KN_ATKB6, kn_atk7);  ai_charge(4); ai_melee(); }
static void kn_atk7(edict_t *e)  { FRAME(e,KN_ATKB7, kn_atk8);  ai_charge(1); ai_melee(); }
static void kn_atk8(edict_t *e)  { FRAME(e,KN_ATKB8, kn_atk9);  ai_charge(3); ai_melee(); }
static void kn_atk9(edict_t *e)  { FRAME(e,KN_ATKB9, kn_atk10); ai_charge(1); }
static void kn_atk10(edict_t *e) { FRAME(e,KN_ATKB10,kn_run1);  ai_charge(5); }

// ---------------------------------------------------------------------------
// Pain
// ---------------------------------------------------------------------------
static void kn_pain1(edict_t *e) { FRAME(e,KN_PAIN1,kn_pain2); }
static void kn_pain2(edict_t *e) { FRAME(e,KN_PAIN2,kn_pain3); }
static void kn_pain3(edict_t *e) { FRAME(e,KN_PAIN3,kn_run1);  }

static void kn_painb1(edict_t *e)  { FRAME(e,KN_PAINB1, kn_painb2);  ai_painforward(0); }
static void kn_painb2(edict_t *e)  { FRAME(e,KN_PAINB2, kn_painb3);  ai_painforward(3); }
static void kn_painb3(edict_t *e)  { FRAME(e,KN_PAINB3, kn_painb4);  }
static void kn_painb4(edict_t *e)  { FRAME(e,KN_PAINB4, kn_painb5);  }
static void kn_painb5(edict_t *e)  { FRAME(e,KN_PAINB5, kn_painb6);  ai_painforward(2); }
static void kn_painb6(edict_t *e)  { FRAME(e,KN_PAINB6, kn_painb7);  ai_painforward(4); }
static void kn_painb7(edict_t *e)  { FRAME(e,KN_PAINB7, kn_painb8);  ai_painforward(2); }
static void kn_painb8(edict_t *e)  { FRAME(e,KN_PAINB8, kn_painb9);  ai_painforward(5); }
static void kn_painb9(edict_t *e)  { FRAME(e,KN_PAINB9, kn_painb10); ai_painforward(5); }
static void kn_painb10(edict_t *e) { FRAME(e,KN_PAINB10,kn_painb11); ai_painforward(0); }
static void kn_painb11(edict_t *e) { FRAME(e,KN_PAINB11,kn_run1);    }

static void knight_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    g->self = self;
    if (self->v.pain_finished > g->time) return;
    eng->SV_StartSound(self, CHAN_VOICE, "knight/khurt.wav", 1, ATTN_NORM);
    if (eng->Random() < 0.85f) {
        kn_pain1(self);
        self->v.pain_finished = g->time + 1;
    } else {
        kn_painb1(self);
        self->v.pain_finished = g->time + 1;
    }
}

// ---------------------------------------------------------------------------
// Death
// ---------------------------------------------------------------------------
static void kn_die1(edict_t *e)  { FRAME(e,KN_DEATH1, kn_die2);  }
static void kn_die2(edict_t *e)  { FRAME(e,KN_DEATH2, kn_die3);  }
static void kn_die3(edict_t *e)  { FRAME(e,KN_DEATH3, kn_die4);  e->v.solid = SOLID_NOT; }
static void kn_die4(edict_t *e)  { FRAME(e,KN_DEATH4, kn_die5);  }
static void kn_die5(edict_t *e)  { FRAME(e,KN_DEATH5, kn_die6);  }
static void kn_die6(edict_t *e)  { FRAME(e,KN_DEATH6, kn_die7);  }
static void kn_die7(edict_t *e)  { FRAME(e,KN_DEATH7, kn_die8);  }
static void kn_die8(edict_t *e)  { FRAME(e,KN_DEATH8, kn_die9);  }
static void kn_die9(edict_t *e)  { FRAME(e,KN_DEATH9, kn_die10); }
static void kn_die10(edict_t *e) { FRAME(e,KN_DEATH10,kn_die10); }

static void kn_dieb1(edict_t *e)  { FRAME(e,KN_DEATHB1, kn_dieb2);  }
static void kn_dieb2(edict_t *e)  { FRAME(e,KN_DEATHB2, kn_dieb3);  }
static void kn_dieb3(edict_t *e)  { FRAME(e,KN_DEATHB3, kn_dieb4);  e->v.solid = SOLID_NOT; }
static void kn_dieb4(edict_t *e)  { FRAME(e,KN_DEATHB4, kn_dieb5);  }
static void kn_dieb5(edict_t *e)  { FRAME(e,KN_DEATHB5, kn_dieb6);  }
static void kn_dieb6(edict_t *e)  { FRAME(e,KN_DEATHB6, kn_dieb7);  }
static void kn_dieb7(edict_t *e)  { FRAME(e,KN_DEATHB7, kn_dieb8);  }
static void kn_dieb8(edict_t *e)  { FRAME(e,KN_DEATHB8, kn_dieb9);  }
static void kn_dieb9(edict_t *e)  { FRAME(e,KN_DEATHB9, kn_dieb10); }
static void kn_dieb10(edict_t *e) { FRAME(e,KN_DEATHB10,kn_dieb11); }
static void kn_dieb11(edict_t *e) { FRAME(e,KN_DEATHB11,kn_dieb11); }

static void knight_die(edict_t *self) {
    g->self = self;
    if (self->v.health < -40) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_knight.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "knight/kdeath.wav", 1, ATTN_NORM);
    if (eng->Random() < 0.5f)
        kn_die1(self);
    else
        kn_dieb1(self);
}

// External overrides for the weak stubs in fight.c. CheckAttack's monster_knight
// special-case (fight.c:55-57) calls these by name to pick between melee and
// running attack — without overrides it dispatched to no-op stubs and the
// knight froze on contact.
void knight_atk1(edict_t *self)    { kn_atk1(self); }
void knight_runatk1(edict_t *self) { kn_runatk1(self); }

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_knight(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/knight.mdl");
    eng->PrecacheModel("progs/h_knight.mdl");
    eng->PrecacheSound("knight/kdeath.wav");
    eng->PrecacheSound("knight/khurt.wav");
    eng->PrecacheSound("knight/ksight.wav");
    eng->PrecacheSound("knight/sword1.wav");
    eng->PrecacheSound("knight/sword2.wav");
    eng->PrecacheSound("knight/idle.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/knight.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,40};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health   = 75;
    e->v.th_stand = kn_stand1;
    e->v.th_walk  = kn_walk1;
    e->v.th_run   = kn_run1;
    e->v.th_melee = kn_atk1;
    e->v.th_pain  = knight_pain;
    e->v.th_die   = knight_die;
    walkmonster_start(e);
}

// monster_fish.c -- Rotfish (fish.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_charge(float dist);
extern void ai_pain(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void swimmonster_start(edict_t *self);

// Frame indices (sequential from $frame directives)
enum {
    FISH_ATTACK1=0,FISH_ATTACK2,FISH_ATTACK3,FISH_ATTACK4,FISH_ATTACK5,FISH_ATTACK6,
    FISH_ATTACK7,FISH_ATTACK8,FISH_ATTACK9,FISH_ATTACK10,FISH_ATTACK11,FISH_ATTACK12,
    FISH_ATTACK13,FISH_ATTACK14,FISH_ATTACK15,FISH_ATTACK16,FISH_ATTACK17,FISH_ATTACK18,
    FISH_DEATH1,FISH_DEATH2,FISH_DEATH3,FISH_DEATH4,FISH_DEATH5,FISH_DEATH6,FISH_DEATH7,
    FISH_DEATH8,FISH_DEATH9,FISH_DEATH10,FISH_DEATH11,FISH_DEATH12,FISH_DEATH13,
    FISH_DEATH14,FISH_DEATH15,FISH_DEATH16,FISH_DEATH17,FISH_DEATH18,FISH_DEATH19,
    FISH_DEATH20,FISH_DEATH21,
    FISH_SWIM1,FISH_SWIM2,FISH_SWIM3,FISH_SWIM4,FISH_SWIM5,FISH_SWIM6,FISH_SWIM7,
    FISH_SWIM8,FISH_SWIM9,FISH_SWIM10,FISH_SWIM11,FISH_SWIM12,FISH_SWIM13,FISH_SWIM14,
    FISH_SWIM15,FISH_SWIM16,FISH_SWIM17,FISH_SWIM18,
    FISH_PAIN1,FISH_PAIN2,FISH_PAIN3,FISH_PAIN4,FISH_PAIN5,FISH_PAIN6,
    FISH_PAIN7,FISH_PAIN8,FISH_PAIN9
};

// Helper: set frame+nextthink+think in one shot
#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void f_stand1(edict_t *e),f_stand2(edict_t *e),f_stand3(edict_t *e),f_stand4(edict_t *e),
            f_stand5(edict_t *e),f_stand6(edict_t *e),f_stand7(edict_t *e),f_stand8(edict_t *e),
            f_stand9(edict_t *e),f_stand10(edict_t *e),f_stand11(edict_t *e),f_stand12(edict_t *e),
            f_stand13(edict_t *e),f_stand14(edict_t *e),f_stand15(edict_t *e),f_stand16(edict_t *e),
            f_stand17(edict_t *e),f_stand18(edict_t *e);
static void f_walk1(edict_t *e),f_walk2(edict_t *e),f_walk3(edict_t *e),f_walk4(edict_t *e),
            f_walk5(edict_t *e),f_walk6(edict_t *e),f_walk7(edict_t *e),f_walk8(edict_t *e),
            f_walk9(edict_t *e),f_walk10(edict_t *e),f_walk11(edict_t *e),f_walk12(edict_t *e),
            f_walk13(edict_t *e),f_walk14(edict_t *e),f_walk15(edict_t *e),f_walk16(edict_t *e),
            f_walk17(edict_t *e),f_walk18(edict_t *e);
static void f_run1(edict_t *e),f_run2(edict_t *e),f_run3(edict_t *e),f_run4(edict_t *e),
            f_run5(edict_t *e),f_run6(edict_t *e),f_run7(edict_t *e),f_run8(edict_t *e),
            f_run9(edict_t *e);
static void f_attack1(edict_t *e),f_attack2(edict_t *e),f_attack3(edict_t *e),f_attack4(edict_t *e),
            f_attack5(edict_t *e),f_attack6(edict_t *e),f_attack7(edict_t *e),f_attack8(edict_t *e),
            f_attack9(edict_t *e),f_attack10(edict_t *e),f_attack11(edict_t *e),f_attack12(edict_t *e),
            f_attack13(edict_t *e),f_attack14(edict_t *e),f_attack15(edict_t *e),f_attack16(edict_t *e),
            f_attack17(edict_t *e),f_attack18(edict_t *e);
static void f_death1(edict_t *e),f_death2(edict_t *e),f_death3(edict_t *e),f_death4(edict_t *e),
            f_death5(edict_t *e),f_death6(edict_t *e),f_death7(edict_t *e),f_death8(edict_t *e),
            f_death9(edict_t *e),f_death10(edict_t *e),f_death11(edict_t *e),f_death12(edict_t *e),
            f_death13(edict_t *e),f_death14(edict_t *e),f_death15(edict_t *e),f_death16(edict_t *e),
            f_death17(edict_t *e),f_death18(edict_t *e),f_death19(edict_t *e),f_death20(edict_t *e),
            f_death21(edict_t *e);
static void f_pain1(edict_t *e),f_pain2(edict_t *e),f_pain3(edict_t *e),f_pain4(edict_t *e),
            f_pain5(edict_t *e),f_pain6(edict_t *e),f_pain7(edict_t *e),f_pain8(edict_t *e),
            f_pain9(edict_t *e);

// ---------------------------------------------------------------------------
// Stand (swims in place)
// ---------------------------------------------------------------------------
static void f_stand1(edict_t *e)  { FRAME(e,FISH_SWIM1, f_stand2);  ai_stand(e); }
static void f_stand2(edict_t *e)  { FRAME(e,FISH_SWIM2, f_stand3);  ai_stand(e); }
static void f_stand3(edict_t *e)  { FRAME(e,FISH_SWIM3, f_stand4);  ai_stand(e); }
static void f_stand4(edict_t *e)  { FRAME(e,FISH_SWIM4, f_stand5);  ai_stand(e); }
static void f_stand5(edict_t *e)  { FRAME(e,FISH_SWIM5, f_stand6);  ai_stand(e); }
static void f_stand6(edict_t *e)  { FRAME(e,FISH_SWIM6, f_stand7);  ai_stand(e); }
static void f_stand7(edict_t *e)  { FRAME(e,FISH_SWIM7, f_stand8);  ai_stand(e); }
static void f_stand8(edict_t *e)  { FRAME(e,FISH_SWIM8, f_stand9);  ai_stand(e); }
static void f_stand9(edict_t *e)  { FRAME(e,FISH_SWIM9, f_stand10); ai_stand(e); }
static void f_stand10(edict_t *e) { FRAME(e,FISH_SWIM10,f_stand11); ai_stand(e); }
static void f_stand11(edict_t *e) { FRAME(e,FISH_SWIM11,f_stand12); ai_stand(e); }
static void f_stand12(edict_t *e) { FRAME(e,FISH_SWIM12,f_stand13); ai_stand(e); }
static void f_stand13(edict_t *e) { FRAME(e,FISH_SWIM13,f_stand14); ai_stand(e); }
static void f_stand14(edict_t *e) { FRAME(e,FISH_SWIM14,f_stand15); ai_stand(e); }
static void f_stand15(edict_t *e) { FRAME(e,FISH_SWIM15,f_stand16); ai_stand(e); }
static void f_stand16(edict_t *e) { FRAME(e,FISH_SWIM16,f_stand17); ai_stand(e); }
static void f_stand17(edict_t *e) { FRAME(e,FISH_SWIM17,f_stand18); ai_stand(e); }
static void f_stand18(edict_t *e) { FRAME(e,FISH_SWIM18,f_stand1);  ai_stand(e); }

// ---------------------------------------------------------------------------
// Walk
// ---------------------------------------------------------------------------
static void f_walk1(edict_t *e)  { FRAME(e,FISH_SWIM1, f_walk2);  ai_walk(8); }
static void f_walk2(edict_t *e)  { FRAME(e,FISH_SWIM2, f_walk3);  ai_walk(8); }
static void f_walk3(edict_t *e)  { FRAME(e,FISH_SWIM3, f_walk4);  ai_walk(8); }
static void f_walk4(edict_t *e)  { FRAME(e,FISH_SWIM4, f_walk5);  ai_walk(8); }
static void f_walk5(edict_t *e)  { FRAME(e,FISH_SWIM5, f_walk6);  ai_walk(8); }
static void f_walk6(edict_t *e)  { FRAME(e,FISH_SWIM6, f_walk7);  ai_walk(8); }
static void f_walk7(edict_t *e)  { FRAME(e,FISH_SWIM7, f_walk8);  ai_walk(8); }
static void f_walk8(edict_t *e)  { FRAME(e,FISH_SWIM8, f_walk9);  ai_walk(8); }
static void f_walk9(edict_t *e)  { FRAME(e,FISH_SWIM9, f_walk10); ai_walk(8); }
static void f_walk10(edict_t *e) { FRAME(e,FISH_SWIM10,f_walk11); ai_walk(8); }
static void f_walk11(edict_t *e) { FRAME(e,FISH_SWIM11,f_walk12); ai_walk(8); }
static void f_walk12(edict_t *e) { FRAME(e,FISH_SWIM12,f_walk13); ai_walk(8); }
static void f_walk13(edict_t *e) { FRAME(e,FISH_SWIM13,f_walk14); ai_walk(8); }
static void f_walk14(edict_t *e) { FRAME(e,FISH_SWIM14,f_walk15); ai_walk(8); }
static void f_walk15(edict_t *e) { FRAME(e,FISH_SWIM15,f_walk16); ai_walk(8); }
static void f_walk16(edict_t *e) { FRAME(e,FISH_SWIM16,f_walk17); ai_walk(8); }
static void f_walk17(edict_t *e) { FRAME(e,FISH_SWIM17,f_walk18); ai_walk(8); }
static void f_walk18(edict_t *e) { FRAME(e,FISH_SWIM18,f_walk1);  ai_walk(8); }

// ---------------------------------------------------------------------------
// Run (skips odd swim frames for speed feel, plays idle sound)
// ---------------------------------------------------------------------------
static void f_run1(edict_t *e) {
    FRAME(e,FISH_SWIM1, f_run2); ai_run(12);
    if (eng->Random() < 0.5f) eng->SV_StartSound(e, CHAN_VOICE, "fish/idle.wav", 1, ATTN_NORM);
}
static void f_run2(edict_t *e) { FRAME(e,FISH_SWIM3, f_run3); ai_run(12); }
static void f_run3(edict_t *e) { FRAME(e,FISH_SWIM5, f_run4); ai_run(12); }
static void f_run4(edict_t *e) { FRAME(e,FISH_SWIM7, f_run5); ai_run(12); }
static void f_run5(edict_t *e) { FRAME(e,FISH_SWIM9, f_run6); ai_run(12); }
static void f_run6(edict_t *e) { FRAME(e,FISH_SWIM11,f_run7); ai_run(12); }
static void f_run7(edict_t *e) { FRAME(e,FISH_SWIM13,f_run8); ai_run(12); }
static void f_run8(edict_t *e) { FRAME(e,FISH_SWIM15,f_run9); ai_run(12); }
static void f_run9(edict_t *e) { FRAME(e,FISH_SWIM17,f_run1); ai_run(12); }

// ---------------------------------------------------------------------------
// Melee / attack
// ---------------------------------------------------------------------------
static void fish_melee(void) {
    edict_t *e = g->self;
    if (!e->v.enemy) return;
    float dx=e->v.enemy->v.origin[0]-e->v.origin[0],
          dy=e->v.enemy->v.origin[1]-e->v.origin[1],
          dz=e->v.enemy->v.origin[2]-e->v.origin[2];
    if (dx*dx+dy*dy+dz*dz > 60.0f*60.0f) return;
    eng->SV_StartSound(e, CHAN_VOICE, "fish/bite.wav", 1, ATTN_NORM);
    T_Damage(e->v.enemy, e, e, (eng->Random()+eng->Random())*3.0f);
}

static void f_attack1(edict_t *e)  { FRAME(e,FISH_ATTACK1, f_attack2);  ai_charge(10); }
static void f_attack2(edict_t *e)  { FRAME(e,FISH_ATTACK2, f_attack3);  ai_charge(10); }
static void f_attack3(edict_t *e)  { FRAME(e,FISH_ATTACK3, f_attack4);  fish_melee(); }
static void f_attack4(edict_t *e)  { FRAME(e,FISH_ATTACK4, f_attack5);  ai_charge(10); }
static void f_attack5(edict_t *e)  { FRAME(e,FISH_ATTACK5, f_attack6);  ai_charge(10); }
static void f_attack6(edict_t *e)  { FRAME(e,FISH_ATTACK6, f_attack7);  ai_charge(10); }
static void f_attack7(edict_t *e)  { FRAME(e,FISH_ATTACK7, f_attack8);  ai_charge(10); }
static void f_attack8(edict_t *e)  { FRAME(e,FISH_ATTACK8, f_attack9);  ai_charge(10); }
static void f_attack9(edict_t *e)  { FRAME(e,FISH_ATTACK9, f_attack10); fish_melee(); }
static void f_attack10(edict_t *e) { FRAME(e,FISH_ATTACK10,f_attack11); ai_charge(10); }
static void f_attack11(edict_t *e) { FRAME(e,FISH_ATTACK11,f_attack12); ai_charge(10); }
static void f_attack12(edict_t *e) { FRAME(e,FISH_ATTACK12,f_attack13); ai_charge(10); }
static void f_attack13(edict_t *e) { FRAME(e,FISH_ATTACK13,f_attack14); ai_charge(10); }
static void f_attack14(edict_t *e) { FRAME(e,FISH_ATTACK14,f_attack15); ai_charge(10); }
static void f_attack15(edict_t *e) { FRAME(e,FISH_ATTACK15,f_attack16); fish_melee(); }
static void f_attack16(edict_t *e) { FRAME(e,FISH_ATTACK16,f_attack17); ai_charge(10); }
static void f_attack17(edict_t *e) { FRAME(e,FISH_ATTACK17,f_attack18); ai_charge(10); }
static void f_attack18(edict_t *e) { FRAME(e,FISH_ATTACK18,f_run1);     ai_charge(10); }

// ---------------------------------------------------------------------------
// Death
// ---------------------------------------------------------------------------
static void f_death1(edict_t *e) {
    FRAME(e,FISH_DEATH1,f_death2);
    eng->SV_StartSound(e, CHAN_VOICE, "fish/death.wav", 1, ATTN_NORM);
}
static void f_death2(edict_t *e)  { FRAME(e,FISH_DEATH2, f_death3);  }
static void f_death3(edict_t *e)  { FRAME(e,FISH_DEATH3, f_death4);  }
static void f_death4(edict_t *e)  { FRAME(e,FISH_DEATH4, f_death5);  }
static void f_death5(edict_t *e)  { FRAME(e,FISH_DEATH5, f_death6);  }
static void f_death6(edict_t *e)  { FRAME(e,FISH_DEATH6, f_death7);  }
static void f_death7(edict_t *e)  { FRAME(e,FISH_DEATH7, f_death8);  }
static void f_death8(edict_t *e)  { FRAME(e,FISH_DEATH8, f_death9);  }
static void f_death9(edict_t *e)  { FRAME(e,FISH_DEATH9, f_death10); }
static void f_death10(edict_t *e) { FRAME(e,FISH_DEATH10,f_death11); }
static void f_death11(edict_t *e) { FRAME(e,FISH_DEATH11,f_death12); }
static void f_death12(edict_t *e) { FRAME(e,FISH_DEATH12,f_death13); }
static void f_death13(edict_t *e) { FRAME(e,FISH_DEATH13,f_death14); }
static void f_death14(edict_t *e) { FRAME(e,FISH_DEATH14,f_death15); }
static void f_death15(edict_t *e) { FRAME(e,FISH_DEATH15,f_death16); }
static void f_death16(edict_t *e) { FRAME(e,FISH_DEATH16,f_death17); }
static void f_death17(edict_t *e) { FRAME(e,FISH_DEATH17,f_death18); }
static void f_death18(edict_t *e) { FRAME(e,FISH_DEATH18,f_death19); }
static void f_death19(edict_t *e) { FRAME(e,FISH_DEATH19,f_death20); }
static void f_death20(edict_t *e) { FRAME(e,FISH_DEATH20,f_death21); }
static void f_death21(edict_t *e) { FRAME(e,FISH_DEATH21,f_death21); e->v.solid=SOLID_NOT; }

// ---------------------------------------------------------------------------
// Pain
// ---------------------------------------------------------------------------
static void f_pain1(edict_t *e) { FRAME(e,FISH_PAIN1,f_pain2); }
static void f_pain2(edict_t *e) { FRAME(e,FISH_PAIN2,f_pain3); ai_pain(6); }
static void f_pain3(edict_t *e) { FRAME(e,FISH_PAIN3,f_pain4); ai_pain(6); }
static void f_pain4(edict_t *e) { FRAME(e,FISH_PAIN4,f_pain5); ai_pain(6); }
static void f_pain5(edict_t *e) { FRAME(e,FISH_PAIN5,f_pain6); ai_pain(6); }
static void f_pain6(edict_t *e) { FRAME(e,FISH_PAIN6,f_pain7); ai_pain(6); }
static void f_pain7(edict_t *e) { FRAME(e,FISH_PAIN7,f_pain8); ai_pain(6); }
static void f_pain8(edict_t *e) { FRAME(e,FISH_PAIN8,f_pain9); ai_pain(6); }
static void f_pain9(edict_t *e) { FRAME(e,FISH_PAIN9,f_run1);  ai_pain(6); }

static void fish_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker; (void)damage;
    g->self = self;
    f_pain1(self);
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_fish(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }
    eng->PrecacheModel("progs/fish.mdl");
    eng->PrecacheSound("fish/death.wav");
    eng->PrecacheSound("fish/bite.wav");
    eng->PrecacheSound("fish/idle.wav");
    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/fish.mdl");
    vec3_t mins = {-16,-16,-24}, maxs = {16,16,24};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health   = 25;
    e->v.th_stand = f_stand1;
    e->v.th_walk  = f_walk1;
    e->v.th_run   = f_run1;
    e->v.th_die   = f_death1;
    e->v.th_pain  = fish_pain;
    e->v.th_melee = f_attack1;
    swimmonster_start(e);
}

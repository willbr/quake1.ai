// fight.c -- Monster attack decision logic (fight.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// Shared AI globals (set by ai_run in ai.c; read here and in monster files)
// ---------------------------------------------------------------------------
float enemy_vis;
float enemy_infront;
float enemy_range;
float enemy_yaw;

// ---------------------------------------------------------------------------
// External dependencies
// ---------------------------------------------------------------------------
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern int  CanDamage(edict_t *targ, edict_t *inflictor);
extern void SUB_AttackFinished(float normal);

// Weak stubs — overridden when the corresponding monster file is ported.
__attribute__((weak)) void  knight_atk1(edict_t *self)   { (void)self; }
__attribute__((weak)) void  knight_runatk1(edict_t *self) { (void)self; }
__attribute__((weak)) void  ogre_smash1(edict_t *self)   { (void)self; }
__attribute__((weak)) void  ogre_swing1(edict_t *self)   { (void)self; }
__attribute__((weak)) void  sham_smash1(edict_t *self)   { (void)self; }
__attribute__((weak)) void  sham_swingr1(edict_t *self)  { (void)self; }
__attribute__((weak)) void  sham_swingl1(edict_t *self)  { (void)self; }
__attribute__((weak)) int   DemonCheckAttack(void)        { return 0; }
__attribute__((weak)) void  Demon_Melee(float side)       { (void)side; }
// Weak stubs — overridden by ai.c (Task 16)
__attribute__((weak)) float visible(edict_t *targ)        { (void)targ; return 0; }
__attribute__((weak)) float infront(edict_t *targ)        { (void)targ; return 0; }
__attribute__((weak)) float range(edict_t *targ)          { (void)targ; return RANGE_FAR; }
__attribute__((weak)) void  ChooseTurn(vec3_t dest)       { (void)dest; }

// ---------------------------------------------------------------------------
// knight_attack -- choose between swing and running attack
// ---------------------------------------------------------------------------
static void knight_attack(void) {
    edict_t *self = g->self;
    float len = eng->VectorLength(
        (vec3_t){
            self->v.enemy->v.origin[0] + self->v.enemy->v.view_ofs[0] - (self->v.origin[0] + self->v.view_ofs[0]),
            self->v.enemy->v.origin[1] + self->v.enemy->v.view_ofs[1] - (self->v.origin[1] + self->v.view_ofs[1]),
            self->v.enemy->v.origin[2] + self->v.enemy->v.view_ofs[2] - (self->v.origin[2] + self->v.view_ofs[2])
        });
    if (len < 80)
        knight_atk1(self);
    else
        knight_runatk1(self);
}

// ---------------------------------------------------------------------------
// ai_face -- turn self to face enemy
// ---------------------------------------------------------------------------
void ai_face(void) {
    edict_t *self = g->self;
    vec3_t diff = {
        self->v.enemy->v.origin[0] - self->v.origin[0],
        self->v.enemy->v.origin[1] - self->v.origin[1],
        self->v.enemy->v.origin[2] - self->v.origin[2]
    };
    self->v.ideal_yaw = eng->VectorToYaw(diff);
    eng->SV_ChangeYaw(self);
}

// ---------------------------------------------------------------------------
// CheckAttack -- can the monster fire at its enemy?
// ---------------------------------------------------------------------------
int CheckAttack(void) {
    edict_t *self = g->self;
    edict_t *targ = self->v.enemy;

    vec3_t spot1 = {
        self->v.origin[0] + self->v.view_ofs[0],
        self->v.origin[1] + self->v.view_ofs[1],
        self->v.origin[2] + self->v.view_ofs[2]
    };
    vec3_t spot2 = {
        targ->v.origin[0] + targ->v.view_ofs[0],
        targ->v.origin[1] + targ->v.view_ofs[1],
        targ->v.origin[2] + targ->v.view_ofs[2]
    };
    eng->SV_Traceline(spot1, spot2, 0, self);

    if (g->trace_ent != targ)          return 0;
    if (g->trace_inopen && g->trace_inwater) return 0;

    if ((int)enemy_range == RANGE_MELEE) {
        if (self->v.th_melee) {
            if (self->v.classname && strcmp(self->v.classname, "monster_knight") == 0)
                knight_attack();
            else
                self->v.th_melee(self);
            return 1;
        }
    }

    if (!self->v.th_missile)           return 0;
    if (g->time < self->v.attack_finished) return 0;
    if ((int)enemy_range == RANGE_FAR) return 0;

    float chance = 0;
    if ((int)enemy_range == RANGE_MELEE) {
        chance = 0.9f;
        self->v.attack_finished = 0;
    } else if ((int)enemy_range == RANGE_NEAR) {
        chance = self->v.th_melee ? 0.2f : 0.4f;
    } else if ((int)enemy_range == RANGE_MID) {
        chance = self->v.th_melee ? 0.05f : 0.1f;
    }

    if (eng->Random() < chance) {
        self->v.th_missile(self);
        SUB_AttackFinished(2*eng->Random());
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ai_charge -- melee charge toward enemy
// ---------------------------------------------------------------------------
void ai_charge(float d) {
    ai_face();
    eng->SV_MoveToGoal(g->self, d);
}

void ai_charge_side(void) {
    edict_t *self = g->self;
    vec3_t diff = {
        self->v.enemy->v.origin[0] - self->v.origin[0],
        self->v.enemy->v.origin[1] - self->v.origin[1],
        self->v.enemy->v.origin[2] - self->v.origin[2]
    };
    self->v.ideal_yaw = eng->VectorToYaw(diff);
    eng->SV_ChangeYaw(self);

    eng->MakeVectors(self->v.angles);
    vec3_t dtemp = {
        self->v.enemy->v.origin[0] - 30*g->v_right[0],
        self->v.enemy->v.origin[1] - 30*g->v_right[1],
        self->v.enemy->v.origin[2] - 30*g->v_right[2]
    };
    vec3_t hdiff = {
        dtemp[0] - self->v.origin[0],
        dtemp[1] - self->v.origin[1],
        dtemp[2] - self->v.origin[2]
    };
    float heading = eng->VectorToYaw(hdiff);
    eng->SV_WalkMove(self, heading, 20);
}

// ---------------------------------------------------------------------------
// ai_melee / ai_melee_side
// ---------------------------------------------------------------------------
void ai_melee(void) {
    edict_t *self = g->self;
    if (!self->v.enemy) return;

    vec3_t delta = {
        self->v.enemy->v.origin[0] - self->v.origin[0],
        self->v.enemy->v.origin[1] - self->v.origin[1],
        self->v.enemy->v.origin[2] - self->v.origin[2]
    };
    if (eng->VectorLength(delta) > 60) return;

    float ldmg = (eng->Random() + eng->Random() + eng->Random()) * 3;
    T_Damage(self->v.enemy, self, self, ldmg);
}

void ai_melee_side(void) {
    edict_t *self = g->self;
    if (!self->v.enemy) return;
    ai_charge_side();

    vec3_t delta = {
        self->v.enemy->v.origin[0] - self->v.origin[0],
        self->v.enemy->v.origin[1] - self->v.origin[1],
        self->v.enemy->v.origin[2] - self->v.origin[2]
    };
    if (eng->VectorLength(delta) > 60) return;
    if (!CanDamage(self->v.enemy, self)) return;

    float ldmg = (eng->Random() + eng->Random() + eng->Random()) * 3;
    T_Damage(self->v.enemy, self, self, ldmg);
}

// ---------------------------------------------------------------------------
// Monster-specific check attack functions
// ---------------------------------------------------------------------------
int SoldierCheckAttack(void) {
    edict_t *self = g->self;
    edict_t *targ = self->v.enemy;

    vec3_t spot1 = {
        self->v.origin[0] + self->v.view_ofs[0],
        self->v.origin[1] + self->v.view_ofs[1],
        self->v.origin[2] + self->v.view_ofs[2]
    };
    vec3_t spot2 = {
        targ->v.origin[0] + targ->v.view_ofs[0],
        targ->v.origin[1] + targ->v.view_ofs[1],
        targ->v.origin[2] + targ->v.view_ofs[2]
    };
    eng->SV_Traceline(spot1, spot2, 0, self);

    if (g->trace_inopen && g->trace_inwater) return 0;
    if (g->trace_ent != targ)                return 0;
    if (g->time < self->v.attack_finished)   return 0;
    if ((int)enemy_range == RANGE_FAR)       return 0;

    float chance = 0;
    if      ((int)enemy_range == RANGE_MELEE) chance = 0.9f;
    else if ((int)enemy_range == RANGE_NEAR)  chance = 0.4f;
    else if ((int)enemy_range == RANGE_MID)   chance = 0.05f;

    if (eng->Random() < chance) {
        self->v.th_missile(self);
        SUB_AttackFinished(1 + eng->Random());
        if (eng->Random() < 0.3f)
            self->v.lefty = !self->v.lefty;
        return 1;
    }
    return 0;
}

int ShamCheckAttack(void) {
    edict_t *self = g->self;

    if ((int)enemy_range == RANGE_MELEE) {
        if (CanDamage(self->v.enemy, self)) {
            self->v.attack_state = AS_MELEE;
            return 1;
        }
    }
    if (g->time < self->v.attack_finished) return 0;
    if (!enemy_vis)                         return 0;

    edict_t *targ = self->v.enemy;
    vec3_t spot1 = {
        self->v.origin[0] + self->v.view_ofs[0],
        self->v.origin[1] + self->v.view_ofs[1],
        self->v.origin[2] + self->v.view_ofs[2]
    };
    vec3_t spot2 = {
        targ->v.origin[0] + targ->v.view_ofs[0],
        targ->v.origin[1] + targ->v.view_ofs[1],
        targ->v.origin[2] + targ->v.view_ofs[2]
    };
    vec3_t diff = {spot1[0]-spot2[0], spot1[1]-spot2[1], spot1[2]-spot2[2]};
    if (eng->VectorLength(diff) > 600) return 0;

    eng->SV_Traceline(spot1, spot2, 0, self);
    if (g->trace_inopen && g->trace_inwater) return 0;
    if (g->trace_ent != targ)                return 0;
    if ((int)enemy_range == RANGE_FAR)       return 0;

    self->v.attack_state = AS_MISSILE;
    SUB_AttackFinished(2 + 2*eng->Random());
    return 1;
}

int OgreCheckAttack(void) {
    edict_t *self = g->self;

    if ((int)enemy_range == RANGE_MELEE) {
        if (CanDamage(self->v.enemy, self)) {
            self->v.attack_state = AS_MELEE;
            return 1;
        }
    }
    if (g->time < self->v.attack_finished) return 0;
    if (!enemy_vis)                         return 0;

    edict_t *targ = self->v.enemy;
    vec3_t spot1 = {
        self->v.origin[0] + self->v.view_ofs[0],
        self->v.origin[1] + self->v.view_ofs[1],
        self->v.origin[2] + self->v.view_ofs[2]
    };
    vec3_t spot2 = {
        targ->v.origin[0] + targ->v.view_ofs[0],
        targ->v.origin[1] + targ->v.view_ofs[1],
        targ->v.origin[2] + targ->v.view_ofs[2]
    };
    eng->SV_Traceline(spot1, spot2, 0, self);
    if (g->trace_inopen && g->trace_inwater) return 0;
    if (g->trace_ent != targ)                return 0;
    if (g->time < self->v.attack_finished)   return 0;
    if ((int)enemy_range == RANGE_FAR)       return 0;

    float chance = 0;
    if      ((int)enemy_range == RANGE_NEAR) chance = 0.10f;
    else if ((int)enemy_range == RANGE_MID)  chance = 0.05f;

    self->v.attack_state = AS_MISSILE;
    SUB_AttackFinished(1 + 2*eng->Random());
    return 1;
}

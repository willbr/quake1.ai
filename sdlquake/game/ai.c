// ai.c -- Monster AI (ai.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// Defined in fight.c; written by ai_run each frame, read by CheckAttack functions.
extern float enemy_vis;
extern float enemy_infront;
extern float enemy_range;
extern float enemy_yaw;

// ---------------------------------------------------------------------------
// Module globals (QC globals from ai.qc)
// ---------------------------------------------------------------------------
edict_t *sight_entity;       // monster that most recently spotted a player
float    sight_entity_time;  // time of that sighting

// ---------------------------------------------------------------------------
// External dependencies
// ---------------------------------------------------------------------------
extern void SUB_AttackFinished(float normal);
extern int  CheckAttack(void);
extern int  SoldierCheckAttack(void);
extern int  OgreCheckAttack(void);
extern int  ShamCheckAttack(void);
extern int  DemonCheckAttack(void);

// Weak stubs — overridden when monster files are ported
__attribute__((weak)) int WizardCheckAttack(void) { return 0; }
__attribute__((weak)) int DogCheckAttack(void)    { return 0; }

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void HuntTarget(void);
static void SightSound(void);
static int  FindTarget(void);
static int  CheckAnyAttack(void);
static int  FacingIdeal(void);
static void ai_run_melee(void);
static void ai_run_missile(void);
static void ai_run_slide(void);

// ---------------------------------------------------------------------------
// anglemod -- normalize angle to [0, 360)
// ---------------------------------------------------------------------------
static float anglemod(float v) {
    while (v >= 360.0f) v -= 360.0f;
    while (v <    0.0f) v += 360.0f;
    return v;
}

// ---------------------------------------------------------------------------
// t_movetarget -- touch handler for path_corner entities.
// Called with self_e = the path_corner, other_e = monster that bumped it.
// ---------------------------------------------------------------------------
static void t_movetarget(edict_t *self_e, edict_t *other_e) {
    if (other_e->v.movetarget != self_e) return;
    if (other_e->v.enemy)                return;

    // Work as the monster from here on (QC self/other swap).
    g->self  = other_e;
    g->other = self_e;

    edict_t *monster = other_e;
    edict_t *corner  = self_e;

    if (monster->v.classname && strcmp(monster->v.classname, "monster_ogre") == 0)
        eng->SV_StartSound(monster, CHAN_VOICE, "ogre/ogdrag.wav", 1, ATTN_IDLE);

    edict_t *next = corner->v.target
        ? eng->ED_Find(g->world, "targetname", corner->v.target)
        : g->world;
    monster->v.goalentity = monster->v.movetarget = next;

    // Compute ideal_yaw before the end-of-path check (matches QC order). If next
    // is world, this points at (0,0,0) — harmless since we immediately stand.
    vec3_t diff = {
        next->v.origin[0] - monster->v.origin[0],
        next->v.origin[1] - monster->v.origin[1],
        next->v.origin[2] - monster->v.origin[2]
    };
    monster->v.ideal_yaw = eng->VectorToYaw(diff);

    // engine_ed_find returns g->world (not NULL) on no-match; check accordingly.
    // Without this, monsters at end-of-path kept walking toward world origin.
    if (next == g->world) {
        monster->v.pausetime = g->time + 999999;
        if (monster->v.th_stand) monster->v.th_stand(monster);
        return;
    }
}

// ---------------------------------------------------------------------------
// spawn_path_corner -- classname "path_corner"
// ---------------------------------------------------------------------------
void spawn_path_corner(edict_t *e) {
    g->self = e;
    if (!e->v.targetname)
        eng->Host_Error("monster_movetarget: no targetname");
    e->v.solid = SOLID_TRIGGER;
    e->v.touch = t_movetarget;
    vec3_t mins = {-8, -8, -8}, maxs = {8, 8, 8};
    eng->SV_SetSize(e, mins, maxs);
}

// ---------------------------------------------------------------------------
// range -- distance category from self to targ (RANGE_MELEE/NEAR/MID/FAR)
// ---------------------------------------------------------------------------
float range(edict_t *targ) {
    edict_t *self = g->self;
    vec3_t diff = {
        self->v.origin[0] + self->v.view_ofs[0] - (targ->v.origin[0] + targ->v.view_ofs[0]),
        self->v.origin[1] + self->v.view_ofs[1] - (targ->v.origin[1] + targ->v.view_ofs[1]),
        self->v.origin[2] + self->v.view_ofs[2] - (targ->v.origin[2] + targ->v.view_ofs[2])
    };
    float r = eng->VectorLength(diff);
    if (r < 120)  return RANGE_MELEE;
    if (r < 500)  return RANGE_NEAR;
    if (r < 1000) return RANGE_MID;
    return RANGE_FAR;
}

// ---------------------------------------------------------------------------
// visible -- LOS check from self to targ (see through other monsters)
// ---------------------------------------------------------------------------
float visible(edict_t *targ) {
    edict_t *self = g->self;
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
    eng->SV_Traceline(spot1, spot2, 1, self);  // nomonsters=1: pass through monsters

    if (g->trace_inopen && g->trace_inwater) return 0;
    if (g->trace_fraction == 1.0f)           return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// infront -- is targ within the forward half-plane of self?
// ---------------------------------------------------------------------------
float infront(edict_t *targ) {
    edict_t *self = g->self;
    eng->MakeVectors(self->v.angles);
    vec3_t raw = {
        targ->v.origin[0] - self->v.origin[0],
        targ->v.origin[1] - self->v.origin[1],
        targ->v.origin[2] - self->v.origin[2]
    };
    vec3_t vec;
    eng->VectorNormalize(raw, vec);
    float dot = vec[0]*g->v_forward[0] + vec[1]*g->v_forward[1] + vec[2]*g->v_forward[2];
    return (dot > 0.3f) ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------------------
// HuntTarget -- begin pursuing self.enemy
// ---------------------------------------------------------------------------
static void HuntTarget(void) {
    edict_t *self = g->self;
    self->v.goalentity = self->v.enemy;
    self->v.think      = self->v.th_run;
    vec3_t diff = {
        self->v.enemy->v.origin[0] - self->v.origin[0],
        self->v.enemy->v.origin[1] - self->v.origin[1],
        self->v.enemy->v.origin[2] - self->v.origin[2]
    };
    self->v.ideal_yaw = eng->VectorToYaw(diff);
    self->v.nextthink  = g->time + 0.1f;
    SUB_AttackFinished(1);
}

// ---------------------------------------------------------------------------
// SightSound -- play this monster's sight-alert sound
// ---------------------------------------------------------------------------
static void SightSound(void) {
    edict_t   *self = g->self;
    const char *cn  = self->v.classname;
    if (!cn) return;

    if      (!strcmp(cn, "monster_ogre"))        eng->SV_StartSound(self, CHAN_VOICE, "ogre/ogwake.wav",       1, ATTN_NORM);
    else if (!strcmp(cn, "monster_knight"))      eng->SV_StartSound(self, CHAN_VOICE, "knight/ksight.wav",     1, ATTN_NORM);
    else if (!strcmp(cn, "monster_shambler"))    eng->SV_StartSound(self, CHAN_VOICE, "shambler/ssight.wav",   1, ATTN_NORM);
    else if (!strcmp(cn, "monster_demon1"))      eng->SV_StartSound(self, CHAN_VOICE, "demon/sight2.wav",      1, ATTN_NORM);
    else if (!strcmp(cn, "monster_wizard"))      eng->SV_StartSound(self, CHAN_VOICE, "wizard/wsight.wav",     1, ATTN_NORM);
    else if (!strcmp(cn, "monster_zombie"))      eng->SV_StartSound(self, CHAN_VOICE, "zombie/z_idle.wav",     1, ATTN_NORM);
    else if (!strcmp(cn, "monster_dog"))         eng->SV_StartSound(self, CHAN_VOICE, "dog/dsight.wav",        1, ATTN_NORM);
    else if (!strcmp(cn, "monster_hell_knight")) eng->SV_StartSound(self, CHAN_VOICE, "hknight/sight1.wav",    1, ATTN_NORM);
    else if (!strcmp(cn, "monster_tarbaby"))     eng->SV_StartSound(self, CHAN_VOICE, "blob/sight1.wav",       1, ATTN_NORM);
    else if (!strcmp(cn, "monster_vomit"))       eng->SV_StartSound(self, CHAN_VOICE, "vomitus/v_sight1.wav",  1, ATTN_NORM);
    else if (!strcmp(cn, "monster_army"))        eng->SV_StartSound(self, CHAN_VOICE, "soldier/sight1.wav",    1, ATTN_NORM);
    else if (!strcmp(cn, "monster_shalrath"))    eng->SV_StartSound(self, CHAN_VOICE, "shalrath/sight.wav",    1, ATTN_NORM);
    else if (!strcmp(cn, "monster_enforcer")) {
        int rsnd = (int)roundf(eng->Random() * 3.0f);
        if      (rsnd == 1) eng->SV_StartSound(self, CHAN_VOICE, "enforcer/sight1.wav", 1, ATTN_NORM);
        else if (rsnd == 2) eng->SV_StartSound(self, CHAN_VOICE, "enforcer/sight2.wav", 1, ATTN_NORM);
        else if (rsnd == 0) eng->SV_StartSound(self, CHAN_VOICE, "enforcer/sight3.wav", 1, ATTN_NORM);
        else                eng->SV_StartSound(self, CHAN_VOICE, "enforcer/sight4.wav", 1, ATTN_NORM);
    }
}

// ---------------------------------------------------------------------------
// FoundTarget -- enemy spotted; broadcast sight_entity and start pursuing
// Overrides the weak stub in combat.c.
// ---------------------------------------------------------------------------
void FoundTarget(void) {
    edict_t *self = g->self;
    if (self->v.enemy->v.classname &&
            strcmp(self->v.enemy->v.classname, "player") == 0) {
        sight_entity      = self;
        sight_entity_time = g->time;
    }
    self->v.show_hostile = g->time + 1;
    SightSound();
    HuntTarget();
}

// ---------------------------------------------------------------------------
// FindTarget -- scan for a player target; returns 1 if one was found
// ---------------------------------------------------------------------------
static int FindTarget(void) {
    edict_t *self = g->self;
    edict_t *client;

    // Piggyback on a nearby monster's recent sighting (unless ambush flag set).
    if (sight_entity_time >= g->time - 0.1f && !(((int)self->v.spawnflags) & 3)) {
        client = sight_entity;
        if (client->v.enemy == self->v.enemy)
            return 0;  // same enemy, nothing new to do
    } else {
        client = eng->ED_CheckClient();
        if (!client) return 0;
    }

    if (client == self->v.enemy)                   return 0;
    if (((int)client->v.flags) & FL_NOTARGET)      return 0;
    if (((int)client->v.items) & IT_INVISIBILITY)  return 0;

    float r = range(client);
    if ((int)r == RANGE_FAR)  return 0;
    if (!visible(client))     return 0;

    if ((int)r == RANGE_NEAR) {
        if (client->v.show_hostile < g->time && !infront(client))
            return 0;
    } else if ((int)r == RANGE_MID) {
        if (!infront(client))
            return 0;
    }

    // Chase down to an actual player (sight_entity may point to a monster).
    self->v.enemy = client;
    if (!self->v.enemy->v.classname ||
            strcmp(self->v.enemy->v.classname, "player") != 0) {
        self->v.enemy = self->v.enemy->v.enemy;
        if (!self->v.enemy || !self->v.enemy->v.classname ||
                strcmp(self->v.enemy->v.classname, "player") != 0) {
            self->v.enemy = g->world;
            return 0;
        }
    }

    FoundTarget();
    return 1;
}

// ---------------------------------------------------------------------------
// Movement primitives — called from monster frame callbacks (g->self already set)
//
// All of these short-circuit on a dead entity. Some death animations (notably
// hellknight's hk_die1/2/3/8/9) keep calling ai_forward mid-anim, which in
// vanilla just translated the now-SOLID_NOT corpse forward harmlessly. With
// corpses now SOLID_BBOX (prone bbox) so they can be over-damaged into gibs,
// a corpse that keeps stomping forward through the death anim looks like a
// dead body sliding toward the player and bumps into them. Stopping movement
// once dead avoids that without changing live-monster behavior.
// ---------------------------------------------------------------------------
void ai_forward(float dist) {
    if (g->self->v.deadflag != DEAD_NO) return;
    eng->SV_WalkMove(g->self, g->self->v.angles[1], dist);
}
void ai_back(float dist) {
    if (g->self->v.deadflag != DEAD_NO) return;
    eng->SV_WalkMove(g->self, g->self->v.angles[1] + 180, dist);
}
void ai_pain(float dist) { ai_back(dist); }
void ai_painforward(float dist) {
    if (g->self->v.deadflag != DEAD_NO) return;
    eng->SV_WalkMove(g->self, g->self->v.ideal_yaw, dist);
}

void ai_walk(float dist) {
    {
        ai_brain_t *b = Sim_AI_GetBrain(g->self);
        if (b && (b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING ||
                  (b->state == AI_IDLE && b->patrol_route_id >= 0)))
            return;
    }
    g->movedist = dist;
    if (FindTarget()) return;
    // Guard: vanilla SV_MoveToGoal -> SV_NewChaseDir dereferences
    // goalentity unconditionally. The vanilla invariant was that
    // walk-frame monsters always have a movetarget (their goalentity),
    // but our retrofit puts movetarget-less monsters into walk mode --
    // so goalentity may be NULL at this point.
    if (!g->self->v.goalentity || g->self->v.goalentity == g->world)
        return;
    eng->SV_MoveToGoal(g->self, dist);
}

void ai_stand(edict_t *self) {
    g->self = self;
    {
        ai_brain_t *b = Sim_AI_GetBrain(self);
        if (b && (b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING ||
                  (b->state == AI_IDLE && b->patrol_route_id >= 0)))
            return;
    }
    if (FindTarget()) return;
    if (g->time > self->v.pausetime) {
        if (self->v.th_walk) self->v.th_walk(self);
        return;
    }
}

void ai_turn(edict_t *self) {
    g->self = self;
    if (FindTarget()) return;
    eng->SV_ChangeYaw(self);
}

// ---------------------------------------------------------------------------
// ChooseTurn -- pick a slide direction around the last trace obstacle.
// Strong definition replacing the weak stub in fight.c.
// ---------------------------------------------------------------------------
void ChooseTurn(vec3_t dest) {
    edict_t *self = g->self;
    vec3_t dir = {
        self->v.origin[0] - dest[0],
        self->v.origin[1] - dest[1],
        self->v.origin[2] - dest[2]
    };
    // Perpendicular to the trace plane normal (XY only).
    float nx =  g->trace_plane_normal[1];
    float ny = -g->trace_plane_normal[0];
    float dotval = dir[0]*nx + dir[1]*ny;
    if (dotval > 0) {
        dir[0] = -g->trace_plane_normal[1];
        dir[1] =  g->trace_plane_normal[0];
    } else {
        dir[0] =  g->trace_plane_normal[1];
        dir[1] = -g->trace_plane_normal[0];
    }
    dir[2] = 0;
    self->v.ideal_yaw = eng->VectorToYaw(dir);
}

// ---------------------------------------------------------------------------
// FacingIdeal -- is self within 45 degrees of ideal_yaw?
// ---------------------------------------------------------------------------
static int FacingIdeal(void) {
    edict_t *self = g->self;
    float delta = anglemod(self->v.angles[1] - self->v.ideal_yaw);
    return (delta <= 45.0f || delta >= 315.0f);
}

// ---------------------------------------------------------------------------
// CheckAnyAttack -- dispatch to monster-specific attack decision
// ---------------------------------------------------------------------------
static int CheckAnyAttack(void) {
    if (!enemy_vis) return 0;
    edict_t    *self = g->self;
    const char *cn   = self->v.classname;
    if (!cn)                          return CheckAttack();
    if (!strcmp(cn, "monster_army"))     return SoldierCheckAttack();
    if (!strcmp(cn, "monster_ogre"))     return OgreCheckAttack();
    if (!strcmp(cn, "monster_shambler")) return ShamCheckAttack();
    if (!strcmp(cn, "monster_demon1"))   return DemonCheckAttack();
    if (!strcmp(cn, "monster_dog"))      return DogCheckAttack();
    if (!strcmp(cn, "monster_wizard"))   return WizardCheckAttack();
    return CheckAttack();
}

// ---------------------------------------------------------------------------
// ai_run_melee -- turn toward enemy and launch melee when facing
// ---------------------------------------------------------------------------
static void ai_run_melee(void) {
    edict_t *self = g->self;
    self->v.ideal_yaw = enemy_yaw;
    eng->SV_ChangeYaw(self);
    if (FacingIdeal()) {
        if (self->v.th_melee) self->v.th_melee(self);
        self->v.attack_state = AS_STRAIGHT;
    }
}

// ---------------------------------------------------------------------------
// ai_run_missile -- turn toward enemy and fire when facing
// ---------------------------------------------------------------------------
static void ai_run_missile(void) {
    edict_t *self = g->self;
    self->v.ideal_yaw = enemy_yaw;
    eng->SV_ChangeYaw(self);
    if (FacingIdeal()) {
        if (self->v.th_missile) self->v.th_missile(self);
        self->v.attack_state = AS_STRAIGHT;
    }
}

// ---------------------------------------------------------------------------
// ai_run_slide -- strafe sideways while maintaining range
// ---------------------------------------------------------------------------
static void ai_run_slide(void) {
    edict_t *self = g->self;
    self->v.ideal_yaw = enemy_yaw;
    eng->SV_ChangeYaw(self);
    float ofs = self->v.lefty ? 90.0f : -90.0f;
    if (eng->SV_WalkMove(self, self->v.ideal_yaw + ofs, g->movedist)) return;
    self->v.lefty = 1.0f - self->v.lefty;
    eng->SV_WalkMove(self, self->v.ideal_yaw - ofs, g->movedist);
}

// ---------------------------------------------------------------------------
// ai_run -- main AI run loop; called each frame from th_run frame callbacks
// ---------------------------------------------------------------------------
void ai_run(float dist) {
    {
        ai_brain_t *b = Sim_AI_GetBrain(g->self);
        if (b && (b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING ||
                  (b->state == AI_IDLE && b->patrol_route_id >= 0)))
            return;
    }
    edict_t *self = g->self;
    g->movedist = dist;

    // notarget: release enemy that has FL_NOTARGET set (toggled by "notarget" command).
    if (self->v.enemy && self->v.enemy != g->world &&
            self->v.enemy->v.health > 0 &&
            ((int)self->v.enemy->v.flags & FL_NOTARGET)) {
        self->v.enemy = g->world;
        if (self->v.movetarget) {
            if (self->v.th_walk) self->v.th_walk(self);
        } else {
            if (self->v.th_stand) self->v.th_stand(self);
        }
        return;
    }

    // If the current enemy is dead, try to find a new target.
    if (!self->v.enemy || self->v.enemy->v.health <= 0) {
        self->v.enemy = g->world;
        if (self->v.oldenemy && self->v.oldenemy->v.health > 0) {
            self->v.enemy = self->v.oldenemy;
            HuntTarget();
            // Fall through: start running toward the new enemy this frame.
        } else {
            if (self->v.movetarget) {
                if (self->v.th_walk) self->v.th_walk(self);
            } else {
                if (self->v.th_stand) self->v.th_stand(self);
            }
            return;
        }
    }

    self->v.show_hostile = g->time + 1;

    enemy_vis = visible(self->v.enemy);
    if (enemy_vis) self->v.search_time = g->time + 5;

    // In coop, periodically look for a better target.
    if ((int)g->coop && self->v.search_time < g->time) {
        if (FindTarget()) return;
    }

    enemy_infront = infront(self->v.enemy);
    enemy_range   = range(self->v.enemy);
    vec3_t ediff  = {
        self->v.enemy->v.origin[0] - self->v.origin[0],
        self->v.enemy->v.origin[1] - self->v.origin[1],
        self->v.enemy->v.origin[2] - self->v.origin[2]
    };
    enemy_yaw = eng->VectorToYaw(ediff);

    if ((int)self->v.attack_state == AS_MISSILE) { ai_run_missile(); return; }
    if ((int)self->v.attack_state == AS_MELEE)   { ai_run_melee();   return; }

    if (CheckAnyAttack()) return;

    if ((int)self->v.attack_state == AS_SLIDING) { ai_run_slide(); return; }

    // Guard against NULL/world goalentity (see ai_walk comment).
    if (!self->v.goalentity || self->v.goalentity == g->world)
        return;
    eng->SV_MoveToGoal(self, dist);
}

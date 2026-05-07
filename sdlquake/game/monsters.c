// monsters.c -- Shared monster startup/lifecycle utilities (monsters.qc port).
// Individual monster files include their own spawn logic; this file is the
// common runtime: monster_use, monster_death_use, and the three *monster_start
// families used by every monster spawn function.

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void SUB_UseTargets(void);
extern void FoundTarget(void);  // ai.c strong definition

static void FoundTarget_think(edict_t *self) { g->self = self; FoundTarget(); }

// ---------------------------------------------------------------------------
// monster_use — use callback that makes a monster attack the activator
// ---------------------------------------------------------------------------
void monster_use(edict_t *self, edict_t *activator) {
    g->self = self;
    if (self->v.enemy)               return;
    if (self->v.health <= 0)         return;
    if ((int)activator->v.items & IT_INVISIBILITY) return;
    if ((int)activator->v.flags & FL_NOTARGET)     return;
    if (!activator->v.classname || strcmp(activator->v.classname, "player") != 0) return;

    // Delay so teleported monsters still play their alert sound
    self->v.enemy     = activator;
    self->v.nextthink = g->time + 0.1f;
    self->v.think     = FoundTarget_think;
}

// ---------------------------------------------------------------------------
// monster_death_use — fires targets with the dead monster's enemy as activator
// ---------------------------------------------------------------------------
void monster_death_use(void) {
    edict_t *self = g->self;
    if ((int)self->v.flags & FL_FLY)
        self->v.flags = (float)((int)self->v.flags - FL_FLY);
    if ((int)self->v.flags & FL_SWIM)
        self->v.flags = (float)((int)self->v.flags - FL_SWIM);
    if (!self->v.target)
        return;
    g->activator = self->v.enemy;
    SUB_UseTargets();
}

// ---------------------------------------------------------------------------
// Walk monsters
// ---------------------------------------------------------------------------
static void walkmonster_start_go(edict_t *self) {
    g->self = self;
    self->v.origin[2] += 1;  // nudge off floor
    eng->SV_DropToFloor(self);

    if (!eng->SV_WalkMove(self, 0, 0)) {
        eng->Con_DPrintf("walkmonster in wall at: ");
        eng->Con_DPrintf(eng->VToS(self->v.origin));
        eng->Con_DPrintf("\n");
    }

    self->v.takedamage = DAMAGE_AIM;
    self->v.ideal_yaw  = self->v.angles[1];  // angles * '0 1 0'
    if (!self->v.yaw_speed) self->v.yaw_speed = 20;
    self->v.view_ofs[0] = 0; self->v.view_ofs[1] = 0; self->v.view_ofs[2] = 25;
    self->v.use         = monster_use;
    self->v.flags       = (float)((int)self->v.flags | FL_MONSTER);

    if (self->v.target) {
        self->v.goalentity = self->v.movetarget =
            eng->ED_Find(g->world, "targetname", self->v.target);
        if (self->v.goalentity != g->world)
            self->v.ideal_yaw = eng->VectorToYaw(
                (vec3_t){ self->v.goalentity->v.origin[0] - self->v.origin[0],
                          self->v.goalentity->v.origin[1] - self->v.origin[1],
                          self->v.goalentity->v.origin[2] - self->v.origin[2] });
        if (self->v.movetarget == g->world) {
            eng->Con_DPrintf("Monster can't find target at ");
            eng->Con_DPrintf(eng->VToS(self->v.origin));
            eng->Con_DPrintf("\n");
        }
        // QC bug: th_stand() always runs (else only guards pausetime assignment)
        if (self->v.movetarget != g->world && self->v.movetarget->v.classname &&
            strcmp(self->v.movetarget->v.classname, "path_corner") == 0)
            self->v.th_walk(self);
        else
            self->v.pausetime = 99999999;
        self->v.th_stand(self);
    } else {
        self->v.pausetime = 99999999;
        self->v.th_stand(self);
    }

    self->v.nextthink += eng->Random() * 0.5f;
}

void walkmonster_start(edict_t *self) {
    g->self = self;
    self->v.nextthink += eng->Random() * 0.5f;
    self->v.think      = walkmonster_start_go;
    g->total_monsters += 1;
}

// ---------------------------------------------------------------------------
// Fly monsters
// ---------------------------------------------------------------------------
static void flymonster_start_go(edict_t *self) {
    g->self = self;
    self->v.takedamage = DAMAGE_AIM;
    self->v.ideal_yaw  = self->v.angles[1];
    if (!self->v.yaw_speed) self->v.yaw_speed = 10;
    self->v.view_ofs[0] = 0; self->v.view_ofs[1] = 0; self->v.view_ofs[2] = 25;
    self->v.use         = monster_use;
    self->v.flags       = (float)((int)self->v.flags | FL_FLY | FL_MONSTER);

    if (!eng->SV_WalkMove(self, 0, 0)) {
        eng->Con_DPrintf("flymonster in wall at: ");
        eng->Con_DPrintf(eng->VToS(self->v.origin));
        eng->Con_DPrintf("\n");
    }

    if (self->v.target) {
        self->v.goalentity = self->v.movetarget =
            eng->ED_Find(g->world, "targetname", self->v.target);
        if (self->v.movetarget == g->world) {
            eng->Con_DPrintf("Monster can't find target at ");
            eng->Con_DPrintf(eng->VToS(self->v.origin));
            eng->Con_DPrintf("\n");
        }
        // QC bug: th_stand() always runs (else only guards pausetime assignment)
        if (self->v.movetarget != g->world && self->v.movetarget->v.classname &&
            strcmp(self->v.movetarget->v.classname, "path_corner") == 0)
            self->v.th_walk(self);
        else
            self->v.pausetime = 99999999;
        self->v.th_stand(self);
    } else {
        self->v.pausetime = 99999999;
        self->v.th_stand(self);
    }
}

void flymonster_start(edict_t *self) {
    g->self = self;
    self->v.nextthink += eng->Random() * 0.5f;
    self->v.think      = flymonster_start_go;
    g->total_monsters += 1;
}

// ---------------------------------------------------------------------------
// Swim monsters
// ---------------------------------------------------------------------------
static void swimmonster_start_go(edict_t *self) {
    g->self = self;
    if (g->deathmatch) {
        eng->ED_Free(self);
        return;
    }

    self->v.takedamage  = DAMAGE_AIM;
    g->total_monsters  += 1;
    self->v.ideal_yaw   = self->v.angles[1];
    if (!self->v.yaw_speed) self->v.yaw_speed = 10;
    self->v.view_ofs[0] = 0; self->v.view_ofs[1] = 0; self->v.view_ofs[2] = 10;
    self->v.use         = monster_use;
    self->v.flags       = (float)((int)self->v.flags | FL_SWIM | FL_MONSTER);

    if (self->v.target) {
        self->v.goalentity = self->v.movetarget =
            eng->ED_Find(g->world, "targetname", self->v.target);
        if (self->v.movetarget == g->world) {
            eng->Con_DPrintf("Monster can't find target at ");
            eng->Con_DPrintf(eng->VToS(self->v.origin));
            eng->Con_DPrintf("\n");
        }
        if (self->v.goalentity != g->world)
            self->v.ideal_yaw = eng->VectorToYaw(
                (vec3_t){ self->v.goalentity->v.origin[0] - self->v.origin[0],
                          self->v.goalentity->v.origin[1] - self->v.origin[1],
                          self->v.goalentity->v.origin[2] - self->v.origin[2] });
        self->v.th_walk(self);
    } else {
        self->v.pausetime = 99999999;
        self->v.th_stand(self);
    }

    self->v.nextthink += eng->Random() * 0.5f;
}

void swimmonster_start(edict_t *self) {
    g->self = self;
    self->v.nextthink += eng->Random() * 0.5f;
    self->v.think      = swimmonster_start_go;
    g->total_monsters += 1;
}

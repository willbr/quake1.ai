// combat.c -- Damage application and radius effects. Source: combat.qc

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <math.h>
#include <string.h>

extern void SUB_Remove(edict_t *e);

extern engine_api_t   *eng;
extern game_globals_t *g;

// Weak stubs — overridden by real definitions in client.c, monsters.c, ai.c.
__attribute__((weak)) void ClientObituary(edict_t *targ, edict_t *attacker)
    { (void)targ; (void)attacker; }
__attribute__((weak)) void monster_death_use(void) {}
__attribute__((weak)) void FoundTarget(void)       {}

int CanDamage(edict_t *targ, edict_t *inflictor)
{
    vec3_t mid, p;

    if (targ->v.movetype == MOVETYPE_PUSH) {
        mid[0] = 0.5f * (targ->v.absmin[0] + targ->v.absmax[0]);
        mid[1] = 0.5f * (targ->v.absmin[1] + targ->v.absmax[1]);
        mid[2] = 0.5f * (targ->v.absmin[2] + targ->v.absmax[2]);
        eng->SV_Traceline(inflictor->v.origin, mid, 1, g->self);
        if (g->trace_fraction == 1.0f) return 1;
        if (g->trace_ent == targ)      return 1;
        return 0;
    }

    eng->SV_Traceline(inflictor->v.origin, targ->v.origin, 1, g->self);
    if (g->trace_fraction == 1.0f) return 1;

    p[0] = targ->v.origin[0] + 15; p[1] = targ->v.origin[1] + 15; p[2] = targ->v.origin[2];
    eng->SV_Traceline(inflictor->v.origin, p, 1, g->self);
    if (g->trace_fraction == 1.0f) return 1;

    p[0] = targ->v.origin[0] - 15; p[1] = targ->v.origin[1] - 15; p[2] = targ->v.origin[2];
    eng->SV_Traceline(inflictor->v.origin, p, 1, g->self);
    if (g->trace_fraction == 1.0f) return 1;

    p[0] = targ->v.origin[0] - 15; p[1] = targ->v.origin[1] + 15; p[2] = targ->v.origin[2];
    eng->SV_Traceline(inflictor->v.origin, p, 1, g->self);
    if (g->trace_fraction == 1.0f) return 1;

    p[0] = targ->v.origin[0] + 15; p[1] = targ->v.origin[1] - 15; p[2] = targ->v.origin[2];
    eng->SV_Traceline(inflictor->v.origin, p, 1, g->self);
    if (g->trace_fraction == 1.0f) return 1;

    return 0;
}

static void Killed(edict_t *targ, edict_t *attacker)
{
    edict_t *oself = g->self;
    g->self = targ;

    // Emit death stims for the AI sense filter.
    {
        stimulus_t s = {0};
        s.origin[0] = targ->v.origin[0];
        s.origin[1] = targ->v.origin[1];
        s.origin[2] = targ->v.origin[2];
        s.source_edict = eng->ED_GetNum(targ);

        s.kind = STIM_CORPSE;
        s.intensity = 1.0f;
        Stim_Emit(&s);

        s.kind = STIM_SOUND;
        s.intensity = 0.5f;
        Stim_Emit(&s);
    }

    if (g->self->v.health < -99.0f)
        g->self->v.health = -99.0f;

    if (g->self->v.movetype == MOVETYPE_PUSH || g->self->v.movetype == MOVETYPE_NONE) {
        if (g->self->v.th_die)
            g->self->v.th_die(g->self);
        g->self = oself;
        return;
    }

    g->self->v.enemy = attacker;

    if ((int)g->self->v.flags & FL_MONSTER) {
        g->killed_monsters++;
        eng->MSG_WriteByte(MSG_ALL, SVC_KILLEDMONSTER);
        eng->spawn_blood_pool(g->self->v.origin);
    }

    ClientObituary(g->self, attacker);

    g->self->v.takedamage = DAMAGE_NO;
    g->self->v.touch      = NULL;

    monster_death_use();
    if (g->self->v.th_die)
        g->self->v.th_die(g->self);

    g->self = oself;
}

void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage)
{
    vec3_t   dir;
    edict_t *oldself;
    float    save, take;

    if (!targ->v.takedamage)
        return;

    g->damage_attacker = attacker;

    if (attacker->v.super_damage_finished > g->time)
        damage *= 4.0f;

    save = ceilf(targ->v.armortype * damage);
    if (save >= targ->v.armorvalue) {
        save = targ->v.armorvalue;
        targ->v.armortype = 0.0f;
        targ->v.items = (float)((int)targ->v.items & ~(IT_ARMOR1 | IT_ARMOR2 | IT_ARMOR3));
    }
    targ->v.armorvalue -= save;
    take = ceilf(damage - save);

    if ((int)targ->v.flags & FL_CLIENT) {
        targ->v.dmg_take     += take;
        targ->v.dmg_save     += save;
        targ->v.dmg_inflictor = inflictor;
    }

    if (inflictor != g->world && targ->v.movetype == MOVETYPE_WALK) {
        dir[0] = targ->v.origin[0] - (inflictor->v.absmin[0] + inflictor->v.absmax[0]) * 0.5f;
        dir[1] = targ->v.origin[1] - (inflictor->v.absmin[1] + inflictor->v.absmax[1]) * 0.5f;
        dir[2] = targ->v.origin[2] - (inflictor->v.absmin[2] + inflictor->v.absmax[2]) * 0.5f;
        eng->VectorNormalize(dir, dir);
        targ->v.velocity[0] += dir[0] * damage * 8.0f;
        targ->v.velocity[1] += dir[1] * damage * 8.0f;
        targ->v.velocity[2] += dir[2] * damage * 8.0f;
    }

    if ((int)targ->v.flags & FL_GODMODE)
        return;
    if (targ->v.invincible_finished >= g->time) {
        if (g->self->v.invincible_sound < g->time) {
            eng->SV_StartSound(targ, CHAN_ITEM, "items/protect3.wav", 1, ATTN_NORM);
            g->self->v.invincible_sound = g->time + 2.0f;
        }
        return;
    }

    if ((int)g->teamplay == 1 && targ->v.team > 0.0f && targ->v.team == attacker->v.team)
        return;

    targ->v.health -= take;

    if (targ->v.health <= 0.0f) {
        Killed(targ, attacker);
        return;
    }

    oldself = g->self;
    g->self = targ;

    if (((int)g->self->v.flags & FL_MONSTER) && attacker != g->world) {
        if (g->self != attacker && attacker != g->self->v.enemy) {
            if (!g->self->v.classname || !attacker->v.classname ||
                strcmp(g->self->v.classname, attacker->v.classname) != 0 ||
                strcmp(g->self->v.classname, "monster_army") == 0) {
                if (g->self->v.enemy &&
                    g->self->v.enemy->v.classname &&
                    strcmp(g->self->v.enemy->v.classname, "player") == 0)
                    g->self->v.oldenemy = g->self->v.enemy;
                g->self->v.enemy = attacker;
                FoundTarget();
            }
        }
    }

    if (g->self->v.th_pain) {
        g->self->v.th_pain(g->self, attacker, take);
        if (eng->Cvar_VariableValue("skill") == 3.0f)
            g->self->v.pain_finished = g->time + 5.0f;
    }

    g->self = oldself;
}

void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore)
{
    float    points;
    edict_t *head;
    vec3_t   org, diff;

    head = eng->ED_FindRadius(inflictor->v.origin, damage + 40.0f);

    while (head) {
        if (head != ignore && head->v.takedamage) {
            org[0] = head->v.origin[0] + (head->v.mins[0] + head->v.maxs[0]) * 0.5f;
            org[1] = head->v.origin[1] + (head->v.mins[1] + head->v.maxs[1]) * 0.5f;
            org[2] = head->v.origin[2] + (head->v.mins[2] + head->v.maxs[2]) * 0.5f;
            diff[0] = inflictor->v.origin[0] - org[0];
            diff[1] = inflictor->v.origin[1] - org[1];
            diff[2] = inflictor->v.origin[2] - org[2];
            points = damage - 0.5f * eng->VectorLength(diff);
            if (head == attacker) points *= 0.5f;
            if (points > 0.0f && CanDamage(head, inflictor)) {
                if (head->v.classname && strcmp(head->v.classname, "monster_shambler") == 0)
                    T_Damage(head, inflictor, attacker, points * 0.5f);
                else
                    T_Damage(head, inflictor, attacker, points);
            }
        }
        head = head->v.chain;
    }
}

void T_BeamDamage(edict_t *attacker, float damage)
{
    float    points;
    edict_t *head;
    vec3_t   diff;

    head = eng->ED_FindRadius(attacker->v.origin, damage + 40.0f);

    while (head) {
        if (head->v.takedamage) {
            diff[0] = attacker->v.origin[0] - head->v.origin[0];
            diff[1] = attacker->v.origin[1] - head->v.origin[1];
            diff[2] = attacker->v.origin[2] - head->v.origin[2];
            points = damage - 0.5f * eng->VectorLength(diff);
            if (head == attacker) points *= 0.5f;
            if (points > 0.0f && CanDamage(head, attacker)) {
                if (head->v.classname && strcmp(head->v.classname, "monster_shambler") == 0)
                    T_Damage(head, attacker, attacker, points * 0.5f);
                else
                    T_Damage(head, attacker, attacker, points);
            }
        }
        head = head->v.chain;
    }
}

// ---------------------------------------------------------------------------
// Gib helpers (player.qc: ThrowGib / ThrowHead / VelocityForDamage)
// ---------------------------------------------------------------------------
static void VelocityForDamage(float dm, vec3_t out) {
    out[0] = 100.0f * (eng->Random()*2.0f - 1.0f);
    out[1] = 100.0f * (eng->Random()*2.0f - 1.0f);
    out[2] = 200.0f + 100.0f * eng->Random();
    float scale;
    if      (dm > -50.0f)  scale = 0.7f;
    else if (dm > -200.0f) scale = 2.0f;
    else                   scale = 10.0f;
    out[0] *= scale; out[1] *= scale; out[2] *= scale;
}

void ThrowGib(const char *gibname, float dm) {
    edict_t *self = g->self;
    edict_t *n = eng->ED_Alloc();
    n->v.origin[0] = self->v.origin[0];
    n->v.origin[1] = self->v.origin[1];
    n->v.origin[2] = self->v.origin[2];
    eng->SV_SetModel(n, gibname);
    vec3_t zero = {0,0,0};
    eng->SV_SetSize(n, zero, zero);
    VelocityForDamage(dm, n->v.velocity);
    n->v.movetype = MOVETYPE_BOUNCE;
    n->v.solid    = SOLID_NOT;
    n->v.avelocity[0] = eng->Random()*600.0f;
    n->v.avelocity[1] = eng->Random()*600.0f;
    n->v.avelocity[2] = eng->Random()*600.0f;
    n->v.think     = SUB_Remove;
    n->v.ltime     = g->time;
    n->v.nextthink = g->time + 10.0f + eng->Random()*10.0f;
    n->v.frame     = 0;
    n->v.flags     = 0;
}

void ThrowHead(const char *gibname, float dm) {
    edict_t *self = g->self;
    eng->SV_SetModel(self, gibname);
    self->v.frame       = 0;
    self->v.nextthink   = -1.0f;
    self->v.movetype    = MOVETYPE_BOUNCE;
    self->v.takedamage  = DAMAGE_NO;
    self->v.solid       = SOLID_NOT;
    self->v.view_ofs[0] = 0; self->v.view_ofs[1] = 0; self->v.view_ofs[2] = 8;
    vec3_t mins = {-16,-16,0}, maxs = {16,16,56};
    eng->SV_SetSize(self, mins, maxs);
    VelocityForDamage(dm, self->v.velocity);
    self->v.origin[2] -= 24.0f;
    self->v.flags = (float)((int)self->v.flags & ~FL_ONGROUND);
    self->v.avelocity[0] = 0;
    self->v.avelocity[1] = (eng->Random()*2.0f - 1.0f) * 600.0f;
    self->v.avelocity[2] = 0;
}

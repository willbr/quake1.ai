// combat.c -- Damage application and radius effects. Source: combat.qc

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <math.h>
#include <string.h>

extern void SUB_Remove(edict_t *e);
extern void SpawnBlood(vec3_t org, vec3_t vel, float damage);
void gib_blood_burst(vec3_t origin);

extern engine_api_t   *eng;
extern game_globals_t *g;

// Weak stubs — overridden by real definitions in client.c, monsters.c, ai.c.
__attribute__((weak)) void ClientObituary(edict_t *targ, edict_t *attacker)
    { (void)targ; (void)attacker; }
__attribute__((weak)) void monster_death_use(void) {}
__attribute__((weak)) void FoundTarget(void)       {}

// ---------------------------------------------------------------------------
// Corpse over-damage → gib (table + lookup; helpers defined further below
// where they have ThrowHead/ThrowGib in scope).
//
// After a monster (or the player) finishes its death animation, its entity
// sticks around as a corpse. Vanilla Quake sets `solid = SOLID_NOT` on it so
// hitscan and traces pass straight through, and `takedamage = DAMAGE_NO` so
// further damage is ignored — corpses are inert.
//
// Here we keep the corpse trace-hittable (flat prone bbox) and damage-able,
// and route any damage taken in that state through GibCorpse() once enough
// damage has been dealt to push health below the per-classname gib threshold.
// ---------------------------------------------------------------------------

typedef struct {
    const char *classname;
    int         gib_threshold;   // gib when health drops below this (negative)
    const char *head_model;
    const char *gib_models[3];
    // Blood-pool radii in world units, scaled to monster size. corpse =
    // pool spawned at monster death; head = pool spawned when the flying
    // head gib settles; gib = pool spawned when a small chunk settles.
    float       pool_corpse;
    float       pool_head;
    float       pool_gib;
} corpse_gib_info_t;

static const corpse_gib_info_t k_corpse_gibs[] = {
    {"monster_army",         -35, "progs/h_guard.mdl",  {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 48, 18, 10},
    {"monster_enforcer",     -35, "progs/h_mega.mdl",   {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 48, 18, 10},
    {"monster_ogre",         -80, "progs/h_ogre.mdl",   {"progs/gib3.mdl","progs/gib3.mdl","progs/gib3.mdl"}, 80, 28, 16},
    {"monster_ogre_marksman",-80, "progs/h_ogre.mdl",   {"progs/gib3.mdl","progs/gib3.mdl","progs/gib3.mdl"}, 80, 28, 16},
    {"monster_demon1",       -80, "progs/h_demon.mdl",  {"progs/gib1.mdl","progs/gib1.mdl","progs/gib1.mdl"}, 72, 26, 14},
    {"monster_knight",       -40, "progs/h_knight.mdl", {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 56, 20, 12},
    {"monster_hell_knight",  -40, "progs/h_hellkn.mdl", {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 56, 20, 12},
    {"monster_shalrath",     -90, "progs/h_shal.mdl",   {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 72, 26, 14},
    {"monster_shambler",     -60, "progs/h_shams.mdl",  {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 96, 32, 18},
    {"monster_wizard",       -40, "progs/h_wizard.mdl", {"progs/gib2.mdl","progs/gib2.mdl","progs/gib2.mdl"}, 56, 20, 12},
    {"monster_dog",          -35, "progs/h_dog.mdl",    {"progs/gib3.mdl","progs/gib3.mdl","progs/gib3.mdl"}, 40, 14,  8},
    {"player",               -40, "progs/h_player.mdl", {"progs/gib1.mdl","progs/gib2.mdl","progs/gib3.mdl"}, 56, 20, 12},
};

static const corpse_gib_info_t *corpse_gib_lookup(const char *classname) {
    if (!classname) return NULL;
    for (size_t i = 0; i < sizeof(k_corpse_gibs)/sizeof(k_corpse_gibs[0]); i++) {
        if (strcmp(classname, k_corpse_gibs[i].classname) == 0)
            return &k_corpse_gibs[i];
    }
    return NULL;
}

// Corpse becomes a non-blocking volume: SOLID_TRIGGER means tracelines and
// player movement pass straight through (world.c:835-839 filters both
// SOLID_NOT and SOLID_TRIGGER from the solid clip list), but PF_findradius
// (pr_cmds.c:898) only skips SOLID_NOT — so T_RadiusDamage still finds the
// body and can over-damage it into gibs via grenades/rockets. Killed() has
// already nulled v.touch, so the trigger volume fires no touch callbacks.
// Hitscan can still over-damage corpses via Corpse_BulletTrace, which weapon
// code calls after SV_Traceline to fold any corpse-line intersection back
// into the trace_* globals.
void Corpse_LayProne(edict_t *self) {
    // SV_LinkEdict sorts entities into solid_edicts vs trigger_edicts based on
    // v.solid at link time; just changing v.solid in place leaves a now-trigger
    // entity stranded in solid_edicts, which trips the
    // "Trigger in clipping list" assert on the next traceline. SV_SetSize
    // re-links, so call it to relocate it.
    //
    // Also flatten Z to a prone slab so the debug bbox hugs the sprawled
    // corpse instead of standing tall. Model->mins/maxs can't help: it's the
    // union across all frames (including standing), so the engine's bbox
    // stays full-height. XY footprint is preserved (monsters sprawl
    // roughly within their walking diameter).
    self->v.solid = SOLID_TRIGGER;
    vec3_t mins = { self->v.mins[0], self->v.mins[1], self->v.mins[2] };
    vec3_t maxs = { self->v.maxs[0], self->v.maxs[1], self->v.mins[2] + 16.0f };
    eng->SV_SetSize(self, mins, maxs);
}

// Hybrid corpse/gib-hit for hitscan weapons. Corpses and gibs are
// SOLID_TRIGGER so the engine's traceline skips them; this helper runs an
// O(edicts) AABB-vs-segment pass over damageable triggers and, if any one is
// hit closer than the current world trace, rewrites g->trace_* to point at
// it. Downstream TraceAttack / multi-damage then routes through T_Damage —
// the deadflag branch over-damages corpses into gibs, and the gib-classname
// branch knocks individual gibs around.
//
// `start` and `end` must match the original SV_Traceline; `skip` is excluded
// (matches SV_Traceline's skip semantics). Call this AFTER eng->SV_Traceline.
void Corpse_BulletTrace(vec3_t start, vec3_t end, edict_t *skip) {
    float d[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };

    float best_t  = g->trace_fraction;  // tighten as we find closer hits
    edict_t *best_ent  = NULL;
    int best_axis = -1;
    int best_sign = 0;

    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == skip) continue;
        if (e->v.solid != SOLID_TRIGGER) continue;
        if (!e->v.takedamage) continue;

        float tmin = 0.0f, tmax = best_t;
        int axis = -1, sign = 0;
        int valid = 1;
        for (int i = 0; i < 3; i++) {
            if (fabsf(d[i]) < 1e-6f) {
                if (start[i] < e->v.absmin[i] || start[i] > e->v.absmax[i]) {
                    valid = 0; break;
                }
                continue;
            }
            float inv = 1.0f / d[i];
            float t1  = (e->v.absmin[i] - start[i]) * inv;
            float t2  = (e->v.absmax[i] - start[i]) * inv;
            // Entry face normal points opposite the ray's component on this axis.
            int s = (d[i] > 0.0f) ? -1 : 1;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) { tmin = t1; axis = i; sign = s; }
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) { valid = 0; break; }
        }
        if (valid && tmin < best_t) {
            best_t = tmin;
            best_ent = e;
            best_axis = axis;
            best_sign = sign;
        }
    }

    if (!best_ent) return;

    g->trace_fraction = best_t;
    g->trace_endpos[0] = start[0] + d[0] * best_t;
    g->trace_endpos[1] = start[1] + d[1] * best_t;
    g->trace_endpos[2] = start[2] + d[2] * best_t;
    g->trace_ent = best_ent;
    g->trace_plane_normal[0] = g->trace_plane_normal[1] = g->trace_plane_normal[2] = 0.0f;
    if (best_axis >= 0) {
        g->trace_plane_normal[best_axis] = (float)best_sign;
    } else {
        // Bullet started inside the bbox. Normal pointing back along the bullet
        // is the best we can do; downstream blood-spray code uses it only to
        // bias the spawn direction, so the magnitude doesn't matter.
        float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (len > 0.0f) {
            g->trace_plane_normal[0] = -d[0] / len;
            g->trace_plane_normal[1] = -d[1] / len;
            g->trace_plane_normal[2] = -d[2] / len;
        }
    }
    g->trace_allsolid = 0.0f;
    g->trace_startsolid = 0.0f;
}

// Forward decl — definition below where ThrowHead/ThrowGib are in scope.
int GibCorpse(edict_t *self);
static void g_throw_parent_reset(void);  // defined alongside ThrowGib/ThrowHead below

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

static void Killed_impl(edict_t *targ, edict_t *attacker)
{
    edict_t *oself = g->self;
    g->self = targ;

    // Already dead/dying: don't re-trigger the death sequence. Without this,
    // further damage on a corpse (now that we leave `takedamage = DAMAGE_YES`)
    // would re-enter Killed → th_die and restart the death animation. Actual
    // corpse over-damage is routed to GibCorpse via T_Damage's deadflag branch.
    if (g->self->v.deadflag != DEAD_NO) {
        g->self = oself;
        return;
    }

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
        const corpse_gib_info_t *pool_info = corpse_gib_lookup(g->self->v.classname);
        float pool_r = pool_info ? pool_info->pool_corpse : 0.0f;  // 0 → cvar default
        eng->spawn_blood_pool(g->self->v.origin, pool_r, g->self);
    }

    ClientObituary(g->self, attacker);

    // Stay damageable so corpses can be over-damaged into gibs. The death
    // callback (th_die) may still call ThrowHead, which sets takedamage=NO
    // on the head entity — heads/gibs are excluded by that takedamage check.
    g->self->v.takedamage = DAMAGE_YES;
    g->self->v.deadflag   = DEAD_DEAD;
    g->self->v.touch      = NULL;

    // Release the sim-AI brain slot. Without this the brain stays in
    // s_brains[] forever (Sim_AI_Frame would reset its state to IDLE
    // each tick, but the inspector reads between ticks and the editor
    // freezes ticks entirely, so a stale SEARCHING/COMBAT state surfaces
    // on a corpse or — via ThrowHead reusing this edict — on a gib).
    Sim_AI_UnregisterByEdictNum(eng->ED_GetNum(g->self));

    monster_death_use();

    // First-kill gib burst: if the killing damage drove health below the
    // per-classname gib threshold, th_die will throw the head + gibs. Mirror
    // GibCorpse's blood burst here so the visual is consistent whether the
    // gibs come from a one-shot kill (this path) or corpse over-damage
    // (GibCorpse). Origin captured before th_die runs since ThrowHead can
    // re-use self as the head entity.
    {
        const corpse_gib_info_t *info = corpse_gib_lookup(g->self->v.classname);
        if (info && g->self->v.health < (float)info->gib_threshold)
            gib_blood_burst(g->self->v.origin);
    }

    if (g->self->v.th_die)
        g->self->v.th_die(g->self);
    g_throw_parent_reset();  // clear ThrowHead's captured parent classname

    g->self = oself;
}
static void Killed(edict_t *targ, edict_t *attacker) {
    SIM_PERF("Killed") Killed_impl(targ, attacker);
}

static void T_Damage_impl(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage)
{
    vec3_t   dir;
    edict_t *oldself;
    float    save, take;

    if (!targ->v.takedamage)
        return;

    // Gib-class entity: any hit applies a directional impulse and schedules
    // removal after a short, visible fling. Grenades/rockets scatter piles
    // of gibs via this branch (T_RadiusDamage's iteration is solidity-agnostic
    // and CanDamage's traceline succeeds through SOLID_NOT heads). Shotguns
    // and nails hit gib bboxes directly.
    if (targ->v.classname && strcmp(targ->v.classname, "gib") == 0) {
        // Only explosions destroy gibs. Hitscan / projectile hits push them
        // (impulse applied at the hit site in weapons.c) but leave their
        // lifetime alone — the persistent-gib system handles eventual cleanup.
        // Client inflictor = hitscan; world inflictor = environmental damage.
        int is_hitscan = ((int)inflictor->v.flags & FL_CLIENT) != 0;
        if (inflictor != g->world && !is_hitscan) {
            vec3_t dir;
            dir[0] = targ->v.origin[0] - (inflictor->v.absmin[0] + inflictor->v.absmax[0]) * 0.5f;
            dir[1] = targ->v.origin[1] - (inflictor->v.absmin[1] + inflictor->v.absmax[1]) * 0.5f;
            dir[2] = targ->v.origin[2] - (inflictor->v.absmin[2] + inflictor->v.absmax[2]) * 0.5f;
            eng->VectorNormalize(dir, dir);
            // Multiplier matches living-target knockback (×8) — earlier I had
            // ×40, which sent heads zooming across the map and (when the
            // explosion landed past the head) right back at the player.
            targ->v.velocity[0] += dir[0] * damage * 8.0f;
            targ->v.velocity[1] += dir[1] * damage * 8.0f;
            targ->v.velocity[2] += dir[2] * damage * 8.0f + 30.0f;
            targ->v.avelocity[0] = (eng->Random()*2.0f - 1.0f) * 400.0f;
            targ->v.avelocity[1] = (eng->Random()*2.0f - 1.0f) * 400.0f;
            targ->v.avelocity[2] = (eng->Random()*2.0f - 1.0f) * 400.0f;
            targ->v.flags = (float)((int)targ->v.flags & ~FL_ONGROUND);
            // Explosion path: pop in ~0.5s so the fling is visible. If the
            // timer is already on a short fuse (multiple hits in quick
            // succession), don't extend it.
            if (targ->v.nextthink < 0.0f || targ->v.nextthink - g->time > 1.0f) {
                targ->v.nextthink = g->time + 0.5f;
                targ->v.think     = SUB_Remove;
            }
        }
        return;
    }

    // Dying / dead path: skip pain, AI re-target, knockback, etc. — none of
    // that applies once an entity has begun dying. We do keep accumulating
    // damage on health so a settled corpse can be over-damaged into gibs.
    if (targ->v.deadflag != DEAD_NO) {
        targ->v.health -= damage;
        if (targ->v.deadflag == DEAD_DEAD) {
            const corpse_gib_info_t *info = corpse_gib_lookup(targ->v.classname);
            if (info && targ->v.health < (float)info->gib_threshold)
                GibCorpse(targ);
        }
        return;
    }

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
void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage) {
    SIM_PERF("T_Damage") T_Damage_impl(targ, inflictor, attacker, damage);
}

void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore)
{
    SIM_PERF("T_RadiusDamage") {
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

// ---------------------------------------------------------------------------
// Persistent gibs: long lifetime, despawn only when out of player sight, with
// an LRU ring as a safety net so sustained spam can't exhaust MAX_EDICTS.
//
// Cap is conservative because MAX_EDICTS (600) is shared with monsters,
// items, projectiles, and Phase 8 sim entities. Reserving ~256 for gibs
// leaves the rest of the world room to function under heavy spam.
//
// Trade-off: a gib won't disappear in front of the player, but at the cap
// (MAX_LIVE_GIBS) the oldest gib is force-freed regardless of visibility.
// The player is rarely staring at the literal oldest gib, so visible pops
// are rare in practice.
// ---------------------------------------------------------------------------

#define MAX_LIVE_GIBS 256

static edict_t *gib_ring[MAX_LIVE_GIBS];
static int      gib_ring_head;   // index of the oldest stored gib
static int      gib_ring_count;  // number of stored gibs (always <= MAX_LIVE_GIBS)

// Returns 1 if the gib's origin has a clean line-of-sight to the player and
// the player is roughly facing it (front 180° hemisphere). Conservative — a
// "yes" defers despawn; a "no" allows it.
static int gib_visible_to_player(edict_t *gib) {
    edict_t *client = eng->ED_CheckClient();
    if (!client || client->free) return 0;

    vec3_t eye = {
        client->v.origin[0] + client->v.view_ofs[0],
        client->v.origin[1] + client->v.view_ofs[1],
        client->v.origin[2] + client->v.view_ofs[2],
    };
    vec3_t target = {
        gib->v.origin[0], gib->v.origin[1], gib->v.origin[2],
    };
    eng->SV_Traceline(eye, target, 1, gib);
    if (g->trace_fraction < 1.0f) return 0;  // wall between player and gib

    // Front hemisphere — use v_angle (where the player is aiming) since that
    // drives the rendered viewport, not v.angles (body yaw).
    eng->MakeVectors(client->v.v_angle);
    vec3_t raw = {
        gib->v.origin[0] - client->v.origin[0],
        gib->v.origin[1] - client->v.origin[1],
        gib->v.origin[2] - client->v.origin[2],
    };
    vec3_t to_gib;
    eng->VectorNormalize(raw, to_gib);
    float dot = to_gib[0]*g->v_forward[0]
              + to_gib[1]*g->v_forward[1]
              + to_gib[2]*g->v_forward[2];
    return (dot > 0.0f);
}

// Think entry — set as v.think on each gib. Defers if the player can see it,
// otherwise frees the edict.
static void Gib_TimedRemove(edict_t *self) {
    g->self = self;
    if (gib_visible_to_player(self)) {
        // Recheck soon — short enough that turning to face a settled gib
        // catches it before it pops.
        self->v.nextthink = g->time + 1.0f + eng->Random();
        return;
    }
    eng->ED_Free(self);
}

// LRU sweep: drop entries from the ring head until we land on a gib pointer
// that still looks like a live gib, then force-free it. Slots whose edicts
// were freed (auto-removal) or repurposed are silently skipped.
static void gib_ring_force_oldest_free(void) {
    while (gib_ring_count > 0) {
        edict_t *oldest = gib_ring[gib_ring_head];
        gib_ring[gib_ring_head] = 0;
        gib_ring_head = (gib_ring_head + 1) % MAX_LIVE_GIBS;
        gib_ring_count--;
        if (!oldest || oldest->free)                       continue;
        if (oldest->v.decal_on_bounce != 1.0f)             continue;
        if ((int)oldest->v.movetype != MOVETYPE_BOUNCE)    continue;
        eng->ED_Free(oldest);
        return;
    }
}

static void gib_ring_add(edict_t *new_gib) {
    if (gib_ring_count == MAX_LIVE_GIBS) {
        // Ring full — force the oldest out so the new gib has headroom.
        gib_ring_force_oldest_free();
    }
    int tail = (gib_ring_head + gib_ring_count) % MAX_LIVE_GIBS;
    gib_ring[tail] = new_gib;
    gib_ring_count++;
}

// Captured by ThrowHead before it overwrites self->v.classname = "gib", so
// the subsequent 3× ThrowGib calls (which see self->v.classname == "gib")
// can still find the parent monster's per-classname pool radii. Cleared
// after the death sequence by g_throw_parent_reset().
static const char *g_throw_parent_classname = NULL;
static void g_throw_parent_reset(void) { g_throw_parent_classname = NULL; }

// Best-effort parent lookup: prefer the ThrowHead-captured classname; fall
// back to self->v.classname (covers monster die handlers that call ThrowGib
// without a preceding ThrowHead, where self is still the monster).
static const corpse_gib_info_t *gib_parent_info(edict_t *self) {
    if (g_throw_parent_classname)
        return corpse_gib_lookup(g_throw_parent_classname);
    return corpse_gib_lookup(self->v.classname);
}

void ThrowGib(const char *gibname, float dm) {
    edict_t *self = g->self;
    const corpse_gib_info_t *info = gib_parent_info(self);
    edict_t *n = eng->ED_Alloc();
    // ED_Alloc reuses slots freed > 0.5s ago. If the reused slot was a
    // monster whose brain was never cleared, the gib would inherit that
    // brain. Killed_impl clears the original death-slot, but a gib's
    // think can free the gib and a future ED_Alloc could land in a
    // different stale slot — clear defensively.
    Sim_AI_UnregisterByEdictNum(eng->ED_GetNum(n));
    n->v.origin[0] = self->v.origin[0];
    n->v.origin[1] = self->v.origin[1];
    n->v.origin[2] = self->v.origin[2];
    n->v.classname = "gib";
    eng->SV_SetModel(n, gibname);
    // SOLID_TRIGGER: non-blocking for player movement and engine tracelines
    // (world.c:835-839 filters triggers from the solid clip list), but
    // PF_findradius (pr_cmds.c:898) still includes it so explosions scatter
    // gibs via the T_Damage gib branch, and Corpse_BulletTrace picks it up
    // so shotgun pellets can knock a gib around. Re-link via SV_SetSize so
    // the edict files under trigger_edicts; setting v.solid in place would
    // strand it in solid_edicts and trip the next traceline's assert.
    n->v.solid     = SOLID_TRIGGER;
    eng->SV_SetSize(n, n->v.mins, n->v.maxs);
    VelocityForDamage(dm, n->v.velocity);
    n->v.movetype  = MOVETYPE_BOUNCE;
    n->v.takedamage = DAMAGE_YES;
    n->v.health    = 1.0f;
    n->v.avelocity[0] = eng->Random()*600.0f;
    n->v.avelocity[1] = eng->Random()*600.0f;
    n->v.avelocity[2] = eng->Random()*600.0f;
    n->v.think     = Gib_TimedRemove;
    n->v.ltime     = g->time;
    // Long initial wait; Gib_TimedRemove keeps deferring if visible. First
    // visibility check fires somewhere between 10–11 min — by then the
    // player has almost certainly looked away at least once.
    n->v.nextthink = g->time + 600.0f + eng->Random()*60.0f;
    n->v.frame     = 0;
    n->v.flags     = 0;
    n->v.decal_on_bounce = 1.0f;
    eng->set_gib_blood_budget(n, 4.0f, info ? info->pool_gib : 0.0f);

    gib_ring_add(n);
}

void ThrowHead(const char *gibname, float dm) {
    edict_t *self = g->self;
    const corpse_gib_info_t *info = corpse_gib_lookup(self->v.classname);
    // Belt-and-suspenders: Killed_impl already cleared the brain on this
    // edict, but ThrowHead can also be called from other death paths
    // (player_die, etc.) that skip Killed_impl. The edict is being
    // repurposed from monster → head-gib so the brain has to go.
    Sim_AI_UnregisterByEdictNum(eng->ED_GetNum(self));
    g_throw_parent_classname = self->v.classname;  // for subsequent ThrowGib calls
    self->v.classname   = "gib";  // override the corpse classname; this is now a gib
    eng->SV_SetModel(self, gibname);
    self->v.frame       = 0;
    self->v.nextthink   = -1.0f;
    self->v.movetype    = MOVETYPE_BOUNCE;
    // SOLID_TRIGGER for the same reasons as ThrowGib: non-blocking for player
    // and traces, but findradius and Corpse_BulletTrace still reach it. The
    // SV_SetSize call re-links the edict into trigger_edicts.
    self->v.takedamage  = DAMAGE_YES;
    self->v.health      = 1.0f;
    self->v.solid       = SOLID_TRIGGER;
    self->v.view_ofs[0] = 0; self->v.view_ofs[1] = 0; self->v.view_ofs[2] = 8;
    eng->SV_SetSize(self, self->v.mins, self->v.maxs);
    VelocityForDamage(dm, self->v.velocity);
    self->v.flags = (float)((int)self->v.flags & ~FL_ONGROUND);
    self->v.avelocity[0] = 0;
    self->v.avelocity[1] = (eng->Random()*2.0f - 1.0f) * 600.0f;
    self->v.avelocity[2] = 0;
    self->v.decal_on_bounce = 1.0f;
    eng->set_gib_blood_budget(self, 4.0f, info ? info->pool_head : 0.0f);
}

// Blood burst for a gib event. Several SpawnBlood calls with randomized
// outward velocities give a fuller spray than one large burst, since
// TE_BLOODSPRAY's particle count is capped at 255 per call and a single
// direction reads as a one-sided plume rather than a corpse rupture.
// Called from both gib paths: first-kill (th_die) via Killed, and
// corpse-over-damage via GibCorpse.
void gib_blood_burst(vec3_t origin) {
    vec3_t centre = { origin[0], origin[1], origin[2] + 8.0f };
    for (int i = 0; i < 6; i++) {
        vec3_t v = {
            (eng->Random() * 2.0f - 1.0f) * 180.0f,
            (eng->Random() * 2.0f - 1.0f) * 180.0f,
            eng->Random() * 200.0f + 40.0f,
        };
        SpawnBlood(centre, v, 40.0f);
    }
}

// Throw a head + 3 gibs from a corpse with the per-classname models. Returns
// 1 on success, 0 if no gib info is registered for this classname (in which
// case the caller should leave the corpse intact). Used by the corpse over-
// damage path; the initial gib-death (when a monster is killed with enough
// overkill that its own th_die spawns gibs) does NOT go through here and
// keeps the vanilla random-velocity fling.
int GibCorpse(edict_t *self) {
    const corpse_gib_info_t *info = corpse_gib_lookup(self->v.classname);
    if (!info) return 0;
    edict_t *oself = g->self;
    g->self = self;
    eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
    gib_blood_burst(self->v.origin);
    ThrowHead(info->head_model, self->v.health);
    // Stationary head: the corpse was already settled when over-damaged, so
    // a random VelocityForDamage on the head looks like the head is sliding
    // around long after the hit. Zeroing here keeps it where the corpse fell;
    // a later grenade can still kick it via the T_Damage gib branch.
    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0.0f;
    self->v.avelocity[0] = self->v.avelocity[1] = self->v.avelocity[2] = 0.0f;
    ThrowGib(info->gib_models[0], self->v.health);
    ThrowGib(info->gib_models[1], self->v.health);
    ThrowGib(info->gib_models[2], self->v.health);
    g_throw_parent_reset();  // clear ThrowHead's captured parent classname
    g->self = oself;
    return 1;
}

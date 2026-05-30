// sim_fire.c -- Burning damage-over-time + fire FX (Phase 8 / M8, stage F1).
//
// A fixed registry keyed by edict number holds which edicts are on fire.
// Fire_Frame() ticks at 10 Hz: applies T_Damage, spawns flame particles,
// sets EF_DIMLIGHT, injects smoke into the wind grid, and emits STIM_FIRE.
// All state is DLL-side; nothing touches entvars_t or the engine ABI.

#include "sim.h"
#include "../game_defs.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
// Corpses are SOLID_TRIGGER, which eng->SV_Traceline skips; this folds any
// closer corpse hit back into g->trace_* (same helper the weapons use).
extern void Corpse_BulletTrace(vec3_t start, vec3_t end, edict_t *skip);

#define FIRE_MAX_BURNING     SIM_MAX_BRAINS    // one slot per possible edict
#define FIRE_TICK_HZ         10.0f
#define FIRE_DMG_INTERVAL    0.5f              // seconds between damage applications
#define FIRE_HAZARD_RADIUS   64.0f             // burning edict's danger radius (unused until F2 oil, kept for clarity)
#define FIRE_AI_AVOID_RADIUS 160.0f            // non-burning monsters flee fire within this
#define FIRE_SMOKE_AMOUNT    0.12f
#define FIRE_SMOKE_RADIUS    40.0f

// --- Oil substance (F2) ---------------------------------------------------
#define OIL_MAX_PATCHES      256
#define OIL_DEFAULT_RADIUS   48.0f
#define OIL_DEFAULT_AMOUNT   1.0f
#define OIL_MERGE_DIST       40.0f     // deposit within this of an unlit patch merges in
#define OIL_TTL_SECS         60.0f     // unlit oil evaporates after this (generous)
#define OIL_BURN_SECS        4.0f      // how long a lit patch burns before the oil is spent
#define OIL_DMG_INTERVAL     0.5f      // seconds between area-DOT applications
#define OIL_PATCH_DPS        6.0f      // damage/sec to edicts standing in burning oil
#define OIL_IGNITE_SECS      3.0f      // burn duration handed to edicts a patch ignites
#define OIL_CASCADE_RADIUS   80.0f     // a lit patch schedules unlit patches within this
#define OIL_CASCADE_DELAY    0.35f     // delay before a scheduled neighbour catches
#define OIL_COAT_SECS        8.0f      // how long an edict stays oil-coated
#define OIL_COAT_BURN_SECS   8.0f      // a coated edict burns this long (vs OIL_IGNITE_SECS)

typedef struct {
    int   active;
    float burn_until;
    float dps;
    int   igniter_edict;     // -1 = world / unknown
    float next_dmg_time;
    int   corpse_timed;      // 1 once the corpse burn window has been applied
} fire_burn_t;

static fire_burn_t s_burning[FIRE_MAX_BURNING];   // indexed by edict number
static float       s_next_tick;

typedef struct {
    int    active;
    int    lit;
    vec3_t origin;
    float  radius;
    float  amount;
    float  deposit_time;
    float  ignite_at;       // >0: scheduled cascade ignition time; 0: not scheduled
    float  burn_until;      // when lit: expiry time
    float  next_dmg_time;   // when lit: next area-DOT application
} oil_patch_t;

static oil_patch_t s_oil[OIL_MAX_PATCHES];

// Coated edicts (by edict number): g->time < value => still oil-coated.
static float s_coated_until[FIRE_MAX_BURNING];

// Defined later (cascade version); Fire_IgniteTraced's oil fallback below
// needs it to light a patch the player aims at.
static void oil_light_patch(oil_patch_t *o);

// O(MAX_EDICTS) walk — acceptable at 10 Hz over a small registry (mirrors
// the documented O(N) pattern in sim_ai.c).
static edict_t *fire_find_edict(int num) {
    if (num < 0) return 0;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e))
        if (eng->ED_GetNum(e) == num) return e;
    return 0;
}

// Centered random in [-1,1], for scattering oil/flame particles across a
// patch. (F1 had this; its cleanup dropped it as unused -- F2 brings it back.)
static float fire_crand(void) { return eng->Random() * 2.0f - 1.0f; }

void Fire_Init(void) {
    memset(s_burning, 0, sizeof(s_burning));
    memset(s_oil, 0, sizeof(s_oil));
    memset(s_coated_until, 0, sizeof(s_coated_until));
    s_next_tick = 0.0f;
    eng->Cvar_Register("sim_fire_debug",  "0");
    eng->Cvar_Register("fire_dps",        "8");
    eng->Cvar_Register("fire_secs",       "5");
    eng->Cvar_Register("fire_ignite_num", "-1");   // MCP/console test hook
    eng->Cvar_Register("fire_noflee",     "0");    // debug: burning monsters stand still
    eng->Cvar_Register("fire_corpse_secs","8");    // how long a corpse smoulders
    eng->Cvar_Register("fire_oil_num",    "-1");   // deposit oil at edict N (test hook)
    eng->Cvar_Register("fire_oil_ignite", "0");    // light nearest oil to player (test hook)
    eng->Cvar_Register("fire_oil_count",  "0");    // Fire_Frame writes live patch count
}

void Fire_LevelInit(void) {
    memset(s_burning, 0, sizeof(s_burning));
    memset(s_oil, 0, sizeof(s_oil));
    memset(s_coated_until, 0, sizeof(s_coated_until));
    s_next_tick = 0.0f;
}

static void fire_clear_slot(int n, edict_t *e) {
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    s_burning[n].active = 0;
    s_burning[n].corpse_timed = 0;
    // Clear any oil coat too, so it can't linger onto a reused edict number
    // when this burn ends (extinguish / timeout / free). Mirrors how the burn
    // slot itself is cleared here. A coated-but-never-ignited edict that's
    // freed is the one narrow case this misses; its coat is time-bounded
    // (OIL_COAT_SECS) and harmless even if the number is reused.
    s_coated_until[n] = 0.0f;
    if (e) e->v.effects = (float)((int)e->v.effects & ~EF_DIMLIGHT);
}

void Fire_Ignite(edict_t *e, float seconds, float dps, edict_t *igniter) {
    if (!e || e->free) return;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    fire_burn_t *f = &s_burning[n];
    float until = g->time + seconds;
    if (!f->active || until > f->burn_until) f->burn_until = until;
    if (!f->active) f->corpse_timed = 0;   // fresh ignite re-evaluates the corpse window
    f->active        = 1;
    // Re-igniting an already-burning edict is last-writer-wins on dps/igniter
    // (the timer above only ever extends, never shortens). Intentional: a
    // fresher/hotter ignition source takes over the ongoing burn.
    f->dps           = dps;
    f->igniter_edict = (igniter && !igniter->free) ? eng->ED_GetNum(igniter) : -1;
    if (f->next_dmg_time < g->time) f->next_dmg_time = g->time + FIRE_DMG_INTERVAL;
}

void Fire_Extinguish(edict_t *e) {
    if (!e || e->free) return;
    fire_clear_slot(eng->ED_GetNum(e), e);
}

int Fire_IsBurning(int edict_num) {
    if (edict_num < 0 || edict_num >= FIRE_MAX_BURNING) return 0;
    return s_burning[edict_num].active;
}

int Fire_GetIgniterOrigin(int edict_num, vec3_t out) {
    if (edict_num < 0 || edict_num >= FIRE_MAX_BURNING) return 0;
    if (!s_burning[edict_num].active) return 0;
    edict_t *ig = fire_find_edict(s_burning[edict_num].igniter_edict);
    if (!ig) return 0;
    out[0] = ig->v.origin[0]; out[1] = ig->v.origin[1]; out[2] = ig->v.origin[2];
    return 1;
}

int Fire_NearestHazard(const vec3_t pos, float radius, vec3_t out) {
    float best2 = radius * radius;
    int found = 0;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        int n = eng->ED_GetNum(e);
        if (n < 0 || n >= FIRE_MAX_BURNING || !s_burning[n].active) continue;
        float dx = pos[0] - e->v.origin[0];
        float dy = pos[1] - e->v.origin[1];
        float dz = pos[2] - e->v.origin[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best2) {
            best2 = d2;
            out[0] = e->v.origin[0]; out[1] = e->v.origin[1]; out[2] = e->v.origin[2];
            found = 1;
        }
    }
    return found;
}

// Deposit a patch of flammable oil at `origin`. radius<=0 / amount<=0 use
// defaults. Merges into a nearby unlit patch so dense deposits don't thrash
// the pool; otherwise allocates a free slot, recycling the oldest if full.
void Fire_AddOil(const vec3_t origin, float radius, float amount) {
    if (radius <= 0.0f) radius = OIL_DEFAULT_RADIUS;
    if (amount <= 0.0f) amount = OIL_DEFAULT_AMOUNT;

    // Merge into a nearby UNLIT patch so dense deposits don't thrash the pool.
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *o = &s_oil[i];
        if (!o->active || o->lit) continue;
        float dx = origin[0] - o->origin[0];
        float dy = origin[1] - o->origin[1];
        float dz = origin[2] - o->origin[2];
        if (dx*dx + dy*dy + dz*dz <= OIL_MERGE_DIST * OIL_MERGE_DIST) {
            o->amount += amount;
            if (radius > o->radius) o->radius = radius;
            o->deposit_time = g->time;
            return;
        }
    }

    // Allocate a free slot; if none, recycle the oldest patch (logged, never silent).
    int slot = -1;
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        if (!s_oil[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        float oldest = 1e30f;
        for (int i = 0; i < OIL_MAX_PATCHES; i++) {
            if (s_oil[i].deposit_time < oldest) { oldest = s_oil[i].deposit_time; slot = i; }
        }
        eng->Con_DPrintf("sim_fire: oil pool full, recycling oldest patch\n");
    }

    oil_patch_t *o = &s_oil[slot];
    memset(o, 0, sizeof(*o));
    o->active       = 1;
    o->origin[0]    = origin[0];
    o->origin[1]    = origin[1];
    o->origin[2]    = origin[2];
    o->radius       = radius;
    o->amount       = amount;
    o->deposit_time = g->time;

    // Coat edicts standing in the fresh oil so a later spark ignites them
    // hotter/longer. (The merge branch returns early and does not coat -- a
    // merge just tops up amount on an existing patch.)
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (!e->v.takedamage) continue;
        int en = eng->ED_GetNum(e);
        if (en < 0 || en >= FIRE_MAX_BURNING) continue;
        float dx = e->v.origin[0] - o->origin[0];
        float dy = e->v.origin[1] - o->origin[1];
        if (dx*dx + dy*dy <= o->radius * o->radius)
            s_coated_until[en] = g->time + OIL_COAT_SECS;
    }
}

// Ignite `e`, but if it is oil-coated, burn it longer (OIL_COAT_BURN_SECS).
// Used wherever a fire source touches an edict (oil-patch contact now; later
// weapons/explosions in F3+).
void Fire_IgniteMaybeCoated(edict_t *e, float base_secs, float dps, edict_t *igniter) {
    if (!e) return;
    int en = eng->ED_GetNum(e);
    float secs = base_secs;
    if (en >= 0 && en < FIRE_MAX_BURNING && g->time < s_coated_until[en])
        secs = OIL_COAT_BURN_SECS;
    Fire_Ignite(e, secs, dps, igniter);
}

// Light the unlit oil patch that world point `pos` falls within (nearest if
// several overlap). Returns 1 if one was lit. Lets the crosshair igniter light
// oil by aiming at it, not just entities.
static int oil_light_at(const vec3_t pos) {
    int best = -1; float best2 = 1e30f;
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *o = &s_oil[i];
        if (!o->active || o->lit) continue;
        float dx = pos[0] - o->origin[0];
        float dy = pos[1] - o->origin[1];
        float dz = pos[2] - o->origin[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 <= o->radius * o->radius && d2 < best2) { best2 = d2; best = i; }
    }
    if (best < 0) return 0;
    oil_light_patch(&s_oil[best]);
    return 1;
}

void Fire_IgniteTraced(edict_t *player) {
    if (!player) return;
    float dps  = eng->Cvar_VariableValue("fire_dps");
    float secs = eng->Cvar_VariableValue("fire_secs");
    eng->MakeVectors(player->v.v_angle);
    vec3_t src = { player->v.origin[0],
                   player->v.origin[1],
                   player->v.origin[2] + player->v.view_ofs[2] };
    vec3_t end = { src[0] + g->v_forward[0] * 2048.0f,
                   src[1] + g->v_forward[1] * 2048.0f,
                   src[2] + g->v_forward[2] * 2048.0f };
    eng->SV_Traceline(src, end, 0, player);
    Corpse_BulletTrace(src, end, player);   // let the trace hit SOLID_TRIGGER corpses
    edict_t *t = g->trace_ent;
    if (t && t != g->world && t->v.takedamage) {
        Fire_Ignite(t, secs, dps, player);
        eng->Con_Print("fire: ignited entity under crosshair\n");
    } else if (oil_light_at(g->trace_endpos)) {
        // No flammable entity, but the crosshair landed in oil -- light it.
        eng->Con_Print("fire: lit oil at crosshair\n");
    } else {
        eng->Con_Print("fire: no flammable entity or oil under crosshair\n");
    }
}

// Debug: trace from the player's view to the floor and deposit oil there.
void Fire_OilTraced(edict_t *player) {
    if (!player) return;
    eng->MakeVectors(player->v.v_angle);
    vec3_t src = { player->v.origin[0],
                   player->v.origin[1],
                   player->v.origin[2] + player->v.view_ofs[2] };
    vec3_t end = { src[0] + g->v_forward[0] * 2048.0f,
                   src[1] + g->v_forward[1] * 2048.0f,
                   src[2] + g->v_forward[2] * 2048.0f };
    eng->SV_Traceline(src, end, 1, player);   // nomonsters: hit the floor
    if (g->trace_fraction < 1.0f) {
        Fire_AddOil(g->trace_endpos, OIL_DEFAULT_RADIUS, OIL_DEFAULT_AMOUNT);
        eng->Con_Print("fire: oil deposited\n");
    }
}

// Light an oil patch: flip it lit, set its burn + DOT windows, and schedule
// nearby UNLIT patches to catch after a short delay so fire races down an oil
// trail (discrete patch-to-patch -- no fluid solver). A just-scheduled patch
// (ignite_at>0) is skipped so the cascade converges.
static void oil_light_patch(oil_patch_t *o) {
    if (!o->active || o->lit) return;
    o->lit           = 1;
    o->burn_until    = g->time + OIL_BURN_SECS;
    o->next_dmg_time = g->time + OIL_DMG_INTERVAL;
    o->ignite_at     = 0.0f;

    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *n = &s_oil[i];
        if (n == o || !n->active || n->lit || n->ignite_at > 0.0f) continue;
        float dx = n->origin[0] - o->origin[0];
        float dy = n->origin[1] - o->origin[1];
        float dz = n->origin[2] - o->origin[2];
        if (dx*dx + dy*dy + dz*dz <= OIL_CASCADE_RADIUS * OIL_CASCADE_RADIUS)
            n->ignite_at = g->time + OIL_CASCADE_DELAY;
    }
}

// The player edict (number 1), or NULL.
static edict_t *oil_find_player(void) {
    return fire_find_edict(1);
}

// Tick the oil-patch pool at the existing 10 Hz fire cadence: process the
// deposit/ignite test hooks, evaporate expired unlit patches, render the unlit
// sheen, cascade-schedule + light scheduled/contacted patches, and run lit
// patch area-DOT / flame / smoke / stim. O(patches), with O(edicts) inner
// scans for the contact/DOT checks -- fine at this tick rate for realistic
// patch counts; would want spatial buckets if patch counts ever grow large.
static void oil_frame(void) {
    // Test hook: deposit oil at edict N's origin once.
    {
        int req = (int)eng->Cvar_VariableValue("fire_oil_num");
        if (req >= 0) {
            edict_t *e = fire_find_edict(req);
            if (e && !e->free) Fire_AddOil(e->v.origin, OIL_DEFAULT_RADIUS, OIL_DEFAULT_AMOUNT);
            eng->Cvar_SetValue("fire_oil_num", -1.0f);
        }
    }

    // Test hook: light the unlit oil patch nearest the player once.
    if (eng->Cvar_VariableValue("fire_oil_ignite") != 0.0f) {
        eng->Cvar_SetValue("fire_oil_ignite", 0.0f);
        edict_t *p = oil_find_player();
        if (p) {
            int best = -1; float best2 = 1e30f;
            for (int i = 0; i < OIL_MAX_PATCHES; i++) {
                if (!s_oil[i].active || s_oil[i].lit) continue;
                float dx = s_oil[i].origin[0] - p->v.origin[0];
                float dy = s_oil[i].origin[1] - p->v.origin[1];
                float dz = s_oil[i].origin[2] - p->v.origin[2];
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < best2) { best2 = d2; best = i; }
            }
            if (best >= 0) oil_light_patch(&s_oil[best]);
        }
    }

    int live = 0;
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *o = &s_oil[i];
        if (!o->active) continue;

        // Unlit oil evaporates after its TTL.
        if (!o->lit && g->time - o->deposit_time > OIL_TTL_SECS) {
            o->active = 0;
            continue;
        }

        live++;

        if (!o->lit) {
            // Cascade: a scheduled neighbour catches when its delay elapses.
            if (o->ignite_at > 0.0f && g->time >= o->ignite_at) {
                oil_light_patch(o);
                continue;   // already counted; handle as lit next tick
            }
            // A burning edict standing in unlit oil sets it alight.
            for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
                int en = eng->ED_GetNum(e);
                if (en < 0 || en >= FIRE_MAX_BURNING || !Fire_IsBurning(en)) continue;
                float dx = e->v.origin[0] - o->origin[0];
                float dy = e->v.origin[1] - o->origin[1];
                if (dx*dx + dy*dy <= o->radius * o->radius) { oil_light_patch(o); break; }
            }
            if (o->lit) continue;   // lit this tick (already counted); handle as lit next tick

            // Sparse dark sheen so the player can see where oil lies (a proper
            // floor decal is a later-stage upgrade; particles are the MVP).
            vec3_t org = { o->origin[0] + fire_crand() * o->radius * 0.6f,
                           o->origin[1] + fire_crand() * o->radius * 0.6f,
                           o->origin[2] + 2.0f };
            vec3_t dir = { 0, 0, 0 };
            eng->SV_Smoke(org, dir, 4.0f, 1.0f);   // colour 4 = dark grey
            continue;
        }

        // --- Lit patch ---
        if (g->time >= o->burn_until) {
            o->active = 0;   // oil consumed
            continue;
        }

        // Area damage + contact ignition for edicts standing in the patch.
        if (g->time >= o->next_dmg_time) {
            o->next_dmg_time = g->time + OIL_DMG_INTERVAL;
            for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
                if (!e->v.takedamage) continue;
                float dx = e->v.origin[0] - o->origin[0];
                float dy = e->v.origin[1] - o->origin[1];
                if (dx*dx + dy*dy > o->radius * o->radius) continue;
                T_Damage(e, g->world, g->world, OIL_PATCH_DPS * OIL_DMG_INTERVAL);
                Fire_IgniteMaybeCoated(e, OIL_IGNITE_SECS,
                                       eng->Cvar_VariableValue("fire_dps"), g->world);
            }
        }

        // Flame plume across the patch + smoke into the wind grid. MUST use
        // SV_Fire (rising pt_fireblob -- the F1 look), NOT SV_Particle: the
        // svc_particle/pt_grav path falls and sprays a debris cone, so it can
        // never read as flame. SV_Fire takes no colour (orange->grey ramp3 is
        // internal); count drives blobs per call.
        for (int p = 0; p < 3; p++) {
            vec3_t org = { o->origin[0] + fire_crand() * o->radius * 0.7f,
                           o->origin[1] + fire_crand() * o->radius * 0.7f,
                           o->origin[2] + 4.0f };
            vec3_t up  = { 0.0f, 0.0f, 12.0f };
            eng->SV_Fire(org, up, 4.0f);
        }
        Wind_AddSmoke(o->origin, FIRE_SMOKE_AMOUNT, o->radius);

        // Fire stimulus so AI registers/avoids the burning oil. F2 note: many
        // patches emitting at 10 Hz can crowd the 512-entry stim ring (see the
        // F1 note in Fire_Frame) -- revisit throttling if it starves stims.
        {
            stimulus_t st;
            memset(&st, 0, sizeof(st));
            st.kind         = STIM_FIRE;
            st.origin[0]    = o->origin[0];
            st.origin[1]    = o->origin[1];
            st.origin[2]    = o->origin[2];
            st.intensity    = 0.8f;
            st.source_edict = -1;   // world-sourced; no self-react concern
            Stim_Emit(&st);
        }
    }

    eng->Cvar_SetValue("fire_oil_count", (float)live);
}

void Fire_Frame(void) {
    if (g->time < s_next_tick) return;
    s_next_tick = g->time + (1.0f / FIRE_TICK_HZ);

    // MCP/console test hook: `fire_ignite_num <N>` ignites edict N once.
    {
        int req = (int)eng->Cvar_VariableValue("fire_ignite_num");
        if (req >= 0) {
            edict_t *e = fire_find_edict(req);
            if (e && !e->free && e->v.takedamage)
                Fire_Ignite(e, eng->Cvar_VariableValue("fire_secs"),
                               eng->Cvar_VariableValue("fire_dps"), g->world);
            eng->Cvar_SetValue("fire_ignite_num", -1.0f);
        }
    }

    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        int n = eng->ED_GetNum(e);
        if (n < 0 || n >= FIRE_MAX_BURNING) continue;
        fire_burn_t *f = &s_burning[n];
        if (!f->active) continue;

        // Water or slime douses the fire; lava must NOT (it is fire). Check
        // both the origin-point contents and the physics waterlevel so the
        // fire goes out even when the origin sits above the surface in
        // shallower water (e.g. landing in a knee/waist-deep pool).
        int   con      = eng->SV_PointContents(e->v.origin);
        int   wtype    = (int)e->v.watertype;
        int   inwater  = (con == CONTENT_WATER || con == CONTENT_SLIME)
                       || ((int)e->v.waterlevel >= 2 &&
                           (wtype == CONTENT_WATER || wtype == CONTENT_SLIME));
        int   dead     = (e->v.health <= 0.0f || e->v.deadflag != DEAD_NO);

        // A burning body is fresh fuel: the first tick it is (or is ignited
        // as) a corpse, give it a full finite burn window from *now* so it
        // smoulders for a satisfying while -- independent of how little of the
        // live burn was left when it died. One-time per ignite (corpse_timed),
        // so it still goes out on its own afterwards.
        if (dead && !f->corpse_timed) {
            f->corpse_timed = 1;
            float until = g->time + eng->Cvar_VariableValue("fire_corpse_secs");
            if (until > f->burn_until) f->burn_until = until;
        }

        // Death does NOT extinguish: corpses are flammable and keep burning
        // until the timer expires (or they're freed / submerged). A monster
        // that dies from the burn therefore keeps smouldering as a corpse.
        if (e->free || g->time >= f->burn_until || inwater) {
            fire_clear_slot(n, e->free ? 0 : e);
            continue;
        }

        // Damage in discrete chunks so armor/save math stays meaningful.
        // Skip on corpses: a dead body just burns visually -- re-damaging it
        // would drive health further negative and gib it mid-burn.
        if (!dead && g->time >= f->next_dmg_time && e->v.takedamage) {
            f->next_dmg_time = g->time + FIRE_DMG_INTERVAL;
            edict_t *attacker = fire_find_edict(f->igniter_edict);
            if (!attacker || attacker->free) attacker = g->world;
            T_Damage(e, g->world, attacker, f->dps * FIRE_DMG_INTERVAL);
        }
        // Fire visuals: a warm dynamic-light glow plus a rising flame plume.
        // SV_Fire spawns engine pt_fire particles (orange ramp3, drift upward,
        // fade orange->grey over ~1.2s). The svc_particle/pt_grav path only
        // falls and sprays a debris cone, so it can't read as flame.
        e->v.effects = (float)((int)e->v.effects | EF_DIMLIGHT);
        {
            // Spawn the plume across the entity's world bbox so it engulfs the
            // body in any pose. Corpses are already flattened to a low prone
            // slab by Corpse_LayProne (combat.c) -- so absmin..absmax hugs the
            // sprawled body, while a standing monster gets a full-height
            // column. Keying off origin+const would float the fire above a
            // corpse, whose origin stays at standing height.
            float *mn = e->v.absmin, *mx = e->v.absmax;
            float cx = (mn[0] + mx[0]) * 0.5f;
            float cy = (mn[1] + mx[1]) * 0.5f;
            float h  = mx[2] - mn[2];
            if (h < 8.0f) h = 8.0f;
            vec3_t up = { 0.0f, 0.0f, 10.0f };
            for (int s = 0; s < 3; s++) {
                vec3_t org = { cx, cy, mn[2] + h * ((s + 0.5f) / 3.0f) };
                eng->SV_Fire(org, up, 5.0f);
            }
        }

        // Feed the M4 wind/smoke grid so fire throws up a smoke screen.
        Wind_AddSmoke(e->v.origin, FIRE_SMOKE_AMOUNT, FIRE_SMOKE_RADIUS);

        // Broadcast a fire stimulus so distant AI can register the threat.
        // F2 note: many simultaneous fire sources (oil patches) emitting at
        // 10 Hz each can crowd the 512-entry stim ring within the 5 s age
        // window and starve sound/sight stims — revisit throttling / ring
        // size when area fire lands.
        {
            stimulus_t st;
            memset(&st, 0, sizeof(st));
            st.kind          = STIM_FIRE;
            st.origin[0]     = e->v.origin[0];
            st.origin[1]     = e->v.origin[1];
            st.origin[2]     = e->v.origin[2];
            st.intensity     = 0.8f;
            st.source_edict  = n;
            Stim_Emit(&st);
        }
    }

    oil_frame();
}

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

#define FIRE_MAX_BURNING     SIM_MAX_BRAINS    // one slot per possible edict
#define FIRE_TICK_HZ         10.0f
#define FIRE_DMG_INTERVAL    0.5f              // seconds between damage applications
#define FIRE_HAZARD_RADIUS   64.0f             // burning edict's danger radius (unused until F2 oil, kept for clarity)
#define FIRE_AI_AVOID_RADIUS 160.0f            // non-burning monsters flee fire within this
#define FIRE_SMOKE_AMOUNT    0.12f
#define FIRE_SMOKE_RADIUS    40.0f

typedef struct {
    int   active;
    float burn_until;
    float dps;
    int   igniter_edict;     // -1 = world / unknown
    float next_dmg_time;
} fire_burn_t;

static fire_burn_t s_burning[FIRE_MAX_BURNING];   // indexed by edict number
static float       s_next_tick;

static float fire_crand(void) { return 2.0f * (eng->Random() - 0.5f); }

// O(MAX_EDICTS) walk — acceptable at 10 Hz over a small registry (mirrors
// the documented O(N) pattern in sim_ai.c).
static edict_t *fire_find_edict(int num) {
    if (num < 0) return 0;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e))
        if (eng->ED_GetNum(e) == num) return e;
    return 0;
}

void Fire_Init(void) {
    memset(s_burning, 0, sizeof(s_burning));
    s_next_tick = 0.0f;
    eng->Cvar_Register("sim_fire_debug",  "0");
    eng->Cvar_Register("fire_dps",        "8");
    eng->Cvar_Register("fire_secs",       "5");
    eng->Cvar_Register("fire_ignite_num", "-1");   // MCP/console test hook
}

void Fire_LevelInit(void) {
    memset(s_burning, 0, sizeof(s_burning));
    s_next_tick = 0.0f;
}

static void fire_clear_slot(int n, edict_t *e) {
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    s_burning[n].active = 0;
    if (e) e->v.effects = (float)((int)e->v.effects & ~EF_DIMLIGHT);
}

void Fire_Ignite(edict_t *e, float seconds, float dps, edict_t *igniter) {
    if (!e || e->free) return;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    fire_burn_t *f = &s_burning[n];
    float until = g->time + seconds;
    if (!f->active || until > f->burn_until) f->burn_until = until;
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
    edict_t *t = g->trace_ent;
    if (t && t != g->world && t->v.takedamage) {
        Fire_Ignite(t, secs, dps, player);
        eng->Con_Print("fire: ignited entity under crosshair\n");
    } else {
        eng->Con_Print("fire: no flammable entity under crosshair\n");
    }
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

        int   con      = eng->SV_PointContents(e->v.origin);
        int   inwater  = (con == CONTENT_WATER || con == CONTENT_SLIME);
        int   dead     = (e->v.health <= 0.0f || e->v.deadflag != DEAD_NO);
        if (e->free || g->time >= f->burn_until || dead || inwater) {
            fire_clear_slot(n, e->free ? 0 : e);
            continue;
        }

        // Damage in discrete chunks so armor/save math stays meaningful.
        if (g->time >= f->next_dmg_time && e->v.takedamage) {
            f->next_dmg_time = g->time + FIRE_DMG_INTERVAL;
            edict_t *attacker = fire_find_edict(f->igniter_edict);
            if (!attacker || attacker->free) attacker = g->world;
            T_Damage(e, g->world, attacker, f->dps * FIRE_DMG_INTERVAL);
        }
        // Dynamic light: a real-time dlight follows the moving fire. (Static
        // oil-patch fires will use Lightmap_AddDelta in F2; moving burning
        // edicts use EF_DIMLIGHT to avoid lightmap-delta accumulation.)
        e->v.effects = (float)((int)e->v.effects | EF_DIMLIGHT);

        // Flame particles — small upward jet in the Quake fire/orange ramp.
        for (int p = 0; p < 3; p++) {
            vec3_t org = { e->v.origin[0] + fire_crand() * 8.0f,
                           e->v.origin[1] + fire_crand() * 8.0f,
                           e->v.origin[2] + 8.0f + eng->Random() * 24.0f };
            vec3_t dir = { fire_crand() * 8.0f,
                           fire_crand() * 8.0f,
                           24.0f + eng->Random() * 24.0f };
            float  color = 109.0f + (float)((int)(eng->Random() * 3.0f)); // 109..111
            eng->SV_Particle(org, dir, color, 1.0f);
        }

        // Feed the M4 wind/smoke grid so fire throws up a smoke screen.
        Wind_AddSmoke(e->v.origin, FIRE_SMOKE_AMOUNT, FIRE_SMOKE_RADIUS);

        // Broadcast a fire stimulus so distant AI can register the threat.
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
}

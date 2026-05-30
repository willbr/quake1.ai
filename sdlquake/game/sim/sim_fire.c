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

void Fire_Ignite(edict_t *e, float seconds, float dps, edict_t *igniter) { (void)e; (void)seconds; (void)dps; (void)igniter; }
void Fire_Extinguish(edict_t *e) { (void)e; }
int  Fire_IsBurning(int edict_num) { (void)edict_num; return 0; }
int  Fire_GetIgniterOrigin(int edict_num, vec3_t out) { (void)edict_num; (void)out; return 0; }
int  Fire_NearestHazard(const vec3_t pos, float radius, vec3_t out) { (void)pos; (void)radius; (void)out; return 0; }
void Fire_IgniteTraced(edict_t *player) { (void)player; }

void Fire_Frame(void) {
    if (g->time < s_next_tick) return;
    s_next_tick = g->time + (1.0f / FIRE_TICK_HZ);
    // Tick body added in Task 2.
}

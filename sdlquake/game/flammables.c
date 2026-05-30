// M8 / F4 flammables -- oil barrels, breakable props, (re)lightable torches.
// All DLL-side; no engine ABI change (GAME_API_VERSION stays 36).
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "game_types.h"
#include "flammables.h"
#include "sim/sim.h"

extern engine_api_t   *eng;
extern game_globals_t *g;

static int is_flammable_light(const char *cn) {
    return cn && (strncmp(cn, "light_torch", 11) == 0 ||
                  strncmp(cn, "light_flame", 11) == 0);
}

// Emit the "lights changed" stimulus the AI sense filter already consumes.
static void torch_light_stim(edict_t *t, edict_t *source) {
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_LIGHT_CHANGE;
    s.origin[0]    = t->v.origin[0];
    s.origin[1]    = t->v.origin[1];
    s.origin[2]    = t->v.origin[2];
    s.intensity    = 0.6f;
    s.source_edict = source ? eng->ED_GetNum(source) : -1;
    Stim_Emit(&s);
}

// Hide the flame, darken the room (AI + renderer via Light_AddOverride),
// stim. No-op unless this is a currently-lit flammable light.
void Torch_Extinguish(edict_t *t, edict_t *source) {
    if (!t || t->free || !is_flammable_light(t->v.classname)) return;
    if ((int)t->v.modelindex == 0) return;     // already out
    t->v.modelindex = 0;                        // model string preserved
    Light_AddOverride(t->v.origin, 192.0f, -80.0f);
    torch_light_stim(t, source);
}

// Restore the flame (re-resolve modelindex from the preserved model string),
// brighten (cancels a prior -80), stim. No-op unless currently extinguished.
void Torch_Relight(edict_t *t, edict_t *source) {
    if (!t || t->free || !is_flammable_light(t->v.classname)) return;
    if ((int)t->v.modelindex != 0) return;     // already lit
    eng->SV_SetModel(t, t->v.model);            // model string never cleared
    Light_AddOverride(t->v.origin, 192.0f, 80.0f);
    torch_light_stim(t, source);
}

// Debug (impulse 215): toggle the nearest flammable light to the player.
void Flammables_DebugToggleNearestTorch(edict_t *player) {
    edict_t *best = NULL;
    float bestd = 1e18f;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (!is_flammable_light(e->v.classname)) continue;
        float dx = e->v.origin[0] - player->v.origin[0];
        float dy = e->v.origin[1] - player->v.origin[1];
        float dz = e->v.origin[2] - player->v.origin[2];
        float d = dx*dx + dy*dy + dz*dz;
        if (d < bestd) { bestd = d; best = e; }
    }
    if (!best) { eng->Con_Print("fire: no torch found\n"); return; }
    if ((int)best->v.modelindex != 0) {
        Torch_Extinguish(best, player);
        eng->Con_Print("fire: extinguished nearest torch\n");
    } else {
        Torch_Relight(best, player);
        eng->Con_Print("fire: relit nearest torch\n");
    }
}

// Defined in misc.c (de-static'd by this task) and combat.c.
extern void barrel_explode(edict_t *self);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);

void Flammables_Init(void) {
    // Reused world models -- precache so map placement and debug spawns work.
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    eng->PrecacheSound("weapons/ax1.wav");   // breakable "crack" placeholder
}

// ---------------------------------------------------------------------------
// Oil barrel: a misc_explobox whose death spills oil first. barrel_explode's
// T_RadiusDamage(160) then lights the spill (combat.c:478) -> burning pool.
// ---------------------------------------------------------------------------
static void oilbarrel_explode(edict_t *self) {
    g->self = self;
    vec3_t c = { self->v.origin[0], self->v.origin[1], self->v.origin[2] };
    Fire_AddOil(c, 72.0f, 2.0f);                          // central pool
    for (int i = 0; i < 6; i++) {                         // surrounding ring
        float a = (float)i / 6.0f * 6.2831853f;
        vec3_t p = { c[0] + (float)cos(a) * 80.0f,
                     c[1] + (float)sin(a) * 80.0f,
                     c[2] };
        Fire_AddOil(p, 48.0f, 1.0f);
    }
    barrel_explode(self);   // flips takedamage/classname, T_RadiusDamage(160)
                            // (lights the oil), boom sound, TE_EXPLOSION, BecomeExplosion
}

void spawn_misc_oilbarrel(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->SV_SetModel(e, "maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    e->v.health     = 20;
    e->v.th_die     = oilbarrel_explode;
    e->v.takedamage = DAMAGE_AIM;
    e->v.origin[2] += 2;
    float oldz = e->v.origin[2];
    eng->SV_DropToFloor(e);
    if (oldz - e->v.origin[2] > 250) {
        eng->Con_DPrintf("item fell out of level\n");
        eng->ED_Free(e);
    }
}

// ---------------------------------------------------------------------------
// misc_oilslick (F6) — a map-spawn oil seed for the ai_t10_fire showcase. Not a
// visible prop: it deposits an oil patch at its origin via Fire_AddOil, deferred
// one frame so the sim/oil pool is fully live at level start, then re-pours
// before the 60s OIL_TTL_SECS so a pre-placed slick stays available across
// repeated demos. Passing 0/0 lets Fire_AddOil default the radius/amount.
// ---------------------------------------------------------------------------
static void oilslick_think(edict_t *self) {
    Fire_AddOil(self->v.origin, 0.0f, 0.0f);
    self->v.nextthink = g->time + 40.0f;   // < OIL_TTL_SECS (60) so it never lapses
}

void spawn_misc_oilslick(edict_t *e) {
    g->self        = e;
    e->v.solid     = SOLID_NOT;
    e->v.movetype  = MOVETYPE_NONE;
    e->v.think     = oilslick_think;
    e->v.nextthink = g->time + 0.5f;       // after frame 1: sim is initialised
}

// ---------------------------------------------------------------------------
// Debug spawn helpers. Place a test prop on reachable floor in front of the
// player: flatten the aim to horizontal (so looking up/down doesn't matter),
// trace forward and stop short of any wall, and seat at the player's own
// height so spawn_*'s DropToFloor lands on the same floor the player stands on
// rather than over a ledge (the naive "forward*96" placement silently fell out
// of the level via the >250-drop guard, depending on where the player faced).
// ---------------------------------------------------------------------------
static void flammables_place_ahead(edict_t *player, edict_t *e) {
    eng->MakeVectors(player->v.v_angle);
    float fx = g->v_forward[0], fy = g->v_forward[1];
    float fl = (float)sqrt(fx*fx + fy*fy);
    if (fl > 0.001f) { fx /= fl; fy /= fl; }
    vec3_t eye   = { player->v.origin[0], player->v.origin[1],
                     player->v.origin[2] + player->v.view_ofs[2] };
    vec3_t ahead = { eye[0] + fx * 80.0f, eye[1] + fy * 80.0f, eye[2] };
    eng->SV_Traceline(eye, ahead, 1, player);
    float d = 80.0f * g->trace_fraction - 20.0f;   // stop short of the wall hit
    if (d < 28.0f) d = 28.0f;
    e->v.origin[0] = player->v.origin[0] + fx * d;
    e->v.origin[1] = player->v.origin[1] + fy * d;
    e->v.origin[2] = player->v.origin[2];
}

// Debug spawn (impulse 213): drop an oil barrel on the floor ahead of the player.
void Flammables_DebugSpawnBarrel(edict_t *player) {
    edict_t *e = eng->ED_Alloc();
    flammables_place_ahead(player, e);
    e->v.classname = "misc_oilbarrel";
    spawn_misc_oilbarrel(e);
    eng->Con_Print("fire: spawned oil barrel ahead\n");
}

// ---------------------------------------------------------------------------
// Breakable props. Flammability is automatic: takedamage + health + th_die
// means the fire DOT (sim_fire.c) burns them down to breakable_die. They also
// break to bullets/axe. health is tuned so fire_dps(8) consumes one in ~3s.
// ---------------------------------------------------------------------------
static void breakable_die(edict_t *self) {
    g->self = self;
    self->v.takedamage = DAMAGE_NO;
    self->v.solid      = SOLID_NOT;

    // The AI already understands this stimulus (sim_ai.c, 768u reference).
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_PROP_BROKEN;
    s.origin[0]    = self->v.origin[0];
    s.origin[1]    = self->v.origin[1];
    s.origin[2]    = self->v.origin[2];
    s.intensity    = 0.8f;
    s.source_edict = eng->ED_GetNum(self);
    Stim_Emit(&s);

    // Splinter puff (brown particle band) + a wood-ish crack. No wood-chunk
    // model ships in shareware, so particles stand in for debris (MVP).
    vec3_t up = { 0.0f, 0.0f, 0.0f };
    eng->SV_Particle(self->v.origin, up, 116, 64);
    eng->SV_StartSound(self, CHAN_BODY, "weapons/ax1.wav", 1, ATTN_NORM);

    // Clear any burn-registry slot before freeing so the about-to-be-recycled
    // edict number can't carry a stale "burning" entry (mirrors the AI-brain
    // hygiene ThrowGib does). Fire_Frame would also clear it next tick from the
    // e->free sweep, but this is the intention-revealing form.
    Fire_Extinguish(self);
    eng->ED_Free(self);
}

// Brush form (map-authored crates/walls): model comes from the .map ("*N").
// Caveat: a brush without an origin brush has origin (0,0,0), so flamethrower-
// cone / oil-contact ignition (which key off v.origin distance) won't reach it
// and breakable_die's puff would spawn at the world origin. Such a crate is
// still ignitable by crosshair (impulse 210) / bullets / explosions (which use
// the bbox). Add an origin brush, or use misc_breakable (point), for full cover.
void spawn_func_breakable(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BSP;
    e->v.movetype = MOVETYPE_PUSH;
    eng->SV_SetModel(e, e->v.model);     // brush model from the map
    if (e->v.health <= 0) e->v.health = 40;
    e->v.takedamage = DAMAGE_AIM;
    e->v.th_die     = breakable_die;
}

// Point form (spawn-anywhere debug/test): reuse the box bmodel.
void spawn_misc_breakable(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->SV_SetModel(e, "maps/b_explob.bsp");
    if (e->v.health <= 0) e->v.health = 25;   // ~3s under fire_dps 8
    e->v.takedamage = DAMAGE_AIM;
    e->v.th_die     = breakable_die;
    e->v.origin[2] += 2;
    eng->SV_DropToFloor(e);
}

// Debug spawn (impulse 214): drop a breakable on the floor ahead of the player.
void Flammables_DebugSpawnBreakable(edict_t *player) {
    edict_t *e = eng->ED_Alloc();
    flammables_place_ahead(player, e);
    e->v.classname = "misc_breakable";
    e->v.health    = 25;
    spawn_misc_breakable(e);
    eng->Con_Print("fire: spawned breakable ahead\n");
}

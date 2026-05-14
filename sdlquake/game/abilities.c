// abilities.c -- Player Blink + Gust (Phase 8 / M3).
//
// Blink: hold +blink to aim, release to commit. Player teleports to the
//        trace endpoint of v_forward (range ph_blink_range). Validity check
//        uses a player-box trace and temporarily disables func_grate
//        entities so the player can pass through them. Emits STIM_SOUND at
//        both source and destination.
// Gust:  on +gust press, applies a forward cone push to nearby monsters
//        and props, and emits STIM_SOUND on impact. Cost + cooldown driven
//        by cvars. Smoke / light-tier interactions are wired in M4 / M5;
//        the hooks fire from here.

#include "abilities.h"
#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// Cvar handles -- read every frame via Cvar_VariableValue.
// ---------------------------------------------------------------------------
#define CVAR(name, def) eng->Cvar_Register(#name, def)
static void register_cvars(void) {
    CVAR(ph_max,            "100");
    CVAR(ph_regen,          "25");
    CVAR(ph_blink_cost,     "25");
    CVAR(ph_gust_cost,      "15");
    CVAR(ph_blink_range,    "320");
    CVAR(ph_gust_range,     "384");
    CVAR(ph_gust_cone_deg,  "30");
    CVAR(ph_gust_cooldown,  "0.5");
    CVAR(ph_gust_impulse,   "800");
    CVAR(ph_blink_intensity,"0.3");
    CVAR(ph_gust_impact_intensity, "0.45");
    CVAR(ph_debug,          "0");
    // Read-only mirror so the imgui dev panel can see energy.
    CVAR(ph_energy,         "100");
}
#undef CVAR

static float CV(const char *n)              { return eng->Cvar_VariableValue(n); }
static void  CVSet(const char *n, float v)  { eng->Cvar_SetValue(n, v); }

// ---------------------------------------------------------------------------
// Per-player state -- single-player only; one entry suffices.
// ---------------------------------------------------------------------------
typedef struct {
    float    energy;
    float    prev_button_blink;
    float    prev_button_gust;
    float    gust_cooldown_end;
    int      blink_active;
    int      blink_valid;
    vec3_t   blink_endpoint;
} player_abil_t;

static player_abil_t s_p;

// ---------------------------------------------------------------------------
// Grate registry. Blink validity traces temporarily set these to SOLID_NOT
// then restore so the trace passes through them.
// ---------------------------------------------------------------------------
#define ABIL_MAX_GRATES 64
static edict_t *s_grates[ABIL_MAX_GRATES];
static float    s_grate_saved_solid[ABIL_MAX_GRATES];
static int      s_grate_count;

void Abilities_RegisterGrate(edict_t *e) {
    if (!e || s_grate_count >= ABIL_MAX_GRATES) return;
    s_grates[s_grate_count++] = e;
}

static void grates_disable(void) {
    for (int i = 0; i < s_grate_count; i++) {
        if (!s_grates[i]) continue;
        s_grate_saved_solid[i] = s_grates[i]->v.solid;
        s_grates[i]->v.solid   = SOLID_NOT;
    }
}
static void grates_restore(void) {
    for (int i = 0; i < s_grate_count; i++) {
        if (!s_grates[i]) continue;
        s_grates[i]->v.solid = s_grate_saved_solid[i];
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Abilities_Init(void) {
    memset(&s_p, 0, sizeof(s_p));
    s_p.energy = 100.0f;
    s_grate_count = 0;
    register_cvars();
}

void Abilities_LevelInit(void) {
    memset(&s_p, 0, sizeof(s_p));
    s_p.energy = CV("ph_max");
    s_grate_count = 0;
}

// ---------------------------------------------------------------------------
// Math helpers
// ---------------------------------------------------------------------------
static float vlen(const vec3_t v) {
    return (float)sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}
static void vsub(const vec3_t a, const vec3_t b, vec3_t out) {
    out[0] = a[0]-b[0]; out[1] = a[1]-b[1]; out[2] = a[2]-b[2];
}
static float vdot(const vec3_t a, const vec3_t b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static void vmadd(const vec3_t a, const vec3_t b, float s, vec3_t out) {
    out[0] = a[0] + b[0]*s;
    out[1] = a[1] + b[1]*s;
    out[2] = a[2] + b[2]*s;
}

// ---------------------------------------------------------------------------
// Emit a stimulus shorthand.
// ---------------------------------------------------------------------------
static void emit_sound_stim(const vec3_t origin, float intensity, int source_num) {
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind = STIM_SOUND;
    s.origin[0] = origin[0];
    s.origin[1] = origin[1];
    s.origin[2] = origin[2];
    s.intensity = intensity;
    s.source_edict = source_num;
    Stim_Emit(&s);
}

// ---------------------------------------------------------------------------
// Blink
// ---------------------------------------------------------------------------
static void blink_trace_endpoint(edict_t *client,
                                 const vec3_t eye,
                                 const vec3_t forward,
                                 float range,
                                 vec3_t out_endpos,
                                 int *out_valid)
{
    static const vec3_t pmins = { -16, -16, -24 };
    static const vec3_t pmaxs = {  16,  16,  32 };
    vec3_t end;
    vmadd(eye, forward, range, end);

    // Player-box trace, but allow grate-passage.
    grates_disable();
    eng->SV_TraceMove((float*)eye, (float*)pmins, (float*)pmaxs, end, 0, client);
    grates_restore();

    int valid = 1;
    if (g->trace_startsolid != 0) valid = 0;
    if (g->trace_allsolid   != 0) valid = 0;

    out_endpos[0] = g->trace_endpos[0];
    out_endpos[1] = g->trace_endpos[1];
    out_endpos[2] = g->trace_endpos[2];

    // Reject endpoints in sky.
    if (valid) {
        int c = eng->SV_PointContents(out_endpos);
        if (c == CONTENT_SKY) valid = 0;
    }

    // Void-fall check: ensure there's a floor within 96 units below.
    if (valid) {
        vec3_t below;
        below[0] = out_endpos[0];
        below[1] = out_endpos[1];
        below[2] = out_endpos[2] - 96.0f;
        eng->SV_Traceline((float*)out_endpos, below, 1, client);
        if (g->trace_fraction == 1.0f) valid = 0;
    }

    *out_valid = valid;
}

static void blink_reticle_particles(const vec3_t pos, int valid) {
    vec3_t dir = {0, 0, 16};
    float color = valid ? 184.0f : 251.0f;   // 184 = green, 251 = bright red
    eng->SV_Particle((float*)pos, dir, color, 4);
}

static void blink_commit(edict_t *client, const vec3_t endpoint) {
    // Snap player. SV_SetOrigin re-links collision; SV_DropToFloor settles.
    vec3_t src;
    src[0] = client->v.origin[0];
    src[1] = client->v.origin[1];
    src[2] = client->v.origin[2];
    eng->SV_SetOrigin(client, (float*)endpoint);
    eng->SV_DropToFloor(client);

    // Drain velocity so the post-blink state isn't a flailing tumble.
    client->v.velocity[0] = 0;
    client->v.velocity[1] = 0;
    client->v.velocity[2] = 0;

    // Spend energy.
    s_p.energy -= CV("ph_blink_cost");
    if (s_p.energy < 0) s_p.energy = 0;

    // Audio + stim emission at both ends.
    eng->SV_StartSound(client, CHAN_AUTO, "items/r_item1.wav", 1.0f, ATTN_NORM);
    int n = eng->ED_GetNum(client);
    float intensity = CV("ph_blink_intensity");
    emit_sound_stim(src,      intensity, n);
    emit_sound_stim(endpoint, intensity, n);

    // Snap view forward to v_forward so the player doesn't get whiplash.
    client->v.fixangle = 1.0f;
}

// ---------------------------------------------------------------------------
// Gust
// ---------------------------------------------------------------------------
static void gust_fire(edict_t *client, const vec3_t eye, const vec3_t forward) {
    float range = CV("ph_gust_range");
    float cone_deg = CV("ph_gust_cone_deg");
    float cone_cos = (float)cos(cone_deg * 3.14159265f / 180.0f);
    float impulse  = CV("ph_gust_impulse");

    // Iterate all non-world edicts; small-N for typical maps. We rely on
    // ED_Next walking the live list rather than ED_FindRadius so we catch
    // entities of every classname (monsters, gibs, dropped weapons).
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == client) continue;
        if (e->v.solid == SOLID_NOT && e->v.movetype != MOVETYPE_TOSS &&
            e->v.movetype != MOVETYPE_BOUNCE) continue;

        vec3_t to;
        to[0] = e->v.origin[0] - eye[0];
        to[1] = e->v.origin[1] - eye[1];
        to[2] = e->v.origin[2] - eye[2];
        float d = vlen(to);
        if (d > range || d < 1.0f) continue;
        // Cone test.
        float dirn[3] = { to[0]/d, to[1]/d, to[2]/d };
        if (vdot(dirn, forward) < cone_cos) continue;

        // LOS check so we don't punch through walls.
        eng->SV_Traceline((float*)eye, e->v.origin, 1, client);
        if (g->trace_fraction != 1.0f &&
            g->trace_ent != e) continue;

        // Apply impulse outward from the player.
        float vy = impulse;
        // Monsters get a smaller shove than light props.
        if (e->v.flags && ((int)e->v.flags & FL_MONSTER)) vy *= 0.4f;

        e->v.velocity[0] += dirn[0] * vy;
        e->v.velocity[1] += dirn[1] * vy;
        e->v.velocity[2] += dirn[2] * vy * 0.5f + 80.0f;  // small upward bias

        // Knock STEP-mode monsters off the ground briefly so the push reads.
        if (e->v.flags && ((int)e->v.flags & FL_ONGROUND)) {
            e->v.flags = (float)((int)e->v.flags & ~FL_ONGROUND);
        }

        // Stim emission at impact.
        emit_sound_stim(e->v.origin,
                        CV("ph_gust_impact_intensity"),
                        eng->ED_GetNum(client));
    }

    // Always emit a stim at the player's position (the audible whoosh).
    emit_sound_stim(eye, CV("ph_blink_intensity"), eng->ED_GetNum(client));

    // Wind grid hooks (M4): inject radial velocity outward in the cone
    // origin and clear smoke along the cone.
    {
        vec3_t dir = { forward[0], forward[1], forward[2] };
        Wind_AddImpulse(eye, dir, impulse * 0.8f, range * 0.5f);
        // Clear smoke along a few sample points down the cone.
        for (int i = 1; i <= 4; i++) {
            float t = (float)i / 4.0f * range;
            vec3_t p = { eye[0] + forward[0]*t,
                         eye[1] + forward[1]*t,
                         eye[2] + forward[2]*t };
            Wind_ClearSmoke(p, 0.5f, range * 0.35f);
        }
    }

    // Spend energy + cooldown.
    s_p.energy -= CV("ph_gust_cost");
    if (s_p.energy < 0) s_p.energy = 0;
    s_p.gust_cooldown_end = g->time + CV("ph_gust_cooldown");

    eng->SV_StartSound(client, CHAN_AUTO, "weapons/sgun1.wav", 0.6f, ATTN_NORM);
}

// ---------------------------------------------------------------------------
// Frame entry point
// ---------------------------------------------------------------------------
void Abilities_Frame(edict_t *client) {
    if (!client) return;
    if (client->v.deadflag) {
        s_p.blink_active = 0;
        return;
    }

    // Regen.
    float ph_max = CV("ph_max");
    if (ph_max < 1) ph_max = 100;
    s_p.energy += CV("ph_regen") * g->frametime;
    if (s_p.energy > ph_max) s_p.energy = ph_max;

    // Recompute forward vector from view angles — PlayerPostThink can run
    // after weapons.c overwrites v_forward.
    eng->MakeVectors(client->v.v_angle);

    vec3_t eye;
    eye[0] = client->v.origin[0] + client->v.view_ofs[0];
    eye[1] = client->v.origin[1] + client->v.view_ofs[1];
    eye[2] = client->v.origin[2] + client->v.view_ofs[2];
    vec3_t forward = { g->v_forward[0], g->v_forward[1], g->v_forward[2] };

    // Blink — held aim, released to commit.
    float bbtn = client->v.button3;
    if (bbtn > 0.5f) {
        s_p.blink_active = 1;
        blink_trace_endpoint(client, eye, forward,
                             CV("ph_blink_range"),
                             s_p.blink_endpoint,
                             &s_p.blink_valid);
        if (s_p.energy < CV("ph_blink_cost")) s_p.blink_valid = 0;
        blink_reticle_particles(s_p.blink_endpoint, s_p.blink_valid);
    } else {
        if (s_p.blink_active && s_p.prev_button_blink > 0.5f) {
            // Edge-down — release commit.
            if (s_p.blink_valid && s_p.energy >= CV("ph_blink_cost"))
                blink_commit(client, s_p.blink_endpoint);
        }
        s_p.blink_active = 0;
    }
    s_p.prev_button_blink = bbtn;

    // Gust — instant cast on rising edge.
    float gbtn = client->v.button4;
    if (gbtn > 0.5f && s_p.prev_button_gust <= 0.5f &&
        g->time >= s_p.gust_cooldown_end &&
        s_p.energy >= CV("ph_gust_cost"))
    {
        gust_fire(client, eye, forward);
    }
    s_p.prev_button_gust = gbtn;

    // Mirror energy to the dev panels.
    CVSet("ph_energy", s_p.energy);

    if (CV("ph_debug") > 0.5f && s_p.blink_active) {
        // Debug print on the console -- once a second.
        static float last;
        if (g->time - last > 1.0f) {
            char buf[128];
            snprintf(buf, sizeof(buf), "blink endpoint valid=%d (%.0f %.0f %.0f)\n",
                     s_p.blink_valid,
                     s_p.blink_endpoint[0], s_p.blink_endpoint[1], s_p.blink_endpoint[2]);
            eng->Con_Print(buf);
            last = g->time;
        }
    }
}

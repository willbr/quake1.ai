// r_emitter.c -- data-driven particle effects runtime. See r_emitter.h and
// docs/superpowers/specs/2026-06-01-particle-editor-design.md.

#include "quakedef.h"
#include "r_emitter.h"

// Shared particle pool (defined in r_part.c).
extern particle_t *active_particles, *free_particles;

// Pool reserve so authored FX can't starve gameplay particles (mirror r_part.c).
#ifndef SMOKE_GAMEPLAY_RESERVE
#define SMOKE_GAMEPLAY_RESERVE 2048
#endif

cvar_t r_emitter_active = { "r_emitter_active", "0" }; // reports live-instance count

// ---- registry -----------------------------------------------------------
static emitter_def_t s_defs[EMIT_MAX_DEFS];

typedef struct {
    int    used;
    int    def_idx;
    vec3_t org, dir;
    float  expire;     // cl.time at which to stop (ignored if def->duration==0)
    float  accum;      // fractional particle carry
} live_emitter_t;

static live_emitter_t s_live[EMIT_MAX_LIVE];

int R_EmitterCount(void)
{
    int n = 0, i;
    for (i = 0; i < EMIT_MAX_DEFS; i++) if (s_defs[i].used) n++;
    return n;
}

emitter_def_t *R_EmitterGetDef(int idx)
{
    if (idx < 0 || idx >= EMIT_MAX_DEFS || !s_defs[idx].used) return NULL;
    return &s_defs[idx];
}

int R_EmitterFind(const char *name)
{
    int i;
    if (!name || !name[0]) return -1;
    for (i = 0; i < EMIT_MAX_DEFS; i++)
        if (s_defs[i].used && !strcmp(s_defs[i].name, name)) return i;
    return -1;
}

static void emitter_set_defaults(emitter_def_t *d, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->used = 1;
    strncpy(d->name, name, EMIT_NAME_LEN - 1);
    d->name[EMIT_NAME_LEN - 1] = 0;
    d->mode         = EMIT_BURST;
    d->count        = 32;
    d->rate         = 20.0f;
    d->duration     = 1.0f;
    d->shape        = SHAPE_POINT;
    d->shape_size   = 8.0f;
    d->cone_angle   = 15.0f;
    d->speed        = 60.0f;
    d->speed_jitter = 10.0f;
    d->dir_mode     = DIR_ALONG_SHAPE;
    d->spread       = 20.0f;
    d->gravity_scale= 1.0f;
    d->drag         = 0.0f;
    d->life_min     = 0.6f;
    d->life_max     = 1.0f;
    d->style        = STYLE_DOT;
    d->size_start   = 1.0f;
    d->size_peak    = 4.0f;
    d->size_end     = 0.0f;
    d->ramp_count   = 2;
    d->ramp_frac[0] = 0.0f; d->ramp_pal[0] = 0x6f; // orange-ish
    d->ramp_frac[1] = 1.0f; d->ramp_pal[1] = 0x61; // dark ember
}

int R_EmitterNew(const char *name)
{
    int i;
    if (R_EmitterFind(name) >= 0) return -1; // name clash
    for (i = 0; i < EMIT_MAX_DEFS; i++) {
        if (!s_defs[i].used) { emitter_set_defaults(&s_defs[i], name); return i; }
    }
    Con_Printf("R_EmitterNew: registry full (%d)\n", EMIT_MAX_DEFS);
    return -1;
}

void R_EmitterDelete(int idx)
{
    int i;
    if (idx < 0 || idx >= EMIT_MAX_DEFS) return;
    // Stop any live instances of this def first (reload-safety; see spec).
    for (i = 0; i < EMIT_MAX_LIVE; i++)
        if (s_live[i].used && s_live[i].def_idx == idx) s_live[i].used = 0;
    s_defs[idx].used = 0;
}

// ---- sampling -----------------------------------------------------------
byte R_EmitterRampColor(const emitter_def_t *d, float t)
{
    int i;
    if (d->ramp_count <= 0) return 0;
    if (t <= d->ramp_frac[0]) return d->ramp_pal[0];
    for (i = 1; i < d->ramp_count; i++) {
        if (t <= d->ramp_frac[i]) {
            // Nearest stop (palette indices don't interpolate meaningfully).
            float mid = 0.5f * (d->ramp_frac[i-1] + d->ramp_frac[i]);
            return (t < mid) ? d->ramp_pal[i-1] : d->ramp_pal[i];
        }
    }
    return d->ramp_pal[d->ramp_count - 1];
}

float R_EmitterSizeEnv(const emitter_def_t *d, float t)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    if (t < 0.5f) {
        float a = t / 0.5f;
        return d->size_start + (d->size_peak - d->size_start) * a;
    } else {
        float a = (t - 0.5f) / 0.5f;
        return d->size_peak + (d->size_end - d->size_peak) * a;
    }
}

// ---- spawn one particle from a def --------------------------------------
static float frand(void)   { return (rand() & 0x7fff) / 32767.0f; }  // [0,1]
static float frand_s(void) { return frand() * 2.0f - 1.0f; }         // [-1,1]

static void spawn_one(int def_idx, const vec3_t base_org, const vec3_t base_dir)
{
    emitter_def_t *d = &s_defs[def_idx];
    particle_t *p;
    vec3_t org, vel, sdir;
    int j;

    if (!free_particles) return;
    p = free_particles;
    free_particles = p->next;
    p->next = active_particles;
    active_particles = p;
    p->flags = 0;
    p->type  = pt_emitter;
    p->def   = (short)def_idx;

    // origin: base + offset + shape scatter
    for (j = 0; j < 3; j++) org[j] = base_org[j] + d->origin_offset[j];
    VectorCopy(base_dir, sdir);
    switch (d->shape) {
    case SHAPE_SPHERE:
    case SHAPE_BOX:
        for (j = 0; j < 3; j++) org[j] += frand_s() * d->shape_size;
        break;
    case SHAPE_CONE:
        for (j = 0; j < 3; j++) sdir[j] = base_dir[j] + frand_s() * (d->cone_angle / 90.0f);
        VectorNormalize(sdir);
        break;
    case SHAPE_POINT:
    default: break;
    }

    // velocity
    {
        vec3_t vdir;
        float spd;
        if (d->dir_mode == DIR_UP)           { vdir[0]=0; vdir[1]=0; vdir[2]=1; }
        else if (d->dir_mode == DIR_INHERIT) { VectorCopy(base_dir, vdir); }
        else                                 { VectorCopy(sdir, vdir); } // ALONG_SHAPE
        for (j = 0; j < 3; j++) vdir[j] += frand_s() * (d->spread / 90.0f);
        VectorNormalize(vdir);
        spd = d->speed + frand_s() * d->speed_jitter;
        for (j = 0; j < 3; j++) vel[j] = vdir[j] * spd;
        if (d->radial_bias != 0.0f) {
            vec3_t rad;
            for (j = 0; j < 3; j++) rad[j] = org[j] - base_org[j];
            VectorNormalize(rad);
            for (j = 0; j < 3; j++) vel[j] += rad[j] * d->radial_bias;
        }
    }

    VectorCopy(org, p->org);
    VectorCopy(vel, p->vel);
    {
        float life = d->life_min + frand() * (d->life_max - d->life_min);
        if (life < 0.05f) life = 0.05f;
        p->birth = cl.time;
        p->die   = cl.time + life;
    }
    p->ramp  = 0;
    p->color = R_EmitterRampColor(d, 0.0f);
}

// Reserve-aware burst (mirror R_AddFire's guard).
static void burst(int def_idx, const vec3_t org, const vec3_t dir, int n)
{
    int need = SMOKE_GAMEPLAY_RESERVE + n, i;
    particle_t *probe = free_particles;
    while (need > 0 && probe) { probe = probe->next; need--; }
    if (need > 0) return; // pool too low; drop the burst
    for (i = 0; i < n; i++) spawn_one(def_idx, org, dir);
}

// ---- public spawn --------------------------------------------------------
int R_SpawnEffectIdx(int idx, vec3_t org, vec3_t dir)
{
    emitter_def_t *d = R_EmitterGetDef(idx);
    int i;
    if (!d) return -1;

    if (d->mode == EMIT_BURST) {
        burst(idx, org, dir, d->count);
        return -1;
    }
    for (i = 0; i < EMIT_MAX_LIVE; i++) {
        if (!s_live[i].used) {
            s_live[i].used = 1;
            s_live[i].def_idx = idx;
            VectorCopy(org, s_live[i].org);
            VectorCopy(dir, s_live[i].dir);
            s_live[i].accum = 0.0f;
            s_live[i].expire = (d->duration > 0.0f) ? (cl.time + d->duration) : 0.0f;
            return i;
        }
    }
    return -1; // live pool full
}

void R_SpawnParticleEffectByName(const char *name, vec3_t org, vec3_t dir)
{
    int idx = R_EmitterFind(name);
    if (idx < 0) {
        static int warned = 0;
        if (!warned) { Con_DPrintf("SpawnParticleEffect: no effect '%s'\n", name); warned = 1; }
        return;
    }
    R_SpawnEffectIdx(idx, org, dir);
}

void R_EmitterStopHandle(int handle)
{
    if (handle >= 0 && handle < EMIT_MAX_LIVE) s_live[handle].used = 0;
}

void R_EmitterStopAll(void)
{
    int i;
    for (i = 0; i < EMIT_MAX_LIVE; i++) s_live[i].used = 0;
}

// ---- per-frame update ----------------------------------------------------
void R_UpdateEmitters(void)
{
    float dt = cl.time - cl.oldtime;
    int i, live = 0;
    if (dt <= 0.0f) { Cvar_SetValue("r_emitter_active", 0); return; }

    for (i = 0; i < EMIT_MAX_LIVE; i++) {
        live_emitter_t *e = &s_live[i];
        emitter_def_t *d;
        int want;
        if (!e->used) continue;
        d = R_EmitterGetDef(e->def_idx);
        if (!d || d->mode != EMIT_CONTINUOUS) { e->used = 0; continue; }
        if (e->expire != 0.0f && cl.time > e->expire) { e->used = 0; continue; }

        e->accum += d->rate * dt;
        want = (int)e->accum;
        if (want > 0) {
            e->accum -= want;
            burst(e->def_idx, e->org, e->dir, want);
        }
        live++;
    }
    Cvar_SetValue("r_emitter_active", (float)live);
}

// ---- console: particle_spawn <name> [x y z] -----------------------------
static void R_ParticleSpawn_f(void)
{
    vec3_t org, dir = {0,0,1};
    vec3_t fwd, right, up;
    int idx;
    if (Cmd_Argc() < 2) { Con_Printf("usage: particle_spawn <name> [x y z]\n"); return; }
    idx = R_EmitterFind(Cmd_Argv(1));
    if (idx < 0) { Con_Printf("no effect '%s' (have %d)\n", Cmd_Argv(1), R_EmitterCount()); return; }
    if (Cmd_Argc() >= 5) {
        org[0]=atof(Cmd_Argv(2)); org[1]=atof(Cmd_Argv(3)); org[2]=atof(Cmd_Argv(4));
    } else {
        // place ~80u in front of the view
        AngleVectors(r_refdef.viewangles, fwd, right, up);
        VectorMA(r_refdef.vieworg, 80, fwd, org);
        VectorCopy(fwd, dir);
    }
    R_SpawnEffectIdx(idx, org, dir);
}

// ---- persistence (Slice 3 fills these in) -------------------------------
void R_EmitterLoadAll(void) {}
int  R_EmitterSave(int idx) { (void)idx; return 0; }

void R_EmitterInit(void)
{
    Cvar_RegisterVariable(&r_emitter_active);
    Cmd_AddCommand("particle_spawn", R_ParticleSpawn_f);
    R_EmitterLoadAll();

    // Bootstrap test def if none loaded yet (Slice 3 replaces with real .pcl).
    if (R_EmitterCount() == 0) {
        int i = R_EmitterNew("test_fountain");
        if (i >= 0) {
            emitter_def_t *d = R_EmitterGetDef(i);
            d->mode = EMIT_CONTINUOUS; d->rate = 60; d->duration = 0;
            d->shape = SHAPE_POINT; d->speed = 120; d->speed_jitter = 30;
            d->dir_mode = DIR_UP; d->spread = 25;
            d->gravity_scale = 1.0f; d->life_min = 0.8f; d->life_max = 1.4f;
            d->style = STYLE_BLOB; d->size_start = 1; d->size_peak = 5; d->size_end = 0;
            d->ramp_count = 3;
            d->ramp_frac[0]=0;    d->ramp_pal[0]=0x6f;
            d->ramp_frac[1]=0.5f; d->ramp_pal[1]=0x6b;
            d->ramp_frac[2]=1;    d->ramp_pal[2]=0x61;
        }
    }
}

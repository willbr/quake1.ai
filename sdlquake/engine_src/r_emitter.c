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

// ---- particle clock ------------------------------------------------------
// Particles are normally driven by cl.time. But while the in-game editor is
// open the world sim is paused (cl.time is frozen via CL_LerpPoint's clamp to
// server message times), which would freeze the preview. So the particle
// subsystem reads time through this clock instead: it tracks cl.time during
// normal play (byte-identical), but advances on an independent real-time
// delta when the editor is previewing a particle effect (play), or freezes
// (pause). r_part.c (R_DrawParticles) + d_part.c (D_DrawEmitterParticle) read
// R_PartTime()/R_PartFrameTime(); particle_clock_update() runs once per frame
// at the top of R_UpdateEmitters (which always precedes R_DrawParticles).
extern int  Editor_ParticlePreviewState(void); // editor.c: 0 none, 1 play, 2 pause
extern void Editor_GetOrbitFocus(vec3_t out);  // editor.c: particle-mode orbit centre

static double s_part_time = 0.0;
static float  s_part_frametime = 0.0f;

double R_PartTime(void)      { return s_part_time; }
float  R_PartFrameTime(void) { return s_part_frametime; }

static void particle_clock_update(void)
{
    static double last_real = 0.0;
    double now = Sys_FloatTime();
    int    st  = Editor_ParticlePreviewState();

    if (st == 0) {
        // Normal play (or map mode): track the client clock exactly.
        s_part_time     = cl.time;
        s_part_frametime = cl.time - cl.oldtime;
    } else {
        // Editor preview: advance on wall-clock time (world is paused).
        float dt = (last_real > 0.0) ? (float)(now - last_real) : 0.0f;
        if (dt > 0.1f) dt = 0.1f;     // clamp big hitches
        if (st == 2)   dt = 0.0f;     // paused
        s_part_time     += dt;
        s_part_frametime = dt;
    }
    last_real = now;
}

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
        double now = R_PartTime();
        if (life < 0.05f) life = 0.05f;
        p->birth = now;
        p->die   = now + life;
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
            s_live[i].expire = (d->duration > 0.0f) ? (R_PartTime() + d->duration) : 0.0f;
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
    float dt;
    int i, live = 0;

    particle_clock_update();      // advance the particle clock once per frame
    dt = R_PartFrameTime();

    for (i = 0; i < EMIT_MAX_LIVE; i++) {
        live_emitter_t *e = &s_live[i];
        emitter_def_t *d;
        int want;
        if (!e->used) continue;
        d = R_EmitterGetDef(e->def_idx);
        if (!d || d->mode != EMIT_CONTINUOUS) { e->used = 0; continue; }
        if (e->expire != 0.0f && R_PartTime() > e->expire) { e->used = 0; continue; }

        // Spawn this tick's quota (skipped when the clock is frozen / paused).
        if (dt > 0.0f) {
            e->accum += d->rate * dt;
            want = (int)e->accum;
            if (want > 0) {
                e->accum -= want;
                burst(e->def_idx, e->org, e->dir, want);
            }
        }
        live++;   // still a live instance even while paused
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
    } else if (Editor_ParticlePreviewState() != 0) {
        // In the particle editor: spawn at the orbit centre so the camera circles it.
        Editor_GetOrbitFocus(org);   // dir stays up {0,0,1}
    } else {
        // place ~80u in front of the view
        AngleVectors(r_refdef.viewangles, fwd, right, up);
        VectorMA(r_refdef.vieworg, 80, fwd, org);
        VectorCopy(fwd, dir);
    }
    R_SpawnEffectIdx(idx, org, dir);
}

// ---- persistence: .pcl (Quake KV-block, parsed with COM_Parse) ----------
static const char *EMIT_MODE_NAMES[]  = { "burst", "continuous" };
static const char *EMIT_SHAPE_NAMES[] = { "point", "sphere", "cone", "box" };
static const char *EMIT_DIR_NAMES[]   = { "along_shape", "inherit", "up" };
static const char *EMIT_STYLE_NAMES[] = { "dot", "blob", "smoke" };

static int parse_enum(const char *s, const char * const *names, int n, int fallback)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(s, names[i])) return i;
    return fallback;
}

// "0.0:111 0.4:107 1.0:8" -> ramp stops
static void parse_ramp(emitter_def_t *d, const char *s)
{
    int n = 0;
    while (*s && n < EMIT_MAX_RAMP) {
        const char *colon;
        float frac;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        frac = (float)atof(s);
        colon = strchr(s, ':');
        if (!colon) break;
        d->ramp_frac[n] = frac;
        d->ramp_pal[n]  = (byte)(atoi(colon + 1) & 0xff);
        n++;
        while (*s && *s != ' ' && *s != '\t') s++;   // advance past token
    }
    if (n == 0) { d->ramp_frac[0] = 0; d->ramp_pal[0] = 15; n = 1; }
    d->ramp_count = n;
}

// Parse one "particle_effect { "k" "v" ... }" block from relpath into a slot.
static void emitter_load_file(const char *relpath)
{
    char *buf = (char *)COM_LoadTempFile((char *)relpath);
    char *data, key[128];
    int idx;
    emitter_def_t *d;
    char stem[EMIT_NAME_LEN];
    const char *base, *p2;

    if (!buf) return;
    data = buf;

    data = COM_Parse(data); if (!data) return;                       // "particle_effect"
    data = COM_Parse(data); if (!data || com_token[0] != '{') return; // "{"

    // Name defaults to the file stem; a "name" key overrides.
    base = relpath;
    for (p2 = relpath; *p2; p2++) if (*p2 == '/' || *p2 == '\\') base = p2 + 1;
    strncpy(stem, base, EMIT_NAME_LEN - 1); stem[EMIT_NAME_LEN - 1] = 0;
    { char *dot = strrchr(stem, '.'); if (dot) *dot = 0; }
    idx = R_EmitterFind(stem);
    if (idx < 0) idx = R_EmitterNew(stem);
    if (idx < 0) return;
    d = R_EmitterGetDef(idx);

    while (1) {
        data = COM_Parse(data);
        if (!data || com_token[0] == '}') break;
        strncpy(key, com_token, sizeof(key) - 1); key[sizeof(key) - 1] = 0;
        data = COM_Parse(data);
        if (!data) break;
        if      (!strcmp(key,"name"))         { strncpy(d->name, com_token, EMIT_NAME_LEN-1); d->name[EMIT_NAME_LEN-1]=0; }
        else if (!strcmp(key,"emission"))     d->mode = parse_enum(com_token, EMIT_MODE_NAMES, 2, EMIT_BURST);
        else if (!strcmp(key,"count"))        d->count = atoi(com_token);
        else if (!strcmp(key,"rate"))         d->rate = atof(com_token);
        else if (!strcmp(key,"duration"))     d->duration = atof(com_token);
        else if (!strcmp(key,"shape"))        d->shape = parse_enum(com_token, EMIT_SHAPE_NAMES, 4, SHAPE_POINT);
        else if (!strcmp(key,"shape_size"))   d->shape_size = atof(com_token);
        else if (!strcmp(key,"cone_angle"))   d->cone_angle = atof(com_token);
        else if (!strcmp(key,"speed"))        d->speed = atof(com_token);
        else if (!strcmp(key,"speed_jitter")) d->speed_jitter = atof(com_token);
        else if (!strcmp(key,"dir_mode"))     d->dir_mode = parse_enum(com_token, EMIT_DIR_NAMES, 3, DIR_ALONG_SHAPE);
        else if (!strcmp(key,"spread"))       d->spread = atof(com_token);
        else if (!strcmp(key,"radial_bias"))  d->radial_bias = atof(com_token);
        else if (!strcmp(key,"gravity"))      d->gravity_scale = atof(com_token);
        else if (!strcmp(key,"drag"))         d->drag = atof(com_token);
        else if (!strcmp(key,"life_min"))     d->life_min = atof(com_token);
        else if (!strcmp(key,"life_max"))     d->life_max = atof(com_token);
        else if (!strcmp(key,"style"))        d->style = parse_enum(com_token, EMIT_STYLE_NAMES, 3, STYLE_DOT);
        else if (!strcmp(key,"size_start"))   d->size_start = atof(com_token);
        else if (!strcmp(key,"size_peak"))    d->size_peak = atof(com_token);
        else if (!strcmp(key,"size_end"))     d->size_end = atof(com_token);
        else if (!strcmp(key,"ramp"))         parse_ramp(d, com_token);
        // unknown keys silently ignored (forward-compat)
    }
}

void R_EmitterLoadAll(void)
{
    // index.txt lists effect names (one per line); we load particles/<name>.pcl.
    // COM_LoadTempFile reuses the temp hunk, so copy the index out before the
    // per-file loads clobber it.
    char *raw = (char *)COM_LoadTempFile("particles/index.txt");
    char index[4096];
    char *p;
    int n;
    if (!raw) { Con_DPrintf("R_EmitterLoadAll: no particles/index.txt\n"); return; }
    strncpy(index, raw, sizeof(index) - 1); index[sizeof(index) - 1] = 0;

    p = index;
    while (*p) {
        char line[EMIT_NAME_LEN + 16];
        n = 0;
        while (*p && *p != '\n' && *p != '\r' && n < (int)sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0;
        while (*p == '\n' || *p == '\r') p++;
        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t')) line[--n] = 0;
        if (n == 0 || line[0] == '#') continue;
        {
            char rel[EMIT_NAME_LEN + 32];
            snprintf(rel, sizeof(rel), "particles/%s.pcl", line);
            emitter_load_file(rel);
        }
    }
}

int R_EmitterSave(int idx)
{
    emitter_def_t *d = R_EmitterGetDef(idx);
    char path[256], body[2048], ramp[256];
    int i, n;

    if (!d) return 0;

    ramp[0] = 0;
    for (i = 0; i < d->ramp_count; i++) {
        char seg[32];
        snprintf(seg, sizeof(seg), "%s%.3g:%d", i ? " " : "", d->ramp_frac[i], (int)d->ramp_pal[i]);
        strncat(ramp, seg, sizeof(ramp) - strlen(ramp) - 1);
    }

    n = snprintf(body, sizeof(body),
        "particle_effect\n{\n"
        "\t\"name\"         \"%s\"\n"
        "\t\"emission\"     \"%s\"\n"
        "\t\"count\"        \"%d\"\n"
        "\t\"rate\"         \"%.3g\"\n"
        "\t\"duration\"     \"%.3g\"\n"
        "\t\"shape\"        \"%s\"\n"
        "\t\"shape_size\"   \"%.3g\"\n"
        "\t\"cone_angle\"   \"%.3g\"\n"
        "\t\"dir_mode\"     \"%s\"\n"
        "\t\"speed\"        \"%.3g\"\n"
        "\t\"speed_jitter\" \"%.3g\"\n"
        "\t\"spread\"       \"%.3g\"\n"
        "\t\"radial_bias\"  \"%.3g\"\n"
        "\t\"gravity\"      \"%.3g\"\n"
        "\t\"drag\"         \"%.3g\"\n"
        "\t\"life_min\"     \"%.3g\"\n"
        "\t\"life_max\"     \"%.3g\"\n"
        "\t\"style\"        \"%s\"\n"
        "\t\"size_start\"   \"%.3g\"\n"
        "\t\"size_peak\"    \"%.3g\"\n"
        "\t\"size_end\"     \"%.3g\"\n"
        "\t\"ramp\"         \"%s\"\n"
        "}\n",
        d->name, EMIT_MODE_NAMES[d->mode], d->count, d->rate, d->duration,
        EMIT_SHAPE_NAMES[d->shape], d->shape_size, d->cone_angle,
        EMIT_DIR_NAMES[d->dir_mode], d->speed, d->speed_jitter, d->spread, d->radial_bias,
        d->gravity_scale, d->drag, d->life_min, d->life_max,
        EMIT_STYLE_NAMES[d->style], d->size_start, d->size_peak, d->size_end, ramp);

    snprintf(path, sizeof(path), "particles/%s.pcl", d->name);
    COM_WriteFile(path, body, n);   // writes under com_gamedir (id1/)

    // Keep index.txt in sync so the effect reloads next launch. Rebuild from
    // the live registry (simple + correct).
    {
        char acc[4096];
        int j, m = 0;
        m += snprintf(acc + m, sizeof(acc) - m, "# auto-maintained by R_EmitterSave\n");
        for (j = 0; j < EMIT_MAX_DEFS && m < (int)sizeof(acc) - EMIT_NAME_LEN; j++)
            if (s_defs[j].used) m += snprintf(acc + m, sizeof(acc) - m, "%s\n", s_defs[j].name);
        COM_WriteFile("particles/index.txt", acc, m);
    }
    return 1;
}

// Reload from disk with reload-safety: free in-flight pt_emitter particles and
// stop live instances BEFORE rebuilding s_defs (stale def indices would be UB).
static void R_ParticleReload_f(void)
{
    particle_t *p;
    int i;
    R_EmitterStopAll();
    for (p = active_particles; p; p = p->next)
        if (p->type == pt_emitter) p->die = -1;  // reaped next R_DrawParticles pass
    for (i = 0; i < EMIT_MAX_DEFS; i++) s_defs[i].used = 0;
    R_EmitterLoadAll();
    Con_Printf("particle_reload: %d effect(s)\n", R_EmitterCount());
}

void R_EmitterInit(void)
{
    Cvar_RegisterVariable(&r_emitter_active);
    Cmd_AddCommand("particle_spawn", R_ParticleSpawn_f);
    Cmd_AddCommand("particle_reload", R_ParticleReload_f);
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

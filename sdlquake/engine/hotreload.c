// hotreload.c -- hot-reloadable game DLL loader
//
// Strategy:
//   - game.dll is compiled by `zig build game` into zig-out/bin/
//   - On first call to HotReload_Init(), the DLL is copied to
//     game_loaded.dll and that copy is loaded.  Keeping a separate
//     copy lets zig overwrite game.dll while we're running.
//   - HotReload_Frame() checks game.dll's mtime every ~1 s.
//     When it changes, the old DLL is unloaded, the new one copied
//     and loaded, and game_api->init(engine, globals) is called again with
//     both the engine API and game globals.
//   - All DLL calls happen on the main thread — no locking needed.

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>

#include "game_api.h"
#include "hotreload.h"
#include "sv_bridge.h"
#include "imgui_support.h"
#include "debug_lines.h"

#if NATIVE_GAME
// Bring in the full edict_t / entvars_s definitions so offsetof works.
// We need link_t and entity_state_t first; define them here so game_types.h
// skips its own definitions via the guard macros.
#define LINK_T_DEFINED
typedef struct link_s { struct link_s *prev, *next; } link_t;
#define ENTITY_STATE_T_DEFINED
typedef struct { float origin[3], angles[3]; int modelindex, frame, colormap, skin, effects; } entity_state_t;
#include "game_types.h"
#endif

// r_livelight.c entries, exposed to the DLL via engine_api_t. Forward-declare
// here rather than including r_livelight.h to keep hotreload.c free of the
// engine_src include path / quakedef.h chain.
extern void Lightmap_AddDelta(const vec3_t pos, float radius,
                              const vec3_t color, int owner);
extern void Lightmap_ClearOwner(int owner);

// Paths relative to the working directory (project root when using zig build run).
#define GAME_DLL_SRC  "zig-out/bin/game.dll"
#define GAME_DLL_LOAD "zig-out/bin/game_loaded.dll"

// ---------------------------------------------------------------------------
// Game-DLL cvar registration shim.
// Quake's cvar_t must persist for the engine's lifetime; we own the storage.
// ---------------------------------------------------------------------------
#define MAX_GAME_CVARS 32

// Must match engine's cvar_t layout: name, string, archive(int), server(int),
// value(float), next(ptr). qboolean is int in Quake.
typedef struct game_reg_cvar_s {
    char   *name;
    char   *string;
    int     archive;
    int     server;
    float   value;
    struct game_reg_cvar_s *next;
} game_reg_cvar_t;

// Forward-declare the engine function (defined in cvar.c).
void Cvar_RegisterVariable(game_reg_cvar_t *cvar);

static game_reg_cvar_t s_game_cvars[MAX_GAME_CVARS];
static char            s_game_cvar_names[MAX_GAME_CVARS][64];
static char            s_game_cvar_defaults[MAX_GAME_CVARS][64];
static int             s_game_cvar_count;

static void engine_cvar_register(const char *name, const char *default_val) {
    if (s_game_cvar_count >= MAX_GAME_CVARS) return;
    int i = s_game_cvar_count++;
    strncpy(s_game_cvar_names[i],    name,        63);
    strncpy(s_game_cvar_defaults[i], default_val, 63);
    s_game_cvar_names[i][63]    = '\0';
    s_game_cvar_defaults[i][63] = '\0';
    memset(&s_game_cvars[i], 0, sizeof(s_game_cvars[i]));
    s_game_cvars[i].name   = s_game_cvar_names[i];
    s_game_cvars[i].string = s_game_cvar_defaults[i];
    Cvar_RegisterVariable(&s_game_cvars[i]);
}

// Forward-declare the Quake engine functions we wrap for the game DLL.
void   Con_Printf(char *fmt, ...);
void   Con_DPrintf(char *fmt, ...);
void   Cvar_Set(char *var_name, char *value);
float  Cvar_VariableValue(char *var_name);
double Sys_FloatTime(void);
void   Host_Error(char *error, ...);
void   Cbuf_AddText(char *text);
void   AngleVectors(float *angles, float *forward, float *right, float *up);
void   R_SpawnBloodPool(vec3_t origin);

// ---------------------------------------------------------------------------
// Entity / server types (condensed forward decls)
// ---------------------------------------------------------------------------

// edict_t is defined via progs.h -> game_types.h (for NATIVE_GAME=1)
// or via the original progs.h (for NATIVE_GAME=0).
// We include it via quakedef.h indirectly, but we only need the forward decl here.
typedef struct edict_s edict_t;

typedef struct { float data[3]; } _vec3_hack; // avoid conflict with vec3_t typedef

// Quake types used below — declared minimally to avoid including all of quakedef.h
typedef unsigned char byte;
typedef int qboolean;

// Must match world.h's plane_t exactly — NOT model.h's mplane_t (which has
// extra type/signbits/pad fields). Mismatch shifts trace_t.ent and produces
// uninitialized reads (the field SV_Move writes lives at a different offset
// than the field this typedef reads).
typedef struct {
    float  normal[3];
    float  dist;
} plane_t;

typedef struct {
    qboolean allsolid;
    qboolean startsolid;
    qboolean inopen, inwater;
    float    fraction;
    float    endpos[3];
    plane_t  plane;
    edict_t *ent;
} trace_t;

typedef struct sizebuf_s {
    qboolean allowoverflow;
    qboolean overflowed;
    byte    *data;
    int      maxsize;
    int      cursize;
} sizebuf_t;

extern int       pr_edict_size;

// MSG write helpers
void MSG_WriteByte  (sizebuf_t *sb, int c);
void MSG_WriteChar  (sizebuf_t *sb, int c);
void MSG_WriteShort (sizebuf_t *sb, int c);
void MSG_WriteLong  (sizebuf_t *sb, int c);
void MSG_WriteAngle (sizebuf_t *sb, float f);
void MSG_WriteCoord (sizebuf_t *sb, float f);
void MSG_WriteString(sizebuf_t *sb, char *s);

// SV functions
void     SV_BroadcastPrintf(const char *fmt, ...);
void     SV_StartSound(edict_t *entity, int channel, char *sample, int volume, float attenuation);
void     SV_StartParticle(float *org, float *dir, int color, int count);
void     SV_LinkEdict(edict_t *ent, qboolean touch_triggers);
void     SV_UnlinkEdict(edict_t *ent);
void     SV_MakeStatic(edict_t *ent);
void     SV_SetSpawnParms(edict_t *ent);
void     SV_LightStyle(int style, char *val); // engine stub if needed
int      SV_ModelIndex(char *name);
int      SV_PointContents(float *p);
qboolean SV_CheckBottom(edict_t *ent);
qboolean SV_movestep(edict_t *ent, float *move, qboolean relink);
#if NATIVE_GAME
void     SV_MoveToGoal(edict_t *ent, float dist);
#else
void     SV_MoveToGoal(void);
#endif

// pr_global_struct forward decl (the minimal globalvars_t lives in progs.h)
// We need EDICT_TO_PROG which uses sv.edicts and pr_edict_size.

#define _EDICT_TO_PROG(e) ((int)((byte *)(e) - (byte *)svb_edicts()))
#define _PROG_TO_EDICT(e) ((edict_t *)((byte *)svb_edicts() + (e)))
#define _NEXT_EDICT(e)    ((edict_t *)((byte *)(e) + pr_edict_size))
#define _EDICT_NUM(n)     ((edict_t *)((byte *)svb_edicts() + pr_edict_size * (n)))

// ---------------------------------------------------------------------------
// game_globals — exposed as non-static so engine files can write to it.
// ---------------------------------------------------------------------------
game_globals_t game_globals = {0};

#if NATIVE_GAME
// ---------------------------------------------------------------------------
// Engine API passed into game DLL — only compiled for NATIVE_GAME=1
// ---------------------------------------------------------------------------

static void engine_con_print(const char *msg)
{
    Con_Printf("%s", msg);
}

static void engine_cvar_set_value(const char *name, float value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%g", value);
    Cvar_Set((char *)name, buf);
}

static float engine_cvar_variable_value(const char *name)
{
    return Cvar_VariableValue((char *)name);
}

// ---------------------------------------------------------------------------
// Entity management wrappers
// ---------------------------------------------------------------------------

edict_t *ED_Alloc(void);
void     ED_Free(edict_t *ed);
edict_t *EDICT_NUM(int n);
int      NUM_FOR_EDICT(edict_t *e);

static edict_t *engine_ed_alloc(void) { return ED_Alloc(); }
static void     engine_ed_free(edict_t *ed) { ED_Free(ed); }

static edict_t *engine_ed_find(edict_t *start, const char *field, const char *value)
{
#if NATIVE_GAME
    // Iterate edicts searching string field by name
    edict_t *e   = start ? _NEXT_EDICT(start) : svb_edicts();
    edict_t *end = _EDICT_NUM(svb_num_edicts());
    size_t   off = (size_t)-1;

    if      (!strcmp(field, "classname"))  { off = offsetof(struct entvars_s, classname); }
    else if (!strcmp(field, "target"))     { off = offsetof(struct entvars_s, target); }
    else if (!strcmp(field, "targetname")) { off = offsetof(struct entvars_s, targetname); }

    if (off == (size_t)-1) return svb_edicts();

    while (e < end) {
        if (!e->free) {
            const char *fval = *(const char **)((char *)&e->v + off);
            if (fval && value && !strcmp(fval, value))
                return e;
        }
        e = _NEXT_EDICT(e);
    }
    return svb_edicts();
#else
    (void)start; (void)field; (void)value;
    return svb_edicts();
#endif
}

static edict_t *engine_ed_findradius(float *origin, float radius)
{
    edict_t *chain = svb_edicts();
    edict_t *e     = _NEXT_EDICT(svb_edicts());
    edict_t *end   = _EDICT_NUM(svb_num_edicts());
    float    r2    = radius * radius;
    while (e < end) {
        if (!e->free && e->v.solid != 0 /*SOLID_NOT=0*/) {
            float dx = origin[0] - (e->v.absmin[0]+e->v.absmax[0])*0.5f;
            float dy = origin[1] - (e->v.absmin[1]+e->v.absmax[1])*0.5f;
            float dz = origin[2] - (e->v.absmin[2]+e->v.absmax[2])*0.5f;
            if (dx*dx+dy*dy+dz*dz <= r2) {
                e->v.chain = chain;
                chain = e;
            }
        }
        e = _NEXT_EDICT(e);
    }
    return chain;
}

static edict_t *engine_ed_next(edict_t *e)
{
    // Iterator: NULL terminates the walk. Don't fall back to the world
    // sentinel — every `for (e = ED_Next(world); e; e = ED_Next(e))` caller
    // would loop forever, since world is non-NULL and ED_Next(world) = edict[1].
    edict_t *next = _NEXT_EDICT(e);
    edict_t *end  = _EDICT_NUM(svb_num_edicts());
    return next < end ? next : NULL;
}

static edict_t *engine_ed_checkclient(void)
{
    int i;
    for (i = 0; i < svb_maxclients(); i++) {
        if (svb_client_active(i))
            return svb_client_edict(i);
    }
    return svb_edicts();
}

static int engine_ed_getnum(edict_t *e) { return NUM_FOR_EDICT(e); }

// ---------------------------------------------------------------------------
// Spatial / physics wrappers
// ---------------------------------------------------------------------------

extern void *pr_global_struct; // treat as opaque; SV_DropToFloor etc read it

// Set pr_global_struct->self (offset 30 in globalvars_t is 'self' as int).
// The minimal globalvars_t in our progs.h has: pad[28], self at offset 28*4=112.
// self is at index 28 (0-based) in the int array.
#define SET_PROG_SELF(e)  (((int*)pr_global_struct)[28] = _EDICT_TO_PROG(e))
#define SET_PROG_OTHER(e) (((int*)pr_global_struct)[29] = _EDICT_TO_PROG(e))
#define SET_PROG_TIME(t)  (((float*)pr_global_struct)[31] = (t))

static void engine_sv_setorigin(edict_t *e, float *org)
{
    e->v.origin[0] = org[0];
    e->v.origin[1] = org[1];
    e->v.origin[2] = org[2];
    SV_LinkEdict(e, 0);
}

static void engine_sv_setsize(edict_t *e, float *mn, float *mx)
{
    e->v.mins[0] = mn[0]; e->v.mins[1] = mn[1]; e->v.mins[2] = mn[2];
    e->v.maxs[0] = mx[0]; e->v.maxs[1] = mx[1]; e->v.maxs[2] = mx[2];
    e->v.size[0] = mx[0]-mn[0]; e->v.size[1] = mx[1]-mn[1]; e->v.size[2] = mx[2]-mn[2];
    SV_LinkEdict(e, 0);
}

static void engine_sv_setmodel(edict_t *e, const char *m)
{
    int i;
    float mins[3], maxs[3];
    for (i = 0; svb_model_precache(i); i++) {
        if (!strcmp(svb_model_precache(i), m)) break;
    }
    e->v.model      = m;
    e->v.modelindex = (float)i;
    // Mirror PF_setmodel: derive entity bbox from the model's bounds so BSP
    // submodels (doors/plats/buttons/trains) get proper mins/maxs/size and
    // SV_LinkEdict places them in the right leafs — otherwise movers stay
    // stuck in the solid leaf and never get sent to clients.
    if (!svb_model_bounds(i, mins, maxs)) {
        mins[0] = mins[1] = mins[2] = 0;
        maxs[0] = maxs[1] = maxs[2] = 0;
    }
    engine_sv_setsize(e, mins, maxs);
}

static int engine_sv_droptofloor(edict_t *e)
{
    // Inline implementation (mirrors PF_droptofloor)
    extern trace_t SV_Move(float *start, float *mins, float *maxs, float *end, int type, edict_t *passedict);
    float end[3];
    trace_t trace;
    end[0] = e->v.origin[0];
    end[1] = e->v.origin[1];
    end[2] = e->v.origin[2] - 256.0f;
    trace = SV_Move(e->v.origin, e->v.mins, e->v.maxs, end, 0, e);
    if (trace.fraction == 1.0f || trace.allsolid) return 0;
    e->v.origin[0] = trace.endpos[0];
    e->v.origin[1] = trace.endpos[1];
    e->v.origin[2] = trace.endpos[2];
    SV_LinkEdict(e, 0);
    e->v.flags = (int)e->v.flags | 512; // FL_ONGROUND = 512
    e->v.groundentity = trace.ent;
    return 1;
}

static int engine_sv_walkmove(edict_t *e, float yaw, float dist)
{
    float move[3];
    float rad = yaw * (float)(3.14159265358979323846) * 2.0f / 360.0f;
    move[0] = (float)cos(rad) * dist;
    move[1] = (float)sin(rad) * dist;
    move[2] = 0.0f;
    if (!((int)e->v.flags & (512|1|4))) return 0; // FL_ONGROUND|FL_FLY|FL_SWIM
    return (int)SV_movestep(e, move, 1);
}

static void engine_sv_changeyaw(edict_t *e)
{
    float current = e->v.angles[1];
    float ideal   = e->v.ideal_yaw;
    float speed   = e->v.yaw_speed;
    float move;
    // anglemod
    while (current >= 360.0f) current -= 360.0f;
    while (current <    0.0f) current += 360.0f;
    if (current == ideal) return;
    move = ideal - current;
    if (ideal > current) { if (move >= 180.0f) move -= 360.0f; }
    else                 { if (move <= -180.0f) move += 360.0f; }
    if (move > 0) { if (move > speed) move = speed; }
    else          { if (move < -speed) move = -speed; }
    e->v.angles[1] = current + move;
    while (e->v.angles[1] >= 360.0f) e->v.angles[1] -= 360.0f;
    while (e->v.angles[1] <    0.0f) e->v.angles[1] += 360.0f;
}

static void engine_sv_traceline(float *start, float *end, int nomons, edict_t *skip)
{
    extern trace_t SV_Move(float *start, float *mins, float *maxs, float *end, int type, edict_t *passedict);
    float zero[3] = {0,0,0};
    trace_t tr = SV_Move(start, zero, zero, end, nomons, skip);
    game_globals.trace_allsolid   = (float)tr.allsolid;
    game_globals.trace_startsolid = (float)tr.startsolid;
    game_globals.trace_fraction   = tr.fraction;
    game_globals.trace_inopen     = (float)tr.inopen;
    game_globals.trace_inwater    = (float)tr.inwater;
    game_globals.trace_plane_dist = tr.plane.dist;
    game_globals.trace_ent        = tr.ent ? tr.ent : svb_edicts();
    game_globals.trace_endpos[0]       = tr.endpos[0];
    game_globals.trace_endpos[1]       = tr.endpos[1];
    game_globals.trace_endpos[2]       = tr.endpos[2];
    game_globals.trace_plane_normal[0] = tr.plane.normal[0];
    game_globals.trace_plane_normal[1] = tr.plane.normal[1];
    game_globals.trace_plane_normal[2] = tr.plane.normal[2];
}

static int engine_sv_checkbottom(edict_t *e) { return (int)SV_CheckBottom(e); }
static int engine_sv_pointcontents(float *p)  { return SV_PointContents(p); }

static void engine_sv_movetogal(edict_t *e, float step)
{
    // Delegate to the engine's native SV_MoveToGoal (sv_move.c, NATIVE_GAME=1).
    // It calls SV_StepDirection(ideal_yaw) which rotates angles[1] toward
    // ideal_yaw via do_changeyaw, and falls back to SV_NewChaseDir when blocked.
    // The previous inline reimplementation skipped the yaw rotation entirely,
    // so patrolling monsters (whose only yaw update happens in t_movetarget)
    // never turned to face their direction of movement.
    SV_MoveToGoal(e, step);
}

// PF_aim port (pr_cmds.c). Returns a forward direction vector with optional
// auto-aim snap to the closest valid DAMAGE_AIM target within sv_aim cosine
// of the player's forward direction. Without this, every weapon that calls
// aim() — shotgun, super shotgun, nailgun, super nailgun, rocket, grenade —
// fires off into the void because dir is treated as a direction vector by
// the callers (e.g. FireBullets does end = src + dir*2048).
static void engine_sv_aim(edict_t *e, float speed, float *out)
{
    extern trace_t SV_Move(float *start, float *mins, float *maxs,
                           float *end, int type, edict_t *passedict);

    (void)speed;

    float fwd[3], rgt[3], upv[3];
    AngleVectors(e->v.v_angle, fwd, rgt, upv);

    float start[3];
    start[0] = e->v.origin[0];
    start[1] = e->v.origin[1];
    start[2] = e->v.origin[2] + 20.0f;

    float zero[3] = {0,0,0};
    float end[3];
    end[0] = start[0] + 2048.0f * fwd[0];
    end[1] = start[1] + 2048.0f * fwd[1];
    end[2] = start[2] + 2048.0f * fwd[2];

    trace_t tr = SV_Move(start, zero, zero, end, 0, e);

    float teamplay = Cvar_VariableValue("teamplay");

    // Straight ahead hits a valid target — no need to snap.
    if (tr.ent && tr.ent != svb_edicts()
        && tr.ent->v.takedamage == 2.0f /* DAMAGE_AIM */
        && (teamplay == 0 || e->v.team <= 0 || e->v.team != tr.ent->v.team))
    {
        out[0] = fwd[0]; out[1] = fwd[1]; out[2] = fwd[2];
        return;
    }

    // Search all candidate targets for the best auto-aim snap.
    edict_t *bestent  = NULL;
    float    bestdist = Cvar_VariableValue("sv_aim");

    edict_t *check = _NEXT_EDICT(svb_edicts());
    edict_t *limit = _EDICT_NUM(svb_num_edicts());
    while (check < limit) {
        if (!check->free
            && check->v.takedamage == 2.0f /* DAMAGE_AIM */
            && check != e
            && !(teamplay != 0 && e->v.team > 0 && e->v.team == check->v.team))
        {
            float center[3];
            center[0] = check->v.origin[0] + 0.5f*(check->v.mins[0]+check->v.maxs[0]);
            center[1] = check->v.origin[1] + 0.5f*(check->v.mins[1]+check->v.maxs[1]);
            center[2] = check->v.origin[2] + 0.5f*(check->v.mins[2]+check->v.maxs[2]);

            float dir[3];
            dir[0] = center[0]-start[0];
            dir[1] = center[1]-start[1];
            dir[2] = center[2]-start[2];
            float len = (float)sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
            if (len > 0.0f) { dir[0]/=len; dir[1]/=len; dir[2]/=len; }

            float dot = dir[0]*fwd[0]+dir[1]*fwd[1]+dir[2]*fwd[2];
            if (dot >= bestdist) {
                tr = SV_Move(start, zero, zero, center, 0, e);
                if (tr.ent == check) {
                    bestdist = dot;
                    bestent  = check;
                }
            }
        }
        check = _NEXT_EDICT(check);
    }

    if (bestent) {
        // Project (bestent.origin - ent.origin) onto fwd to keep horizontal
        // aim, then preserve the actual z-delta so the shot rises/falls onto
        // the target. Renormalize.
        float dir[3];
        dir[0] = bestent->v.origin[0] - e->v.origin[0];
        dir[1] = bestent->v.origin[1] - e->v.origin[1];
        dir[2] = bestent->v.origin[2] - e->v.origin[2];
        float dot = dir[0]*fwd[0]+dir[1]*fwd[1]+dir[2]*fwd[2];
        float aimv[3] = { fwd[0]*dot, fwd[1]*dot, fwd[2]*dot };
        aimv[2] = dir[2];
        float len = (float)sqrt(aimv[0]*aimv[0]+aimv[1]*aimv[1]+aimv[2]*aimv[2]);
        if (len > 0.0f) {
            out[0] = aimv[0]/len; out[1] = aimv[1]/len; out[2] = aimv[2]/len;
        } else {
            out[0] = fwd[0]; out[1] = fwd[1]; out[2] = fwd[2];
        }
    } else {
        out[0] = fwd[0]; out[1] = fwd[1]; out[2] = fwd[2];
    }
}

// ---------------------------------------------------------------------------
// Output / messaging wrappers
// ---------------------------------------------------------------------------

static void engine_sv_bprint(int level, const char *msg)
{
    (void)level;
    SV_BroadcastPrintf("%s", msg);
}

static void engine_sv_sprint(edict_t *client, int level, const char *msg)
{
    svb_sv_sprint(client, level, msg);
}

static void engine_sv_centerprint(edict_t *client, const char *msg)
{
    svb_sv_centerprint(client, msg);
}

static void engine_sv_stuffcmd(edict_t *client, const char *cmd)
{
    svb_sv_stuffcmd(client, cmd);
}

static void engine_cbuf_addtext(const char *cmd) { Cbuf_AddText((char *)cmd); }
static void engine_con_dprintf(const char *msg)  { Con_DPrintf("%s", msg); }
static void engine_host_error(const char *msg)   { Host_Error("%s", msg); }

// ---------------------------------------------------------------------------
// MSG write wrappers
// MSG_BROADCAST=0, MSG_ONE=1, MSG_ALL=2, MSG_INIT=3
// ---------------------------------------------------------------------------

static sizebuf_t *get_msg_buf(int dest)
{
    if (dest == 0) return svb_datagram();
    if (dest == 2) return svb_reliable_datagram();
    if (dest == 3) return svb_signon();
    // dest == 1: write to msg_entity's client
    if (game_globals.msg_entity) {
        int i;
        for (i = 0; i < svb_maxclients(); i++) {
            if (svb_client_edict(i) == game_globals.msg_entity && svb_client_active(i))
                return svb_client_message(i);
        }
    }
    return svb_datagram();
}

static void engine_msg_writebyte(int dest, int v)     { MSG_WriteByte(get_msg_buf(dest), v); }
static void engine_msg_writechar(int dest, int v)     { MSG_WriteChar(get_msg_buf(dest), v); }
static void engine_msg_writeshort(int dest, int v)    { MSG_WriteShort(get_msg_buf(dest), v); }
static void engine_msg_writelong(int dest, int v)     { MSG_WriteLong(get_msg_buf(dest), v); }
static void engine_msg_writeangle(int dest, float v)  { MSG_WriteAngle(get_msg_buf(dest), v); }
static void engine_msg_writecoord(int dest, float v)  { MSG_WriteCoord(get_msg_buf(dest), v); }
static void engine_msg_writestring(int dest, const char *s) { MSG_WriteString(get_msg_buf(dest), (char *)s); }
static void engine_msg_writeentity(int dest, edict_t *e) { MSG_WriteShort(get_msg_buf(dest), NUM_FOR_EDICT(e)); }

// ---------------------------------------------------------------------------
// Audio wrappers
// ---------------------------------------------------------------------------

static void engine_sv_startsound(edict_t *e, int ch, const char *samp, float vol, float att)
{
    SV_StartSound(e, ch, (char *)samp, (int)(vol * 255.0f), att);
}

static void engine_sv_ambientsound(float *org, const char *samp, float vol, float att)
{
    // Ambient sounds are written to the signon packet directly (mirrors PF_ambientsound).
    int i, soundnum = -1;
    for (i = 0; svb_sound_precache(i); i++) {
        if (!strcmp(svb_sound_precache(i), samp)) { soundnum = i; break; }
    }
    if (soundnum < 0) {
        Con_Printf("engine_sv_ambientsound: no precache: %s\n", samp);
        return;
    }
    MSG_WriteByte(svb_signon(), 29); // svc_spawnstaticsound
    MSG_WriteCoord(svb_signon(), org[0]);
    MSG_WriteCoord(svb_signon(), org[1]);
    MSG_WriteCoord(svb_signon(), org[2]);
    MSG_WriteByte(svb_signon(), soundnum);
    MSG_WriteByte(svb_signon(), (int)(vol * 255.0f));
    MSG_WriteByte(svb_signon(), (int)(att * 64.0f));
}

static void engine_sv_lightstyle(int style, const char *val)
{
    svb_sv_lightstyle(style, val);
}

// ---------------------------------------------------------------------------
// Math wrappers
// ---------------------------------------------------------------------------

static void engine_makevectors(float *angles)
{
    AngleVectors(angles,
        game_globals.v_forward,
        game_globals.v_right,
        game_globals.v_up);
}

static void engine_vectornormalize(float *v, float *out)
{
    float len = (float)sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if (len > 0.0f) { out[0]=v[0]/len; out[1]=v[1]/len; out[2]=v[2]/len; }
    else { out[0]=out[1]=out[2]=0.0f; }
}

static float engine_vectorlength(float *v) {
    return (float)sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
}

static float engine_vectortoyaw(float *v)
{
    float yaw;
    if (v[1] == 0.0f && v[0] == 0.0f) return 0.0f;
    yaw = (float)((int)(atan2(v[1], v[0]) * 180.0 / 3.14159265358979323846));
    if (yaw < 0.0f) yaw += 360.0f;
    return yaw;
}

static void engine_vectortoangles(float *v, float *out)
{
    float yaw, pitch, forward;
    if (v[1] == 0.0f && v[0] == 0.0f) {
        yaw = 0.0f;
        pitch = (v[2] > 0.0f) ? 90.0f : 270.0f;
    } else {
        yaw   = (float)((int)(atan2(v[1], v[0]) * 180.0 / 3.14159265358979323846));
        if (yaw < 0.0f) yaw += 360.0f;
        forward = (float)sqrt(v[0]*v[0]+v[1]*v[1]);
        pitch = (float)((int)(atan2(v[2], forward) * 180.0 / 3.14159265358979323846));
        if (pitch < 0.0f) pitch += 360.0f;
    }
    out[0] = pitch;
    out[1] = yaw;
    out[2] = 0.0f;
}

static float engine_random(void) { return (rand() & 0x7fff) / (float)0x7fff; }
static float engine_fabsf(float f) { return f < 0.0f ? -f : f; }

// ---------------------------------------------------------------------------
// String conversion
// ---------------------------------------------------------------------------

static const char *engine_ftos(float f)
{
    static char buf[32];
    if (f == (int)f) snprintf(buf, sizeof(buf), "%d", (int)f);
    else             snprintf(buf, sizeof(buf), "%g", f);
    return buf;
}

static const char *engine_vtos(float *v)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "'%g %g %g'", v[0], v[1], v[2]);
    return buf;
}

// ---------------------------------------------------------------------------
// Precache wrappers (mirrors PF_precache_model / PF_precache_sound)
// ---------------------------------------------------------------------------

static const char *engine_precachemodel(const char *s)
{
    int i;
    if (!s || !s[0]) return s;
    for (i = 0; i < 256; i++) {
        if (!svb_model_precache(i)) {
            svb_set_model_precache(i, s);
            // Mirror PF_precache_model: also load the model into sv.models[i]
            // so engine_sv_setmodel can read its bbox back out. Without this,
            // any entity whose collision comes from the model (e.g. barrels
            // using maps/b_explob.bsp, which don't call SV_SetSize after
            // SV_SetModel) ends up with a zero-size bbox and is non-solid.
            svb_load_model(i, s);
            return s;
        }
        if (!strcmp(svb_model_precache(i), s)) return s;
    }
    Con_Printf("engine_precachemodel: overflow\n");
    return s;
}

static const char *engine_precachesound(const char *s)
{
    int i;
    if (!s || !s[0]) return s;
    for (i = 0; i < 256; i++) {
        if (!svb_sound_precache(i)) { svb_set_sound_precache(i, s); return s; }
        if (!strcmp(svb_sound_precache(i), s)) return s;
    }
    Con_Printf("engine_precachesound: overflow\n");
    return s;
}

static const char *engine_precachefile(const char *s) { return s; } // no-op

// ---------------------------------------------------------------------------
// Misc wrappers
// ---------------------------------------------------------------------------

static void engine_sv_changelevel(const char *map)
{
    char cmd[128];
    if (svb_changelevel_issued()) return;
    svb_set_changelevel_issued(1);
    snprintf(cmd, sizeof(cmd), "changelevel %s\n", map);
    Cbuf_AddText(cmd);
}

static void engine_sv_particle(float *org, float *dir, float color, float count)
{
    SV_StartParticle(org, dir, (int)color, (int)count);
}

static void engine_sv_makestatic(edict_t *e)
{
    sizebuf_t *signon = svb_signon();
    MSG_WriteByte(signon, 20); // svc_spawnstatic
    MSG_WriteByte(signon, SV_ModelIndex((char *)e->v.model));
    MSG_WriteByte(signon, (int)e->v.frame);
    MSG_WriteByte(signon, (int)e->v.colormap);
    MSG_WriteByte(signon, (int)e->v.skin);
    MSG_WriteCoord(signon, e->v.origin[0]);
    MSG_WriteAngle(signon, e->v.angles[0]);
    MSG_WriteCoord(signon, e->v.origin[1]);
    MSG_WriteAngle(signon, e->v.angles[1]);
    MSG_WriteCoord(signon, e->v.origin[2]);
    MSG_WriteAngle(signon, e->v.angles[2]);
    ED_Free(e);
}

static void engine_sv_setspawnparms(edict_t *e)
{
    int idx = NUM_FOR_EDICT(e);
    int j;
    float *sp;
    if (idx < 1 || idx > svb_maxclients()) return;
    sp = svb_client_spawn_parms(idx - 1);
    for (j = 0; j < 16; j++)
        game_globals.parm[j] = sp[j];
}

// ---------------------------------------------------------------------------
// imgui AI panel shims
// ---------------------------------------------------------------------------

static void imgui_ai_push_shim(int edict_num, int state, float alert_level,
                               const float last_known_pos[3], int target_edict)
{
    imgui_ai_row_t r;
    r.edict_num = edict_num;
    r.state = state;
    r.alert_level = alert_level;
    r.last_known_pos[0] = last_known_pos[0];
    r.last_known_pos[1] = last_known_pos[1];
    r.last_known_pos[2] = last_known_pos[2];
    r.target_edict = target_edict;
    ImguiSupport_AI_Push(&r);
}

extern int imgui_ai_panel_open;  // defined in imgui_layer.c
extern int ImguiLayer_IsOpen(void);  // defined in imgui_layer.c
static int imgui_ai_active_shim(void) {
    return imgui_ai_panel_open && ImguiLayer_IsOpen();
}

// ---------------------------------------------------------------------------
// VFS file loader — reads through Quake's PAK-aware filesystem.
// ---------------------------------------------------------------------------
extern int  COM_OpenFile(char *filename, int *hndl);
extern void COM_CloseFile(int h);
extern int  Sys_FileRead(int handle, void *dest, int count);

static void *engine_load_file(const char *path, int *out_size) {
    int h = -1;
    int len = COM_OpenFile((char *)path, &h);
    if (len < 0) return NULL;
    void *buf = malloc(len + 1);
    if (!buf) { COM_CloseFile(h); return NULL; }
    Sys_FileRead(h, buf, len);
    COM_CloseFile(h);
    ((char *)buf)[len] = '\0';
    if (out_size) *out_size = len;
    return buf;
}

static void engine_debug_line(float *start, float *end, int color, int ztest) {
    DebugLines_Add(start, end, color, ztest);
}

static void engine_get_view_origin(float *out) {
    extern float r_origin[3];
    out[0] = r_origin[0];
    out[1] = r_origin[1];
    out[2] = r_origin[2];
}

static void engine_sv_tracemove(float *start, float *mins, float *maxs,
                                float *end, int nomons, edict_t *skip)
{
    extern trace_t SV_Move(float *start, float *mins, float *maxs,
                           float *end, int type, edict_t *passedict);
    trace_t tr = SV_Move(start, mins, maxs, end, nomons, skip);
    game_globals.trace_allsolid   = (float)tr.allsolid;
    game_globals.trace_startsolid = (float)tr.startsolid;
    game_globals.trace_fraction   = tr.fraction;
    game_globals.trace_inopen     = (float)tr.inopen;
    game_globals.trace_inwater    = (float)tr.inwater;
    game_globals.trace_plane_dist = tr.plane.dist;
    game_globals.trace_ent        = tr.ent ? tr.ent : svb_edicts();
    game_globals.trace_endpos[0]       = tr.endpos[0];
    game_globals.trace_endpos[1]       = tr.endpos[1];
    game_globals.trace_endpos[2]       = tr.endpos[2];
    game_globals.trace_plane_normal[0] = tr.plane.normal[0];
    game_globals.trace_plane_normal[1] = tr.plane.normal[1];
    game_globals.trace_plane_normal[2] = tr.plane.normal[2];
}

static void engine_spawn_blood_pool(const vec3_t origin)
{
    vec3_t o;
    o[0] = origin[0]; o[1] = origin[1]; o[2] = origin[2];
    R_SpawnBloodPool(o);
}

// Lightmap sample at world-space point (Phase 8 / M5). Reuses the renderer's
// existing R_LightPoint which traces down through the BSP, samples the
// surface lightmap with style mixing, and clamps against ambientlight.
// R_LightPoint already handles a NULL worldmodel by returning 255 (full
// bright), so we don't need to guard against it here.
static int engine_sample_lightmap(vec3_t pos)
{
    extern int R_LightPoint(vec3_t p);
    vec3_t p = { pos[0], pos[1], pos[2] };
    return R_LightPoint(p);
}

// ---------------------------------------------------------------------------
// imgui Nav panel shims
// ---------------------------------------------------------------------------
static void imgui_nav_set_shim(const void *pts_xy, int np,
                               const void *edges_ushort_pairs, int ne) {
    ImguiSupport_Nav_Set((const imgui_nav_point_t *)pts_xy, np,
                         (const imgui_nav_edge_t  *)edges_ushort_pairs, ne);
}

static void imgui_nav_setpath_shim(const void *path_xy_floats, int n) {
    imgui_nav_active_t p;
    memset(&p, 0, sizeof(p));
    p.has_path = (n > 0);
    p.path_len = (n > 32) ? 32 : n;
    if (p.path_len > 0)
        memcpy(p.path_xy, path_xy_floats, sizeof(float) * 2 * p.path_len);
    ImguiSupport_Nav_SetPath(&p);
}

// ---------------------------------------------------------------------------
// Full engine_funcs table
// ---------------------------------------------------------------------------

static engine_api_t engine_funcs = {
    engine_con_print,
    engine_cvar_set_value,
    engine_cvar_variable_value,
    Sys_FloatTime,

    engine_ed_alloc,
    engine_ed_free,
    engine_ed_find,
    engine_ed_findradius,
    engine_ed_next,
    engine_ed_checkclient,
    engine_ed_getnum,

    engine_sv_setorigin,
    engine_sv_setmodel,
    engine_sv_setsize,
    engine_sv_droptofloor,
    engine_sv_walkmove,
    engine_sv_changeyaw,
    engine_sv_traceline,
    engine_sv_checkbottom,
    engine_sv_pointcontents,
    engine_sv_movetogal,
    engine_sv_aim,

    engine_sv_bprint,
    engine_sv_sprint,
    engine_sv_centerprint,
    engine_sv_stuffcmd,
    engine_cbuf_addtext,
    engine_con_dprintf,
    engine_host_error,

    engine_msg_writebyte,
    engine_msg_writechar,
    engine_msg_writeshort,
    engine_msg_writelong,
    engine_msg_writeangle,
    engine_msg_writecoord,
    engine_msg_writestring,
    engine_msg_writeentity,

    engine_sv_startsound,
    engine_sv_ambientsound,
    engine_sv_lightstyle,

    engine_makevectors,
    engine_vectornormalize,
    engine_vectorlength,
    engine_vectortoyaw,
    engine_vectortoangles,
    engine_random,
    engine_fabsf,

    engine_ftos,
    engine_vtos,

    engine_precachemodel,
    engine_precachesound,
    engine_precachefile,

    engine_sv_changelevel,
    engine_sv_particle,
    engine_sv_makestatic,
    engine_sv_setspawnparms,

    ImguiSupport_AI_Clear,
    imgui_ai_push_shim,
    imgui_ai_active_shim,
    imgui_nav_set_shim,
    imgui_nav_setpath_shim,
    engine_cvar_register,
    engine_load_file,
    engine_debug_line,
    engine_get_view_origin,
    engine_sv_tracemove,
    engine_spawn_blood_pool,
    engine_sample_lightmap,
    Lightmap_AddDelta,
    Lightmap_ClearOwner,
};

// ---------------------------------------------------------------------------
// DLL state
// ---------------------------------------------------------------------------

static SDL_SharedObject *game_so    = NULL;
game_api_t              *g_game_api = NULL;  // exposed for NATIVE_GAME dispatch guards
static SDL_Time          dll_mtime  = 0;

static SDL_Time get_mtime(const char *path)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) return 0;
    return info.modify_time;
}

static void do_unload(void)
{
    if (g_game_api) { g_game_api->shutdown(); g_game_api = NULL; }
    if (game_so)    { SDL_UnloadObject(game_so); game_so = NULL; }
}

static void do_load(void)
{
    do_unload();

    // Copy so zig can overwrite game.dll while game_loaded.dll stays mapped.
    if (!SDL_CopyFile(GAME_DLL_SRC, GAME_DLL_LOAD))
    {
        Con_Printf("hotreload: copy failed: %s\n", SDL_GetError());
        return;
    }

    game_so = SDL_LoadObject(GAME_DLL_LOAD);
    if (!game_so)
    {
        Con_Printf("hotreload: load failed: %s\n", SDL_GetError());
        return;
    }

    Game_GetAPI_fn get_api =
        (Game_GetAPI_fn)(void *)SDL_LoadFunction(game_so, "Game_GetAPI");
    if (!get_api)
    {
        Con_Printf("hotreload: Game_GetAPI not found\n");
        SDL_UnloadObject(game_so); game_so = NULL;
        return;
    }

    game_api_t *api = get_api();
    if (!api || api->version != GAME_API_VERSION)
    {
        Con_Printf("hotreload: ABI version mismatch (want %d)\n", GAME_API_VERSION);
        SDL_UnloadObject(game_so); game_so = NULL;
        return;
    }

    g_game_api = api;
    Con_Printf("hotreload: game.dll reloaded\n");
    // Set world pointer before init so game code can reference sv.edicts safely
    game_globals.world = svb_edicts();
    g_game_api->init(&engine_funcs, &game_globals);
    dll_mtime = get_mtime(GAME_DLL_SRC);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static int polling_enabled = 0;

#define RELOAD_CHECK_INTERVAL 60   // frames between mtime polls (~1 s at 60 fps)

void HotReload_Init(void)
{
    SDL_Time t = get_mtime(GAME_DLL_SRC);
    if (t != 0) do_load();
}

void HotReload_Frame(float dt)
{
    (void)dt;
    if (!polling_enabled) return;

    static int counter = 0;
    if (++counter >= RELOAD_CHECK_INTERVAL)
    {
        counter = 0;
        SDL_Time t = get_mtime(GAME_DLL_SRC);
        if (t != 0 && t != dll_mtime)
            do_load();
    }
    // Note: start_frame is now called from sv_phys.c SV_Physics() with proper
    // game_globals sync, not here. HotReload_Frame is just for reload polling.
}

void HotReload_Shutdown(void)
{
    do_unload();
}

void HotReload_EnablePolling(void)
{
    polling_enabled = 1;
    Con_Printf("hotreload: polling enabled\n");
}

#else /* !NATIVE_GAME — game DLL not used */

game_api_t *g_game_api = NULL;

void HotReload_Init(void)     {}
void HotReload_Frame(float dt) { (void)dt; }
void HotReload_Shutdown(void) {}
void HotReload_EnablePolling(void) {}

#endif /* NATIVE_GAME */

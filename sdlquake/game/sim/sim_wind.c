// sim_wind.c -- 3D voxel wind/smoke field (Phase 8 / M4).
//
// Coarse Eulerian grid covering the map's world bounds. Per cell: a wind
// velocity vector and a 0..1 smoke density. The field is updated at 10 Hz
// (not per-frame): semi-Lagrangian advection of the smoke field, multiplied
// per-tick decay, and velocity damping. There is no pressure solve -- this
// is a transport field, not a fluid sim.
//
// Public hooks:
//   * Wind_AddImpulse        -- Gust drops a radial outward velocity burst.
//   * Wind_AddSmoke          -- smoke source (smoke grenade, fire prop).
//   * Wind_GetSmokeAt        -- single-point sample (AI sense filter).
//   * Wind_PathOcclusion     -- integral of smoke along a segment (LOS).
//   * Wind_RegisterSource    -- info_wind_source entities inject continuously.
//
// Sizing: the grid is sized at level load. Cell side defaults to
// SIM_WIND_CELL_SIZE (64 units, ~1m). If the world's bbox would exceed
// SIM_WIND_MAX_DIM in any axis, the cell size is doubled until it fits.

#include "sim.h"
#include "../game_defs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

#define SIM_WIND_CELL_SIZE   64.0f
#define SIM_WIND_MAX_DIM     64        // cap per axis (64^3 = 262144 cells)
#define SIM_WIND_TICK_HZ     10.0f
#define SIM_WIND_DECAY       0.97f
#define SIM_WIND_VEL_DAMP    0.85f
#define SIM_WIND_MAX_SOURCES 32
#define SIM_WIND_SMOKE_STIM_THRESHOLD 0.4f

typedef struct {
    float vx, vy, vz;
    float smoke;
} wind_cell_t;

static int          s_dim_x, s_dim_y, s_dim_z;
static float        s_cell_size;
static vec3_t       s_origin;            // world-space corner of cell (0,0,0)
static wind_cell_t *s_cells;             // s_dim_x * s_dim_y * s_dim_z
static wind_cell_t *s_scratch;           // double-buffer for advection
static float        s_next_tick_time;
static int          s_ready;

static struct {
    edict_t *e;
    float    vel[3];
} s_sources[SIM_WIND_MAX_SOURCES];
static int s_source_count;

// Per-cell debounce so we don't spam STIM_SMOKE.
static unsigned char *s_stim_emitted;   // sized like s_cells

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int idx_clamped(int x, int y, int z) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (z < 0) z = 0;
    if (x >= s_dim_x) x = s_dim_x - 1;
    if (y >= s_dim_y) y = s_dim_y - 1;
    if (z >= s_dim_z) z = s_dim_z - 1;
    return (z * s_dim_y + y) * s_dim_x + x;
}

static int idx_or_neg(int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0) return -1;
    if (x >= s_dim_x || y >= s_dim_y || z >= s_dim_z) return -1;
    return (z * s_dim_y + y) * s_dim_x + x;
}

static int worldpos_to_cell(const vec3_t pos, int *cx, int *cy, int *cz) {
    if (!s_ready) return 0;
    float fx = (pos[0] - s_origin[0]) / s_cell_size;
    float fy = (pos[1] - s_origin[1]) / s_cell_size;
    float fz = (pos[2] - s_origin[2]) / s_cell_size;
    *cx = (int)floorf(fx);
    *cy = (int)floorf(fy);
    *cz = (int)floorf(fz);
    return 1;
}

static float trilinear_smoke(float fx, float fy, float fz) {
    int   x0 = (int)floorf(fx), y0 = (int)floorf(fy), z0 = (int)floorf(fz);
    float tx = fx - x0, ty = fy - y0, tz = fz - z0;
    int   x1 = x0 + 1,  y1 = y0 + 1,  z1 = z0 + 1;

    float c000 = s_cells[idx_clamped(x0,y0,z0)].smoke;
    float c100 = s_cells[idx_clamped(x1,y0,z0)].smoke;
    float c010 = s_cells[idx_clamped(x0,y1,z0)].smoke;
    float c110 = s_cells[idx_clamped(x1,y1,z0)].smoke;
    float c001 = s_cells[idx_clamped(x0,y0,z1)].smoke;
    float c101 = s_cells[idx_clamped(x1,y0,z1)].smoke;
    float c011 = s_cells[idx_clamped(x0,y1,z1)].smoke;
    float c111 = s_cells[idx_clamped(x1,y1,z1)].smoke;

    float c00 = c000*(1-tx) + c100*tx;
    float c10 = c010*(1-tx) + c110*tx;
    float c01 = c001*(1-tx) + c101*tx;
    float c11 = c011*(1-tx) + c111*tx;
    float c0  = c00 *(1-ty) + c10 *ty;
    float c1  = c01 *(1-ty) + c11 *ty;
    return c0 *(1-tz) + c1 *tz;
}

// ---------------------------------------------------------------------------
// World-bounds discovery. Walks all spawned edicts and unions their
// absmin/absmax into a single bbox. Used at level init to size the grid.
// ---------------------------------------------------------------------------
static void discover_world_bounds(vec3_t out_mins, vec3_t out_maxs) {
    out_mins[0] = out_mins[1] = out_mins[2] =  1e9f;
    out_maxs[0] = out_maxs[1] = out_maxs[2] = -1e9f;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e->v.absmin[0] < out_mins[0]) out_mins[0] = e->v.absmin[0];
        if (e->v.absmin[1] < out_mins[1]) out_mins[1] = e->v.absmin[1];
        if (e->v.absmin[2] < out_mins[2]) out_mins[2] = e->v.absmin[2];
        if (e->v.absmax[0] > out_maxs[0]) out_maxs[0] = e->v.absmax[0];
        if (e->v.absmax[1] > out_maxs[1]) out_maxs[1] = e->v.absmax[1];
        if (e->v.absmax[2] > out_maxs[2]) out_maxs[2] = e->v.absmax[2];
    }
    // Fallback for very-early calls (no entities yet).
    if (out_mins[0] > out_maxs[0]) {
        out_mins[0] = out_mins[1] = out_mins[2] = -2048;
        out_maxs[0] = out_maxs[1] = out_maxs[2] =  2048;
    }
    // Pad by a couple of cells so impulses near the edge still work.
    for (int i = 0; i < 3; i++) {
        out_mins[i] -= SIM_WIND_CELL_SIZE * 2;
        out_maxs[i] += SIM_WIND_CELL_SIZE * 2;
    }
}

// ---------------------------------------------------------------------------
// Allocation -- we use a global static buffer pool to avoid heap churn on
// hot-reload.  Sized for the worst case 64^3 grid.
// ---------------------------------------------------------------------------
#define SIM_WIND_MAX_CELLS  (SIM_WIND_MAX_DIM*SIM_WIND_MAX_DIM*SIM_WIND_MAX_DIM)
static wind_cell_t s_cells_storage[SIM_WIND_MAX_CELLS];
static wind_cell_t s_scratch_storage[SIM_WIND_MAX_CELLS];
static unsigned char s_stim_storage[SIM_WIND_MAX_CELLS];

void Wind_Init(void) {
    s_cells   = s_cells_storage;
    s_scratch = s_scratch_storage;
    s_stim_emitted = s_stim_storage;
    s_dim_x = s_dim_y = s_dim_z = 0;
    s_cell_size = SIM_WIND_CELL_SIZE;
    s_origin[0] = s_origin[1] = s_origin[2] = 0;
    s_ready = 0;
    s_source_count = 0;
    s_next_tick_time = 0;

    eng->Cvar_Register("sim_wind_debug",  "0");
    eng->Cvar_Register("sim_wind_enable", "1");
}

void Wind_LevelInit(void) {
    s_ready = 0;
    s_source_count = 0;

    if (eng->Cvar_VariableValue("sim_wind_enable") < 0.5f) return;

    vec3_t wmins, wmaxs;
    discover_world_bounds(wmins, wmaxs);

    float dx = wmaxs[0] - wmins[0];
    float dy = wmaxs[1] - wmins[1];
    float dz = wmaxs[2] - wmins[2];
    float cell = SIM_WIND_CELL_SIZE;
    while ((int)ceilf(dx/cell) > SIM_WIND_MAX_DIM ||
           (int)ceilf(dy/cell) > SIM_WIND_MAX_DIM ||
           (int)ceilf(dz/cell) > SIM_WIND_MAX_DIM)
    {
        cell *= 2.0f;
        if (cell > 1024.0f) break;
    }
    s_cell_size = cell;
    s_dim_x = (int)ceilf(dx/cell);
    s_dim_y = (int)ceilf(dy/cell);
    s_dim_z = (int)ceilf(dz/cell);
    if (s_dim_x > SIM_WIND_MAX_DIM) s_dim_x = SIM_WIND_MAX_DIM;
    if (s_dim_y > SIM_WIND_MAX_DIM) s_dim_y = SIM_WIND_MAX_DIM;
    if (s_dim_z > SIM_WIND_MAX_DIM) s_dim_z = SIM_WIND_MAX_DIM;
    s_origin[0] = wmins[0];
    s_origin[1] = wmins[1];
    s_origin[2] = wmins[2];

    int total = s_dim_x * s_dim_y * s_dim_z;
    if (total > SIM_WIND_MAX_CELLS) {
        s_dim_x = s_dim_y = s_dim_z = 0;
        return;
    }
    memset(s_cells,        0, sizeof(wind_cell_t) * total);
    memset(s_scratch,      0, sizeof(wind_cell_t) * total);
    memset(s_stim_emitted, 0, total);

    s_next_tick_time = g->time + (1.0f / SIM_WIND_TICK_HZ);
    s_ready = 1;
}

// ---------------------------------------------------------------------------
// Tick (10 Hz)
// ---------------------------------------------------------------------------
static void emit_smoke_stims(void) {
    int total = s_dim_x * s_dim_y * s_dim_z;
    for (int i = 0; i < total; i++) {
        if (s_cells[i].smoke >= SIM_WIND_SMOKE_STIM_THRESHOLD &&
            !s_stim_emitted[i])
        {
            // Compute cell centre.
            int   z = i / (s_dim_x * s_dim_y);
            int   r = i - z * s_dim_x * s_dim_y;
            int   y = r / s_dim_x;
            int   x = r - y * s_dim_x;
            stimulus_t s;
            memset(&s, 0, sizeof(s));
            s.kind = STIM_SMOKE;
            s.origin[0] = s_origin[0] + (x + 0.5f) * s_cell_size;
            s.origin[1] = s_origin[1] + (y + 0.5f) * s_cell_size;
            s.origin[2] = s_origin[2] + (z + 0.5f) * s_cell_size;
            s.intensity = s_cells[i].smoke;
            s.source_edict = -1;
            Stim_Emit(&s);
            s_stim_emitted[i] = 1;
        } else if (s_cells[i].smoke < SIM_WIND_SMOKE_STIM_THRESHOLD * 0.5f) {
            s_stim_emitted[i] = 0;   // re-arm
        }
    }
}

// Spawn light-grey particles in cells with non-trivial smoke density so the
// field is visible to the player. Called every frame (not every tick), with
// jittered spawn positions across the cell volume so the fog reads as a
// volume rather than a grid of dots.
static float rand_unit(void) {
    return ((float)rand() / (float)RAND_MAX) - 0.5f;
}
static void emit_smoke_particles(void) {
    int total = s_dim_x * s_dim_y * s_dim_z;
    for (int i = 0; i < total; i++) {
        float d = s_cells[i].smoke;
        if (d < 0.05f) continue;
        int z = i / (s_dim_x * s_dim_y);
        int r = i - z * s_dim_x * s_dim_y;
        int y = r / s_dim_x;
        int x = r - y * s_dim_x;
        int bursts = 2 + (int)(d * 6.0f);   // 2..8 bursts/cell/frame
        for (int b = 0; b < bursts; b++) {
            vec3_t pos = {
                s_origin[0] + (x + 0.5f) * s_cell_size + rand_unit() * s_cell_size,
                s_origin[1] + (y + 0.5f) * s_cell_size + rand_unit() * s_cell_size,
                s_origin[2] + (z + 0.5f) * s_cell_size + rand_unit() * s_cell_size,
            };
            vec3_t drift = { rand_unit() * 8.0f,
                             rand_unit() * 8.0f,
                             8.0f + rand_unit() * 4.0f };
            eng->SV_Smoke(pos, drift, 8, 3);
        }
    }
}

static void wind_tick(float dt) {
    int total = s_dim_x * s_dim_y * s_dim_z;

    // Apply continuous sources before advection so the parcel they inject
    // is carried by the same frame's velocity.
    for (int i = 0; i < s_source_count; i++) {
        edict_t *e = s_sources[i].e;
        if (!e || e->free) continue;
        int cx, cy, cz;
        if (!worldpos_to_cell(e->v.origin, &cx, &cy, &cz)) continue;
        int idx = idx_or_neg(cx, cy, cz);
        if (idx < 0) continue;
        s_cells[idx].vx += s_sources[i].vel[0] * dt;
        s_cells[idx].vy += s_sources[i].vel[1] * dt;
        s_cells[idx].vz += s_sources[i].vel[2] * dt;
        // info_wind_source is wind, not smoke -- no smoke injection by default.
    }

    // Semi-Lagrangian advection of smoke.
    memcpy(s_scratch, s_cells, sizeof(wind_cell_t) * total);
    for (int z = 0; z < s_dim_z; z++) {
        for (int y = 0; y < s_dim_y; y++) {
            for (int x = 0; x < s_dim_x; x++) {
                int idx = (z * s_dim_y + y) * s_dim_x + x;
                float fx = (float)x - (s_cells[idx].vx * dt) / s_cell_size;
                float fy = (float)y - (s_cells[idx].vy * dt) / s_cell_size;
                float fz = (float)z - (s_cells[idx].vz * dt) / s_cell_size;
                s_scratch[idx].smoke = trilinear_smoke(fx, fy, fz);
                s_scratch[idx].smoke *= SIM_WIND_DECAY;
                if (s_scratch[idx].smoke < 0.001f) s_scratch[idx].smoke = 0;
                // Velocity damping (no advection of the velocity field
                // itself in this v1 -- transport only).
                s_scratch[idx].vx = s_cells[idx].vx * SIM_WIND_VEL_DAMP;
                s_scratch[idx].vy = s_cells[idx].vy * SIM_WIND_VEL_DAMP;
                s_scratch[idx].vz = s_cells[idx].vz * SIM_WIND_VEL_DAMP;
            }
        }
    }
    wind_cell_t *tmp = s_cells;
    s_cells = s_scratch;
    s_scratch = tmp;

    emit_smoke_stims();
}

void Wind_Frame(void) {
    if (!s_ready) return;
    emit_smoke_particles();
    if (g->time < s_next_tick_time) return;
    float dt = 1.0f / SIM_WIND_TICK_HZ;
    wind_tick(dt);
    s_next_tick_time = g->time + dt;
}

// ---------------------------------------------------------------------------
// Sample / occlusion API
// ---------------------------------------------------------------------------
float Wind_GetSmokeAt(const vec3_t pos) {
    if (!s_ready) return 0;
    int cx, cy, cz;
    worldpos_to_cell(pos, &cx, &cy, &cz);
    int i = idx_or_neg(cx, cy, cz);
    if (i < 0) return 0;
    return s_cells[i].smoke;
}

float Wind_PathOcclusion(const vec3_t a, const vec3_t b) {
    if (!s_ready) return 0;
    // 3D DDA along the line, summing smoke per cell touched.
    vec3_t d = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    float len = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (len < 1.0f) return 0;
    int steps = (int)(len / s_cell_size) + 1;
    if (steps > 128) steps = 128;
    float sum = 0;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        vec3_t p = { a[0] + d[0]*t, a[1] + d[1]*t, a[2] + d[2]*t };
        sum += Wind_GetSmokeAt(p);
    }
    // Average density along segment, scaled to a 0..1 occlusion factor.
    float avg = sum / (steps + 1);
    if (avg > 1.0f) avg = 1.0f;
    return avg;
}

// ---------------------------------------------------------------------------
// Impulse and smoke injection
// ---------------------------------------------------------------------------
void Wind_AddImpulse(const vec3_t origin, const vec3_t dir, float magnitude,
                     float radius)
{
    if (!s_ready) return;
    int cx, cy, cz;
    worldpos_to_cell(origin, &cx, &cy, &cz);
    int crange = (int)ceilf(radius / s_cell_size);
    for (int dz = -crange; dz <= crange; dz++)
    for (int dy = -crange; dy <= crange; dy++)
    for (int dx = -crange; dx <= crange; dx++) {
        int i = idx_or_neg(cx+dx, cy+dy, cz+dz);
        if (i < 0) continue;
        float rx = dx * s_cell_size;
        float ry = dy * s_cell_size;
        float rz = dz * s_cell_size;
        float r  = sqrtf(rx*rx + ry*ry + rz*rz);
        if (r > radius || r < 1.0f) continue;
        float falloff = 1.0f - (r / radius);
        s_cells[i].vx += dir[0] * magnitude * falloff;
        s_cells[i].vy += dir[1] * magnitude * falloff;
        s_cells[i].vz += dir[2] * magnitude * falloff;
    }
}

void Wind_AddSmoke(const vec3_t origin, float amount, float radius) {
    if (!s_ready) return;
    int cx, cy, cz;
    worldpos_to_cell(origin, &cx, &cy, &cz);
    int crange = (int)ceilf(radius / s_cell_size);
    for (int dz = -crange; dz <= crange; dz++)
    for (int dy = -crange; dy <= crange; dy++)
    for (int dx = -crange; dx <= crange; dx++) {
        int i = idx_or_neg(cx+dx, cy+dy, cz+dz);
        if (i < 0) continue;
        float rx = dx * s_cell_size;
        float ry = dy * s_cell_size;
        float rz = dz * s_cell_size;
        float r  = sqrtf(rx*rx + ry*ry + rz*rz);
        if (r > radius) continue;
        float falloff = 1.0f - (r / radius);
        s_cells[i].smoke += amount * falloff;
        if (s_cells[i].smoke > 1.0f) s_cells[i].smoke = 1.0f;
    }
}

void Wind_ClearSmoke(const vec3_t origin, float amount, float radius) {
    if (!s_ready) return;
    int cx, cy, cz;
    worldpos_to_cell(origin, &cx, &cy, &cz);
    int crange = (int)ceilf(radius / s_cell_size);
    for (int dz = -crange; dz <= crange; dz++)
    for (int dy = -crange; dy <= crange; dy++)
    for (int dx = -crange; dx <= crange; dx++) {
        int i = idx_or_neg(cx+dx, cy+dy, cz+dz);
        if (i < 0) continue;
        float rx = dx * s_cell_size;
        float ry = dy * s_cell_size;
        float rz = dz * s_cell_size;
        float r  = sqrtf(rx*rx + ry*ry + rz*rz);
        if (r > radius) continue;
        s_cells[i].smoke -= amount;
        if (s_cells[i].smoke < 0) s_cells[i].smoke = 0;
    }
}

// ---------------------------------------------------------------------------
// info_wind_source registration
// ---------------------------------------------------------------------------
void Wind_RegisterSource(edict_t *e, const vec3_t velocity) {
    if (!e || s_source_count >= SIM_WIND_MAX_SOURCES) return;
    s_sources[s_source_count].e = e;
    s_sources[s_source_count].vel[0] = velocity[0];
    s_sources[s_source_count].vel[1] = velocity[1];
    s_sources[s_source_count].vel[2] = velocity[2];
    s_source_count++;
}

// ---------------------------------------------------------------------------
// Debug overlay -- called from game_debug_draw_overlays. Draws a vertical
// line per cell with smoke > threshold, color-scaled by density.
// ---------------------------------------------------------------------------
void Wind_DebugDraw(void) {
    if (!s_ready) return;
    int ztest = Sim_DebugZTest("sim_wind_debug");
    if (ztest < 0) return;

    vec3_t view;
    eng->Get_ViewOrigin(view);
    float max_cull = 1024.0f;

    for (int z = 0; z < s_dim_z; z++) {
        for (int y = 0; y < s_dim_y; y++) {
            for (int x = 0; x < s_dim_x; x++) {
                int idx = (z * s_dim_y + y) * s_dim_x + x;
                float d = s_cells[idx].smoke;
                if (d < 0.1f) continue;
                vec3_t p0 = { s_origin[0] + (x+0.5f)*s_cell_size,
                              s_origin[1] + (y+0.5f)*s_cell_size,
                              s_origin[2] + (z+0.5f)*s_cell_size };
                float dx = p0[0]-view[0], dy = p0[1]-view[1], dz = p0[2]-view[2];
                if (dx*dx + dy*dy + dz*dz > max_cull*max_cull) continue;
                vec3_t p1 = { p0[0], p0[1], p0[2] + 16.0f };
                int color = (d > 0.7f) ? 251 : (d > 0.4f) ? 192 : 80;
                eng->SV_DebugLine(p0, p1, color, ztest);
            }
        }
    }
}

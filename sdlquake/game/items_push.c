// items_push.c -- shootable pickups: convert weapon damage into a velocity
// impulse so ammo/health/armor/weapons/keys/powerups skid when hit.
//
// Pickups are SOLID_TRIGGER + MOVETYPE_TOSS + FL_ITEM. MOVE_NORMAL bullet
// traces skip SOLID_TRIGGER, so the regular weapon trace never sees them.
// We mirror the Spike_GibPathScan / Corpse_BulletTrace pattern: walk the
// edict list, run a segment-vs-AABB (or radius) check against FL_ITEM ents,
// and push them. World traces are untouched -- bullets still mark walls.
//
// MOVETYPE_TOSS already handles gravity, wall clip and floor-snap (vanilla
// backpacks ride this path), so we only set velocity/avelocity.

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

#define ITEM_VEL_CAP 400.0f

// Slab-method AABB-vs-segment. Mirrors weapons.c:segment_hits_aabb -- kept
// local so this module doesn't depend on weapons.c internals.
static int seg_hits_aabb(vec3_t start, vec3_t end, float *mins, float *maxs) {
    float d[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
    float tmin = 0.0f, tmax = 1.0f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-6f) {
            if (start[i] < mins[i] || start[i] > maxs[i]) return 0;
            continue;
        }
        float inv = 1.0f / d[i];
        float t1 = (mins[i] - start[i]) * inv;
        float t2 = (maxs[i] - start[i]) * inv;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }
    return 1;
}

// Filter: only push live, on-the-ground pickups. Just-consumed pickups
// (SOLID_NOT + model cleared) and respawning DM items (SOLID_NOT between
// regen ticks) are skipped.
static int is_pushable_item(edict_t *e) {
    if (!e || e == g->world) return 0;
    if (!((int)e->v.flags & FL_ITEM)) return 0;
    if (e->v.solid != SOLID_TRIGGER) return 0;
    if (e->v.modelindex == 0) return 0;
    return 1;
}

static void clamp_vel(vec3_t v) {
    float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (l > ITEM_VEL_CAP) {
        float s = ITEM_VEL_CAP / l;
        v[0] *= s; v[1] *= s; v[2] *= s;
    }
}

// dir must be normalized.
static void apply_impulse(edict_t *e, vec3_t dir, float damage) {
    e->v.velocity[0] += dir[0] * damage * 8.0f;
    e->v.velocity[1] += dir[1] * damage * 8.0f;
    // SV_Physics_Toss early-returns on FL_ONGROUND and zeroes velocity on
    // any ground contact (sv_phys.c:1463 / :1603). Without a small upward
    // delta the box re-grounds the very next tick and the slide dies. Floor
    // velocity[2] to a fixed minimum so the box clears the floor for a
    // couple of frames -- never amplify by shot angle, that's what was
    // launching them. Doesn't compound on repeat hits.
    if (e->v.velocity[2] < 25.0f) e->v.velocity[2] = 25.0f;
    clamp_vel(e->v.velocity);
    // No tumble: items keep their upright orientation, and any prior spin
    // (e.g. DM rotating powerups) is killed on hit.
    e->v.avelocity[0] = e->v.avelocity[1] = e->v.avelocity[2] = 0;
    e->v.flags = (float)((int)e->v.flags & ~FL_ONGROUND);
}

// Sweep a bullet/beam segment and push every pickup the line crosses. Call
// with `end` clamped to the wall-hit point (g->trace_endpos) so items behind
// the wall don't get phantom-pushed. `dir` must be normalized.
void Items_BulletSweep(vec3_t start, vec3_t end, vec3_t dir, float damage, edict_t *ignore) {
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == ignore) continue;
        if (!is_pushable_item(e)) continue;
        if (seg_hits_aabb(start, end, e->v.absmin, e->v.absmax))
            apply_impulse(e, dir, damage);
    }
}

// Radial push from an explosion centre. Linear distance falloff; direction
// is bbox-centre-minus-origin so a rocket landing next to a stack of items
// fans them outward instead of all in one direction.
void Items_RadiusPush(vec3_t origin, float radius, float base_impulse, edict_t *ignore) {
    SIM_PERF("Items_RadiusPush")
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == ignore) continue;
        if (!is_pushable_item(e)) continue;
        vec3_t to = {
            (e->v.absmin[0]+e->v.absmax[0])*0.5f - origin[0],
            (e->v.absmin[1]+e->v.absmax[1])*0.5f - origin[1],
            (e->v.absmin[2]+e->v.absmax[2])*0.5f - origin[2],
        };
        float dist = sqrtf(to[0]*to[0] + to[1]*to[1] + to[2]*to[2]);
        if (dist >= radius || dist < 0.01f) continue;
        float falloff = 1.0f - dist/radius;
        float inv = 1.0f / dist;
        vec3_t dir = { to[0]*inv, to[1]*inv, to[2]*inv };
        apply_impulse(e, dir, base_impulse * falloff);
    }
}

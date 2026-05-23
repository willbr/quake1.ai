# Bouncing particles + lightning-gun sparks

**Date:** 2026-05-23
**Status:** design / not implemented

## 1. Goals & scope

Add per-particle world collision (opt-in via flag bits) so:

- A new `pt_spark` type bounces once off geometry, sticks on the second hit, color-ramps from cyan-white through hot-metal cool-down to a dark ember, and lingers as a dark dot for ~0.5 s after the ramp finishes.
- Existing blood-spray (`R_BloodSpray`) and water/slime/lava splash (`R_WaterSplash`) droplets stick on first contact instead of falling through the world.
- Every other particle type (smoke, rocket fire, explosion, teleport, tracer, etc.) is byte-identical to today. Collision is opt-in only.

The lightning gun spawns a 20-spark hemispherical burst at the bolt's visible impact point, oriented by the surface normal, replacing the current `eng->SV_Particle(...)` call in `sdlquake/game/weapons.c:792`.

## 2. Architecture overview

Three pieces:

1. **Extend `particle_t`** with a `byte flags` field. Bits encode collision behavior (`PARTFL_BOUNCE`, `PARTFL_STICK_ON_HIT`) and state (`PARTFL_BOUNCED`, `PARTFL_STUCK`, `PARTFL_RAMP_HOLD`, `PARTFL_DWELL`). Extend `ptype_t` with `pt_spark`.

2. **`R_TraceParticle(start, end, trace_t *out)`** — thin wrapper in `r_part.c` over `SV_RecursiveHullCheck(cl.worldmodel->hulls + 0, …)`. Returns true on hit, fills `endpos` and `plane.normal`. Guards `cl.worldmodel == NULL`. World-only — no entity tests.

3. **Per-frame collision in `R_DrawParticles`** — after the wind nudge, before position commit: if a particle's flags say it can collide, trace `org → org + vel*dt` and dispatch on flag combination (reflect / stick).

A new client-side spawn helper `R_SparkBurst(origin, normal, count)` and a new temp-entity wire opcode `TE_SPARKBURST` (so demo recording and the existing `MSG_BROADCAST`/temp-entity pattern still work). The game DLL emits it from `weapons.c` instead of `SV_Particle`.

**Considered & rejected:** point-contents-only collision (no traceline). It can detect "spark entered solid" but yields no surface normal, so reflection is impossible — defeats the design.

## 3. Particle struct extension

```c
// d_iface.h
typedef enum {
    pt_static, pt_grav, pt_slowgrav, pt_fire, pt_explode, pt_explode2,
    pt_blob, pt_blob2, pt_smoke, pt_spark
} ptype_t;

#define PARTFL_BOUNCE       0x01  // bounce once at r_sparks_restitution; 2nd hit sticks
#define PARTFL_STICK_ON_HIT 0x02  // first contact zeroes vel and freezes pos
#define PARTFL_BOUNCED      0x04  // state: one bounce consumed
#define PARTFL_STUCK        0x08  // state: skip integration + collision
#define PARTFL_RAMP_HOLD    0x10  // pt_spark: hold cyan until first bounce
#define PARTFL_DWELL        0x20  // pt_spark: in post-ramp dark-ember dwell

typedef struct particle_s {
    vec3_t  org;
    float   color;
    struct particle_s *next;
    vec3_t  vel;
    float   ramp;
    float   die;
    ptype_t type;
    float   birth;
    byte    flags;   // PARTFL_*
} particle_t;
```

`d_ifacea.h` offsets are bumped for bookkeeping (the asm files are not compiled in the SDL build; the comment in `d_iface.h` already notes this).

All existing spawn sites that touch `particle_t` continue to zero-init `flags` (taken from a freshly-recycled slot in the linked-list pool) — but recycled slots carry stale flags from their last use. Therefore every spawn function MUST explicitly set `p->flags = 0` (or the intended flag set) when reusing a slot. This is a one-line edit at every existing `free_particles`→`active_particles` site.

## 4. Per-particle physics in `R_DrawParticles`

For each active particle, in order:

1. **If `PARTFL_STUCK`** — skip wind nudge, skip integration, but advance ramp/lifetime so dwell and `die` still progress.
2. **Else** — apply wind nudge (unchanged). Compute `newpos = org + vel * dt`.
3. **If `flags & (PARTFL_BOUNCE | PARTFL_STICK_ON_HIT)`** — call `R_TraceParticle(org, newpos, &tr)`:
   - **Hit + `PARTFL_STICK_ON_HIT`**: `org = tr.endpos + normal * 0.5`, `vel = 0`, `flags |= PARTFL_STUCK`.
   - **Hit + `PARTFL_BOUNCE` and `PARTFL_BOUNCED` already set**: identical to the stick case — stop and freeze.
   - **Hit + `PARTFL_BOUNCE` first time**: inline reflection — `d = dot(vel, normal); vel -= 2 * d * normal; vel *= r_sparks_restitution.value;` — then move to `tr.endpos + normal * 0.5`, set `PARTFL_BOUNCED`. For `pt_spark` also clear `PARTFL_RAMP_HOLD` and set `ramp = 0` so the cool-down starts at white-hot. (`ClipVelocity` lives in `sv_phys.c` and isn't visible to `r_part.c` — keep the reflection inline rather than re-exposing it.)
   - **No hit**: `org = newpos`.
4. **Else** — `org = newpos` (existing fall-through, no perf change for non-collidable particles).

Then the existing per-type switch runs (gravity, ramp animation). New case:

```c
case pt_spark:
    if (p->flags & PARTFL_RAMP_HOLD) {
        p->color = 244 + (rand() % 3);          // cyan flicker; no cool-down yet
    } else if (!(p->flags & PARTFL_DWELL)) {
        p->ramp += time2;                       // same rate as pt_explode
        if (p->ramp >= 8) {
            p->color = ramp1[7];                // 0x61 — dark ember
            p->flags |= PARTFL_DWELL;
            p->die = cl.time + r_sparks_settle_dwell.value;
        } else {
            p->color = ramp1[(int)p->ramp];
        }
    }
    if (!(p->flags & PARTFL_STUCK))
        p->vel[2] -= grav;                      // light gravity (matches pt_slowgrav)
    break;
```

Note ordering: position+collision happens *before* the type switch so that a fresh bounce can clear `PARTFL_RAMP_HOLD` and the very next ramp step is the first cool-down frame.

## 5. Spawn functions

### `R_SparkBurst(origin, normal, count)` — new in `r_part.c`

- Spawns `count` particles (caller picks; lightning gun passes 20).
- Direction: uniform-random in the hemisphere oriented by `normal`. Sample a random vector in the unit sphere via three normal-distributed components (or rejection sample inside a unit cube); flip its sign if `dot(v, normal) < 0` so it lies in the +normal hemisphere; multiply by a random speed in `[200, 500]`.
- Lifetime: `0.8 + (rand() & 31) * 0.0125` (≈0.8–1.2 s).
- Color: `244 + (rand() % 3)`.
- `org`: `origin + normal * 1.0` (1 unit offset, prevents trace-immediately-into-spawn-surface on the first integration step).
- `type = pt_spark`, `flags = PARTFL_BOUNCE | PARTFL_RAMP_HOLD`, `ramp = 0`, `birth = cl.time`.

`count` is multiplied by `r_sparks_count_mul.value` (clamped to ≥ 0) inside `R_SparkBurst` so the cvar gates work uniformly regardless of caller.

### `R_BloodSpray` — modified

Every newly-spawned droplet: `p->flags = PARTFL_STICK_ON_HIT;`. No other changes.

### `R_WaterSplash` — modified

Every newly-spawned droplet: `p->flags = PARTFL_STICK_ON_HIT;`. No other changes.

### All other spawn sites — audit pass

Walk every `free_particles → active_particles` site in `r_part.c` and `cl_tent.c` and add `p->flags = 0;` if absent, since the linked-list pool recycles slots with stale flag bits.

## 6. Wire protocol — `TE_SPARKBURST`

Next free temp-entity opcode in `protocol.h` (current highest is `TE_BLOODSPRAY`; add `TE_SPARKBURST` after it). Wire payload:

```
byte     TE_SPARKBURST
coord×3  origin
char×3   normal × 127, divided by 127 on read
byte     count
```

`MSG_WriteChar` is the byte-quantised normal channel already used for `MSG_ReadChar() * (1.0/16)` in `R_ParseParticleEffect`; we use a tighter 1/127 quantisation here because the normal is unit-length and small (<1) values lose too much precision at /16.

Client parser in `cl_tent.c` (new case in the switch inside `CL_ParseTEnt`):

```c
case TE_SPARKBURST: {
    vec3_t org, normal;
    int count;
    for (int i = 0; i < 3; i++) org[i] = MSG_ReadCoord();
    for (int i = 0; i < 3; i++) normal[i] = MSG_ReadChar() * (1.0f / 127.0f);
    count = MSG_ReadByte();
    R_SparkBurst(org, normal, count);
    break;
}
```

### Game DLL change in `sdlquake/game/weapons.c` (~lines 781–797)

Replace the entire spark-shower block — the `spark_vel` assignment, the explanatory comment about palette 244-246 and the `SV_Particle` call — with a temp-entity write. `spark_vel` is no longer needed since direction is encoded as the surface normal on the wire.

```c
// before:
vec3_t spark_vel = {
    g->trace_plane_normal[0] * 30,
    g->trace_plane_normal[1] * 30,
    g->trace_plane_normal[2] * 30 + 20
};
eng->SV_Particle(g->trace_endpos, spark_vel, 245, 60);

// after:
eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
eng->MSG_WriteByte(MSG_BROADCAST, TE_SPARKBURST);
eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[0]);
eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[1]);
eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[2]);
eng->MSG_WriteChar(MSG_BROADCAST, (int)(g->trace_plane_normal[0] * 127.0f));
eng->MSG_WriteChar(MSG_BROADCAST, (int)(g->trace_plane_normal[1] * 127.0f));
eng->MSG_WriteChar(MSG_BROADCAST, (int)(g->trace_plane_normal[2] * 127.0f));
eng->MSG_WriteByte(MSG_BROADCAST, 20);
```

`TE_SPARKBURST` must be defined identically wherever the game DLL reads TE constants today (check `game/game_api.h` or wherever the existing `TE_LIGHTNING2` is reachable from `weapons.c`). No `engine_api_t` ABI bump needed — `MSG_Write*` are already in `engine_api_t`. `GAME_API_VERSION` stays at 21.

## 7. cvars

Register in `R_Init` next to the existing `r_smoke_*` block:

- `r_sparks_count_mul` (default `"1"`) — multiplier applied inside `R_SparkBurst` on the incoming count (`0` disables sparks, `2` doubles). Float; clamped to ≥ 0.
- `r_sparks_settle_dwell` (default `"0.5"`) — seconds the dark-ember dwell lasts after the cool-down ramp finishes.
- `r_sparks_restitution` (default `"0.5"`) — bounce energy retention on the first hit.

All three are runtime-settable and read live by the draw path; no engine restart needed.

## 8. Performance

- Peak active sparks under sustained lightning fire: 20 × 10 Hz × ~1 s = ~200. Plus rare blood/water bursts: peak ~300 collidable particles.
- 300 × 60 fps = 18 k tracelines/sec. `SV_RecursiveHullCheck` on hull 0 is a BSP descent; budget well under 1 ms/sec total. Negligible at our scales.
- Stuck/dwell particles short-circuit before integration and tracing — zero ongoing cost.
- Smoke (up to 32 k particles) is unaffected: no flag, no trace.

## 9. Testing (manual, single-player)

- **Lightning gun at a wall** — sparks fan out into the hemisphere, one bounce is visible on each, settle as fading orange→red→dark dots on the floor for ~0.5 s before vanishing.
- **Lightning gun at a corner** — sparks bounce out of the corner, no clipping into geometry, no sparks stuck inside walls.
- **Shotgun into water** (using existing `R_WaterSplash` impact) — splash droplets land on the shore and sit for their ~1 s lifetime, no fall-through.
- **Gib a grunt** — blood spray splats on walls and floor instead of vanishing in mid-arc.
- **Loading screen** (`cl.worldmodel == NULL`) — collision skipped cleanly inside `R_TraceParticle`, no crash if a residual particle integrates during the transition.
- **Spark spawned right at a wall** — 1-unit normal offset prevents instant self-collision; the spark flies free for at least one frame.
- **Sustained lightning fire** — pool not exhausted (200 active << 32 k pool), other gameplay particles still spawn normally.

## 10. Out of scope

- Sparks fizzling on water/lava contact.
- Sparks emitting dynamic light (the lightning beam itself already lights the impact area).
- Bouncing off entity surfaces (monsters, doors) — world brushes only.
- Multiplayer / dedicated server testing.
- Gravity-aware "roll along slopes" behaviour for settled sparks — they freeze where they stick.

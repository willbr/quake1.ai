# Bouncing Particles + Lightning-Gun Sparks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add opt-in world collision to the particle system (new `pt_spark` type, plus `PARTFL_STICK_ON_HIT` for blood/water droplets), and replace the lightning-gun impact effect with a 20-spark hemispherical burst that bounces once, sticks, color-ramps from cyan to dark ember, and lingers for ~0.5 s.

**Architecture:** Three pieces — (1) a `byte flags` field on `particle_t` plus a new `pt_spark` value of `ptype_t`; (2) a `R_TraceParticle` wrapper over `SV_RecursiveHullCheck` on `cl.worldmodel->hulls[0]` called per-frame for collidable particles inside `R_DrawParticles`; (3) a new `TE_SPARKBURST` temp-entity opcode carrying origin + normal + count, parsed client-side into `R_SparkBurst`. The game DLL emits it from `weapons.c`.

**Tech Stack:** C89/gnu89 engine code (`-std=gnu89 -fcommon`), Zig build system (`zig build`), Quake's BSP traceline via `SV_RecursiveHullCheck`, single-player rendering through the existing `R_DrawParticles` path. No test framework; verification is `zig build` (compile) + manual in-game playtest per the spec.

**Reference:** `docs/superpowers/specs/2026-05-23-bouncing-particles-and-sparks-design.md` (the full design).

---

## File Structure

| File | Role |
|---|---|
| `sdlquake/engine_src/d_iface.h` | Extend `ptype_t` (add `pt_spark`), add `PARTFL_*` bit defines, add `byte flags` to `particle_t`. |
| `sdlquake/engine_src/protocol.h` | Add `TE_SPARKBURST 18` opcode. |
| `sdlquake/engine_src/render.h` | Declare `R_SparkBurst`. |
| `sdlquake/engine_src/r_main.c` | Define and register 3 new cvars (`r_sparks_count_mul`, `r_sparks_settle_dwell`, `r_sparks_restitution`). |
| `sdlquake/engine_src/r_part.c` | Most of the work. Extern the new cvars, audit every `free_particles → active_particles` site to zero `p->flags`, implement `R_TraceParticle` static helper, implement `R_SparkBurst`, add `pt_spark` case to type switch, add collision dispatch in `R_DrawParticles`, modify `R_BloodSpray` and `R_WaterSplash` to set `PARTFL_STICK_ON_HIT`. |
| `sdlquake/engine_src/cl_tent.c` | Parse `TE_SPARKBURST` and call `R_SparkBurst`. |
| `sdlquake/game/weapons.c` | Replace the `eng->SV_Particle(...)` spark-shower call (~lines 781–797) with a `MSG_Write*` block emitting `TE_SPARKBURST`. |

Commit cadence: one commit per task. Commit straight to master (project preference — no branches, no worktrees).

---

## Task 1: Extend `particle_t` and `ptype_t`

**Files:**
- Modify: `sdlquake/engine_src/d_iface.h` (lines 34–57)

- [ ] **Step 1: Edit the enum and struct**

Replace the existing `ptype_t` enum and `particle_t` struct with the extended versions. Original location is `sdlquake/engine_src/d_iface.h` around line 34. Final state:

```c
typedef enum {
	pt_static, pt_grav, pt_slowgrav, pt_fire, pt_explode, pt_explode2, pt_blob, pt_blob2,
	pt_smoke,	// pt_static behaviour, but D_DrawSmokeParticle scales size by ramp and dithers
	pt_spark	// bouncing electrical ember; cyan birth, ramp1 cool-down, stick on 2nd hit
} ptype_t;

// Per-particle behaviour flags. Stored in particle_t::flags. Recycled
// free-list slots carry stale bits, so every spawn site MUST set
// p->flags explicitly (either 0 or the intended combination).
#define PARTFL_BOUNCE		0x01	// bounce once at r_sparks_restitution; 2nd hit sticks
#define PARTFL_STICK_ON_HIT	0x02	// first contact zeroes vel and freezes pos
#define PARTFL_BOUNCED		0x04	// state: one bounce consumed
#define PARTFL_STUCK		0x08	// state: skip integration + collision
#define PARTFL_RAMP_HOLD	0x10	// pt_spark: hold cyan flicker until first bounce
#define PARTFL_DWELL		0x20	// pt_spark: in post-ramp dark-ember dwell

// !!! if this is changed, it must be changed in d_ifacea.h too !!!
//     (asm files aren't compiled in the SDL build, so the offsets there
//      only matter if we ever re-enable id386. Update both if you do.)
typedef struct particle_s
{
// driver-usable fields
	vec3_t		org;
	float		color;
// drivers never touch the following fields
	struct particle_s	*next;
	vec3_t		vel;
	float		ramp;
	float		die;
	ptype_t		type;
	// pt_smoke uses birth to drive a "puff: small+dense -> big+faded"
	// curve in D_DrawSmokeParticle. Other ptypes ignore it; R_AddSmokePuff
	// is the only spawn path that sets it.
	float		birth;
	// Per-particle collision/state bits. See PARTFL_* above.
	byte		flags;
} particle_t;
```

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. No code references `flags`, `pt_spark`, or the new `PARTFL_*` macros yet, so the build should pass clean.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/d_iface.h
git commit -m "$(cat <<'EOF'
feat(particles): add flags byte + pt_spark + PARTFL_* defines

Lays the foundation for opt-in particle/world collision. No spawn site
sets flags yet; recycled slots will still pick up stale bits, but the
upcoming audit pass forces explicit p->flags assignment at every site.

EOF
)"
```

---

## Task 2: Add `TE_SPARKBURST` opcode (engine + game DLL)

The game DLL doesn't include `engine_src/protocol.h` directly — it carries its own subset of `TE_*` constants in `sdlquake/game/game_defs.h`. Both files must declare `TE_SPARKBURST = 18` identically.

**Files:**
- Modify: `sdlquake/engine_src/protocol.h` (after the existing `TE_BLOODSPRAY 17` line)
- Modify: `sdlquake/game/game_defs.h` (after the existing `TE_BLOODSPRAY 17` line, ~line 133)

- [ ] **Step 1: Add the opcode to `protocol.h`**

In `sdlquake/engine_src/protocol.h`, after the existing line `#define TE_BLOODSPRAY		17`, insert:

```c
#define TE_SPARKBURST		18	// hemispherical spark burst (origin+normal+count)
```

- [ ] **Step 2: Add the matching opcode to `game_defs.h`**

In `sdlquake/game/game_defs.h`, after the existing line `#define TE_BLOODSPRAY  17` (around line 133), insert:

```c
#define TE_SPARKBURST  18
```

Indentation and alignment match the surrounding `TE_*` block in this file (single-space separators, no tabs).

- [ ] **Step 3: Verify the build still compiles**

Run: `zig build`
Expected: success. Nothing references the new opcode yet.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/protocol.h sdlquake/game/game_defs.h
git commit -m "feat(protocol): TE_SPARKBURST opcode (engine + game DLL)"
```

---

## Task 3: Declare + register three new cvars

**Files:**
- Modify: `sdlquake/engine_src/r_main.c` (cvar block ~line 159, registration in `R_Init` ~line 262)
- Modify: `sdlquake/engine_src/r_part.c` (extern block ~line 35)

- [ ] **Step 1: Add the cvar definitions in `r_main.c`**

In `sdlquake/engine_src/r_main.c`, immediately after the existing line `cvar_t	r_smoke_cell_threshold = {"r_smoke_cell_threshold", "0.02"};` (around line 161), insert:

```c
cvar_t	r_sparks_count_mul     = {"r_sparks_count_mul",     "1"};	// 0 disables sparks; 2 doubles per-burst count
cvar_t	r_sparks_settle_dwell  = {"r_sparks_settle_dwell",  "0.5"};	// seconds a dark ember lingers after cool-down ramp finishes
cvar_t	r_sparks_restitution   = {"r_sparks_restitution",   "0.5"};	// energy retained on first bounce (clamped to [0,1])
```

- [ ] **Step 2: Register them in `R_Init`**

In the same file, inside `R_Init`, after `Cvar_RegisterVariable (&r_smoke_cell_threshold);` (around line 263), insert:

```c
	Cvar_RegisterVariable (&r_sparks_count_mul);
	Cvar_RegisterVariable (&r_sparks_settle_dwell);
	Cvar_RegisterVariable (&r_sparks_restitution);
```

- [ ] **Step 3: Extern them in `r_part.c`**

In `sdlquake/engine_src/r_part.c`, after the existing extern block (around line 38, after `extern cvar_t r_smoke_emit_div;`), insert:

```c
// Spark tunables — declared in r_main.c, registered in R_Init.
extern cvar_t r_sparks_count_mul;
extern cvar_t r_sparks_settle_dwell;
extern cvar_t r_sparks_restitution;
```

- [ ] **Step 4: Verify the build still compiles**

Run: `zig build`
Expected: success.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_main.c sdlquake/engine_src/r_part.c
git commit -m "feat(particles): r_sparks_{count_mul,settle_dwell,restitution} cvars"
```

---

## Task 4: Declare `R_SparkBurst` in `render.h`

**Files:**
- Modify: `sdlquake/engine_src/render.h` (line 148 area — after `R_TeleportSplash`)

- [ ] **Step 1: Add the declaration**

In `sdlquake/engine_src/render.h`, after `void R_TeleportSplash (vec3_t org);` (around line 148), insert:

```c
void R_SparkBurst (vec3_t origin, vec3_t normal, int count);
```

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. Only the prototype is declared; no body yet. No caller yet either.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/render.h
git commit -m "feat(render): R_SparkBurst declaration"
```

---

## Task 5: Audit every particle spawn site — explicit `p->flags`

The linked-list pool recycles slots; without an explicit assignment a recycled slot would carry stale `flags` bits from its previous life as some other particle. This task adds the assignment at every site.

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (12 spawn sites — see list below)

- [ ] **Step 1: Add `p->flags = 0;` after each `active_particles = p;` site**

For each of the following functions in `sdlquake/engine_src/r_part.c`, locate the line `active_particles = p;` and insert `p->flags = 0;` immediately after it (with matching indentation). The pattern looks like:

```c
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;
		p->flags = 0;       // <-- new line, matches indentation of line above
```

Functions to update (one occurrence each unless noted, all currently between lines 200 and 900 of `r_part.c`):

1. `R_EntityParticles` (line ~207)
2. `R_ReadPointFile_f` (line ~273)
3. `R_ParticleExplosion` (line ~331)
4. `R_ParticleExplosion2` (line ~376)
5. `R_BlobExplosion` (line ~409)
6. `R_AddSmokePuff` (line ~527)
7. `R_RunParticleEffect` (line ~570)
8. `R_LavaSplash` (line ~640)
9. `R_WaterSplash` (line ~699) — will be overridden to `PARTFL_STICK_ON_HIT` in Task 10; for now set `0` here so this task stays a pure mechanical pass.
10. `R_BloodSpray` (line ~737) — same caveat as above.
11. `R_TeleportSplash` (line ~772)
12. `R_RocketTrail` (line ~820)

(The QUAKE2-guarded `R_DarkFieldParticles` at line ~130 can be left alone — `QUAKE2` is not defined in this build.)

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success.

- [ ] **Step 3: Smoke-test that gameplay particles are unchanged**

Run: `zig build run -- +map e1m1`
- Fire the shotgun at a wall → puff sparks.
- Fire the rocket launcher → trail + explosion.
- Walk into a teleporter → teleport sparkle.

Each effect should look byte-identical to before. We've only zeroed an uninitialised field whose value the rest of the code is still ignoring.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "$(cat <<'EOF'
chore(particles): explicit p->flags = 0 at every spawn site

Recycled free_particles slots carry stale flag bits. No site reads flags
yet, so this is a no-op; it becomes load-bearing once collision dispatch
ships in the next commits.

EOF
)"
```

---

## Task 6: Implement `R_TraceParticle` helper

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (add a static helper near the top of the file, after the existing `void R_PushSmokeTube(...)` block — i.e. around line 506)

- [ ] **Step 1: Add the helper**

In `sdlquake/engine_src/r_part.c`, after the closing `}` of `R_PushSmokeTube` and before the opening of `R_AddSmokePuff`, insert:

```c
// Forward decl from world.c — keeps r_part.c off the full world.h include
// (which pulls server-side types). World-hull traceline only; no entity
// list, no contents-flag handling.
extern qboolean SV_RecursiveHullCheck (hull_t *hull, int num, float p1f, float p2f,
                                       vec3_t p1, vec3_t p2, trace_t *trace);

/*
===============
R_TraceParticle

Thin wrapper over SV_RecursiveHullCheck on cl.worldmodel->hulls[0]. Used
by R_DrawParticles to test whether a particle's per-frame integration step
crosses a world brush surface. World-only (no entity collision); returns 1
on hit, 0 on clean traversal or when no worldmodel is loaded.
===============
*/
static int R_TraceParticle (const vec3_t start, const vec3_t end, trace_t *trace)
{
	vec3_t p1, p2;
	if (!cl.worldmodel) return 0;

	memset (trace, 0, sizeof(*trace));
	trace->fraction = 1.0f;

	VectorCopy (start, p1);
	VectorCopy (end,   p2);

	SV_RecursiveHullCheck (&cl.worldmodel->hulls[0], 0, 0.0f, 1.0f, p1, p2, trace);
	return (trace->fraction < 1.0f) ? 1 : 0;
}
```

Note: `hull_t` and `trace_t` types are available because `r_part.c` already `#include`s `quakedef.h`, which transitively pulls `world.h`. If the build complains about either type, add `#include "world.h"` near the existing `#include "r_local.h"` at the top of the file.

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. The helper is unused; the compiler should not warn about that because it's `static` but only declared, not called — wait, `static` + unused will warn under `-Wunused-function`. The engine compile flags (`-std=gnu89 -fcommon -fno-sanitize=undefined`) don't add `-Wall`/`-Wunused-function`, so this should be silent. If a warning surfaces unexpectedly, leaving it is acceptable; Task 8 will use the helper and silence it.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): R_TraceParticle - world-hull traceline for collision"
```

---

## Task 7: Implement `R_SparkBurst`

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (insert new function near `R_BloodSpray`/`R_WaterSplash`, around line 717 or just above `R_TeleportSplash` at line 758)

- [ ] **Step 1: Add the spawn function**

In `sdlquake/engine_src/r_part.c`, after `R_BloodSpray` (around line 750) and before `R_TeleportSplash`, insert:

```c
/*
===============
R_SparkBurst

Hemispherical spark burst at `origin`, biased along `normal` (so sparks
fan away from the surface they spawn on). Used by TE_SPARKBURST (lightning
gun impacts). Each spark: pt_spark, PARTFL_BOUNCE | PARTFL_RAMP_HOLD,
cyan-white birth colour, lifetime ~0.8-1.2 s, speed 200-500 u/s.

count is multiplied by r_sparks_count_mul.value (clamped >= 0) so the
cvar gates spawns uniformly regardless of caller. Origin is nudged 1 unit
along normal so the first integration step doesn't trace-immediately-back
into the spawn surface.
===============
*/
void R_SparkBurst (vec3_t origin, vec3_t normal, int count)
{
	int		i, j, scaled_count;
	particle_t	*p;
	float		mul, speed, vlen;
	vec3_t		v, base;

	mul = r_sparks_count_mul.value;
	if (mul <= 0.0f) return;
	scaled_count = (int)(count * mul);
	if (scaled_count <= 0) return;

	// 1-unit spawn offset along the surface normal.
	base[0] = origin[0] + normal[0];
	base[1] = origin[1] + normal[1];
	base[2] = origin[2] + normal[2];

	for (i = 0; i < scaled_count; i++) {
		if (!free_particles) return;
		p = free_particles;
		free_particles = p->next;
		p->next = active_particles;
		active_particles = p;

		// Hemisphere-oriented direction: sample inside unit cube, reject
		// the obviously-bad zero vector, flip into +normal hemisphere.
		do {
			v[0] = ((rand() & 1023) - 512) * (1.0f / 512.0f);
			v[1] = ((rand() & 1023) - 512) * (1.0f / 512.0f);
			v[2] = ((rand() & 1023) - 512) * (1.0f / 512.0f);
			vlen = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
		} while (vlen < 0.01f);

		// Flip into the hemisphere defined by `normal`.
		if (v[0]*normal[0] + v[1]*normal[1] + v[2]*normal[2] < 0.0f) {
			v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2];
		}

		// Normalise then scale to a random speed in [200, 500].
		vlen = 1.0f / sqrtf (vlen);
		speed = 200.0f + (rand() & 255) * (300.0f / 256.0f);
		for (j = 0; j < 3; j++) p->vel[j] = v[j] * vlen * speed;

		VectorCopy (base, p->org);
		p->color = 244 + (rand() % 3);		// cyan-white core: 244..246
		p->ramp = 0;
		p->birth = cl.time;
		p->die = cl.time + 0.8f + (rand() & 31) * (0.4f / 31.0f);	// 0.8-1.2 s
		p->type = pt_spark;
		p->flags = PARTFL_BOUNCE | PARTFL_RAMP_HOLD;
	}
}
```

If the `sqrtf` symbol is unresolved, the file already `#include <math.h>` (see line 25); no extra include needed.

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): R_SparkBurst - hemisphere spark spawn for lightning impacts"
```

---

## Task 8: Add `pt_spark` to the type-switch in `R_DrawParticles`

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (the `switch (p->type)` inside `R_DrawParticles`, around line 1035)

- [ ] **Step 1: Add the new case**

In `sdlquake/engine_src/r_part.c`, inside `R_DrawParticles`'s type switch, immediately after the existing `case pt_static: case pt_smoke: break;` block (around line 1039), and before `case pt_fire:`, insert:

```c
		case pt_spark:
			if (p->flags & PARTFL_RAMP_HOLD) {
				// In-flight cyan-white flicker — no cool-down yet.
				p->color = 244 + (rand() % 3);
			} else if (!(p->flags & PARTFL_DWELL)) {
				p->ramp += time2;	// same advance rate as pt_explode
				if (p->ramp >= 8) {
					p->color = ramp1[7];		// 0x61 — dark ember
					p->flags |= PARTFL_DWELL;
					p->die = cl.time + r_sparks_settle_dwell.value;
				} else {
					p->color = ramp1[(int)p->ramp];
				}
			}
			if (!(p->flags & PARTFL_STUCK))
				p->vel[2] -= grav;	// light gravity (matches pt_slowgrav)
			break;
```

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. `pt_spark` is wired up to a colour ramp; nothing spawns one yet (R_SparkBurst exists but no caller invokes it).

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): pt_spark colour-ramp + dwell behaviour in R_DrawParticles"
```

---

## Task 9: Add collision dispatch to `R_DrawParticles`

This is the load-bearing behaviour change. The integration step (`p->org[N] += p->vel[N]*frametime;` around lines 1031–1033) is wrapped with a flag-gated traceline so that bouncing/sticking particles react to world geometry.

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (around lines 1031–1033 inside `R_DrawParticles`)

- [ ] **Step 1: Replace the three-line integration with the collision-aware version**

In `sdlquake/engine_src/r_part.c`, locate these three lines inside the per-particle loop in `R_DrawParticles`:

```c
		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;
```

Replace them with:

```c
		// Stuck particles freeze position and skip integration entirely.
		// They still tick through the type switch below so the ramp/dwell
		// timer (and p->die) progresses normally.
		if (p->flags & PARTFL_STUCK) {
			// no position update
		} else if (p->flags & (PARTFL_BOUNCE | PARTFL_STICK_ON_HIT)) {
			vec3_t newpos;
			trace_t tr;
			newpos[0] = p->org[0] + p->vel[0] * frametime;
			newpos[1] = p->org[1] + p->vel[1] * frametime;
			newpos[2] = p->org[2] + p->vel[2] * frametime;

			if (R_TraceParticle (p->org, newpos, &tr)) {
				vec3_t n;
				VectorCopy (tr.plane.normal, n);

				if ((p->flags & PARTFL_STICK_ON_HIT) ||
				    (p->flags & PARTFL_BOUNCED)) {
					// Stick: park at impact, freeze velocity, mark stuck.
					p->org[0] = tr.endpos[0] + n[0] * 0.5f;
					p->org[1] = tr.endpos[1] + n[1] * 0.5f;
					p->org[2] = tr.endpos[2] + n[2] * 0.5f;
					p->vel[0] = p->vel[1] = p->vel[2] = 0;
					p->flags |= PARTFL_STUCK;
				} else {
					// First bounce: inline reflection at r_sparks_restitution.
					float r = r_sparks_restitution.value;
					float d = p->vel[0]*n[0] + p->vel[1]*n[1] + p->vel[2]*n[2];
					p->vel[0] = (p->vel[0] - 2.0f * d * n[0]) * r;
					p->vel[1] = (p->vel[1] - 2.0f * d * n[1]) * r;
					p->vel[2] = (p->vel[2] - 2.0f * d * n[2]) * r;
					p->org[0] = tr.endpos[0] + n[0] * 0.5f;
					p->org[1] = tr.endpos[1] + n[1] * 0.5f;
					p->org[2] = tr.endpos[2] + n[2] * 0.5f;
					p->flags |= PARTFL_BOUNCED;
					// For pt_spark: cooldown ramp starts now from white-hot.
					if (p->type == pt_spark) {
						p->flags &= ~PARTFL_RAMP_HOLD;
						p->ramp = 0;
					}
				}
			} else {
				VectorCopy (newpos, p->org);
			}
		} else {
			// Non-collidable: original behaviour, unchanged.
			p->org[0] += p->vel[0]*frametime;
			p->org[1] += p->vel[1]*frametime;
			p->org[2] += p->vel[2]*frametime;
		}
```

Note: the existing wind-nudge block (lines ~995–1029) sits *above* this integration block and is unchanged. The post-integration type switch is also unchanged. The new collision dispatch runs strictly between them.

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. No spawn sites use `PARTFL_BOUNCE` or `PARTFL_STICK_ON_HIT` yet, so the new branch is dead code — every particle still falls through to the existing `else` arm.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "$(cat <<'EOF'
feat(particles): collision dispatch in R_DrawParticles

Particles with PARTFL_BOUNCE or PARTFL_STICK_ON_HIT traceline against
cl.worldmodel hull 0 each frame and either reflect (first hit, 0.5
restitution) or stick (subsequent hits, or any STICK_ON_HIT). Stuck
particles skip integration but still age out via the type switch.
Non-flagged particles take the original fall-through path.

EOF
)"
```

---

## Task 10: Set `PARTFL_STICK_ON_HIT` on blood-spray and water-splash droplets

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (`R_BloodSpray` ~line 729, `R_WaterSplash` ~line 671)

- [ ] **Step 1: Update `R_BloodSpray`**

In `sdlquake/engine_src/r_part.c`, inside the for-loop body of `R_BloodSpray`, find the line `p->flags = 0;` that Task 5 added (right after `active_particles = p;`). Change it to:

```c
		p->flags = PARTFL_STICK_ON_HIT;
```

- [ ] **Step 2: Update `R_WaterSplash`**

In the same file, inside the for-loop body of `R_WaterSplash`, make the same change: replace the `p->flags = 0;` line with:

```c
		p->flags = PARTFL_STICK_ON_HIT;
```

- [ ] **Step 3: Verify the build still compiles**

Run: `zig build`
Expected: success.

- [ ] **Step 4: In-game smoke test**

Run: `zig build run -- +map e1m1`
- Gib a grunt with the rocket launcher → blood spray droplets should now splat on walls/floor (visible red dots) instead of vanishing in mid-arc.
- Fire shotgun into water/slime/lava → splash droplets should sit on adjacent surfaces for ~1 s instead of disappearing immediately.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(fx): blood-spray + water-splash droplets stick on world contact"
```

---

## Task 11: Parse `TE_SPARKBURST` on the client

**Files:**
- Modify: `sdlquake/engine_src/cl_tent.c` (insert new `case` in the temp-entity switch, just after the existing `case TE_BLOODSPRAY:` block at line 299–312)

- [ ] **Step 1: Add the parser case**

In `sdlquake/engine_src/cl_tent.c`, after the closing `break;` of the existing `case TE_BLOODSPRAY:` block (around line 312) and before `case TE_TELEPORT:`, insert:

```c
	case TE_SPARKBURST:
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		{
			vec3_t normal;
			int count;
			normal[0] = MSG_ReadChar () * (1.0f / 127.0f);
			normal[1] = MSG_ReadChar () * (1.0f / 127.0f);
			normal[2] = MSG_ReadChar () * (1.0f / 127.0f);
			count = MSG_ReadByte ();
			R_SparkBurst (pos, normal, count);
		}
		break;
```

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. `pos` and `vec3_t` are already declared by the surrounding function (see the neighbouring `TE_BLOODSPRAY` case, line 299–312, which uses the same idiom).

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/cl_tent.c
git commit -m "feat(cl): parse TE_SPARKBURST and invoke R_SparkBurst"
```

---

## Task 12: Emit `TE_SPARKBURST` from the lightning gun

**Files:**
- Modify: `sdlquake/game/weapons.c` (lines 781–797 — the spark-shower block immediately after the `TE_LIGHTNING2` broadcast)

- [ ] **Step 1: Replace the spark-shower block**

In `sdlquake/game/weapons.c`, locate the block from the comment `// Spark shower at the bolt's visible endpoint.` through the `eng->SV_Particle(...)` call. The current text is:

```c
    // Spark shower at the bolt's visible endpoint. Palette 244-246 is
    // the cyan-white core of progs/bolt2.mdl (244=#7fbfff,245=#abe7ff,
    // 246=#d7ffff) — sparks match the bolt's own skin. R_RunParticleEffect
    // narrows the mask to 244-246 for this range; ±150 per-particle
    // velocity jitter gives spiky random directions; the small
    // normal-aligned base velocity biases the burst away from the wall.
    vec3_t spark_vel = {
        g->trace_plane_normal[0] * 30,
        g->trace_plane_normal[1] * 30,
        g->trace_plane_normal[2] * 30 + 20
    };
    eng->SV_Particle(g->trace_endpos, spark_vel, 245, 60);
```

Replace it with:

```c
    // Spark shower at the bolt's visible endpoint. TE_SPARKBURST carries
    // origin + surface normal + count to the client, which spawns 20
    // cyan-white pt_spark particles in the hemisphere oriented by the
    // normal. They bounce once (r_sparks_restitution), stick on the second
    // hit, ramp through ramp1[] (white -> yellow -> orange -> red -> dark),
    // and linger as embers for r_sparks_settle_dwell seconds.
    eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte (MSG_BROADCAST, TE_SPARKBURST);
    eng->MSG_WriteCoord (MSG_BROADCAST, g->trace_endpos[0]);
    eng->MSG_WriteCoord (MSG_BROADCAST, g->trace_endpos[1]);
    eng->MSG_WriteCoord (MSG_BROADCAST, g->trace_endpos[2]);
    eng->MSG_WriteChar (MSG_BROADCAST, (int)(g->trace_plane_normal[0] * 127.0f));
    eng->MSG_WriteChar (MSG_BROADCAST, (int)(g->trace_plane_normal[1] * 127.0f));
    eng->MSG_WriteChar (MSG_BROADCAST, (int)(g->trace_plane_normal[2] * 127.0f));
    eng->MSG_WriteByte (MSG_BROADCAST, 20);
```

- [ ] **Step 2: Verify the build still compiles**

Run: `zig build`
Expected: success. The game DLL (`zig-out/bin/game.dll`) rebuilds. `TE_SPARKBURST` resolves through `game_defs.h` (added in Task 2), which is how the surrounding `TE_LIGHTNING2` reference already works (see weapons.c:772).

- [ ] **Step 3: Commit**

```bash
git add sdlquake/game/weapons.c
git commit -m "$(cat <<'EOF'
feat(weapons): lightning gun emits TE_SPARKBURST instead of SV_Particle

Replaces the 60-particle cyan puff with a 20-spark hemispherical burst
that bounces, sticks, ramps colour, and lingers. spark_vel is no longer
needed since direction is encoded as the surface normal on the wire.

EOF
)"
```

---

## Task 13: Full playtest

**Files:** none — verification only.

- [ ] **Step 1: Clean build**

Run: `zig build`
Expected: success.

- [ ] **Step 2: Lightning-gun at a flat wall**

Run: `zig build run -- +map e1m1`
- Grab the lightning gun (`give g`, `give 9`, or whatever cheat path is wired up) and pour the bolt into a wall.
- Expected: a cyan-white burst at the impact, then a sparse shower of warm-coloured sparks bouncing once off the wall, arcing down to the floor, settling as dark dots that fade after ~0.5 s.
- No sparks clipping into the wall, no sparks visible inside geometry.

- [ ] **Step 3: Lightning-gun into a corner**

- Aim at an inside corner and fire.
- Expected: sparks bounce out into open space, never trapped inside the corner.

- [ ] **Step 4: Lightning-gun straight down**

- Look at the floor and fire.
- Expected: sparks fan upward into the room, fall back down, stick on the floor near the impact.

- [ ] **Step 5: Liquid splashes**

- Shoot the shotgun into a body of water (e.g. e1m1's start pool).
- Expected: splash droplets land on adjacent surfaces and sit ~1 s before vanishing. No droplets fall through the floor.

- [ ] **Step 6: Gib spray**

- Rocket-launcher a grunt.
- Expected: blood-spray droplets splat on walls/floor instead of evaporating in flight.

- [ ] **Step 7: cvar smoke-tests**

In console:
- `r_sparks_count_mul 0` → fire lightning again → no sparks at all.
- `r_sparks_count_mul 2` → fire lightning again → roughly twice as many sparks per impact.
- `r_sparks_count_mul 1` (restore) → `r_sparks_restitution 0` → sparks should stick on first contact (no bounce visible).
- `r_sparks_restitution 0.5` (restore) → `r_sparks_settle_dwell 3` → fire lightning, watch the dark embers linger for ~3 s on the floor.

- [ ] **Step 8: Loading-screen safety**

Quickload or `map e1m1` while sparks are still in flight. Expected: no crash; sparks just disappear with the level transition. (`R_TraceParticle` guards `cl.worldmodel == NULL` for this case.)

- [ ] **Step 9: No regressions in other particle effects**

- Rocket explosion → unchanged.
- Rocket trail → unchanged.
- Teleporter → unchanged.
- Smoke grenade → unchanged.

If any regression appears, the most likely cause is a missing `p->flags = 0;` in Task 5 — revisit that audit and add the line at the offending spawn site.

- [ ] **Step 10: Commit (no code change) — tag the milestone**

If desired, write a chore commit documenting the playtest:

```bash
git commit --allow-empty -m "chore: playtest bouncing-particles + lightning sparks"
```

(Optional. Skip if the previous 12 commits feel sufficient.)

---

## Done criteria

- `zig build` succeeds at every task boundary.
- Lightning gun produces a visible spark shower that bounces once and settles on the floor with a dark-ember dwell.
- Blood-spray and water-splash droplets stick on world contact.
- All non-collidable particle effects (smoke, rocket trail, explosion, teleport, tracer, etc.) look byte-identical to pre-change behaviour.
- Three new cvars (`r_sparks_count_mul`, `r_sparks_settle_dwell`, `r_sparks_restitution`) are runtime-settable and gate the behaviour as documented.

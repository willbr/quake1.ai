# Wall-Slide Drop-Off Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sliding blood/water droplets drop off the bottom edge of a wall and fall under gravity instead of creeping into open air.

**Architecture:** At stick time, binary-search the wall's vertical extent (~7 sideways probes), store `wall_bottom_z` in `p->vel[2]`. The per-frame slide step becomes a single Z compare — no trace. When the droplet's Z reaches `wall_bottom_z`, run one disambiguation trace: if a surface is immediately below → snap & stop (current floor-stop behavior); else → release `STUCK | WALL_STICK`, zero velocity, let the existing per-type gravity in the type switch take over. The existing `PARTFL_STICK_ON_HIT` flag remains set so the falling droplet re-sticks on landing.

**Tech Stack:** C (forked WinQuake software renderer), Zig build.

**Spec:** `docs/superpowers/specs/2026-05-23-particle-wall-slide-dropoff-design.md`

**Notes for the engineer:**

- All work is in `sdlquake/engine_src/r_part.c`. Engine code requires `zig build run` (no hot-reload).
- No test framework — verification is `zig build` clean + manual playtest at the end.
- The engine compiles with `-std=gnu89 -fcommon` (see `CLAUDE.md`). Avoid C99 features like compound literals (`(vec3_t){...}`) and designated initialisers in struct literals. Declare locals at the top of blocks.
- Ignore LSP diagnostics complaining about `game_api.h not found`, `vec3_t undeclared`, etc. — those are pre-existing noise from the LSP not seeing the engine's include order. `zig build` is the source of truth.
- The current slide step lives at `r_part.c:1839–1865`; the stick branch is at `r_part.c:1653–1665`. These will both be modified.

---

### Task 1: Add `R_FindWallBottom` helper

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` — add a new `static` helper near the existing `R_TraceParticle` definition (around line 698).

- [ ] **Step 1: Add the helper**

In `sdlquake/engine_src/r_part.c`, locate the `R_TraceParticle` function definition (currently `static int R_TraceParticle (const vec3_t start, const vec3_t end, trace_t *trace)` around line 698). Find the end of that function (its closing `}`). Immediately below it, add:

```c
/*
================
R_FindWallBottom

Binary-search the vertical extent of the wall the particle just stuck to.
Returns the lowest Z where the wall surface is still present (the Z just above
the wall's bottom edge).  At stick time the wall is known present at impact[2];
the search probes downward by 256 u, then bisects to ~2 u precision.

`n` is the contact normal (points away from the wall). The probe is a 1-unit
trace through the wall plane at a given Z: HIT means the wall is there, MISS
means it's gone (air below the wall's bottom, a hole, etc.).
================
*/
static float R_FindWallBottom (const vec3_t impact, const vec3_t n)
{
	trace_t	tr;
	vec3_t	a, b;
	float	z_hit  = impact[2];
	float	z_miss = impact[2] - 256.0f;

	// Probe the search cap first.  If the wall is still there, the wall
	// extends past our search range — return the cap and let the slide
	// either reach it (rare: 64+ s at default 4 u/s, longer than blood
	// lifetime) or hit the floor first via R_SlideRelease's disambig.
	a[0] = impact[0] + n[0] * 0.5f;
	a[1] = impact[1] + n[1] * 0.5f;
	a[2] = z_miss;
	b[0] = impact[0] - n[0] * 0.5f;
	b[1] = impact[1] - n[1] * 0.5f;
	b[2] = z_miss;
	if (R_TraceParticle (a, b, &tr))
		return z_miss;

	// Bisect.  Invariant: probe(z_hit) HITs, probe(z_miss) MISSes.
	while (z_hit - z_miss > 2.0f) {
		float z_mid = (z_hit + z_miss) * 0.5f;
		a[2] = z_mid;
		b[2] = z_mid;
		if (R_TraceParticle (a, b, &tr))
			z_hit = z_mid;
		else
			z_miss = z_mid;
	}
	return z_hit;
}
```

- [ ] **Step 2: Verify build**

Run: `zig build`
Expected: clean build. The helper is declared `static` and unused at this point; the compiler may emit an unused-function warning, but the engine builds with `-Wno-unused-function`-style tolerance via the existing flags (if a warning appears, that's OK — Task 2 will use it).

If you see a "declared but not used" error (not warning), let me know — we'll suppress with `(void)R_FindWallBottom;` or move the function definition to after its first caller.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): add R_FindWallBottom binary-search helper"
```

---

### Task 2: Call `R_FindWallBottom` at the stick site; store `wall_bottom_z` in `p->vel[2]`

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` around line 1660 (the stick branch's `WALL_STICK` classifier added in the previous feature).

- [ ] **Step 1: Replace the WALL_STICK setter with a search-and-store block**

Locate this code in `R_DrawParticles` (currently `r_part.c:1660–1665`):

```c
					p->flags |= PARTFL_STUCK;
					// Wall-ish contact (|n.z|<0.7) → also flag for the
					// slide step below so blood/water droplets droop
					// downward instead of freezing flat on the wall.
					if (fabs(n[2]) < 0.7f)
						p->flags |= PARTFL_WALL_STICK;
```

Replace with:

```c
					p->flags |= PARTFL_STUCK;
					// Wall-ish contact (|n.z|<0.7) → also flag for the
					// slide step below so blood/water droplets droop
					// downward instead of freezing flat on the wall.
					// Binary-search the wall's bottom Z once and stash
					// it in vel[2] so the per-frame slide step is a
					// cheap compare instead of a trace.
					if (fabs(n[2]) < 0.7f) {
						p->flags |= PARTFL_WALL_STICK;
						p->vel[2] = R_FindWallBottom (p->org, n);
					}
```

Note: `p->vel[0]`, `p->vel[1]`, and `p->vel[2]` were all zeroed on the line just above (`p->vel[0] = p->vel[1] = p->vel[2] = 0;`). We're now overwriting `p->vel[2]` with the wall-bottom Z. `vel[0]` and `vel[1]` stay 0 (unused while WALL_STICK is set).

- [ ] **Step 2: Verify build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): stash wall_bottom_z in p->vel[2] at stick time"
```

---

### Task 3: Rewrite the slide step + add `R_SlideRelease` helper

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` — add `R_SlideRelease` helper near `R_FindWallBottom` (immediately below it); rewrite the slide step at line ~1839.

- [ ] **Step 1: Add `R_SlideRelease` helper**

Immediately below `R_FindWallBottom` (added in Task 1), add:

```c
/*
================
R_SlideRelease

Called when a sliding droplet's Z reaches its stashed wall_bottom_z.  One
short downward trace decides what happened:

  - Hit a floor-ish surface immediately below (n.z >= 0.7): the wall ends
    at the floor; snap and stop (same visual as today's floor-stop).
  - Hit a non-floor surface immediately below: the wall ends at the start
    of another wall/ceiling; snap and stop.
  - Trace clear (open air below): the wall ends in open air (doorway top,
    ledge, window).  Clear STUCK | WALL_STICK and zero vel — the per-type
    gravity in R_DrawParticles' type switch will accelerate the droplet
    downward.  PARTFL_STICK_ON_HIT is still set on the particle, so the
    next collision re-runs the existing stick branch and the droplet
    re-sticks on whatever it lands on.
================
*/
static void R_SlideRelease (particle_t *p)
{
	trace_t	tr;
	vec3_t	below;

	below[0] = p->org[0];
	below[1] = p->org[1];
	below[2] = p->org[2] - 4.0f;

	if (R_TraceParticle (p->org, below, &tr)) {
		// Surface immediately below — snap and stop sliding.
		p->org[0] = tr.endpos[0] + tr.plane.normal[0] * 0.5f;
		p->org[1] = tr.endpos[1] + tr.plane.normal[1] * 0.5f;
		p->org[2] = tr.endpos[2] + tr.plane.normal[2] * 0.5f;
		p->vel[0] = p->vel[1] = p->vel[2] = 0;
		p->flags &= ~PARTFL_WALL_STICK;
		// STUCK stays set: droplet is at rest on the new surface.
	} else {
		// Open air below — release for free-fall.
		p->vel[0] = p->vel[1] = p->vel[2] = 0;
		p->flags &= ~(PARTFL_STUCK | PARTFL_WALL_STICK);
	}
}
```

- [ ] **Step 2: Rewrite the slide step**

Replace the existing slide step (currently `r_part.c:1839–1865`):

```c
		// Sliding wall droplets: blood/water that stuck to a wall
		// (|n.z|<0.7 at stick time) creeps downward at the cvar's
		// speed.  Straight-down step keeps a vertical wall's
		// normal-offset (0.5u) unchanged.  A short trace catches
		// floors and ledges so the droplet stops cleanly instead
		// of tunnelling.
		if ((p->flags & (PARTFL_STUCK | PARTFL_WALL_STICK))
		    == (PARTFL_STUCK | PARTFL_WALL_STICK)) {
			float slide = r_particle_slide_speed.value;
			if (slide < 0.0f) slide = 0.0f;
			else if (slide > 32.0f) slide = 32.0f;
			float dz = slide * frametime;
			if (dz > 0.0f) {
				vec3_t newpos = { p->org[0], p->org[1], p->org[2] - dz };
				trace_t tr;
				if (R_TraceParticle (p->org, newpos, &tr)) {
					// Hit something on the way down — snap to
					// contact and stop sliding.
					p->org[0] = tr.endpos[0] + tr.plane.normal[0] * 0.5f;
					p->org[1] = tr.endpos[1] + tr.plane.normal[1] * 0.5f;
					p->org[2] = tr.endpos[2] + tr.plane.normal[2] * 0.5f;
					p->flags &= ~PARTFL_WALL_STICK;
				} else {
					p->org[2] = newpos[2];
				}
			}
		}
```

with the new (zero-trace) version:

```c
		// Sliding wall droplets: blood/water flagged WALL_STICK at
		// stick time creep downward at the cvar's speed.  vel[2]
		// holds the wall's bottom Z (found via binary search at
		// stick time), so this is a cheap compare per frame.  When
		// the droplet reaches the bottom, R_SlideRelease decides
		// whether to snap to a floor or free-fall into open air.
		if ((p->flags & (PARTFL_STUCK | PARTFL_WALL_STICK))
		    == (PARTFL_STUCK | PARTFL_WALL_STICK)) {
			float slide = r_particle_slide_speed.value;
			if (slide < 0.0f) slide = 0.0f;
			else if (slide > 32.0f) slide = 32.0f;
			float dz = slide * frametime;
			if (dz > 0.0f) {
				float new_z = p->org[2] - dz;
				if (new_z <= p->vel[2]) {
					R_SlideRelease (p);
				} else {
					p->org[2] = new_z;
				}
			}
		}
```

- [ ] **Step 3: Verify build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): drop off wall when slide reaches wall_bottom_z"
```

---

### Task 4: Manual verification

**Files:** none modified.

- [ ] **Step 1: Drop-off into a doorway**

Run: `zig build run -- +map e1m1`

In-game:
1. Walk to any doorway with a wall above it (most doorways in e1m1 qualify — the start hall, the slipgate room, etc.).
2. Stand back from the doorway and fire shotgun blasts at the wall above it.
3. Watch a blood droplet for 10–20 seconds. Expected: droplet creeps down, reaches the top edge of the doorway opening, then falls into the doorway under gravity (visibly accelerating), and re-sticks to the floor inside the doorway.

- [ ] **Step 2: Regression — wall flush with floor**

Still in-game:
1. Find a wall section that goes flush to the floor (no doorway underneath).
2. Splatter blood on it and watch droplets slide.
3. Expected: same behavior as before — droplets slide to the floor and stop. No visible difference from the previous version.

- [ ] **Step 3: Water splash drop-off**

Still in-game:
1. Find a pool with a wall above it. The slipgate room in e1m1 has one.
2. Aim hitscan/projectile so its splash hits a wall above the pool, in a spot where the wall has a bottom edge above the water.
3. Expected: splash droplets slide down the wall, reach the wall's bottom edge, then fall into the pool. Once in the pool they should settle on the water surface (existing `PARTFL_LIQUID_SURF` behavior, which fires from the integration block once STUCK is cleared).

- [ ] **Step 4: Fast slide regression**

Console: `r_particle_slide_speed 16`
Splatter fresh blood above a doorway.
Expected: droplet slides ~4× faster, releases at the doorway top correctly. Does not tunnel through the floor or skip the disambiguation.

Console: `r_particle_slide_speed 4`
Restore default.

- [ ] **Step 5: Disabled cvar regression**

Console: `r_particle_slide_speed 0`
Splatter fresh blood on a wall.
Expected: droplets freeze in place (slide step's `if (dz > 0.0f)` short-circuits; binary search still runs at stick time, but is harmless when no sliding occurs).

- [ ] **Step 6: Report results**

If all five visual checks pass, the feature is done. If any check fails, capture the cvar value, the map, the surface, and what you saw, and stop — surface the failure mode for review.

---

## Self-review notes

- Spec coverage: binary search at stick (Task 1+2), zero-trace slide step (Task 3), disambiguation release with three cases — floor, non-floor, open air (Task 3 in `R_SlideRelease`). ✓
- No placeholders. ✓
- Function names consistent: `R_FindWallBottom`, `R_SlideRelease`, `R_TraceParticle`. ✓
- Storage layout matches spec: `vel[0]=0`, `vel[1]=0`, `vel[2]=wall_bottom_z` while STUCK|WALL_STICK. ✓
- No new flags, no new cvars, no ABI changes. ✓
- One subtle correctness check: at release-time with "open air below," we don't reposition `p->org` — the droplet free-falls from where the slide left it (just above the wall's bottom edge). The next integration tick sees vel=0 (zeroed by release) plus `vel[2] -= grav * 20` (added by the type switch), then traces and re-sticks if it lands on anything. ✓

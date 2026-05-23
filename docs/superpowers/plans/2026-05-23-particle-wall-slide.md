# Particle Wall Slide Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wall-stuck blood splatter and wall-stuck water splash droplets slowly creep downward instead of freezing in place.

**Architecture:** Add a new `PARTFL_WALL_STICK` flag set when a `PARTFL_STICK_ON_HIT` particle is parked against a vertical surface (`|n.z| < 0.7`). Each frame, particles with both `PARTFL_STUCK | PARTFL_WALL_STICK` step downward at `r_particle_slide_speed` units/sec, gated by an `R_TraceParticle` call so they stop cleanly when they hit a floor or ledge.

**Tech Stack:** C (forked WinQuake software renderer), Zig build system.

**Spec:** `docs/superpowers/specs/2026-05-23-particle-wall-slide-design.md`

**Notes for the engineer:**

- All work is in the engine (`sdlquake/engine_src/`), not in `game.dll`. Engine changes require a full rebuild (`zig build run`), not a hot-reload.
- There is no automated test suite for this repo. "Verification" is `zig build` succeeding without warnings + a manual in-game playtest at the end of the plan. Build success and visual correctness are the only signals.
- Follow the existing comment style of `r_part.c` — short explanatory blocks above non-obvious blocks of code, no decorative banners.
- Read `docs/superpowers/specs/2026-05-23-particle-wall-slide-design.md` once before starting so the threshold values and cvar name match exactly.

---

### Task 1: Add `PARTFL_WALL_STICK` flag

**Files:**
- Modify: `sdlquake/engine_src/d_iface.h`

- [ ] **Step 1: Add the flag**

In `sdlquake/engine_src/d_iface.h`, locate the `PARTFL_*` block (currently ending at line 50 with `PARTFL_LIQUID_SURF`). Add immediately below it:

```c
#define PARTFL_WALL_STICK	0x80	// stuck on a wall-ish surface (|n.z|<0.7); slides downward each frame at r_particle_slide_speed
```

- [ ] **Step 2: Verify build**

Run: `zig build`
Expected: clean build, no warnings introduced by the flag definition. (The flag is declared but not yet used; that's fine.)

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/d_iface.h
git commit -m "feat(particles): add PARTFL_WALL_STICK flag"
```

---

### Task 2: Define and register the `r_particle_slide_speed` cvar

**Files:**
- Modify: `sdlquake/engine_src/r_main.c`

- [ ] **Step 1: Define the cvar**

In `sdlquake/engine_src/r_main.c`, in the particle-cvar definitions block (currently ends around line 164 with `r_sparks_restitution`), add immediately below `r_sparks_restitution`:

```c
cvar_t	r_particle_slide_speed = {"r_particle_slide_speed", "4"};	// units/sec that wall-stuck blood and water droplets creep downward; 0 disables sliding
```

- [ ] **Step 2: Register the cvar**

Still in `r_main.c`, in `R_Init` find the registration block (currently ends around line 271 with `Cvar_RegisterVariable (&r_sparks_restitution);`). Add immediately below that line:

```c
	Cvar_RegisterVariable (&r_particle_slide_speed);
```

- [ ] **Step 3: Verify build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Smoke-check the cvar at runtime**

Run: `zig build run -- +map e1m1`
At the console, type: `r_particle_slide_speed`
Expected: prints `"r_particle_slide_speed" is "4"`. Quit.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_main.c
git commit -m "feat(particles): add r_particle_slide_speed cvar (default 4 u/s)"
```

---

### Task 3: Set `PARTFL_WALL_STICK` at the stick site

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` around line 1650

- [ ] **Step 1: Add the wall classifier at the stick branch**

In `R_DrawParticles`, locate the stick branch (currently `r_part.c:1650`):

```c
				if ((p->flags & PARTFL_STICK_ON_HIT) ||
				    (p->flags & PARTFL_BOUNCED)) {
					// Stick: park at impact, freeze velocity, mark stuck.
					p->org[0] = tr.endpos[0] + n[0] * 0.5f;
					p->org[1] = tr.endpos[1] + n[1] * 0.5f;
					p->org[2] = tr.endpos[2] + n[2] * 0.5f;
					p->vel[0] = p->vel[1] = p->vel[2] = 0;
					p->flags |= PARTFL_STUCK;
				} else {
```

Modify it to set `PARTFL_WALL_STICK` when the surface is wall-ish:

```c
				if ((p->flags & PARTFL_STICK_ON_HIT) ||
				    (p->flags & PARTFL_BOUNCED)) {
					// Stick: park at impact, freeze velocity, mark stuck.
					p->org[0] = tr.endpos[0] + n[0] * 0.5f;
					p->org[1] = tr.endpos[1] + n[1] * 0.5f;
					p->org[2] = tr.endpos[2] + n[2] * 0.5f;
					p->vel[0] = p->vel[1] = p->vel[2] = 0;
					p->flags |= PARTFL_STUCK;
					// Wall-ish contact (|n.z|<0.7) → also flag for the
					// slide step below so blood/water droplets droop
					// downward instead of freezing flat on the wall.
					if (fabs(n[2]) < 0.7f)
						p->flags |= PARTFL_WALL_STICK;
				} else {
```

- [ ] **Step 2: Verify build**

Run: `zig build`
Expected: clean build. (`fabs` is already in scope — `<math.h>` is pulled in by `quakedef.h`.)

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): flag wall-stuck droplets with PARTFL_WALL_STICK"
```

---

### Task 4: Add the slide step in `R_DrawParticles`

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (declare extern cvar near top; add slide step after type switch)

- [ ] **Step 1: Make the cvar visible to r_part.c**

Near the top of `r_part.c`, in the `extern cvar_t` block (currently includes `r_sparks_settle_dwell` and `r_sparks_restitution` around line 42), add:

```c
extern cvar_t r_particle_slide_speed;
```

- [ ] **Step 2: Add the slide step**

Find the end of the per-particle `switch (p->type)` block in `R_DrawParticles`. It ends with the closing brace of the switch, followed immediately by the loop tail (`}` of the for-each-particle body). Just **before** that closing switch brace's matching loop-body close — i.e., after the switch ends but inside the per-particle loop — insert the slide step.

To locate it precisely: the switch starts at `r_part.c:1695` (`switch (p->type)`). Find its matching close brace, then add this code immediately after that close brace, before the next `}` (which ends the per-particle body):

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

- [ ] **Step 3: Verify build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "feat(particles): wall-stuck blood and water droplets slide down"
```

---

### Task 5: Manual verification

**Files:** none modified.

- [ ] **Step 1: Launch and verify blood sliding**

Run: `zig build run -- +map e1m1`

In-game:
1. Open the console (`~`) and run `r_particle_slide_speed` — expected `"4"`.
2. Find a vertical wall (the start hall has plenty).
3. Fire several shotgun blasts at the wall to splatter blood, or `impulse 9` and use the rocket launcher to gib something against a wall.
4. Stand still and watch the blood droplets for 5–10 seconds. Expected: droplets visibly droop downward; the splatter pattern slowly elongates instead of staying frozen.

- [ ] **Step 2: Verify water sliding**

Still in-game:
1. Find a pool with a vertical wall above it (the slipgate room in e1m1 works).
2. Aim the lightning gun (or any hitscan/projectile) so its impact splashes water onto the wall above the pool.
3. Watch the wall-stuck splash droplets — they should slide downward at the same speed as blood.

- [ ] **Step 3: Verify floor-stuck droplets do NOT slide**

Still in-game:
1. Splatter blood on a flat floor (fire a rocket at the floor).
2. Watch the resulting floor blood — it should remain stationary (the wall classifier should have left `PARTFL_WALL_STICK` clear).

- [ ] **Step 4: Verify cvar disables cleanly**

Console: `r_particle_slide_speed 0`
Splatter fresh blood on a wall. Expected: droplets freeze in place (regression baseline behaviour).

Console: `r_particle_slide_speed 16`
Splatter fresh blood on a wall. Expected: droplets visibly creep ~4× faster than the default.

Console: `r_particle_slide_speed 4`
Restore default.

- [ ] **Step 5: Verify no geometry tunnelling**

Splatter blood on a wall directly above a floor (low height — pick a spot where droplets will reach the floor within their ~8–32s lifetime).
Watch the droplets slide all the way to the floor. Expected: droplets stop cleanly at the floor contact (they should not visibly intrude into the floor surface or disappear into the brush).

- [ ] **Step 6: Report results**

If all five visual checks pass, the feature is done. If any check fails, capture the cvar value and a description of what you saw, and stop before committing further fixes — surface the failure mode for review.

---

## Self-review notes

- Spec coverage: every requirement in the spec (`PARTFL_WALL_STICK`, wall-classifier threshold `|n.z|<0.7`, cvar name + default + clamp range, straight-down slide, trace-gated stop on contact, files touched) has a task. ✓
- No placeholders, no TBDs. ✓
- Naming consistent: `PARTFL_WALL_STICK`, `r_particle_slide_speed`, `R_TraceParticle` are used identically everywhere they appear. ✓
- No ABI changes — `game.dll` ABI version stays at 21. ✓

# Wind-Reactive Particles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Couple engine particles (smoke, fire, embers, explosion shards, etc.) to the Phase 8 M4 voxel wind grid so they visibly drift in steady wind, get pushed by player Gust impulses, and settle as gusts decay. Stock maps with no wind sources must remain byte-identical to today.

**Architecture:** New `game_api_t` entry `Wind_SampleVelocity` (engine calls into the DLL). Engine particle loop samples wind at each particle's position and lerps the particle velocity toward the sampled wind velocity with a per-type drag coefficient and framerate-independent rate. `1 - exp(-k*dt)` form. Skip the nudge when |wind| < 1 u/s so still-air behaviour is unchanged.

**Tech Stack:** C (gnu89 for engine, modern C for game.dll); Zig build system; Quake's existing software-renderer particle pipeline; the M4 wind voxel grid in `sdlquake/game/sim/sim_wind.c`; `DebugLines_Add` for debug overlay.

**Spec:** `docs/superpowers/specs/2026-05-22-wind-reactive-particles-design.md`

**Note on verification:** This project has no test suite. CLAUDE.md is explicit: "Build success and visual/audio correctness in-game are the verification methods." Each task therefore verifies via `zig build run` + in-engine observation, not unit tests.

---

## File map

| File | Action | Responsibility |
|---|---|---|
| `sdlquake/game/game_api.h` | modify | Add `Wind_SampleVelocity` slot to `game_api_t`. Bump `GAME_API_VERSION` 22→23. |
| `sdlquake/game/sim/sim.h` | modify | Declare `void Wind_SampleVelocity(const vec3_t pos, vec3_t out_vel);` |
| `sdlquake/game/sim/sim_wind.c` | modify | Implement `Wind_SampleVelocity` (trilerp over 8 neighbouring cells reading `vx/vy/vz`). |
| `sdlquake/game/game_main.c` | modify | Append `Wind_SampleVelocity` to the `s_api` positional initialiser. |
| `sdlquake/engine_src/r_part.c` | modify | Per-type `wind_drag_k[]` table; wind-nudge insertion in `R_DrawParticles` loop; debug line emission. |
| `sdlquake/engine_src/r_main.c` | modify | Declare + register three new cvars (`r_particle_wind_scale`, `r_particle_wind_debug`, `r_particle_wind_disable`). |

---

## Task 1: ABI — declare and implement `Wind_SampleVelocity`

**Files:**
- Modify: `sdlquake/game/game_api.h:7` (version bump) and `:200` (append slot)
- Modify: `sdlquake/game/sim/sim.h` (forward declaration)
- Modify: `sdlquake/game/sim/sim_wind.c` (implementation)
- Modify: `sdlquake/game/game_main.c:90-114` (append to `s_api`)

- [ ] **Step 1: Bump GAME_API_VERSION**

Edit `sdlquake/game/game_api.h` line 7:

```c
#define GAME_API_VERSION 23
```

(was `22`).

- [ ] **Step 2: Add `Wind_SampleVelocity` slot to `game_api_t`**

In `sdlquake/game/game_api.h`, append a new function-pointer field at the very end of the `game_api_t` struct (just before the closing `}`), after `game_mcp_damage` / `Doors_OpenAllSecret` block, immediately above the closing brace of `typedef struct game_api_s { ... } game_api_t;`:

```c
    // Sample the wind voxel grid at a world position. Writes wind
    // velocity (units/sec) into out_vel. Outside the grid bounds or
    // before the grid is initialised, writes (0,0,0). Implemented in
    // sim_wind.c. Used by r_part.c to drift visual particles.
    void  (*Wind_SampleVelocity)(const vec3_t pos, vec3_t out_vel);
```

- [ ] **Step 3: Declare the function in `sim.h`**

Find the existing block in `sdlquake/game/sim/sim.h` that declares `Wind_GetSmokeAt`, `Wind_PathOcclusion`, `Wind_AddImpulse`, etc. (around line 140-145, alongside `Wind_DebugDraw`). Add:

```c
void  Wind_SampleVelocity(const vec3_t pos, vec3_t out_vel);
```

- [ ] **Step 4: Implement `Wind_SampleVelocity` in `sim_wind.c`**

Open `sdlquake/game/sim/sim_wind.c`. Just after the existing `Wind_GetSmokeAt` (around line 325), add the trilerp velocity sampler. (Unlike `Wind_GetSmokeAt`, which is nearest-cell, this one trilerps because visible particle drift between cells must be smooth.)

```c
void Wind_SampleVelocity(const vec3_t pos, vec3_t out_vel) {
    out_vel[0] = out_vel[1] = out_vel[2] = 0;
    if (!s_ready) return;

    // Continuous cell-space coordinate (cell-corner-centered).
    float fx = (pos[0] - s_origin[0]) / s_cell_size;
    float fy = (pos[1] - s_origin[1]) / s_cell_size;
    float fz = (pos[2] - s_origin[2]) / s_cell_size;

    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    int z0 = (int)floorf(fz);
    float tx = fx - x0;
    float ty = fy - y0;
    float tz = fz - z0;

    // Trilerp across the 8 neighbours. Any out-of-bounds neighbour
    // contributes zero, so edges fade naturally to no-wind.
    for (int dz = 0; dz < 2; dz++)
    for (int dy = 0; dy < 2; dy++)
    for (int dx = 0; dx < 2; dx++) {
        int i = idx_or_neg(x0 + dx, y0 + dy, z0 + dz);
        if (i < 0) continue;
        float wx = (dx ? tx : 1.0f - tx);
        float wy = (dy ? ty : 1.0f - ty);
        float wz = (dz ? tz : 1.0f - tz);
        float w  = wx * wy * wz;
        out_vel[0] += s_cells[i].vx * w;
        out_vel[1] += s_cells[i].vy * w;
        out_vel[2] += s_cells[i].vz * w;
    }
}
```

(Use whatever names the file already uses for `vx/vy/vz` — verify by reading the `wind_cell_t` struct at `sim_wind.c:52`; the existing impulse code at `sim_wind.c:280` confirms the names are `vx`, `vy`, `vz`.)

- [ ] **Step 5: Wire into `s_api`**

Edit `sdlquake/game/game_main.c`. The `s_api` positional initialiser at line 90 ends with `game_mcp_damage` followed by `};`. Append a comma after `game_mcp_damage,` and add a new line:

```c
    game_mcp_damage,
    Wind_SampleVelocity,
};
```

Confirm `sim.h` (or `sim/sim.h`) is included near the top of `game_main.c` so the symbol is visible. If not, add `#include "sim/sim.h"` next to the other game-side includes.

- [ ] **Step 6: Build**

Run: `zig build` from the repo root.
Expected: clean build, no warnings about missing function or struct-init mismatch.

- [ ] **Step 7: Run sanity check**

Run: `zig build run -- +map start`
Expected: game starts normally; no `hotreload: ABI version mismatch` line in the console. Particles look exactly the same as before — Task 1 added the API only, no engine code calls it yet.

- [ ] **Step 8: Commit**

```bash
git add sdlquake/game/game_api.h sdlquake/game/sim/sim.h sdlquake/game/sim/sim_wind.c sdlquake/game/game_main.c
git commit -m "$(cat <<'EOF'
feat(sim): Wind_SampleVelocity engine API (ABI 22->23)

Trilerp sampler over the M4 voxel wind grid; will be called from
r_part.c to drift visual particles. Not wired into the particle loop
yet — that's the next commit.
EOF
)"
```

---

## Task 2: Engine-side cvars

**Files:**
- Modify: `sdlquake/engine_src/r_main.c:141-143` area (declare new cvars next to existing `r_drawflat`, `r_lightmap_dither`) and `:228-237` area (register).

Convention check: `r_part.c` currently declares zero cvars of its own — all renderer cvars (`r_drawflat`, `r_lightmap_dither`, `r_waterwarp`, etc.) are declared *and* registered in `r_main.c`. The new cvars follow that convention.

- [ ] **Step 1: Declare cvars in `r_main.c`**

In `sdlquake/engine_src/r_main.c`, near line 141-143 (just below `cvar_t r_drawflat = {...};` and `cvar_t r_lightmap_dither = {...};`), add:

```c
cvar_t	r_particle_wind_scale   = {"r_particle_wind_scale",   "1"};
cvar_t	r_particle_wind_disable = {"r_particle_wind_disable", "0"};
cvar_t	r_particle_wind_debug   = {"r_particle_wind_debug",   "0"};
```

(Two-field `{name, default}` form; the third field `archived` defaults to false, which is correct for these debug cvars.)

- [ ] **Step 2: Register the cvars in `R_Init`**

In `sdlquake/engine_src/r_main.c` around line 237 (just after `Cvar_RegisterVariable (&r_waterwarp);` and the surrounding `r_*` registrations), add three new lines:

```c
	Cvar_RegisterVariable (&r_particle_wind_scale);
	Cvar_RegisterVariable (&r_particle_wind_disable);
	Cvar_RegisterVariable (&r_particle_wind_debug);
```

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Verify cvars exist at runtime**

Run: `zig build run -- +map start`
Drop the console (`~`) and type each of:

```
r_particle_wind_scale
r_particle_wind_disable
r_particle_wind_debug
```

Expected for each: a line like `"r_particle_wind_scale" is "1"`. (The console prints current value when a cvar is queried by name with no value.)

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_part.c sdlquake/engine_src/r_main.c
git commit -m "$(cat <<'EOF'
feat(render): r_particle_wind_{scale,disable,debug} cvars

Unwired so far — Task 1's API is declared, these cvars are declared,
nothing yet calls them. Next commit lights up the particle loop.
EOF
)"
```

---

## Task 3: Per-type drag table

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (add static table near top, alongside `ramp1/2/3`)

- [ ] **Step 1: Add the drag table**

In `sdlquake/engine_src/r_part.c`, near the existing `ramp1[8]`, `ramp2[8]`, `ramp3[8]` global tables (search for `int ramp1[8]`), add:

```c
// Per-type wind drag coefficients.  Used by R_DrawParticles to lerp
// each active particle's velocity toward the locally-sampled wind
// velocity.  Higher k snaps faster; 0 disables for that type.
// Indexed by ptype_t enum (see r_particle_t).  pt_grav is excluded
// (heavy blood from gibs should not billow).
static const float wind_drag_k[] = {
    [pt_static]   = 3.0f,
    [pt_smoke]    = 4.0f,
    [pt_fire]     = 2.0f,
    [pt_explode]  = 0.6f,
    [pt_explode2] = 0.6f,
    [pt_blob]     = 0.4f,
    [pt_blob2]    = 0.4f,
    [pt_grav]     = 0.0f,
    [pt_slowgrav] = 2.5f,
};
```

(Designated initialisers in gnu89 are supported by clang/zig cc. If this fails to compile, fall back to a positional array — verify `ptype_t`'s order by reading `r_particle_t`'s declaration in `r_part.c` near the top.)

- [ ] **Step 2: Build**

Run: `zig build`
Expected: clean build; the table is referenced by nothing yet — confirm no "unused variable" warning fires (it's static const, but `-Wno-unused` may be relied on; if the warning appears, leave it — Task 4 will use the table).

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "$(cat <<'EOF'
feat(render): wind_drag_k per-particle-type table

Per-type lerp coefficients used by the next commit to drift particles
on the M4 wind grid.  Smoke (4.0) snaps to ambient air fast; explosion
shards (0.4-0.6) drift weakly; pt_grav is 0 so heavy blood stays put.
EOF
)"
```

---

## Task 4: Particle loop integration

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (insert wind nudge in `R_DrawParticles` around line 780)
- Reference: `sdlquake/engine/hotreload.c:1013` for the engine-side `g_game_api` pointer

- [ ] **Step 1: Pull in `g_game_api`, math.h, and the new cvars**

Near the top of `sdlquake/engine_src/r_part.c`, alongside existing includes and externs, add:

```c
#include <math.h>          // expf

// game.dll API table — set by HotReload_Apply when the DLL loads;
// NULL during startup/hot-reload swap.
struct game_api_s;
extern struct game_api_s *g_game_api;

extern cvar_t r_particle_wind_scale;
extern cvar_t r_particle_wind_disable;
extern cvar_t r_particle_wind_debug;
```

(If `hotreload.h` already exports `g_game_api`, you may `#include "../engine/hotreload.h"` instead — check whether existing engine_src files cross the `engine/` include boundary. The forward-decl form above avoids that question.)

- [ ] **Step 2: Insert the wind nudge in `R_DrawParticles`**

In `sdlquake/engine_src/r_part.c`, locate the position-integration block in `R_DrawParticles` (around line 783):

```c
		p->org[0] += p->vel[0]*frametime;
		p->org[1] += p->vel[1]*frametime;
		p->org[2] += p->vel[2]*frametime;
```

Immediately *before* the first of those three lines, insert:

```c
		// Wind nudge — drift particle velocity toward locally-sampled
		// wind velocity.  Pre-integration so the kick takes effect
		// this same frame.  Guarded so still-air or no-DLL behaviour
		// is byte-identical to legacy r_part.c.
		if (!r_particle_wind_disable.value) {
			vec3_t wind = {0, 0, 0};
			if (g_game_api && g_game_api->Wind_SampleVelocity)
				g_game_api->Wind_SampleVelocity(p->org, wind);
			float wlen2 = wind[0]*wind[0] + wind[1]*wind[1] + wind[2]*wind[2];
			if (wlen2 > 1.0f) {
				float k = wind_drag_k[p->type] * r_particle_wind_scale.value;
				if (k > 0) {
					float a = 1.0f - expf(-k * frametime);
					p->vel[0] += (wind[0] - p->vel[0]) * a;
					p->vel[1] += (wind[1] - p->vel[1]) * a;
					p->vel[2] += (wind[2] - p->vel[2]) * a;
				}
			}
		}
```

(Step 1 already added `<math.h>` for `expf`.)

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Verify still-air parity**

Run: `zig build run -- +map e1m1`
Fire a rocket at a wall. Watch the explosion shards.
Expected: visually indistinguishable from the previous build (e1m1 has no wind sources, so the trilerp returns near-zero and the `|wind| < 1` guard skips the lerp).

If anything looks different, set `r_particle_wind_disable 1` from the console and re-fire. The behaviour should now be exactly identical to legacy. If it is, the bug is somewhere in the new block; if it is not, the bug is elsewhere.

- [ ] **Step 5: Verify wind-on response**

Run: `zig build run -- +map m7_skeleton` (M7 has the `info_wind_source` and `misc_smokegrenade` props per CLAUDE.md Phase 8 notes).

Stand in the wind cone — confirm via `sim_wind_debug 1` that wind cells are nonzero where you are. Fire a smoke grenade. Smoke puffs should visibly sweep downstream rather than rise straight up. Toggle `r_particle_wind_scale 0` and re-fire — smoke should now rise straight again. Toggle back to `1`. Try `r_particle_wind_scale 3` — drift should look exaggerated.

- [ ] **Step 6: Verify Gust impulse**

Spawn or pick up a smoke grenade in `m7_skeleton`, throw it, and as the smoke puffs out, hit `+gust` (default f) aimed through the cloud. Expected: smoke is visibly punched in the gust direction, then settles. Without the wind nudge (`r_particle_wind_disable 1`), the gust has no effect on the smoke puffs (it still pushes the *density field*, but visual particles ignore it).

- [ ] **Step 7: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "$(cat <<'EOF'
feat(render): particles drift on the M4 wind grid

R_DrawParticles samples Wind_SampleVelocity once per active particle
and lerps p->vel toward the sampled wind using a per-type drag k and
framerate-independent rate 1-exp(-k*dt).  Smoke and fire snap fast;
explosion shards drift weakly; pt_grav opts out.  Still air and no-DLL
paths are byte-identical to legacy via the wlen2 > 1 guard.
EOF
)"
```

---

## Task 5: Debug visualisation

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (extend the wind block to emit debug lines and recolour particles)

- [ ] **Step 1: Wire up `DebugLines_Add`**

At the top of `sdlquake/engine_src/r_part.c`, add:

```c
#include "../engine/debug_lines.h"
```

(Verify the relative path from `engine_src/` to `engine/`; if `engine/debug_lines.h` isn't found, prefer the include style other engine_src files already use to reference `engine/` — e.g., grep `engine_src/` for any existing `#include "../engine/`.)

- [ ] **Step 2: Emit a debug line per affected particle**

Find the wind-nudge block from Task 4. After the `p->vel[...] += ...` lines, just before the closing brace of the `if (k > 0)` block, insert:

```c
				if (r_particle_wind_debug.value >= 1) {
					vec3_t tip = {
						p->org[0] + wind[0] * 0.1f,
						p->org[1] + wind[1] * 0.1f,
						p->org[2] + wind[2] * 0.1f,
					};
					// Colour 15 = white in id1 palette; 1 = ztest on.
					DebugLines_Add(p->org, tip, 15, 1);
				}
```

- [ ] **Step 3: Colour-by-bucket override in `r_particle_wind_debug 2`**

Still inside the same `if (k > 0)` block, after the debug-line emission, add:

```c
				if (r_particle_wind_debug.value >= 2) {
					// Recolour particle by its drag bucket so the
					// per-type tuning is visually distinguishable.
					if      (k >= 3.0f) p->color = 192;  // bright cyan: smoke/static
					else if (k >= 1.5f) p->color = 110;  // green: fire/slowgrav
					else                p->color =  79;  // red: explode/blob debris
				}
```

(Exact palette indices are id1's `quake.pal` — 192 is in the cyan ramp, 110 the green ramp, 79 the red ramp. Tweak if needed after seeing it in-engine.)

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 5: Verify**

Run: `zig build run -- +map m7_skeleton`
Console:

```
r_particle_wind_debug 1
```

Throw a smoke grenade in the wind cone. Expected: short white line segments emanating from each smoke puff, pointing downstream, length ~10× wind magnitude in units (so a 50 u/s wind = 5 unit line, just visible). Set `r_particle_wind_debug 2` — smoke now reads as cyan, explosion sparks (try firing a rocket) as red.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine_src/r_part.c
git commit -m "$(cat <<'EOF'
feat(render): r_particle_wind_debug 1/2 overlay

Mode 1 draws a velocity vector on each wind-pushed particle via
DebugLines_Add.  Mode 2 also recolours particles by drag bucket
(cyan/green/red) so per-type tuning is visually distinguishable
without diffing screenshots.
EOF
)"
```

---

## Task 6: Verification scenes + tuning pass

**Files:** none modified; this is a documented playtest. Outcomes inform whether to revise `wind_drag_k[]` or palette indices in Task 5.

- [ ] **Step 1: Parity screenshot on `e1m1`**

```
zig build run -- +map e1m1
```

Fire a rocket at the start-hall wall. With `screenshot_png` (already a console command per `e915f64`), capture a frame mid-explosion. Then `r_particle_wind_disable 1`, repeat, capture. Diff visually — they should be identical (no wind sources in e1m1; the `|wind| < 1` guard short-circuits regardless).

- [ ] **Step 2: M7 wind cone**

```
zig build run -- +map m7_skeleton
sim_wind_debug 1
```

Walk along the wind cone's centerline. Confirm cell vectors are nonzero where you stand. Throw a smoke grenade up-cone; observe puffs being swept down-cone. Tune `r_particle_wind_scale` from 0.5 to 2.0 to find the value that "reads" best — if 1.0 looks too subtle, consider raising `wind_drag_k[pt_smoke]` from 4.0 to 6.0 in a follow-up. Capture screenshots.

- [ ] **Step 3: Gust + smoke interaction**

In M7, throw a smoke grenade; let smoke build up. `+gust` (f key) through the cloud. Capture. Expected: smoke is punched in the gust direction, then settles back to ambient drift as the impulse decays.

- [ ] **Step 4: Explosion shards in wind**

In M7, fire a rocket against a wall *inside the wind cone*. Watch the `pt_explode` / `pt_explode2` shards. Expected: high-velocity initial trajectories barely deflect (`k=0.6` is intentionally weak), but as their existing per-type damping decays them, the wind nudge grows visible and shards drift downstream as embers. If this reads as too subtle, raise `wind_drag_k[pt_explode] / pt_explode2` from 0.6 to 1.0 in a follow-up commit; if shards look unnaturally "blown" mid-explosion, lower to 0.3.

- [ ] **Step 5: pt_grav exclusion**

In stock Quake, gib a grunt with a rocket (in M7 or any id1 map; the rocket launcher is granted via `give 6` console). Confirm pt_grav blood trails fall under gravity uninfluenced by wind. If `wind_drag_k[pt_grav]` was accidentally non-zero, this scene would expose it.

- [ ] **Step 6: Document outcomes**

Append a short "Tuning notes" section to the bottom of `docs/superpowers/specs/2026-05-22-wind-reactive-particles-design.md` capturing which `k` values held after playtesting and which were revised (if any). Commit:

```bash
git add docs/superpowers/specs/2026-05-22-wind-reactive-particles-design.md
git commit -m "$(cat <<'EOF'
docs(spec): wind-reactive particles — tuning notes from playtest

Records the post-playtest k values and any revisions from the
initially-specified table.
EOF
)"
```

---

## Done

After Task 6, the feature is shipped and tuned. The four cvars (`r_particle_wind_scale`, `r_particle_wind_disable`, `r_particle_wind_debug`, plus the existing `sim_wind_debug`) remain in-place for future debugging. The spec's "Non-goals" section is the gate against scope creep — dust motes, grass-stir, rain, per-particle drag fields, and an engine-side mirror are all explicitly deferred.

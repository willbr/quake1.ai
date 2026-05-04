# Immersive-Sim Systems for Quake — Design

**Date:** 2026-05-04
**Status:** Proposed
**Phase:** 8 (follows planned Phases 6 and 7 in CLAUDE.md)

## Goal

Layer immersive-sim *systems* on top of Quake's existing run-and-gun core, in the spirit of Far Cry / Dishonored / BotW *emergence* — not a stealth-game conversion. The player keeps Quake's combat verbs and gains two new ones (Blink, Gust); the world gains physics, reactive AI, wind/smoke, and a light/shadow tier; the existing maps gain new texture by virtue of those systems.

We are explicitly NOT building a chaos meter, a lethal/non-lethal system, civilian NPCs, or a possess power.

## Identity / Pillars

1. **Systemic playground, not stealth game.** Stealth is a *strategy*, not the goal. Combat is still first-class.
2. **Verbs that pull on systems.** Blink and Gust are designed so each interacts with multiple systems (physics, AI, wind, light).
3. **Emergence from a small set of orthogonal systems.** Physics + reactive AI + wind/smoke + light tier, communicating exclusively through a stimulus bus.
4. **Iteration loop respected.** Everything that needs tuning lives in `game.dll` and hot-reloads.
5. **MP-aware, MP-deferred.** All gameplay must be reproducible from inputs. Single-player is built first; the design must not paint into a multiplayer corner. No client-side time dilation.

## Architecture (Approach 1 — All in `game.dll`)

All new gameplay systems live inside the existing hot-reloadable `game.dll`. Engine additions are minimal: only utilities the engine already has the data for (lightmap sample, LOS trace) are exposed via `engine_api_t`.

### File layout

```
sdlquake/game/
  game_main.c            (existing)
  abilities.c            NEW — Blink + Gust input, mana, FX hooks
  sim/
    sim.h                NEW — public types (Stimulus, WindCell, ai_brain_t), event-bus API
    sim_stimulus.c       NEW — ring-buffer event bus, emit/poll API
    sim_wind.c           NEW — 3D grid wind field, advection, gust impulse
    sim_ai.c             NEW — 4-state FSM, sense filtering, patrol routes
    sim_nav.c            NEW — BSP navmesh bake + A*
    sim_light.c          NEW — lightmap sampler wrapper (lit/shadowed tier)
    sim_physics.c        NEW — physics-prop entity glue, kick/throw, impact stims
```

`sim.h` is the only header crossed between sim modules. Modules don't `#include` each other directly — they communicate through the stimulus bus or shared `sim.h` types.

### Engine ABI delta

`engine_api_t` gains:

- `int Sample_Lightmap(vec3_t origin)` — 0–255 light value at a point. Implementation reuses the lightmap lookup the renderer already does.
- `void Trace_LOS(vec3_t a, vec3_t b, trace_t *out)` — only if existing `SV_Move` isn't already exposed via the API. Otherwise reuse.

Bumps `GAME_API_VERSION` once.

### Hot-reload coverage

Everything in `game/` (including `sim/`) hot-reloads. The wind grid, AI side-table, and navmesh handle live in the DLL — the DLL's `init()` rebuilds wind grid bounds from the current map and re-loads the cached navmesh. AI state machines re-init (acceptable: patrols restart from node 0). The navmesh *cache file* (`.nav`) is on disk and is unaffected by reload.

## System 1 — Stimulus bus

The spine of the sim. Anything that wants AI to notice it emits a stimulus; AI sense filters consume them.

### Event struct (in `sim.h`)

```c
typedef enum {
    STIM_SOUND,        // gunshot, footstep, explosion, prop impact, blink whoosh
    STIM_SIGHT_ENTITY, // a visible thing (player, corpse, broken door)
    STIM_SMOKE,        // smoke arrived in an AI's sight cone
    STIM_LIGHT_CHANGE, // torch out, light on
    STIM_CORPSE,       // newly dead body in this spot
    STIM_PROP_BROKEN,  // crate smashed, door kicked
} stim_kind_t;

typedef struct {
    stim_kind_t kind;
    vec3_t      origin;
    float       intensity;    // 0..1, attenuates with distance + walls
    float       time;         // host time at emission
    int         source_edict; // who/what caused it (-1 = world)
    int         flags;        // kind-specific
} stimulus_t;
```

### Storage

A fixed ring buffer in `sim_stimulus.c`, ~512 events. New stimuli evict old. AIs consume by *peeking* (non-destructive) — the buffer is purely time-windowed (events older than ~5s are ignored by sense filters).

### API

- `Stim_Emit(stimulus_t)` — anyone can call (physics, weapons, smoke advection, AI death).
- `Stim_QueryNear(vec3_t pos, float radius, float since_time, stimulus_t *out, int max)` — sense filter scans recent events near a monster.

### Attenuation

`intensity` is set by the emitter (loud explosion = 1.0, footstep = 0.2). The sense filter computes effective intensity at the listener:

```
effective = intensity * falloff(distance) * los_factor
```

For sound: `los_factor = 1.0` through walls (sound carries, but distance still attenuates). For sight stimuli: `los_factor = 0` if blocked.

### Cross-system couplings already enabled

| Source | Stimulus emitted |
|---|---|
| Physics: prop hits floor | `STIM_SOUND` + `STIM_PROP_BROKEN` |
| Wind: smoke parcel enters volume | `STIM_SMOKE` |
| Light: torch extinguishes | `STIM_LIGHT_CHANGE` |
| AI: monster dies | `STIM_CORPSE` |
| Player: Blink released | `STIM_SOUND` (intensity 0.3, source + dest) |
| Player: Gust impacts target | `STIM_SOUND` |

## System 2 — Reactive AI (FSM + sense filter)

Each monster gets a per-edict state block in `sim_ai.c`'s side-table, keyed by edict index — keeps the engine's `edict_t` untouched.

### Per-monster state

```c
typedef enum {
    AI_IDLE,        // patrolling or holding post
    AI_SUSPICIOUS,  // heard/saw something, raising alert
    AI_SEARCHING,   // lost LOS, hunting last-known position
    AI_COMBAT,      // engaged
} ai_state_t;

typedef struct {
    ai_state_t  state;
    float       state_entered_time;
    float       alert_level;        // 0..1, accumulates from stimuli
    vec3_t      last_known_pos;
    int         target_edict;       // -1 = none
    int         patrol_route_id;    // -1 = stationary
    int         patrol_node_idx;
    float       sense_sight_range;  // base 1024 units
    float       sense_hearing_mult;
} ai_brain_t;
```

### Sense filter (per think tick, ~10Hz)

For each fresh stimulus near the monster:

1. Compute effective intensity (distance falloff + LOS for sight + smoke check).
2. For sight stimuli aimed at the player: multiply by **light tier** (lit = 1.0, shadowed = 0.5). Crouching = additional ×0.7.
3. Add to `alert_level` (capped 0..1).

### Transitions

| From | To | Trigger |
|---|---|---|
| IDLE | SUSPICIOUS | alert_level > 0.25 |
| SUSPICIOUS | SEARCHING | saw player full-strength OR alert_level > 0.6 |
| SUSPICIOUS | IDLE | no fresh stimuli for 8s, alert decays |
| SEARCHING | COMBAT | LOS to player |
| SEARCHING | IDLE | no fresh stimuli for 20s |
| COMBAT | SEARCHING | lost LOS for 3s |

### Behavior

- **IDLE** — follow patrol route OR Quake's existing idle/wander.
- **SUSPICIOUS** — face stimulus origin, slow approach, weapons holstered.
- **SEARCHING** — A* to `last_known_pos` via the navmesh, then arc-sweep neighborhood.
- **COMBAT** — delegate to existing Quake monster attack code. The FSM gates entry; it does not rewrite combat.

### Patrol routes

A new entity `info_patrol_node` placed in maps. A node points at the next via `target` / `targetname` (Quake's existing convention). For id1 retrofitting, patrol nodes are injected programmatically based on monster spawn positions and the navmesh.

### Faction note (deferred)

Monster infighting already exists in vanilla Quake. The stimulus bus naturally extends it — a `STIM_SIGHT_ENTITY` from a non-player edict triggers the same path. No special faction code needed in v1.

### What this does NOT do

- No global alert level, no chaos meter.
- No civilian/neutral monsters; existing monsters stay hostile-on-sight in COMBAT.
- No replacement of Quake's combat AI; the FSM is a *gate* on top of it.

## System 3 — Wind & smoke

A coarse 3D voxel field over the map's bounding box. Cell size **64 units** (~1m in-fiction). Typical id1 map ~2048³ → ~32³ = 32k cells. Cell:

```c
typedef struct {
    vec3_t velocity;   // wind vector, units/sec
    float  smoke;      // 0..1 density
    float  gas;        // 0..1, stub for future
} wind_cell_t;
```

~32 bytes × 32k = ~1 MB.

### Update step (10 Hz, not per-frame)

1. **Advection (semi-Lagrangian):** `smoke_new[c] = sample(smoke, c - velocity*dt)`, linear interpolation. Slightly diffusive — fine.
2. **Decay:** `smoke *= 0.97` per tick.
3. **Velocity damping:** `velocity *= 0.85` per tick.
4. **No pressure solve.** This is a *transport field*, not a fluid sim.

### Sources

- `info_wind_source` entity → constant velocity injection in nearby cells.
- Player Gust → spherical impulse (~600 u/s outward, 0.2s) + smoke clear (smoke -= 0.5).
- Explosion → similar but stronger and shorter.
- Smoke grenade / fire prop → continuous `smoke += 0.3` per tick in source cell.

### Stimuli emitted

When a cell's smoke crosses 0.4 and intersects an AI's sight cone, emit `STIM_SMOKE` once per AI affected (debounced).

### LOS coupling

`Wind_PathIsObscured(a, b)` DDA-rasterizes through the wind grid, sums smoke density, returns true if integral > threshold. Called only when an AI is considering a sight stim. ~30 cell touches per check.

### Rendering (v1)

Sprite-per-cell, alpha = smoke density. Functional, not pretty. Volumetric rendering is a polish phase.

### Sizing risk

id1's largest BSP bounds exceed 2048³. Mitigation: cap grid at 64³ and scale cell size up per map. Larger maps get coarser smoke (acceptable).

### NOT in v1

- No pressure solve, no fire propagation, no gas (struct field stubbed).
- No outdoor/indoor distinction.
- No buoyancy.

## System 4 — Light tier

`sim_light.c` exposes `Light_TierAt(vec3_t pos) → float` (returns 1.0 = lit, 0.5 = shadowed). It is the only thing the AI sense filter calls; no other module calls `Sample_Lightmap` directly.

Implementation: `Light_TierAt` calls the engine's `Sample_Lightmap`, then applies any DLL-side overrides (see torch-extinguish below), then thresholds:

- `>= 128` → 1.0 (lit)
- `< 128` → 0.5 (shadowed)

Crouching player applies a further ×0.7 in the sense filter (not in `Light_TierAt`, since crouch is a per-listener-vs-player concern, not a per-point concern).

No detection meter, no continuous sneak slider — just two tiers.

### Gust extinguishes torch

Torches and candles become entities with a `lit` flag. When extinguished by Gust:

1. Emit `STIM_LIGHT_CHANGE` at the torch position.
2. Add an entry to a DLL-side **light-override table** (in `sim_light.c`): `{position, radius, delta}` where `delta` is negative (e.g. radius 192, delta -80).
3. `Light_TierAt(pos)` sums `delta` from all overrides whose sphere contains `pos` and applies the sum to the engine's lightmap sample before thresholding.

The engine's lightmap is never modified — overrides are entirely DLL-side state. The renderer continues to draw the world with the original lightmap (so the visual scene gets darker via a separate dynamic-light effect or simply via the torch sprite vanishing — visual treatment is a polish concern, not part of this design). What matters here is that *AI sees the area as shadowed*. Vanilla maps have few light-emitting props; the bespoke map uses many.

## System 5 — Player abilities

Both abilities live in `abilities.c`. Cvars expose tuning so we can iterate without recompiling.

### Phase energy

`phase_energy` float, 0..100, regen 25/sec when not casting. Cvars: `ph_max`, `ph_regen`, `ph_blink_cost`, `ph_gust_cost`. HUD placement is open (see Open questions) — the cheapest path is adding a small bar next to ammo without redesigning the status bar.

### Blink (MP-safe, no time dilation)

- Bound to `+blink`. **Hold-to-aim, release-to-commit.**
- While held: a reticle is drawn at the trace endpoint of the player's current look direction. Range 320 units (~5m). Player keeps full real-time movement.
- Reticle: green cross = valid landing, red X = invalid (in solid, off cliff into void, OOB).
- Release with valid → instant translation, ~4 frames of post-fx, cost 25 phase. Release with invalid → no-op, refund.
- No charge time; tap-fire allowed.
- Cost: 25 phase. Cooldown: none beyond cost.

**Grate-passing trait.** Validity check uses a thin box trace (player half-width × 2 horizontally only), so Blink can pass through brushes flagged `CONTENTS_GRATE` — a new content type added via QBSP convention `func_grate`. Vanilla maps have no grates; retrofit phase adds a few; bespoke map uses them as a signature traversal idea.

**Stealth cost.** Blink emits `STIM_SOUND` at *both* source and destination, intensity 0.3, radius 192. Audible to AI but quiet.

### Gust

- Bound to `+gust`. Instant cast. Cost 15 phase. Cooldown 0.5s.
- Cone in front of player, length 384, half-angle 30°.
- Effects within cone:
  - Wind grid: inject radial-outward velocity ~800 u/s in cone cells, 0.3s.
  - Smoke: cells with smoke > 0.2 lose smoke -= 0.5 (clears).
  - Flammable props (torches, candles) extinguished. Emits `STIM_LIGHT_CHANGE`.
  - Physics props (crates, corpses, dropped weapons) get an axial impulse, magnitude scales with mass.
  - Monsters shoved 96 units back, light stagger (no damage). Triggers `STIM_SOUND` at impact.

### HUD

- Phase bar replaces armor pip.
- Blink reticle: single sprite at trace endpoint.
- Gust: no reticle; FX (dust cone, smoke clearing) is the feedback.

### Designed interactions

- Blink to a shadowed alcove → light tier 0.5 → AI sight halved.
- Gust a torch out → area shadowed → AI sight further reduced.
- Gust a barrel down a stairwell → impact noise → patrol investigates → Blink past.
- Gust into smoke → clears your line of fire OR clears smoke off an AI who'd lost LOS.

## System 6 — Navmesh (bake + cache)

Lives in `sim_nav.c`. Used by SEARCHING state's A*.

### Bake (first load of a map)

1. Walk all BSP faces; keep faces with `normal · up > 0.7` (walkable floors).
2. Distribute sample points on a 64-unit grid over walkable faces; drop a small trace down to confirm a stable foothold.
3. For each pair of nearby points (within ~96 units), call `walkmove` between them; if it succeeds, add a graph edge with weight = distance.
4. Detect step-up edges (height ≤ 18 units, Quake's step height) and water entry/exit edges.
5. Serialize the graph to `id1/cache/navmesh/<mapname>-<bsp_hash>.nav`. The hash invalidates the cache if the map changes.

### Runtime

- On map load: try to load `<mapname>-<bsp_hash>.nav`. If missing or hash mismatch, kick off bake on a worker thread.
- While baking, AI fallback is "stand at last_known_pos and sweep" (the M2 fallback).
- Bake-time target: <5s for typical id1 maps. Slower maps get a progress message.
- A* uses the cached graph; SEARCHING plans a path to `last_known_pos`, walks via existing `movetogoal`.

### Bonus uses (out of scope, noted to avoid painting into a corner)

- Patrol routes auto-generated by sampling cycles in monster spawn neighborhoods (better than the simple "spawn-position scatter" plan in M6).
- MP bot navigation later.

## Multiplayer constraint (designed-for, deferred)

The spec is single-player-first but explicitly preserves MP viability:

- No client-side time dilation. (Applied: removed time-slow from Blink.)
- All systems must be reproducible from server inputs. Player abilities run server-side.
- Wind grid: in MP, server is authoritative; deltas replicate to clients. Smoke renders client-side from replicated state. Out of v1 scope but the system shape supports it.
- AI: already server-only in Quake; nothing changes.
- Stimulus bus: server-only. Clients never read it.

## Build order

Each milestone is a working, playable state.

### M1 — Stimulus bus + sense filter, no new content

`sim_stimulus.c`, ring buffer, API. Wire existing Quake events into the bus: gunshots emit `STIM_SOUND`, monster deaths emit `STIM_CORPSE`. Add the AI side-table and sense filter, but FSM still defaults to vanilla behavior — sense filter just *logs* what it would react to (imgui overlay shows alert level per monster).

**Verify:** firing a shotgun in a hallway shows alert spikes on nearby monsters in the imgui overlay.

### M2 — AI FSM live (with stand-and-sweep search fallback)

Plug in IDLE/SUSPICIOUS/SEARCHING/COMBAT transitions. Add `info_patrol_node` entity. Procedural arenas: a small generator that spawns a 1024³ box room with 4 patrol nodes and 2-3 grunts. SEARCHING uses the stand-and-sweep fallback (no navmesh yet).

**Verify:** in the arena, a grunt patrols, hears a shot, investigates the spot, gives up after 20s.

### M2.5 — Navmesh bake & A*

`sim_nav.c`. Bake on first map load, cache to disk, A* powers SEARCHING.

**Verify:** in arena, grunt now hunts to last-known-pos via an actual path (visible as a debug polyline in imgui).

### M3 — Blink + Gust (no wind yet)

Abilities, phase energy, HUD bar, `CONTENTS_GRATE` content type, QBSP support for `func_grate`. Gust shoves props/monsters and emits stims; smoke clearing is a no-op until M4.

**Verify:** Blink + Gust feel good in an arena, AI alerts on Blink whoosh, props get shoved.

### M4 — Wind grid + smoke

`sim_wind.c`, advection, sources, smoke rendering (sprite-per-cell), Gust integrates with the field, AI LOS uses `Wind_PathIsObscured`.

**Verify:** smoke grenade prop in arena → AI loses LOS → searches.

### M5 — Light tier

`Sample_Lightmap` engine API, sense filter multiplies by light tier. Add `Gust extinguishes torch` interaction.

**Verify:** standing in a dark alcove halves AI sight in arena; gusting a torch creates a new dark zone.

### M6 — Retrofit pass

Entity-injection on map load: scatter patrol nodes near monster spawns (using the navmesh), place a few physics props, add a couple of `func_grate` regions in id1 maps via a side-data file.

**Verify:** e1m1 plays through with the new systems active and is *more interesting*, not broken.

### M7 — Bespoke mini-level

Three connected areas, ~15 enemies, soft "reach the slipgate unseen" objective (success bonus = phase energy regen boost on subsequent levels, or just screen text — TBD during M7). Hand-placed patrols, grates, props, light/dark zones, smoke sources.

**Verify:** the level is winnable in at least three distinct ways (combat, stealth, blink-traversal-heavy).

## Explicit scope cuts (NOT in v1)

- No fluid pressure solve, no fire propagation, no gas.
- No possess, no chaos meter, no lethal/non-lethal scoring.
- No civilian/neutral monsters.
- No multiplayer implementation (designed-for, not built).
- No new monster types — existing roster only.
- No replacement of Quake combat AI.
- No HUD redesign — phase bar is a small added element, not a status-bar rework.
- No proper smoke rendering — sprite-per-cell only.
- No third-person, no VR, no networking changes.

## Success criteria

1. In a procedural arena, a grunt can be distracted by a thrown crate, lose LOS in smoke, and lose interest after a 20s search.
2. Blink + Gust + light tier + smoke + props + AI all interact at least once in a single 60-second gameplay clip.
3. e1m1 still completable, AI still threatening, but feels different.
4. Bespoke map has three distinct viable playthroughs.

## Risks

- **Wind grid memory in big maps.** Mitigation: cap grid at 64³, scale cell size per map.
- **Hot-reload + side-tables.** AI brain side-table keyed by edict index. State resets on reload (acceptable: patrols restart from node 0).
- **Navmesh bake time on first load.** Mitigation: worker thread + stand-and-sweep fallback while baking.
- **Lightmap-overlay for extinguished torches** must compose correctly with the renderer's existing dynamic-light path. Worth a spike before M5.
- **Smoke rendering perf** with sprite-per-cell on a dense field. Acceptable for v1; flagged for polish.

## Open questions for the implementation plan

- Exact `phase_energy` HUD widget styling.
- Whether to expose Blink/Gust as `+attack2` / `+attack3` (Q3 binding convention) or new `+blink` / `+gust` commands.
- Patrol-node auto-generation algorithm for retrofitting (sample cycles vs. nearest-N-walkable-neighbors).
- M7's "soft objective" success effect — whether to ship anything beyond a screen-text confirmation.

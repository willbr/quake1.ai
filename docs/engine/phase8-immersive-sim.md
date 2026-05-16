# Phase 8 — Immersive-Sim Layer

Lives entirely inside `game.dll`, under `sdlquake/game/sim/`. The engine
doesn't know it exists; everything talks to the game through the existing
`engine_api_t` / `game_api_t` ABI.

The layer adds: a stimulus bus (the AI's "what just happened" feed); a
four-state AI FSM driven by stimuli; a navmesh-driven SEARCHING state; a
wind/smoke voxel grid; a light-tier system; and two new player abilities
(Blink + Gust).

See `docs/superpowers/specs/2026-05-04-immersive-sim-systems-design.md` for
the broader design rationale.

## Files

```
sdlquake/game/
├── abilities.c     Blink + Gust player abilities (Phase 8 / M3)
├── sim/
│   ├── sim.h           Inter-module API (this is the entry point)
│   ├── sim_main.c      Lifecycle: Init / LevelInit / Frame dispatcher
│   ├── sim_stimulus.c  Stimulus bus (ring buffer of recent events)
│   ├── sim_ai.c        FSM (IDLE/SUSPICIOUS/SEARCHING/COMBAT)
│   ├── sim_nav.c       Navmesh bake + A*
│   ├── sim_wind.c      Voxel grid, smoke advection, Gust impulse, sources
│   ├── sim_light.c     Lightmap-tier sampling + flammable-light overrides
│   ├── sim_retrofit.c  Auto-tag id1 maps with patrol routes from navmesh
│   └── sim_arena.c     Test fixtures
```

The whole layer is initialised via `Sim_Init` (called once from
`game_main.c::game_init`) and `Sim_LevelInit` (called automatically on map
change — detected by `Sim_Frame` watching for `g->mapname` change or
`g->time` reset).

## Stimulus bus

```c
typedef struct {
    stim_kind_t kind;          // SOUND, SIGHT_ENTITY, SMOKE, LIGHT_CHANGE,
                               // CORPSE, PROP_BROKEN
    vec3_t      origin;
    float       intensity;     // 0..1 at source
    float       time;          // g->time at emission
    int         source_edict;  // -1 = world
    int         flags;
} stimulus_t;
```

`Stim_Emit` pushes a stimulus into a 512-slot ring (`SIM_STIM_RING_SIZE`).
`Stim_QueryNear(pos, radius, since_time, …)` fetches recent stimuli for
the AI to react to. Stimuli older than `SIM_STIM_MAX_AGE_S` (5 s) are
ignored.

Emitters: weapon discharges (loud SOUND), monster deaths (CORPSE),
Gust + wind sources (SMOKE), Gust-extinguished lights (LIGHT_CHANGE),
broken props (PROP_BROKEN).

## AI FSM

`sim_ai.c`. Each registered monster has an `ai_brain_t` slot in a fixed
600-slot pool (matches `MAX_EDICTS`). The brain runs at 10 Hz
(`SIM_AI_TICK_HZ`).

States:

- **IDLE** — stationary or on patrol route. Looks for stimuli.
- **SUSPICIOUS** — picked up a stimulus but doesn't know where the
  player is. Looks around in place ("stand and sweep").
- **SEARCHING** — committed to investigate the stimulus origin.
  Drives a navmesh A* path; sweeps at each waypoint.
- **COMBAT** — sees the player. Vanilla Quake combat thinks take over;
  the FSM tracks `last_known_pos` for the next state transition.

Transitions follow a cumulative `alert_level` (0..1). Sight raises it
fast, sound slowly, prop noises briefly. When it crosses thresholds the
state advances; idle decay drops it back over time.

`stuck_ticks` counts consecutive AI ticks where the monster tried to
move but didn't displace; on overflow we replan or fall back to IDLE.

The ImGui AI panel pulls live brain state via
`engine_api->ImguiAI_Push` — calls are no-op when the panel is hidden.

## Navmesh

`sim_nav.c`. Built at map load by sampling the BSP via `SV_TraceMove`
with the player's bounding box (32×32×56). The bake is async — a
background SDL thread walks an open-set of seed points, drops floor
samples, prunes by traceability, and links neighbours into an
undirected graph. `Sim_Nav_IsReady` returns 0 while baking.

Path queries (`Sim_Nav_PathTo`) run A* with octile heuristic and
write up to 32 waypoints into the caller's buffer. The brain stores
this path in `ai_brain_t.path_pts[32]` and follows it via
`SV_WalkMove` between waypoints.

Visualisation: `sim_nav_debug 1` (ztest) or `2` (xray); pushed through
`engine_api->ImguiNav_Set`. The F12 Debug Render panel exposes the
toggle.

## Patrol routes

`info_patrol_point` entities (one per node) are linked by `targetname`
chains. `sim_retrofit.c` (M6) walks every id1 map at level load and
auto-binds monsters to the nearest navmesh-connected patrol chain — so
stock Quake monsters now move along patrol routes without re-authoring
any maps.

## Wind / smoke voxel grid

`sim_wind.c`. A ≤64³ voxel grid (16-unit cells) aligned to the map
bounds. Each cell stores a velocity and a smoke density. Per-frame:

- **Source integration**: every `info_wind_source` and
  `misc_smokegrenade` adds smoke + velocity to nearby cells.
- **Advection**: semi-Lagrangian — sample density and velocity from
  upstream cells.
- **Impulse decay**: Gust adds an impulse via `Wind_AddImpulse`; this
  decays exponentially per frame.

`Wind_PathOcclusion(a, b)` is folded into AI sight LOS — smoke breaks
line of sight even when the BSP says it's clear. `Wind_GetSmokeAt` is
queried by abilities for visual feedback.

Debug viz: `sim_wind_debug 1/2` draws cell densities; visible through
the F12 panel.

## Light tier

`sim_light.c`. `Light_TierAt(pos)` returns a normalised light level by
querying the lightmap via `engine_api->Sample_Lightmap` (which reuses
the renderer's `R_LightPoint`). Threshold at **128** splits "lit" from
"shadow"; the AI's `sense_sight_range` is scaled accordingly.

A small DLL-side override table (`Light_AddOverride`) lets Gust
temporarily darken a sphere around an extinguished light without
needing the engine to rebake the lightmap. The override decays back
over time so the world re-brightens once the wind passes.

## Abilities: Blink + Gust

`abilities.c`. Two player abilities bound by default:

- **Blink** (`+blink`, default key `q`): hold-aim, release-commit
  teleport. While held, a marker traces forward along the player's
  view. Release commits the teleport, including passing through
  `func_grate` entities marked `pass_blink`.
- **Gust** (`+gust`, default key `f`): cone-shaped wind push. Kicks
  TOSS-moveType props (gibs, weapons, grenades) along the cone;
  applies a `Wind_AddImpulse` for smoke disruption; emits `STIM_SOUND`
  for AI; extinguishes flammable lights inside the cone (`Light_AddOverride`).
  Downward push is clamped so ground items lift rather than slam into
  the floor.

Wiring takes the [button-bit pattern](../../memory/project_button_bits_pattern.md):
new `clc_move` bits → `entvars_t.button3` / `button4` → game-side
`abilities.c` polls them every think. That's why bumping abilities
required `GAME_API_VERSION` 16 → 17.

## Phase 8 milestones

| M | Status | What it adds |
|---|---|---|
| M1 | ✅ | Stimulus bus + sense filter |
| M2 | ✅ | AI FSM with stand-and-sweep search |
| M2.5 | ✅ | Navmesh bake + A* path-driven SEARCHING |
| M3 | ✅ | Blink, Gust, `func_grate`, button3/4 |
| M4 | ✅ | Wind/smoke grid, sources, occlusion |
| M5 | ✅ | Light tier, flammable extinguish |
| M6 | ✅ | id1 retrofit (auto-patrol routes) |
| M7 | 🚧 skeleton | `id1/maps/m7_skeleton.map` exercises every system; full bespoke level is deferred. |

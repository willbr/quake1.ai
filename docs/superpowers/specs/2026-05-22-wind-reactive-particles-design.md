# Wind-reactive particles

Status: design

## Goal

Couple the engine's particle system (`r_part.c`) to the Phase 8 M4 voxel wind grid
(`sdlquake/game/sim/sim_wind.c`) so that smoke puffs, fire embers, explosion shards,
and other particle types visibly drift with steady wind sources, get pushed by
player Gust impulses, and settle naturally as gusts decay. Stock maps with no wind
sources must look identical to today.

## Architecture

Particles live in the engine. The wind voxel grid lives in `game.dll`. The engine
particle loop will sample wind velocity at each particle's position via a new
`game_api_t` entry that `sim_wind.c` fills in and the engine calls.

Direction of the new API hop: engine → DLL. `game_api_t` is the table the DLL
exposes to the engine (alongside `init`, `shutdown`, `start_frame`, etc.); this
is a new entry on it.

### New ABI entry

Append to `game_api_t` in `sdlquake/game/game_api.h`:

```c
// Sample the wind voxel grid at a world position.
// Writes wind velocity (units/sec) into out_vel.
// Outside the grid bounds, writes (0,0,0).
void (*Wind_SampleVelocity)(const vec3_t pos, vec3_t out_vel);
```

Bump `GAME_API_VERSION` 22 → 23.

Implementation in `sim_wind.c`: trilerp over the 8 neighbouring cells of `s_cells[]`,
identical in shape to the existing private `Wind_GetSmokeAt` (which already does
the same trilerp on the density field). The function reads `vx/vy/vz` from each
cell instead of the density. `sim_main.c`'s `game_api_t` initialiser wires the
pointer.

Engine side (`sdlquake/engine/hotreload.c`): the engine holds `g_game_api`
(set on DLL load, nulled on shutdown / mid-reload). The particle loop guards
the call with `if (g_game_api && g_game_api->Wind_SampleVelocity)` — same
pattern the engine already uses for other game callbacks (e.g. `mcp_damage`
at hotreload.c:1117). When the pointer is null, the particle loop skips the
wind nudge entirely and falls through to today's behaviour.

## Particle loop integration

In `r_part.c`'s main particle update loop, add one new step *before* the
existing `p->org += p->vel * frametime` integration:

```c
vec3_t wind = {0,0,0};
if (g_game_api && g_game_api->Wind_SampleVelocity)
    g_game_api->Wind_SampleVelocity(p->org, wind);
float wlen2 = wind[0]*wind[0] + wind[1]*wind[1] + wind[2]*wind[2];
if (wlen2 > 1.0f) {                                // skip if |wind| < 1 u/s
    float k = wind_drag_k[p->type] * r_particle_wind_scale.value;
    float a = 1.0f - expf(-k * frametime);
    p->vel[0] += (wind[0] - p->vel[0]) * a;
    p->vel[1] += (wind[1] - p->vel[1]) * a;
    p->vel[2] += (wind[2] - p->vel[2]) * a;
}
```

The existing `p->org += p->vel * frametime` and per-type physics `switch`
(gravity, ramp, damping) run unchanged. Wind composes with — does not replace —
existing per-type behaviour.

The `1 - exp(-k*dt)` form makes drag framerate-independent; Quake's `frametime`
varies frame to frame and a naive `k*dt` lerp would feel wrong under load.

### Per-type drag constants

`wind_drag_k[]` is a static table in `r_part.c`:

| Type          | k    | Rationale                                  |
|---------------|------|--------------------------------------------|
| `pt_smoke`    | 4.0  | drifts with air almost immediately         |
| `pt_static`   | 3.0  | sparks/embers, light                        |
| `pt_slowgrav` | 2.5  | blood mist / dust-like                      |
| `pt_fire`     | 2.0  | hot but light                               |
| `pt_explode`  | 0.6  | heavy debris, weak drift                    |
| `pt_explode2` | 0.6  | same                                        |
| `pt_blob`     | 0.4  | heaviest shards                             |
| `pt_blob2`    | 0.4  | same                                        |
| `pt_grav`     | 0    | excluded entirely (heavy blood from gibs)   |

`pt_grav`'s slot is set to 0 so the lerp is a no-op even if the guard somehow
slips.

## Edge cases

- **Outside the wind grid** — `Wind_SampleVelocity` writes `(0,0,0)`; the
  `wlen2 > 1.0` guard skips the lerp. Maps with no wind sources have a
  near-zero grid everywhere, so behaviour is unchanged.
- **DLL not loaded / mid-hot-reload** — engine fallback writes `(0,0,0)`;
  same path as outside-the-grid. No crash, no visual glitch.
- **Sub-1 u/s wind** — treated as zero by the guard. Avoids floating-point
  noise in zero cells causing particle creep.
- **Gust impulse spike** — `Wind_AddImpulse` writes a high-magnitude radial
  burst into cells. With `k=4` and 72 fps, `a ≈ 0.055`; a 600 u/s gust pulls
  smoke ~33 u/s in one frame, climbing as the burst lingers. Reads as a real
  push, not a teleport.
- **Particle count** — `MAX_PARTICLES = 2048`. 2048 trilerps plus 2048
  function-pointer calls per frame is negligible.
- **Smoke double-coupling concern** — `pt_smoke` particles are *visual*;
  the smoke *density field* advected inside `sim_wind.c` is what AI senses
  through `Wind_PathOcclusion`. The two are independent. Visual smoke
  particles drifting with wind does not change AI sight LOS — intentional.

## Debug + testing

Three new cvars in `r_part.c`:

- `r_particle_wind_debug` (0/1/2). `0` off, `1` draws a small velocity vector
  at each wind-affected particle, `2` additionally colourises particles by
  their `pt_type` bucket. Re-uses the editor's `R_DrawDebugLine` overlay.
- `r_particle_wind_scale` (default 1.0). Global multiplier on the per-type
  `k` table. Lets us tune feel from the console without a rebuild.
- `r_particle_wind_disable` (default 0). Bypass switch; forces `a=0` for all
  particles. Confirms the feature is the cause of any visual change without
  restarting.

Verification scenes:

1. **Stock `e1m1`** — drop a rocket, watch the explosion shards. With
   `r_particle_wind_disable 1` (or no wind sources in the map), behaviour
   must be byte-identical to today's `r_part.c`. `screenshot_png` before /
   after confirms parity.
2. **`m7_skeleton.map`** — already has an `info_wind_source` and
   `misc_smokegrenade` props. Walk through the wind cone with the rocket
   launcher; explosion shards visibly bend downstream.
3. **Player Gust on grenade smoke** — `+gust` against a cluster of
   `pt_smoke` from a smoke grenade. Smoke sweeps with the radial burst,
   then settles as the impulse decays.

No unit tests — the project has none, and visual particle behaviour is not
a good fit. Verification is the three in-engine scenes above plus the
screenshot parity check.

## Non-goals

- `pt_grav` is excluded. Heavy blood from gibs shouldn't billow.
- The smoke *density* field (AI-sensed, via `Wind_PathOcclusion`) is
  unchanged. Only *visual* `pt_smoke` particles are coupled here.
- No new particle types. Dust motes, grass-stir, rain — all separate
  features.
- No per-particle mass/drag override. A future feature could add a `drag`
  field to `particle_t` for spawn-time variety; YAGNI for now.
- No engine-side mirror of the wind grid. The per-particle ABI hop is
  cheap; revisit only if profiling shows otherwise.
- Decals, pt_smoke "puff" trails on smoke grenades, and other persistent
  visuals are untouched — they have no velocity to couple.

## Tuning notes

Post-implementation playtest revisions to `wind_drag_k[]`:

- **`pt_fire`: 2.0 → 0.4.** `pt_fire` is used exclusively by `R_RocketTrail`
  (rocket and smoke-trail variants), which spawn particles at zero initial
  velocity. With the original `k=2.0`, m7_skeleton's 200 u/s wind source
  yanked trail particles ~100 u/s downstream over their 2 s lifetime,
  smearing the trail away from the rocket's flight path. `k=0.4` (matching
  the heavy-debris bucket of pt_blob / pt_blob2) keeps the trail visually
  anchored behind the projectile while still allowing subtle drift.

Other values held at their initial settings — pt_smoke (4.0) confirmed
correct via Gust-on-smokegrenade visuals on m7_skeleton; explosion and
blob shards untested in-engine because m7's rocket-trail rendering had a
pre-existing issue unrelated to this feature (see commit log).

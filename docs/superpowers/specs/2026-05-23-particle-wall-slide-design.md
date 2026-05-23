# Sliding blood and water particles

Status: design accepted.

## Goal

Blood splatter and water splash droplets that stick to walls should slowly creep downward instead of freezing in place. The effect is purely cosmetic; it should read as drips drooping under gravity without spawning extra particles or running expensive physics.

## Scope

In scope:

- `pt_blood` droplets that stick to a wall (spawned by `R_BloodSpray`, `r_part.c:1185`).
- `pt_grav` water splash droplets that stick to a wall (spawned by `R_RunWaterEffect`, `r_part.c:1104` — the `PARTFL_STICK_ON_HIT | PARTFL_LIQUID_SURF` path).

Out of scope:

- Particles stuck to floors or ceilings (normal too vertical — no slide).
- Particles settled on the liquid surface via `PARTFL_LIQUID_SURF` (those use `birth` as the water-plane Z; they're on a horizontal "floor", not a wall).
- Sparks, smoke, sticky black shotgun specks, gib trails, or any other particle type.
- Trail-spawning, acceleration curves, sliding along sloped floors after dripping off a wall.

## Mechanics

### Detecting a wall stick

At the existing stick site in `R_DrawParticles` (`r_part.c:1650–1657`), when a particle with `PARTFL_STICK_ON_HIT` is parked against a surface, classify the contact:

- If `fabs(tr.plane.normal[2]) < 0.7` → the surface is wall-ish → set a new flag `PARTFL_WALL_STICK = 0x80` on the particle.
- Otherwise (floor or ceiling) → leave the flag clear; the particle freezes as it does today.

`PARTFL_WALL_STICK` occupies the last free bit in `particle_t::flags` (the byte already carries seven flags `0x01..0x40`). No new fields are added to `particle_t`.

### Sliding

After the per-type physics switch and before the death check, add a slide step that runs once per stuck wall-mounted droplet per frame:

```
if ((p->flags & (PARTFL_STUCK | PARTFL_WALL_STICK))
        == (PARTFL_STUCK | PARTFL_WALL_STICK)) {
    float dz = r_particle_slide_speed.value * frametime;
    if (dz > 0.0f) {
        vec3_t newpos = { p->org[0], p->org[1], p->org[2] - dz };
        trace_t tr;
        if (R_TraceParticle(p->org, newpos, &tr)) {
            // Hit something on the way down.
            p->org[0] = tr.endpos[0];
            p->org[1] = tr.endpos[1];
            p->org[2] = tr.endpos[2] + tr.plane.normal[2] * 0.5f;
            p->flags &= ~PARTFL_WALL_STICK;   // stop sliding
        } else {
            p->org[2] = newpos[2];            // clear: keep creeping
        }
    }
}
```

Notes:

- The straight-down step assumes a vertical wall (normal in the XY plane). The droplet retains its original normal offset because moving only in Z doesn't change distance to a wall whose normal has no Z component. For walls with a small Z-component normal (`|n.z| < 0.7` but non-zero), the droplet drifts a fraction of a unit away from or into the wall surface per second — invisible at the scales we care about and bounded by the trace if it intrudes.
- When the slide trace hits anything (floor below, a step out, the corner of an adjacent brush), the droplet stops sliding and freezes. We do not try to keep it sliding along the new surface.
- The slide happens after the type switch, so `pt_blood`'s ramp/dwell timer continues to age the droplet exactly as today. Dripping doesn't extend lifetime.

### Cvar

`r_particle_slide_speed` — float, default `4.0`, clamped to `[0, 32]` on read. Units per second. Default chosen so a droplet creeps roughly one player-height (56u) in ~14 s, visible during combat lulls but not distracting.

Setting the cvar to `0` cleanly disables sliding without requiring a separate toggle (the `if (dz > 0.0f)` guard skips the trace entirely).

## Cost

One `R_TraceParticle` per stuck-on-wall droplet per frame while `r_particle_slide_speed > 0`. `pt_blood` lifetimes range 8–32 s; worst-case combat splatter accumulates on the order of a hundred wall-stuck droplets. A hundred BSP-hull traces per frame is well under a millisecond on the 1996-era software renderer's frame budget.

When `r_particle_slide_speed == 0`, the only added work is one branch per stuck-on-wall droplet per frame.

## Files touched

- `sdlquake/engine_src/d_iface.h` — add `PARTFL_WALL_STICK` flag.
- `sdlquake/engine_src/r_part.c` — set flag at stick site; add cvar registration in `R_InitParticles`; add slide step in `R_DrawParticles`.

No changes to `game.dll`, no `engine_api_t`/`game_api_t` ABI bump (this is engine-internal rendering state).

## Verification

- Manual: fire shotgun blasts and rocket gibs at a wall in e1m1, watch blood droplets droop visibly over ~5–10 s. Fire the lightning gun into a pool of water near a wall, watch the wall-hit splash droplets drip.
- Set `r_particle_slide_speed 0` — droplets freeze as before (regression baseline).
- Set `r_particle_slide_speed 16` — droplets creep noticeably; confirm they stop on contact with floors / ledges and don't tunnel through geometry.
- Confirm pt_blood droplets stuck to floors do NOT translate (regression check on the wall-vs-floor classifier).

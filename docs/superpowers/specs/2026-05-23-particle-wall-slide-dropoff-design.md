# Wall-slide drop-off

Status: design accepted.

Follow-on to [particle wall slide](2026-05-23-particle-wall-slide-design.md): when a sliding droplet reaches the bottom edge of its wall (a doorway top, a ledge, the lip of a window) it should fall under gravity instead of continuing to creep straight down into open air.

## Goal

A wall-stuck blood or water droplet that slides past the bottom of its wall is released to free-fall under the existing per-type gravity code. When it lands on a surface it re-sticks via the existing `PARTFL_STICK_ON_HIT` path. Visual: droplet creeps down a doorway, reaches the top of the opening, drops into the doorway, splats on the floor inside.

## Approach

### Why not precompute with one downward trace

A single downward trace from the impact point can't distinguish "wall extends all the way to the floor" from "wall ends at the top of a doorway" — in the latter case the trace passes through the doorway opening and hits the floor below, which would make the droplet slide all the way down the (non-existent) wall instead of falling into the doorway.

### Why not probe sideways every frame

Correct but expensive: one trace per stuck-wall droplet per frame, ~1800 traces over a 30 s droplet lifetime.

### Binary search at stick time

Find the wall's bottom Z extent once, with a binary search of sideways probes. The probe at a given Z is a 1-unit trace through where the wall surface should be:

```
probe(Z) = R_TraceParticle(impact_xy + n*0.5 at Z,  impact_xy - n*0.5 at Z)
         HIT  → wall surface present at this Z
         MISS → wall surface absent (open air, hole, or past the wall's bottom)
```

Search between `Z_top = impact_z` (known HIT — that's where we just stuck) and `Z_search = impact_z - 256` (search range). 256 units is roughly a player-height column — covers typical Quake wall heights without overshooting into adjacent rooms.

- If `probe(Z_search)` HITs → wall extends past our search range. Set `wall_bottom_z = Z_search` and stop searching. Slide will reach `Z_search` only if the droplet lives long enough; at 4 u/s default that's 64 s, longer than `pt_blood`'s 8–32 s lifetime, so in practice the slide stops on the floor first.
- Otherwise bisect to ~2 u precision: log₂(256/2) = 7 probes per droplet.

Store the found `wall_bottom_z` in `p->vel[2]` (zeroed and unused while STUCK; no new field needed). `p->vel[0]` and `p->vel[1]` stay 0 — the normal is only needed during the binary search itself, which happens at stick time with the normal still in local scope.

### Slide step (per frame)

Zero traces. Just compare:

```c
float dz = r_particle_slide_speed.value * frametime;
if (p->org[2] - dz <= p->vel[2]) {
    // Reached the wall's bottom — disambiguate and release/snap.
    Slide_Release(p);
} else {
    p->org[2] -= dz;
}
```

### `Slide_Release` (one trace at release time)

When the droplet reaches `wall_bottom_z`, do one short downward trace to find out *why* the wall ended:

```c
trace = R_TraceParticle(p->org, p->org - (0, 0, 4));
```

- **Hit with `tr.plane.normal[2] >= 0.7`** (floor immediately below): the wall ends at the floor. Snap to the floor surface (same code as today's floor-stop branch), zero `vel`, clear `WALL_STICK`, keep `STUCK`. Droplet stops, visually identical to today.
- **Hit with `n.z < 0.7`** (a wall or ceiling immediately below): rare — the wall ends at the start of another surface. Treat as "wall continues differently" — snap to the new contact, clear `WALL_STICK`, keep `STUCK`. Droplet stops.
- **No hit** (open air below): clear `STUCK | WALL_STICK`, zero `p->vel`, leave `p->org` where it is. The next integration tick runs the per-type physics — `pt_blood` and `pt_grav` both apply `vel[2] -= grav * 20`, so the droplet accelerates downward. The existing `PARTFL_STICK_ON_HIT` flag is still set (we never cleared it), so on the next collision the existing stick branch fires and the droplet re-sticks to whatever it lands on (floor, another wall lip — emergent re-slide is fine).

## Mechanics summary

| Phase | Traces | Where |
|---|---|---|
| Stick (existing) | 1 (the impact trace from integration) | already there |
| Stick (new) | 7 (binary search) | new helper at the existing stick site |
| Slide (per frame) | 0 | existing slide-step site, rewritten |
| Release | 1 (disambiguation) | new helper called from slide step |

Total per droplet lifetime: 9 traces (vs. ~1800 for the per-frame approach).

## Cost

- **At spawn:** ~7 extra traces per wall-stuck droplet. For a 50-droplet `R_BloodSpray` against a wall, that's ~350 sideways probes in one frame, well under a millisecond — invisible during combat.
- **Per slide frame:** zero. The slide step is now a single `<=` compare and an assignment.
- **At release:** 1 trace, fires once per droplet.

Net result: substantial steady-state savings during combat. Long-lived pt_blood droplets (8–32 s) gain the most.

## Caveats

- **Wall with a mid-height hole** (e.g., a wall containing a window slot): binary search converges to the top of the hole, so droplets above the hole fall into it instead of past it. For id1 maps this is rare; visually the result is benign — blood falling into a window slot reads correctly.
- **Walls taller than 256 units:** the search saturates at `Z_search` (`impact_z - 256`). Droplets keep sliding until they hit the (capped) bottom, then run the disambiguation trace — same release behavior. Worst case is a 256u-tall wall with a doorway underneath the search range — droplet would slide to the search cap, run the disambig trace, and probably hit the doorway top correctly anyway.
- **Slanted walls** (normal with non-zero Z component, `|n.z| < 0.7`): binary search probes along straight-down Z while the wall surface drifts laterally. Probes may go slightly off the wall surface for very slanted walls (e.g., `|n.z| = 0.6`), causing early `MISS` and an overly-high `wall_bottom_z`. Droplet releases higher than it should — visually it would just look like the droplet sheds the wall sooner than expected. Acceptable.

## Storage layout while `STUCK | WALL_STICK`

| Field | Meaning |
|---|---|
| `p->vel[0]` | 0 (unused) |
| `p->vel[1]` | 0 (unused) |
| `p->vel[2]` | `wall_bottom_z` (absolute world Z) |

On release: `p->vel[2]` is zeroed before the integration block runs (so the type switch's velocity-update assumptions hold from rest).

## Files touched

- `sdlquake/engine_src/r_part.c` — modify the stick branch (set `WALL_STICK`, run binary search, populate `p->vel`); rewrite the slide step; add a `Slide_Release` helper (or inline it).

No header change, no new flag, no new cvar, no ABI bump.

## Verification

- Stand on the start of e1m1, fire shotgun at a wall above a doorway. Blood droplets should slide down, reach the top of the doorway, fall into the doorway, splat on the floor below.
- Splatter blood on a wall that extends flush to the floor. Droplets should slide all the way down and stop at the floor (regression check — same as today).
- `r_particle_slide_speed 16` — verify fast slide still releases correctly at doorway tops without overshooting through the floor.
- `r_particle_slide_speed 0` — droplets freeze in place (binary search still runs at stick time, but the slide step never triggers a release because dz is 0).
- Water splash on a wall above a pool: droplets slide, then drop into the water, then settle on the surface (via the existing `PARTFL_LIQUID_SURF` path — that path runs in the integration block once STUCK is cleared, so it should still work for water splashes specifically).

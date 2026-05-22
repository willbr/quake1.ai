# Hitscan impulse on gibs

## Problem

Shooting a gib with shotgun / super shotgun / nailgun produces no visible
movement. Explosions scatter gibs correctly. The cause: the impulse code in
`T_Damage`'s gib branch (`combat.c:289`) uses `damage * 8` for the velocity
scale, which was tuned down from ×40 to keep rockets from sending heads
flying — but that multiplier leaves a single hitscan pellet (4 dmg) at only
32 ups of impulse.

## Design

Apply hitscan-specific impulse at the hit site, where the bullet's direction
is in scope, instead of trying to reconstruct it from inflictor midpoint in
`T_Damage`.

### `sdlquake/game/weapons.c` — `TraceAttack`

In the existing `if (g->trace_ent->v.takedamage)` branch, after
`AddMultiDamage`, detect gibs and apply impulse along the ray direction:

- `v.velocity += dir * damage * 40`
- `v.velocity.z += 30` (small lift; matches the explosion path)
- `v.avelocity` randomized to ±400 each axis (tumble)
- clear `FL_ONGROUND`

`dir` is the normalized ray direction already passed as the `TraceAttack`
parameter, so no reconstruction needed.

### `sdlquake/game/combat.c` — `T_Damage` gib branch

The existing gib branch (`combat.c:279`) computes direction from inflictor
midpoint, applies impulse, and schedules a +0.5s `SUB_Remove`.

Two policy changes:

1. **Only explosions destroy gibs.** Hitscan / projectile hits push the
   gib but leave its lifetime alone — the persistent-gib system handles
   long-term cleanup. The +0.5s timed-removal moves *inside* the
   non-client / non-world branch so only explosions take that path.
2. **No double-impulse.** When `inflictor` has `FL_CLIENT` (the player
   firing hitscan), skip the entire velocity-application block — the hit
   site already applied the correct impulse along the bullet direction.

Detection: `((int)inflictor->v.flags & FL_CLIENT) != 0`.

Rockets and grenades are not clients, so they still hit the velocity
+ scheduled-remove path.

## Nailgun / super-nailgun (projectile path)

Initial assumption was that spike projectiles would route through
`spike_touch` / `superspike_touch` the same way as hitscan via TraceAttack.
They don't: gibs are `SOLID_TRIGGER`, the engine's missile trace
(`SV_Move` with `MOVE_MISSILE`) only consults `solid_edicts`, and
`SV_TouchLinks` at the spike's final wall position only fires a touch when
the spike's bbox overlaps the gib's bbox *at the link point*. A 1000ups
spike traveling ~14 units per server tick will routinely skip past a small
8-unit gib bbox in a single frame.

Fix: a pre-physics sweep each tick from a new `Spike_GibPathScan()`
function in `weapons.c`, called from `game_start_frame` before the engine
runs physics. For every in-flight `MOVETYPE_FLYMISSILE` with classname
`"spike"`:

1. Sweep the segment `origin → origin + velocity * dt` (dt = 0.02, an
   over-estimate of one tick) against every gib's AABB using the slab
   method.
2. On the closest hit: spawn blood, apply
   `gib_apply_hit_impulse(gib, velocity_normalized, damage)`, and
   `ED_Free` the spike before physics moves it.

Over-estimating dt means a spike can be freed up to ~1 frame before it
would visually reach the gib (a fast tiny sprite — imperceptible).
Under-estimating would miss gibs at lower framerates, so we err high.

Super-spike vs. regular spike damage (18 vs. 9) is discriminated by the
touch handler pointer (`e->v.touch == superspike_touch`) — both share the
`"spike"` classname.

## Verification

No test suite. Manual: spawn into a map with corpses, kill a monster to
spawn gibs, shoot each gib with shotgun / SSG / nailgun / super-nailgun and
confirm visible motion along the shot direction. Shoot a gib with a rocket
and confirm the existing scatter behavior is unchanged.

## Out of scope

- Tuning the explosion-path multiplier — that case already works.
- Knockback on living monsters from hitscan — handled separately by
  `v_angle`/`punchangle` and the existing damage knockback path.

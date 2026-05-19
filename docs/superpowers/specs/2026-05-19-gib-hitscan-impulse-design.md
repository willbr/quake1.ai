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
midpoint and applies a smaller impulse. This still runs after the new
hitscan path adds `multi_damage`, producing a redundant secondary impulse.

Suppress the redundancy: skip the velocity-application portion when
`inflictor` is a client (i.e. hitscan). The branch still runs the timed-
removal scheduling so the gib pops after the fling.

Detection: `((int)inflictor->v.flags & FL_CLIENT) != 0`.

The radius-damage (explosion) path is unchanged — rockets/grenades are not
clients, so they retain the existing ×8 inflictor-midpoint impulse.

## Verification

No test suite. Manual: spawn into a map with corpses, kill a monster to
spawn gibs, shoot each gib with shotgun / SSG / nailgun and confirm visible
motion along the shot direction. Shoot a gib with a rocket and confirm the
existing scatter behavior is unchanged.

## Out of scope

- Tuning the explosion-path multiplier — that case already works.
- Knockback on living monsters from hitscan — handled separately by
  `v_angle`/`punchangle` and the existing damage knockback path.

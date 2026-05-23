# Shootable pickup pushback

Shooting (or splash-damaging) a pickup should impart a visible velocity impulse, so ammo, health, armor, weapons, powerups and keys skid/tumble when hit. They remain indestructible — only movable.

## Why this needs work

Pickups are `SOLID_TRIGGER + MOVETYPE_TOSS + FL_ITEM`. `MOVE_NORMAL` bullet traces skip `SOLID_TRIGGER`, so projectiles currently pass through pickups without registering. We need a parallel scan pattern to detect hits, matching the existing precedent set by `Spike_GibPathScan` and `Corpse_BulletTrace`.

## Approach

A new module `sdlquake/game/items_push.c` exposes:

- **`Items_BulletSweep(start, end, dir, damage, ignore)`** — per-pellet / per-trace segment vs. AABB sweep over all live `FL_ITEM` entities. On hit, apply impulse and continue (the caller's wall trace is unaffected — bullets still mark the wall behind/beside).
- **`Items_RadiusPush(origin, radius, base_impulse, ignore)`** — explosion-time loop, linear distance falloff, push direction = normalized `(item.origin - origin)`.

### Impulse formula

```c
vel  += dir * (damage * 6.0f);
vel[2] += 50.0f;                // small upward pop so it tumbles, not just skids
avelocity = random_spin();      // ~200°/s on each axis, random sign
// clamp |vel| <= 400 u/s
```

### Live-item filter

```c
(self->v.flags & FL_ITEM) && self->v.solid == SOLID_TRIGGER && self->v.modelindex != 0
```

Catches just-consumed pickups (which set `SOLID_NOT` + `model = NULL` after touch) and respawning DM items (also `SOLID_NOT` between regen ticks).

## Wire-up sites

| Weapon | Site | Call |
|---|---|---|
| Shotgun / Super shotgun | pellet trace loop in `FireBullets` (weapons.c) | `Items_BulletSweep` |
| Lightning | beam trace | `Items_BulletSweep` |
| Nails / spikes | extend `Spike_GibPathScan` (combat.c/weapons.c) to include `FL_ITEM` ents | inline |
| Rockets / grenades | next to the splash-damage call site (`T_RadiusDamage`) | `Items_RadiusPush` |
| Axe | skipped — melee on a health box reads weird, and the user said "shooting" |

## Out of scope

- Damaging / destroying pickups — indestructible by design.
- Hit-thunk SFX — could add later; skipping for now.
- AI stim broadcasting — skidding items shouldn't alert monsters.
- Backpacks are `FL_ITEM` and will become pushable; deliberate, feels right.

## Risks

- A direct rocket on a tightly-packed item cluster could fling items into unreachable geometry. The 400 u/s velocity clamp and the existing `MOVETYPE_TOSS` floor-snap should keep this rare.
- Items that get pushed onto angled surfaces may slide forever in vanilla `SV_Physics_Toss`. Acceptable — same behavior as backpacks today.

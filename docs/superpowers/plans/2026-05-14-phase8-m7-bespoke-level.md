# Phase 8 / M7 — Bespoke mini-level design

**Date:** 2026-05-14
**Status:** Skeleton committed; polish + multi-room layout deferred
**Phase:** 8 (M3-M6 complete in code; M7 is content)

## Goal (from spec)

Three connected areas, ~15 enemies, soft "reach the slipgate unseen" objective. Hand-placed patrols, grates, props, light/dark zones, smoke sources. Three distinct viable playthroughs (combat, stealth, blink-traversal-heavy).

## What ships now

`id1/maps/m7_skeleton.map` is a single 1024×512 room with an internal partition wall and a corridor between the two halves. It exercises *every* Phase 8 system at least once:

| System | M7 hookup |
|---|---|
| Stimulus bus (M1) | Monsters' Sim_AI_RegisterMonster brains receive STIM_SOUND on player shots / Blink whoosh / Gust impact. |
| AI FSM + sense filter (M2) | 3 grunts + 1 ogre in IDLE→SUSPICIOUS→SEARCHING→COMBAT. |
| Navmesh A* (M2.5) | sim_nav bake on first map load; SEARCHING grunts hunt via A*. |
| Blink + Gust (M3) | Cvars `ph_*` tunable. q to Blink, f to Gust. |
| Wind / smoke (M4) | `misc_smokegrenade` at (0,0,32) persistently fills the central corridor. `info_wind_source` at (0,0,64) blows the smoke eastward, so the east room fills up over time. |
| Light tier (M5) | West room lit by a `light`, east room by two `light_torch_small_walltorch`. Gust extinguishes the torches; their darkness override propagates to AI sense filter. |
| Retrofit (M6) | Monsters automatically get patrol routes from the navmesh at level-init. |

The slipgate is a `trigger_changelevel` brush in the east room that transitions to `e1m1`.

## How to compile and run

```
zig build run -- +map m7_skeleton
```

If the .bsp is missing, open the editor (F8) and run:

```
editor_compile_export
```

The qbsp + vis + light pipeline writes `id1/maps/m7_skeleton.bsp` and the matching `.lit`. Reload the map.

## Verify (smoke)

* In the east room, smoke billows in over ~10s -- monsters lose LOS.
* `notarget` on, walk past the grunts -- they stay IDLE.
* `notarget` off, fire shotgun in the west room -- grunts in the corridor turn yellow (SUSPICIOUS) in the imgui AI overlay.
* Gust an east-room torch -- AI sense range drops in that area (visible by spawning a grunt there and watching alert level).
* Blink through the corridor -- player teleports to a valid landing; phase_energy drains by 25.

## Remaining design work (the actual M7)

The spec calls for "three distinct viable playthroughs." That is content work that benefits from playtesting:

1. **Combat path** -- frontal assault, shotgun + nailgun pickups in the west.
2. **Stealth path** -- crouch through smoke + shadowed alcoves, Gust torches to extend dark zones.
3. **Blink-heavy path** -- vertical traversal via Blink onto raised ledges, `func_grate` partitions that only the player can pass.

For each path the level should reward a different verb combination. The skeleton has the *systems* wired but the *geometry* is one room -- expanding to three connected areas needs the editor and playtesting iteration.

Suggested next steps (when picking M7 back up):

* Cut the skeleton into three real rooms: west (combat-friendly, lit), centre (smoke + dark), east (vertical + grates + slipgate).
* Add 3-4 `func_grate` partitions so Blink has signature traversal options.
* Place 10-12 more monsters (mix of grunts, dogs, an enforcer or two).
* Hand-place `info_patrol_node` entities -- the retrofit auto-generator's routes are uniform; hand-placed routes can guide the player into encounters.
* Tune `ph_*` cvars based on playtest feel.

## Why this isn't a real bespoke level yet

Level design is a creative + playtest loop, not something to do in one batch overnight. The engine + systems support is complete (M3-M6 shipped). The skeleton proves the pipeline, and M7 becomes a focused content pass when the user picks it up next.

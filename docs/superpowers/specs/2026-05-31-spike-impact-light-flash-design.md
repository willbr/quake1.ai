# Spike-impact light flash — design

**Date:** 2026-05-31
**Status:** approved (design)
**Scope:** engine-side only (`sdlquake/engine_src/`); no protocol change, no `game.dll` change, no ABI bump.

## Summary

When a spike hits a wall, emit a brief muzzle-flash-style dynamic light at the
impact point so the hit visibly throws light on the surface. Applies to all
four spike-impact temp entities: the player's Nailgun (`TE_SPIKE`) and Super
Nailgun (`TE_SUPERSPIKE`), plus enemy spikes from the Scrag (`TE_WIZSPIKE`) and
Hell Knight (`TE_KNIGHTSPIKE`). Shotgun pellets (`TE_GUNSHOT`) are deliberately
excluded.

## Motivation

Spike impacts currently spawn particles, a decal, and a ricochet/tink sound but
cast no light. A short flash — the same mechanism as the gun's own muzzle flash
— makes nailgun fire feel punchier and reads especially well in dim areas,
where a stream of nails momentarily stipples the wall with light.

## Approach

Client-side dynamic light in `CL_ParseTEnt` (`cl_tent.c`), reusing the existing
`CL_AllocDlight` system. This mirrors the muzzle flash and explosion flash that
already live in the same function — same `dlight_t` fields (`origin`, `color`,
`radius`, `die`), same warm-yellow `DLIGHT_COLOR_MUZZLE`.

The impact position is already broadcast in each spike temp entity (read into
`pos`), so nothing new goes on the wire and the game DLL is untouched.

### Rejected alternatives

- **Server-side effect flag / new temp entity.** Would need an ABI + protocol
  change to carry information that is already on the wire. No benefit.
- **Particle-only bright sparks.** Cheaper (no surface relight), but does not
  actually illuminate the wall — the request is a *flash of light, like a
  muzzle flash*, i.e. a real dlight on the surface.

## Design

In each of the four spike cases in `CL_ParseTEnt`, after the existing
particle/decal/sound work, add (guarded by the new cvar):

```c
if (cl_spikeflash.value)
{
    dl = CL_AllocDlight (0);              // key 0 = transient, like the explosion flash
    VectorCopy (pos, dl->origin);
    VectorCopy (DLIGHT_COLOR_MUZZLE, dl->color);
    dl->radius = <per-type>;
    dl->die = cl.time + 0.06;            // snappy; muzzle flash is 0.1
}
```

`dl` is already declared at the top of `CL_ParseTEnt`; `rand()`, `cl.time`, and
`CL_AllocDlight` are all already in use in this function.

### Per-type parameters

| Temp entity     | Source             | radius              |
|-----------------|--------------------|---------------------|
| `TE_SPIKE`      | Nailgun            | `80 + (rand()&15)`  |
| `TE_SUPERSPIKE` | Super Nailgun      | `100 + (rand()&15)` |
| `TE_WIZSPIKE`   | Scrag              | `90 + (rand()&15)`  |
| `TE_KNIGHTSPIKE`| Hell Knight        | `90 + (rand()&15)`  |

Common to all four:

- **Color:** `DLIGHT_COLOR_MUZZLE` (warm yellow) for a cohesive "impact spark"
  across player and enemy spikes. Per-type tinting (green Scrag / red Knight)
  was considered and deferred — cohesive warm-yellow is the chosen default.
- **Duration:** `die = cl.time + 0.06` — a quick snap, deliberately shorter
  than the 0.1 s muzzle flash. No `minlight` and no `decay`, so the flash does
  not linger as a floor or trail off.
- **Radius:** deliberately "little" — well under the 200-radius muzzle flash;
  the Super Nailgun reads a touch beefier.

### Cvar

`cl_spikeflash` — new client cvar, default `"1"`, registered in `CL_Init`
(`cl_main.c`) alongside the other `cl_*` cvars. Lets the player disable the
effect if it feels busy during sustained Super Nailgun fire. Matches the
codebase's cvar-heavy house style.

## Files touched

- `sdlquake/engine_src/cl_tent.c` — dlight block in the four spike cases.
- `sdlquake/engine_src/cl_main.c` — define + register `cl_spikeflash`.

~15 lines total.

## Performance

Short-lived, small-radius dlights re-light surfaces in range each frame they
are alive (`R_PushDlights` / `R_MarkLights`). At a 0.06 s lifetime, roughly one
is alive per nail stream at any instant, so the added cost is negligible. A
dense crowd of Scrags could briefly stack several; the `cl_spikeflash 0` toggle
is the escape hatch. `CL_AllocDlight`'s oldest-slot reuse bounds the worst case
regardless.

## Verification

No automated test suite; verified live in-game:

1. Fire the Nailgun at a wall in a dim area — confirm a brief warm pool of
   light at each impact.
2. Fire the Super Nailgun — confirm a slightly larger flash.
3. Let a Scrag and a Hell Knight fire spikes at a wall — confirm their impacts
   flash too.
4. `cl_spikeflash 0` — confirm the flash disappears while particles/decal/sound
   remain.

## Out of scope

- Shotgun/Super Shotgun pellet impacts (`TE_GUNSHOT`).
- Per-monster color tinting of enemy spikes (possible easy follow-up: two new
  `DLIGHT_COLOR_*` constants).
- Any change to the spike *particle*, *decal*, or *sound* behaviour.

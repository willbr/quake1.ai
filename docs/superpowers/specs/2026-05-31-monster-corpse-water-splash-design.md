# Monster & Corpse Water-Entry Splash — Design

**Date:** 2026-05-31
**Status:** Approved (design); implementation pending
**Scope:** One engine-side branch in `SV_CheckWaterTransition`. No protocol change, no `game_api` ABI bump.

## Goal

When a monster (alive or dead) crosses from air into a liquid, it should throw a
person-sized splash, matching the splash the player already makes. Today only the
player produces a visible plunk; monsters and corpses appear to make no splash.

User-chosen parameters (from brainstorming):

- **Effect:** visual particles (the entry sound already fires; keep it as-is).
- **Liquids:** water, slime, and lava — tinted per liquid.
- **Trigger:** any air→liquid entry, with splash size **scaled by entry speed**.

## Current state (what already exists)

This fork already has a full water-splash system. This feature extends it; it does
not build it.

- **`TE_WATERSPLASH`** — a custom temp entity (`protocol.h:174`, value 16). Wire
  format: `pos[3]` + `kind` byte (`0`=water, `1`=slime, `2`=lava) + `strength`
  byte. The client parses it at `cl_tent.c:301`, renders tinted droplet particles
  via `R_WaterSplash(pos, kind, strength)`, **and** plays a positional `h2ohit`
  sound. Per-liquid tinting is therefore already handled — the producer only has to
  pass the correct `kind`.

- **`SV_CheckWaterTransition`** (`sv_phys.c:1305`) is the single engine chokepoint
  for air↔liquid transitions. On an air→liquid crossing it plays `misc/h2ohit1.wav`
  and, **only if `vmag >= 100`**, walks up to the surface (4096u "air above" probe +
  14-step binary search) and emits a `TE_WATERSPLASH` with
  `strength = clamp(vmag * 0.03, 8, 16)` (≈1× bullet). Rockets (`classname
  "missile"`) emit 4 bursts and grenades 3, at small random XY offsets; everything
  else emits 1. The datagram is budgeted (`cursize >= MAX_DATAGRAM - 16` breaks).
  It is called from `SV_Physics_Toss` (`sv_phys.c:1558`, `:1640`) and
  `SV_Physics_Step` (`:1765`, `:1795`).

- **The player** does **not** go through `SV_CheckWaterTransition`
  (`SV_Physics_Client`'s `MOVETYPE_WALK` path uses `SV_WalkMove`, which calls
  `SV_CheckWater` — no splash). The player's plunk comes from a game-side path:
  `client.c` `WaterMove` (line ~567), on the `FL_INWATER` transition, calls
  `splash_underwater_explosion(feet, 96)` plus a second burst ~32u ahead. Strength
  **96 ≈ 6× bullet, two bursts**. `splash_underwater_explosion` (`weapons.c:545`)
  emits the same `TE_WATERSPLASH`.

## The gap

Monsters and corpses only get the engine path, which:

1. Is **hard-gated at `vmag >= 100`**, so slow wade-ins and corpses slumping in emit
   nothing at all; and
2. Even when it fires is a **1× projectile flick** (strength 8–16), invisible next
   to the player's deliberate 6× double-burst.

Net effect in-game: monsters and corpses read as "no splash at all."

## Load-bearing facts (verified)

- **`FL_MONSTER` (=32, `game_defs.h:11`) is never cleared on death.** No `& ~FL_MONSTER`
  exists in the game code; thrown heads explicitly keep it (`weapons.c:642`). So a
  single `flags & FL_MONSTER` test identifies live monsters, corpses, and thrown
  heads alike, while excluding gibs (`classname "gib"`, no flag) and projectiles.
- **Corpses keep `MOVETYPE_STEP`.** `Corpse_LayProne` (`combat.c:83`) changes only
  `solid` and bbox size; it does not touch `movetype`. Heads become `MOVETYPE_BOUNCE`.
  Both movetypes flow through `SV_CheckWaterTransition`, so all bodies are already
  covered by the chokepoint.

## Design

Add one branch to `SV_CheckWaterTransition`, evaluated **before** the existing
`missile`/`grenade` classname logic. A "body" is any entity with `FL_MONSTER` set.

### Control flow

```
is_body = (flags & FL_MONSTER) && sv_bodysplash != 0

if (is_body || vmag >= 100):          // bodies bypass the old speed gate
    if (no air within 4096u above):   // existing guard — skip detached splashes
        skip
    surface_z = binary-search up to the air/liquid boundary   // existing code, shared
    kind = 0/1/2 from the contents entered                    // existing code, shared

    if is_body:
        strength = clamp(32 + vmag * 0.16, 32, 96)
        n_bursts = 2
        offset_r = 10
    else:                              // unchanged projectile path
        strength = clamp(vmag * 0.03, 8, 16)
        n_bursts / offset_r from missile / grenade / default

    emit n_bursts × TE_WATERSPLASH at surface_z (random XY within offset_r),
        kind, strength   // existing emit loop, datagram-budgeted
```

The surface-find, `kind` selection, datagram budgeting, and emit loop are the
existing code; only the gate condition and the per-body `strength`/`n_bursts`/
`offset_r` are new.

### Strength curve

`strength = clamp(32 + vmag * 0.16, 32, 96)`

| Entry speed (u/s) | Strength | Feel |
|---|---|---|
| ~0 (slump/wade) | 32 (≈2× bullet) | small but visible |
| ~150 (walk-in) | ~56 (≈3.5×) | clear ripple |
| ~400+ (fall) | 96 (=player plunk) | full body splash |

Ceiling 96 matches the player's fixed plunk; the floor of 32 honours "any entry"
by keeping near-stationary entries visible. `vmag` is the full velocity magnitude
(already computed in the function).

### Classification precedence

`FL_MONSTER` is checked first. Gibs (`classname "gib"`, no flag) and projectiles
fall through to the unchanged projectile path, so their existing subtle splashes are
preserved.

### Cvar

- **`sv_bodysplash`** — float, default `1`. Register near the other `sv_` cvars
  (e.g. where `sv_gravity` is registered). When `0`, the body branch is skipped
  entirely and bodies revert to current behaviour (projectile path only, gated at
  `vmag >= 100`). This is a live A/B kill-switch for playtesting.

The floor (32), ceiling (96), speed scale (0.16), burst count (2), and offset
radius (10) stay as named constants in `SV_CheckWaterTransition`; tune by eye.

## Components touched

- `sdlquake/engine_src/sv_phys.c` — the new branch in `SV_CheckWaterTransition`;
  declare/register `sv_bodysplash` (cvar declaration near other `sv_` cvars,
  `Cvar_RegisterVariable` in the server cvar init).

No other files change. `TE_WATERSPLASH`, `R_WaterSplash`, the protocol, and the
`game_api` ABI are all untouched.

## Edge cases

- **Corpses / thrown heads:** covered by `FL_MONSTER` + Step/Toss physics (verified
  above).
- **Swimming monsters (fish, `FL_SWIM`):** spawn already in liquid, so `watertype`
  is preset on spawn and the `!watertype` early-return suppresses a spurious splash.
  A fish that leaps out and re-enters will splash — acceptable, even desirable.
- **Deeply submerged entries:** the "air within 4096u above" guard skips them, as
  today, so a splash never floats detached from a surface.
- **Sound:** a body now emits `misc/h2ohit1.wav` (via `SV_StartSound`) plus the
  TE's positional `h2ohit` — the same mild doubling the projectile path already has,
  and still quieter than the player (who adds `player/inh2o.wav` on top). Left as-is;
  revisit only if it sounds wrong in playtest.
- **Performance:** transitions are rare (only on an actual crossing); the binary
  search runs only on that frame. Negligible even with many monsters.
- **Datagram pressure:** unchanged — the existing per-burst `MAX_DATAGRAM - 16`
  break still applies; bodies add at most 2 bursts.

## Out of scope

- Unifying the player's fixed-96 plunk with this speed-scaled curve (the player path
  is separate and works; leave it).
- Any AI / stimulus reaction to the splash (purely cosmetic for now).
- Gib splash tuning (left at current subtle behaviour).

## Testing

Build, then verify in-game (the project's standard is behaviour confirmation, not
just compilation):

1. Drop a grunt and an ogre into **water**, **slime**, and **lava** (fight them at a
   pool edge, or position them with MCP). Confirm a person-sized, correctly-tinted
   splash on both the live fall-in **and** the corpse going under.
2. Confirm a **slow wade-in** still produces a smaller (but visible) splash.
3. Confirm **gibs** are unchanged (still subtle).
4. Toggle `sv_bodysplash 0` and confirm bodies revert to the old near-invisible
   behaviour; `sv_bodysplash 1` restores it.

# Doom Pistol Polish — Design Spec

**Date:** 2026-05-07
**Phase:** 6 (Wolf3D + Doom1 weapon port)
**Scope:** Polish-pass on the Doom pistol — the only Phase 6 weapon that's fully wired up — bringing it from "functional" to "feels like Doom".

## Goal

Lift the Doom pistol from its current functional-but-rough state to one that visually and behaviorally matches Doom's pistol (`S_PISTOL1`..`S_PISTOL4` + `S_PISTOLFLASH` in `p_pspr.c`). Four discrete polish items, all in scope:

1. Visible muzzle-flash sprite overlay during firing (PISGB0+PISFA0 already composited at extract time, but currently shown for too short to perceive).
2. Authentic Doom per-frame tic timing (4/6/4/5 instead of uniform 0.1 s).
3. Doom-authentic out-of-ammo behavior (stay on the empty pistol, silent — replaces today's auto-swap to a Quake weapon).
4. Pause-respecting weapon bob (currently advances during pause/console because it reads `cl.time` directly).

## Non-goals

- Other Phase 6 weapons (DoomShotgun, DoomChaingun, etc. remain `void X(void) {}` stubs).
- Out-of-ammo dry-click sound (Doom plays no sound on dry trigger; we match Doom).
- A full Doom psprite-style two-track animation (separate `ps_flash` slot drawn over the gun). The composited approach we already use is good enough once timing is fixed.
- Any change to world-space sprite rendering, HUD, or palette LUT plumbing — these were settled in prior Phase 6 work.

## Background

The Doom pistol is currently implemented across:

- `sdlquake/game/weapons_phase6.c::W_FirePhase6_DoomPistol` — fires the hitscan, decrements ammo, plays sound, sets `EF_MUZZLEFLASH`, kicks off the animation chain.
- `sdlquake/game/player_phase6.c` — animation chain `player_doompistol1..5_think`, uniform 0.1 s/frame, four attack frames before returning to idle.
- `sdlquake/engine_src/r_sprite.c::R_DrawViewModelSprite` — bottom-centered screen-space blit with Doom-style weapon bob driven by `cl.velocity` magnitude and `cl.time`.
- `tools/extract_phase6/manifest.zig::doom_pisg_frames` — five-frame manifest (PISGA0, PISGB0+PISFA0, PISGC0, PISGD0, PISGE0); the extractor's `compositeFlash` helper aligns Doom's per-sprite leftoffset/topoffset so the flash sits over the muzzle.

Doom's reference state machine (from `p_pspr.c`):

```
S_PISTOL1     PISGA  4 tics  NULL
S_PISTOL2     PISGB  6 tics  A_FirePistol     ← bullet leaves here, ps_flash := S_PISTOLFLASH
S_PISTOL3     PISGC  4 tics  NULL
S_PISTOL4     PISGB  5 tics  A_ReFire
S_PISTOLFLASH PISFA  7 tics  fullbright       ← overlaid as ps_flash, lasts 7 of the 19 chain tics
```

Two things in this state machine drive our polish design:

- **Pre-fire idle hold.** S_PISTOL1 holds PISGA0 for 4 tics (~0.114 s) before A_FirePistol runs in S_PISTOL2. The bullet does not leave on the press; there's a 4-tic lockout that's part of Doom's feel.
- **PISGB0 appears twice.** Once in S_PISTOL2 (with the flash) and once in S_PISTOL4 (without). Our current SPR has PISGB0 only with the flash composited in; we need an unflashed variant for the second showing.

## Architecture

### Decision 1 — replicate Doom's 4-tic pre-fire lag

`W_FirePhase6_DoomPistol` no longer fires the bullet itself. It performs the ammo check, sets `attack_finished`, kicks off the animation chain at `player_doompistol1`, and returns. The bullet, sound, muzzle-flash dlight, and punchangle move into `player_doompistol2_think`, which runs after the 4-tic idle hold.

User-facing consequence: pressing fire shows the idle pose for ~0.114 s, then the recoil pose (with flash) appears as the bullet leaves. This is Doom feel and was explicitly chosen over Quake-style instant-fire.

### Decision 2 — drop unused PISGD0/PISGE0; add unflashed PISGB0

`v_doompistol.spr` shrinks from 5 frames to 4. PISGD0/PISGE0 are not referenced by Doom's pistol state machine and were extracted defensively. We replace them with a second PISGB0 frame that has no flash composited, used in the recoil-settle phase. New extractor manifest:

```zig
const doom_pisg_frames = [_]DoomFrameSpec{
    .{ .lump = "PISGA0" },                       // frame 0: idle / pre-fire hold
    .{ .lump = "PISGB0", .flash = "PISFA0" },    // frame 1: fire (composited flash)
    .{ .lump = "PISGC0" },                       // frame 2: smoke
    .{ .lump = "PISGB0" },                       // frame 3: recoil settle (no flash)
};
```

Re-extracting regenerates the file. No engine code reads frame indices ≥ 4 for the pistol, and the new layout matches the chain below 1:1.

### Decision 3 — animation chain with Doom's 4/6/4/5 tic distribution

```
press fire (W_FirePhase6_DoomPistol)
   │
   ├─ if ammo < 1: return silent (no chain, no swap)
   ├─ attack_finished = g->time + 0.543f
   ├─ punchangle = -1 (moved here so it's set on chain start, not on bullet)
   └─ player_doompistol1_think (kicks off chain)

player_doompistol1_think  (4 tics ≈ 0.114 s)  weaponframe = 0  (PISGA0 idle)
   nextthink = +0.114, think = player_doompistol2_think

player_doompistol2_think  (6 tics ≈ 0.171 s)  weaponframe = 1  (PISGB0 + flash)
   ├─ ammo_bullets -= 1; currentammo = ammo_bullets
   ├─ EF_MUZZLEFLASH on player effects
   ├─ SV_StartSound CHAN_WEAPON "phase6/doom_pistol.wav"
   ├─ p6_fire_bullet(damage, aim, 0.01, 0.01)   ← hitscan with spread
   └─ nextthink = +0.171, think = player_doompistol3_think

player_doompistol3_think  (4 tics ≈ 0.114 s)  weaponframe = 2  (PISGC0 smoke)
   nextthink = +0.114, think = player_doompistol4_think

player_doompistol4_think  (5 tics ≈ 0.143 s)  weaponframe = 3  (PISGB0 no flash)
   nextthink = +0.143, think = player_doompistol5_think

player_doompistol5_think
   weaponframe = 0; player_run(self)
```

Total chain: 19 tics ≈ 0.543 s. `attack_finished = g->time + 0.543f` gates the next press through the existing `button0 && time >= attack_finished` check in the engine; A_ReFire's "still holding fire?" semantics fall out for free.

`punchangle[0] = -1` migrates from `W_FirePhase6_DoomPistol` (where it was set on press, before the bullet) to whichever site keeps it visually correct — leading candidate is `player_doompistol2_think` so it lines up with the actual recoil moment. Implementation can land it either way; both feel correct in casual play.

### Decision 4 — out-of-ammo: silent stay on the empty pistol

`W_FirePhase6_DoomPistol`'s ammo branch becomes:

```c
if (self->v.ammo_bullets < 1) {
    // Doom-authentic: keep the pistol up, do nothing on press.
    // Player must switch weapons manually (impulse) or pick up another.
    return;
}
```

No chain, no sound, no swap. Returning without setting `attack_finished` means the engine re-polls us every frame the fire button is held — fine; the function is a few comparisons and a return.

User-facing consequence: holding fire on an empty pistol has zero visual or audio effect. The pistol stays selected. To switch weapons the player uses an impulse (existing 30..39 mapping) or picks up a new weapon.

### Decision 5 — pause-respecting bob phase accumulator

`R_DoomViewBobAmount` computes amplitude from `cl.velocity` and is unchanged. The phase term moves from a direct `cl.time * (2π / period)` calculation to an accumulator that's gated on `!cl.paused`. New file-static state in `r_sprite.c`:

```c
static float  r_doom_bob_phase     = 0.0f;
static double r_doom_bob_last_time = 0.0;
```

Updated body inside `R_DrawViewModelSprite`, replacing the current `angle = cl.time * ...` line:

```c
if (!cl.paused)
    r_doom_bob_phase += (float)((cl.time - r_doom_bob_last_time)
                                * (2.0 * M_PI / DOOM_BOB_PERIOD));
r_doom_bob_last_time = cl.time;

bob = R_DoomViewBobAmount();
if (bob > 0.0f) {
    sx += (int)(bob * cos (r_doom_bob_phase));
    sy += (int)(bob * fabs(sin(r_doom_bob_phase)));
}
```

`r_doom_bob_last_time` is updated unconditionally so resuming from pause does not produce a phase jump (we'd otherwise integrate the entire pause duration on the first un-paused frame). `r_doom_bob_phase` is allowed to grow without bound; `cos`/`sin` are well-defined for large arguments and the float will not lose precision over a single play session.

If a future feature pauses `cl.time` itself during pause (some engines do; this one doesn't on the SDL port today), the gate is still safe — the delta becomes zero and nothing advances.

## Components touched

| File | Why |
|---|---|
| `tools/extract_phase6/manifest.zig` | Update `doom_pisg_frames`: drop PISGD0/PISGE0, add unflashed PISGB0 |
| `sdlquake/game/weapons_phase6.c` | `W_FirePhase6_DoomPistol`: remove auto-swap, defer fire to chain step 2, drop hitscan/sound/muzzleflash setup, set new attack_finished |
| `sdlquake/game/player_phase6.c` | New chain shape (4/6/4/5 tics + bullet fire moves into step 2) |
| `sdlquake/engine_src/r_sprite.c` | Bob phase accumulator gated on `!cl.paused` |

No header signature changes. No new translation units. No `engine_api_t` / `game_api_t` ABI bump.

## Verification

This project has no automated test suite (per CLAUDE.md). Manual in-game verification:

1. **Muzzle flash visible.** `impulse 100; impulse 31; +attack` — the gun's muzzle should clearly show the bright PISFA0 flash sprite during the second animation frame (PISGB0). Compare against the previous behavior where it was effectively imperceptible.
2. **Pre-fire lag perceptible.** Single-tap fire — there should be a noticeable ~0.1 s pause before the recoil pose appears (Doom feel). Bullet timing matches the recoil pose, not the press.
3. **Authentic chain timing.** Hold fire; the cycle from one bullet to the next should be ~0.54 s (compare against the prior 0.4 s — slower and more deliberate).
4. **Empty pistol stays.** Fire until ammo runs out. The pistol should stay selected and idle; pressing fire produces no animation and no sound. `impulse 1` should switch to the axe (Quake fallback path still works for impulses).
5. **Bob freezes during pause.** Move forward at full speed, press pause. The gun should stop bobbing immediately. Unpause; it resumes from where it was without a jump.
6. **Bob still works during play.** Walking and running show smooth bob; standing still shows no bob.

Smoke checks for non-regression:

7. **Other weapons unchanged.** `impulse 1` (axe), `impulse 2` (shotgun) — Quake stock weapons still cycle their normal animations, fire, and switch.
8. **Re-extraction is idempotent.** Running `zig build extract` twice produces identical `id1/progs/v_doompistol.spr` bytes the second time.

## Open questions / future work (out of scope)

- Whether Doom's actual psprite two-track flash overlay (separate `ps_flash` slot drawn on top of the gun, animated independently) is worth implementing later. The composited approach loses fidelity in edge cases like the chaingun's CHGFA/CHGFB alternation, where the flash should change every other shot but stays locked to the gun frame in a composite.
- Whether to expose the chain timing constants (4/6/4/5 tics, 0.543 s total) as cvars for in-game tweaking. Skipped here; rebuild iteration is fast enough.
- Whether `punchangle[0]` should be larger than -1 to better convey Doom's heavier kick (Doom has no punchangle equivalent; we're using a Quake mechanism). Casual playtest first; tune later if it feels too subtle.

## References

- `ref/DOOM-master/linuxdoom-1.10/p_pspr.c` — `A_FirePistol`, `S_PISTOL*` state table, `A_WeaponReady` bob math
- `ref/DOOM-master/linuxdoom-1.10/info.c` — sprite frame index → lump name table
- `docs/superpowers/specs/2026-05-07-doom-palette-switching-design.md` — palette LUT background, settled
- `tools/extract_phase6/manifest.zig` — `compositeFlash`, frame manifest format

# Doom Pistol Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the Doom pistol from "functional" to "feels like Doom" — visible muzzle-flash overlay, authentic Doom 4/6/4/5 tic timing with a 4-tic pre-fire idle hold, Doom-authentic out-of-ammo behaviour (silent stay), and pause-respecting weapon bob.

**Architecture:** Four discrete polish items, each with a self-contained commit. Bob uses a phase accumulator gated on `!cl.paused`. Out-of-ammo replaces the auto-swap-to-Quake-weapon branch with a silent return. Animation chain refactored to Doom's `S_PISTOL1`..`S_PISTOL4` tic distribution, with the bullet fire/sound/muzzleflash deferred from `W_FirePhase6_DoomPistol` into the second think callback (matching Doom's A_FirePistol-on-S_PISTOL2 semantics). Extractor manifest drops unused `PISGD0`/`PISGE0` and adds an unflashed `PISGB0` for the recoil-settle phase.

**Tech Stack:** C (gnu89 for engine, modern C for platform layer + game DLL), Zig 0.16 build system, SDL3 vendored. Hot-reload `game.dll` ABI is unchanged by this plan.

**Spec:** `docs/superpowers/specs/2026-05-07-doom-pistol-polish-design.md`

**Verification approach:** This project has no automated test suite (per CLAUDE.md). Each task ends with `zig build` + `zig build run -- +map e1m1` and an in-game check.

---

## Task ordering rationale

The four tasks land in dependency order so each commit is independently verifiable and master is never broken.

1. **Bob fix** — fully isolated to `r_sprite.c`. No interaction with weapons code; safest to land first.
2. **Out-of-ammo** — small, isolated change in `weapons_phase6.c`. Independent of the chain refactor.
3. **Chain refactor + deferred fire** — touches `weapons_phase6.c`, `weapons_phase6.h`, and `player_phase6.c` together. Uses the existing 5-frame SPR. The recoil-settle phase will temporarily render `PISGD0` (an unused-by-Doom alternate pose) instead of the intended `PISGB0` no-flash; this is a minor visual oddity, not a bug.
4. **Extractor cutover** — manifest change re-extracts `v_doompistol.spr` to 4 frames with `PISGB0` no-flash at index 3. After this commit, the visual matches Doom exactly.

The reverse order (extractor first, then chain) would leave a worse intermediate state: the existing chain references frame index 4, which would clamp to the idle pose mid-animation (R_DrawViewModelSprite clamps out-of-bounds frame indices to 0). Better to land the chain first using the still-present frame 3 (PISGD0).

---

## Task 1: Pause-respecting bob phase accumulator

**Files:**
- Modify: `sdlquake/engine_src/r_sprite.c`

The current bob computation reads `cl.time` directly to derive the phase angle, which means the gun keeps wobbling during pause and console. Replace with an accumulator that only advances when `!cl.paused`.

- [ ] **Step 1: Add static accumulator state and replace the phase computation**

In `sdlquake/engine_src/r_sprite.c`, the existing `R_DoomViewBobAmount` helper sits just above `R_DrawViewModelSprite`. Add the accumulator state immediately above `R_DoomViewBobAmount` (right after the `DOOM_BOB_*` `#define`s):

```c
// Bob phase accumulator. Advanced only when !cl.paused so the gun freezes
// during pause/console; r_doom_bob_last_time is updated unconditionally so
// resuming doesn't integrate the entire pause duration on the first frame.
static float  r_doom_bob_phase     = 0.0f;
static double r_doom_bob_last_time = 0.0;
```

Then in `R_DrawViewModelSprite`, the current bob block at the end of the function is:

```c
	bob = R_DoomViewBobAmount ();
	if (bob > 0.0f)
	{
		angle = cl.time * (2.0f * M_PI / DOOM_BOB_PERIOD);
		sx += (int)(bob * cos (angle));
		sy += (int)(bob * fabs (sin (angle)));
	}
```

Replace it with:

```c
	if (!cl.paused)
		r_doom_bob_phase += (float)((cl.time - r_doom_bob_last_time)
		                            * (2.0 * M_PI / DOOM_BOB_PERIOD));
	r_doom_bob_last_time = cl.time;

	bob = R_DoomViewBobAmount ();
	if (bob > 0.0f)
	{
		sx += (int)(bob * cos (r_doom_bob_phase));
		sy += (int)(bob * fabs (sin (r_doom_bob_phase)));
	}
```

The local `angle` variable is no longer used. Remove it from the variable declaration block at the top of `R_DrawViewModelSprite` — change:

```c
	float           bob, angle;
```

to:

```c
	float           bob;
```

- [ ] **Step 2: Build**

Run: `zig build`
Expected: clean build, no `unused-variable` warning for `angle` (we removed it), no warnings about `r_doom_bob_phase` or `r_doom_bob_last_time`.

- [ ] **Step 3: Run and verify pause behaviour**

Run: `zig build run -- +map e1m1`

In-game checks:
1. `impulse 100; impulse 31` — brings up the Doom pistol.
2. Hold W to walk forward at full speed. Confirm the gun bobs side-to-side and dips down (existing bob behaviour, unchanged).
3. Tap `pause` in the console (or bind it to a key — `bind p pause` if needed). The gun should immediately freeze in its current bob position and stay there.
4. Tap `pause` again to resume. The gun should resume bobbing from the same screen position with no visible "snap" or jump.
5. Open the console with `~`, leave it open for ~3 seconds, then close it. Same expected behaviour: bob freezes while console is up, resumes smoothly on close.
6. Type `quit` (and `Y` if a confirm prompt appears).

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_sprite.c
git commit -m "$(cat <<'EOF'
feat(phase6): pause-respecting Doom weapon bob

Bob phase was driven directly by cl.time, so the gun kept wobbling
during pause/console. Accumulate phase only when !cl.paused; update
the last-time stamp unconditionally so resume doesn't integrate the
entire pause duration on the first frame.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Out-of-ammo silent stay

**Files:**
- Modify: `sdlquake/game/weapons_phase6.c`

Today, firing the Doom pistol with zero bullets silently swaps to a Quake weapon. Doom keeps the gun up and does nothing. Change to match Doom.

- [ ] **Step 1: Replace the auto-swap branch**

In `sdlquake/game/weapons_phase6.c`, the current `W_FirePhase6_DoomPistol` opens with:

```c
void W_FirePhase6_DoomPistol(void) {
    edict_t *self = g->self;

    if (self->v.ammo_bullets < 1) {
        // No bullets — fall back to Quake's best available weapon.
        self->v.weapon2 = 0;
        self->v.weapon  = W_BestWeapon();
        W_SetCurrentAmmo();
        return;
    }
```

Replace the body of the `if` with:

```c
void W_FirePhase6_DoomPistol(void) {
    edict_t *self = g->self;

    if (self->v.ammo_bullets < 1) {
        // Doom-authentic: keep the pistol up, do nothing on press.
        // Player must switch weapons manually (impulse) or pick up another.
        return;
    }
```

The rest of the function (sound, attack_finished, punchangle, ammo decrement, hitscan, animation chain kickoff) is unchanged in this task — Task 3 refactors it.

- [ ] **Step 2: Build**

Run: `zig build`
Expected: clean build. There may be an unused-function warning for `W_BestWeapon` if Task 2 removed the only call site; the symbol stays exported for future Phase 6 weapons that may need it, so no follow-up is needed. The weak-stub pattern means an unused `extern` is harmless.

- [ ] **Step 3: Run and verify dry-fire**

Run: `zig build run -- +map e1m1`

In-game checks:
1. `impulse 100; impulse 31` — gives all weapons + selects Doom pistol.
2. Open the console (`~`) and type `give bullets -49` (or whatever brings `bullets` to 0 in the HUD). If `give bullets -N` isn't supported, just hold fire and burn through the 50 bullets the give-all cheat granted; about 50 × 0.543 s ≈ 27 s of holding fire.
3. With `bullets` reading 0, hold fire. The pistol should stay on screen, idle pose. No sound, no animation, no swap to a Quake weapon.
4. Press `impulse 1` — should switch to the axe normally.
5. Press `impulse 31` — should switch back to the (empty) Doom pistol; selection works regardless of ammo because `Phase6_ChangeWeapon` checks `items2 & flag` (ownership), not ammo.
6. Pick up some shells/bullets if any are nearby in e1m1, or `impulse 9` for full restock — pistol should now fire normally again.
7. Quit.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/weapons_phase6.c
git commit -m "$(cat <<'EOF'
feat(phase6): empty Doom pistol stays silent instead of swapping

Doom keeps the pistol up on a dry trigger; we now match that. Removes
the auto-swap-to-W_BestWeapon path. Player switches weapons manually
via impulse or by picking up a new weapon, just like in Doom.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Authentic Doom 4/6/4/5 chain + deferred bullet fire

**Files:**
- Modify: `sdlquake/game/weapons_phase6.h`
- Modify: `sdlquake/game/weapons_phase6.c`
- Modify: `sdlquake/game/player_phase6.c`

Refactor the animation chain to Doom's `S_PISTOL1`..`S_PISTOL4` distribution (4/6/4/5 tics) and move the bullet/sound/muzzleflash from `W_FirePhase6_DoomPistol` into the second chain step, matching Doom's "press fire → 4 tics idle hold → A_FirePistol on S_PISTOL2" semantics.

The chain references frame index 3 (PISGD0 in the current 5-frame SPR — unused-by-Doom alternate pose). Task 4 swaps it for PISGB0 no-flash. Until then, the recoil-settle phase shows PISGD0 instead of PISGB0; this is a minor visual oddity for one commit only.

- [ ] **Step 1: Add the new public helper to weapons_phase6.h**

In `sdlquake/game/weapons_phase6.h`, just below the existing `player_doompistol1` declaration in the "Animation chains" section, add:

```c
// Called from player_doompistol2_think when the recoil pose appears,
// matching Doom's A_FirePistol-on-S_PISTOL2 semantics. Keeps the bullet,
// sound, EF_MUZZLEFLASH, punchangle, and ammo decrement co-located in
// weapons_phase6.c rather than scattering them into player_phase6.c.
void DoomPistol_DoFire (edict_t *self);
```

So the bottom section of `weapons_phase6.h` becomes:

```c
// ---------------------------------------------------------------------------
// Animation chains (declared so weapons.c can fire-trigger them and
// player.c's extern block can forward-declare without circular includes).
// ---------------------------------------------------------------------------
void player_doompistol1 (edict_t *self);

// Called from player_doompistol2_think when the recoil pose appears,
// matching Doom's A_FirePistol-on-S_PISTOL2 semantics. Keeps the bullet,
// sound, EF_MUZZLEFLASH, punchangle, and ammo decrement co-located in
// weapons_phase6.c rather than scattering them into player_phase6.c.
void DoomPistol_DoFire (edict_t *self);

#endif // WEAPONS_PHASE6_H
```

- [ ] **Step 2: Refactor `W_FirePhase6_DoomPistol` and add `DoomPistol_DoFire` in weapons_phase6.c**

In `sdlquake/game/weapons_phase6.c`, the current `W_FirePhase6_DoomPistol` (after Task 2) is:

```c
void W_FirePhase6_DoomPistol(void) {
    edict_t *self = g->self;

    if (self->v.ammo_bullets < 1) {
        // Doom-authentic: keep the pistol up, do nothing on press.
        // Player must switch weapons manually (impulse) or pick up another.
        return;
    }

    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_pistol.wav", 1, ATTN_NORM);
    self->v.attack_finished = g->time + 0.4f;
    self->v.punchangle[0]   = -1;

    // Spawn a one-frame dynamic light at the player so the muzzle flash
    // illuminates dark areas. Cleared in sv_main.c after the snapshot is
    // sent (same handshake stock Quake uses for its 8 weapons).
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);

    self->v.ammo_bullets -= 1;
    self->v.currentammo   = self->v.ammo_bullets;

    int dmg = 5 * ((rand_byte() % 3) + 1);
    vec3_t aim;
    eng->SV_Aim(self, 100000, aim);
    p6_fire_bullet((float)dmg, aim, 0.01f, 0.01f);

    // Kick off the viewmodel animation chain (frame 0 → 1 → 2 → 3 → 0).
    player_doompistol1(self);
}
```

Replace the entire function with:

```c
void W_FirePhase6_DoomPistol(void) {
    edict_t *self = g->self;

    if (self->v.ammo_bullets < 1) {
        // Doom-authentic: keep the pistol up, do nothing on press.
        // Player must switch weapons manually (impulse) or pick up another.
        return;
    }

    // Doom's S_PISTOL1..S_PISTOL4 = 4+6+4+5 = 19 tics ≈ 0.543 s. The bullet,
    // sound, muzzleflash, punchangle, and ammo decrement happen on entering
    // S_PISTOL2 (the recoil pose) — see DoomPistol_DoFire, called from
    // player_doompistol2_think. Press → 4-tic idle hold → bullet leaves;
    // this lockout is intentional Doom feel.
    self->v.attack_finished = g->time + 0.543f;
    player_doompistol1(self);
}

// Called when the chain enters the recoil pose (player_doompistol2_think).
void DoomPistol_DoFire(edict_t *self) {
    if (self->v.ammo_bullets < 1)
        return;  // defensive: ammo could have been spent by another path

    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_pistol.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -1;

    // Spawn a one-frame dynamic light at the player so the muzzle flash
    // illuminates dark areas. Cleared in sv_main.c after the snapshot is
    // sent (same handshake stock Quake uses for its 8 weapons).
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);

    self->v.ammo_bullets -= 1;
    self->v.currentammo   = self->v.ammo_bullets;

    int dmg = 5 * ((rand_byte() % 3) + 1);
    vec3_t aim;
    eng->SV_Aim(self, 100000, aim);
    p6_fire_bullet((float)dmg, aim, 0.01f, 0.01f);
}
```

- [ ] **Step 3: Replace the chain in player_phase6.c**

In `sdlquake/game/player_phase6.c`, the current chain block (everything from the `P6_FRAME_STEP` macro definition through `void player_doompistol1(edict_t *self)`) is:

```c
#define P6_FRAME_STEP(body_fr, weap_fr, next_fn) do { \
    g->self = self; \
    g->self->v.frame       = (body_fr); \
    g->self->v.weaponframe = (weap_fr); \
    g->self->v.nextthink   = g->time + 0.1f; \
    g->self->v.think       = (next_fn); \
} while (0)

// ---------------------------------------------------------------------------
// Doom pistol -- 4 attack frames (PISGB0..E0), then back to idle (PISGA0).
// ---------------------------------------------------------------------------

static void player_doompistol2_think(edict_t *self);
static void player_doompistol3_think(edict_t *self);
static void player_doompistol4_think(edict_t *self);
static void player_doompistol5_think(edict_t *self);

static void player_doompistol1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, player_doompistol2_think);
}
static void player_doompistol2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 2, player_doompistol3_think);
}
static void player_doompistol3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 3, player_doompistol4_think);
}
static void player_doompistol4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 4, player_doompistol5_think);
}
static void player_doompistol5_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;       // back to idle sprite frame
    player_run(self);
}

void player_doompistol1(edict_t *self) {
    player_doompistol1_think(self);
}
```

Replace it with:

```c
// P6_FRAME_STEP — body-frame, viewmodel-frame, hold-time, next-think.
// Doom timings vary per chain step (4/6/4/5 tics for the pistol), so dt
// is parameterized rather than baked at 0.1f.
#define P6_FRAME_STEP(body_fr, weap_fr, dt, next_fn) do { \
    g->self = self; \
    g->self->v.frame       = (body_fr); \
    g->self->v.weaponframe = (weap_fr); \
    g->self->v.nextthink   = g->time + (dt); \
    g->self->v.think       = (next_fn); \
} while (0)

// ---------------------------------------------------------------------------
// Doom pistol -- S_PISTOL1..S_PISTOL4 (4/6/4/5 tics ≈ 0.114/0.171/0.114/0.143 s).
// Frame layout in v_doompistol.spr (after extractor cutover):
//   0 = PISGA0 (idle / pre-fire pose)
//   1 = PISGB0 + PISFA0 composited (recoil pose with muzzle flash)
//   2 = PISGC0 (smoke pose)
//   3 = PISGB0 no flash (recoil-settle pose)   ← Task 4 makes this PISGB0;
//                                                until then, the SPR has PISGD0
//                                                here (an unused-by-Doom alt pose).
// ---------------------------------------------------------------------------

static void player_doompistol2_think(edict_t *self);
static void player_doompistol3_think(edict_t *self);
static void player_doompistol4_think(edict_t *self);
static void player_doompistol5_think(edict_t *self);

// S_PISTOL1: idle pose held 4 tics before the bullet leaves.
static void player_doompistol1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     0, 0.114f, player_doompistol2_think);
}

// S_PISTOL2: recoil pose with muzzle flash. A_FirePistol fires here.
static void player_doompistol2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 1, 0.171f, player_doompistol3_think);
    DoomPistol_DoFire(self);
}

// S_PISTOL3: smoke pose.
static void player_doompistol3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 2, 0.114f, player_doompistol4_think);
}

// S_PISTOL4: recoil settle (PISGB without flash).
static void player_doompistol4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 3, 0.143f, player_doompistol5_think);
}

// Back to idle. attack_finished was already set in W_FirePhase6_DoomPistol
// to gate refire; the engine's `button0 && time >= attack_finished` check
// in W_WeaponFrame retriggers a new chain if fire is still held.
static void player_doompistol5_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_doompistol1(edict_t *self) {
    player_doompistol1_think(self);
}
```

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build. Both `weapons_phase6.c` and `game.dll` rebuild; the engine binary itself only rebuilds if Zig's hash detects a header change.

- [ ] **Step 5: Run and verify Doom-authentic chain**

Run: `zig build run -- +map e1m1`

In-game checks:
1. `impulse 100; impulse 31`
2. Single-tap fire. Listen for the sound and watch the gun:
   - On press: gun stays in idle pose for ~0.114 s (a perceptible but small lag).
   - Then: gun snaps to recoil pose with the bright muzzle flash overlaid; sound plays; bullet impacts whatever you aimed at.
   - Then: smoke pose for ~0.114 s.
   - Then: recoil-settle pose for ~0.143 s. **Until Task 4 lands, this shows PISGD0 (an alternate Doom pose) instead of PISGB no-flash.** That's expected here.
   - Then: back to idle.
3. Hold fire continuously. Each shot cycle should take ~0.54 s; significantly more deliberate than the previous ~0.4 s.
4. Walk up to a Quake monster (the soldier in the e1m1 starting room) and confirm hitscan damage still works.
5. Punchangle still kicks visibly on each shot.
6. `impulse 1` (axe), `impulse 2` (shotgun) — Quake stock weapons unchanged.
7. Quit.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/weapons_phase6.h sdlquake/game/weapons_phase6.c sdlquake/game/player_phase6.c
git commit -m "$(cat <<'EOF'
feat(phase6): authentic Doom 4/6/4/5 pistol chain with deferred fire

W_FirePhase6_DoomPistol now just gates ammo and kicks off the chain.
The bullet, sound, muzzleflash, punchangle, and ammo decrement move
into DoomPistol_DoFire, called from player_doompistol2_think on entry
to the recoil pose — matching Doom's S_PISTOL1 -> S_PISTOL2 transition
(4-tic idle hold, then A_FirePistol). Per-frame timing matches Doom's
S_PISTOL1..4 distribution (4/6/4/5 tics, 0.543s total). attack_finished
bumps to 0.543s so refire gates correctly via the engine's existing
button0 + attack_finished check.

The recoil-settle frame shows PISGD0 until the next commit re-extracts
v_doompistol.spr with PISGB no-flash at frame 3.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Extractor cutover — drop PISGD0/PISGE0, add unflashed PISGB0

**Files:**
- Modify: `tools/extract_phase6/manifest.zig`
- Regenerate (gitignored): `id1/progs/v_doompistol.spr`

Doom's pistol state machine never references `PISGD0` or `PISGE0`; they were extracted defensively. Replace them with a second `PISGB0` frame that has no flash composited, used for the recoil-settle phase (`S_PISTOL4`).

- [ ] **Step 1: Edit doom_pisg_frames in manifest.zig**

In `tools/extract_phase6/manifest.zig`, the current declaration (lines around 62–68) is:

```zig
const doom_pisg_frames  = [_]DoomFrameSpec{
    .{ .lump = "PISGA0" },
    .{ .lump = "PISGB0", .flash = "PISFA0" },
    .{ .lump = "PISGC0" },
    .{ .lump = "PISGD0" },
    .{ .lump = "PISGE0" },
};
```

Replace it with:

```zig
const doom_pisg_frames  = [_]DoomFrameSpec{
    .{ .lump = "PISGA0" },                       // 0: idle / pre-fire hold
    .{ .lump = "PISGB0", .flash = "PISFA0" },    // 1: fire (composited PISFA0 flash)
    .{ .lump = "PISGC0" },                       // 2: smoke
    .{ .lump = "PISGB0" },                       // 3: recoil settle (no flash)
};
```

- [ ] **Step 2: Re-extract**

Run: `zig build extract`
Expected stdout includes a line `wrote id1/progs/v_doompistol.spr` (or similar — the format is `  wrote {path} ({n} frames)`). The reported frame count should be `4`, not `5`.

- [ ] **Step 3: Build engine**

Run: `zig build`
Expected: clean build. (The engine doesn't change in this task; building is just a sanity check that re-extraction didn't leave anything inconsistent.)

- [ ] **Step 4: Run and verify the recoil-settle pose**

Run: `zig build run -- +map e1m1`

In-game checks:
1. `impulse 100; impulse 31; +attack`
2. Watch the recoil-settle phase carefully (the ~0.143 s phase between smoke and idle). It should now show the same gun pose as the firing frame (PISGB) but without the bright flash overlay — i.e., you see PISGB+flash → PISGC → PISGB-no-flash → idle. Compare against the previous commit, where the same phase showed an alternate pose (PISGD0).
3. The overall animation should feel smoother and visibly Doom-authentic: the gun "drops back" from recoil through PISGC to PISGB with no flash, then to idle.
4. The muzzle flash should still be unmistakable on the firing frame (visible for ~0.171 s — well above human-perceivable).
5. Quake weapons (`impulse 1`, `impulse 2`) unchanged.
6. Quit.

- [ ] **Step 5: Verify the SPR file dropped from 5 frames to 4**

This is a sanity check that the manifest change actually took effect. PowerShell:

```powershell
(Get-Item id1/progs/v_doompistol.spr).Length
```

The exact byte count depends on per-frame dimensions, but it should be smaller than the file before re-extraction (since we dropped two frames and added one smaller PISGB0 — net one fewer frame, and PISGD0/PISGE0 are typically larger than a bare PISGB0 because Doom's later pistol poses include more pixels). Just confirm the file is smaller than the prior `v_doompistol.spr` (no exact target value asserted).

If you want explicit confirmation: re-running `zig build extract` is idempotent — the re-extracted file should match the just-extracted file byte-for-byte:

```powershell
$h1 = (Get-FileHash id1/progs/v_doompistol.spr).Hash
zig build extract
$h2 = (Get-FileHash id1/progs/v_doompistol.spr).Hash
$h1 -eq $h2
```

Expected: `True`.

- [ ] **Step 6: Commit**

```bash
git add tools/extract_phase6/manifest.zig
git commit -m "$(cat <<'EOF'
feat(phase6): drop unused PISGD0/PISGE0, add PISGB no-flash frame

Doom's pistol state machine only references PISGA, PISGB, PISGC. The
PISGD0/PISGE0 frames were extracted defensively but never appear in any
S_PISTOL state. Replace them with a second PISGB0 frame (no flash) used
for the recoil-settle phase that matches S_PISTOL4 in p_pspr.c.

v_doompistol.spr now has 4 frames; the chain in player_phase6.c
references all of them. Recoil settle visually matches Doom now.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Spec coverage check

| Spec section | Plan task |
| --- | --- |
| §Architecture / Decision 1 — pre-fire 4-tic lag, bullet from chain step 2 | Task 3 |
| §Architecture / Decision 2 — drop PISGD/E, add PISGB no-flash | Task 4 |
| §Architecture / Decision 3 — 4/6/4/5 tic chain | Task 3 |
| §Architecture / Decision 4 — out-of-ammo silent stay | Task 2 |
| §Architecture / Decision 5 — pause-respecting bob accumulator | Task 1 |
| §Components touched — manifest.zig | Task 4 |
| §Components touched — weapons_phase6.c | Tasks 2, 3 |
| §Components touched — player_phase6.c | Task 3 |
| §Components touched — r_sprite.c | Task 1 |
| §Verification 1 (muzzle flash visible) | Task 4 step 4 |
| §Verification 2 (pre-fire lag) | Task 3 step 5 |
| §Verification 3 (authentic chain timing) | Task 3 step 5 |
| §Verification 4 (empty pistol stays) | Task 2 step 3 |
| §Verification 5 (bob freezes during pause) | Task 1 step 3 |
| §Verification 6 (bob still works during play) | Task 1 step 3 |
| §Verification 7 (other weapons unchanged) | Task 3 step 5, Task 4 step 4 |
| §Verification 8 (re-extraction idempotent) | Task 4 step 5 |

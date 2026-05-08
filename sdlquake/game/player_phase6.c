// player_phase6.c -- Animation chains for Phase 6 viewmodels.
//
// Each chain advances `weaponframe` through its attack poses, then returns to
// player_run (which resets `weaponframe` to 0 = idle sprite frame). Body-frame
// animation reuses Quake's shotgun-attack stance (FR_SHOTATT1) since we don't
// have Doom/Wolf-specific body poses. The held-sprite-frame index is what
// matters — that drives R_DrawViewModelSprite's frame selection.
//
// Tic timing: Doom runs at 35 Hz, so 1 tic ≈ 0.0286 s. Per-chain timings are
// documented in each chain block (e.g. the pistol uses Doom's S_PISTOL1..4
// distribution of 4/6/4/5 tics ≈ 0.114/0.171/0.114/0.143 s, total 0.543 s).

#include "weapons_phase6.h"

// ---------------------------------------------------------------------------
// Tic-time conversions. Doom runs at 35 Hz, Wolf3D at 70 Hz. Per-step
// chain timings come straight from p_pspr.c info.c state durations or
// WL_AGENT.C's attackinfo[] tic counts -- prefer the named constants
// over bare floats so a future eyeball-check against the source matches
// the symbol, not a 3-decimal magic number.
// ---------------------------------------------------------------------------
#define DOOM_TIC_S    (1.0f / 35.0f)
#define WOLF_TIC_S    (1.0f / 70.0f)

#define DOOM_3_TIC    (3 * DOOM_TIC_S)   // 0.086 -- shotgun S_SGUN1 / S_SGUN8
#define DOOM_4_TIC    (4 * DOOM_TIC_S)   // 0.114 -- pistol S_PISTOL1, fist S_PUNCH1/2/4, etc.
#define DOOM_5_TIC    (5 * DOOM_TIC_S)   // 0.143 -- fist S_PUNCH3/5, shotgun pump
#define DOOM_6_TIC    (6 * DOOM_TIC_S)   // 0.171 -- pistol S_PISTOL2 (recoil)
#define DOOM_7_TIC    (7 * DOOM_TIC_S)   // 0.200 -- shotgun S_SGUN2 fire
#define DOOM_8_TIC    (8 * DOOM_TIC_S)   // 0.229 -- rocket S_MISSILE1 flash hold
#define DOOM_12_TIC   (12 * DOOM_TIC_S)  // 0.343 -- rocket S_MISSILE2 fire

#define WOLF_6_TIC    (6 * WOLF_TIC_S)   // 0.086 -- all Wolf weapons step duration

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void player_run(edict_t *self);

#define FR_PHASE6_BODY  113   // FR_SHOTATT1 — generic shooting stance

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
// Frame layout in v_doompistol.spr:
//   0 = PISGA0 (idle / pre-fire pose)
//   1 = PISGB0 + PISFA0 composited (recoil pose with muzzle flash)
//   2 = PISGC0 (smoke pose)
//   3 = PISGB0 no flash (recoil-settle pose)
// ---------------------------------------------------------------------------

static void player_doompistol2_think(edict_t *self);
static void player_doompistol3_think(edict_t *self);
static void player_doompistol4_think(edict_t *self);
static void player_doompistol5_think(edict_t *self);

// S_PISTOL1: idle pose held 4 tics before the bullet leaves.
static void player_doompistol1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     0, DOOM_4_TIC, player_doompistol2_think);
}

// S_PISTOL2: recoil pose with muzzle flash. A_FirePistol fires here.
static void player_doompistol2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 1, DOOM_6_TIC, player_doompistol3_think);
    DoomPistol_DoFire(self);
}

// S_PISTOL3: smoke pose.
static void player_doompistol3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 2, DOOM_4_TIC, player_doompistol4_think);
}

// S_PISTOL4: recoil settle (PISGB without flash).
static void player_doompistol4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 3, DOOM_5_TIC, player_doompistol5_think);
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

// ---------------------------------------------------------------------------
// Doom fist -- S_PUNCH1..S_PUNCH5 (4/4/5/4/5 tics ~= 0.114/0.114/0.143/0.114/0.143 s).
// Frame layout in v_doomfist.spr:
//   0 = PUNGA (idle)         -- not used in attack chain
//   1 = PUNGB (windup)       -- S_PUNCH1, S_PUNCH5
//   2 = PUNGC (impact)       -- S_PUNCH2 (A_Punch fires here), S_PUNCH4
//   3 = PUNGD (recoil)       -- S_PUNCH3
// ---------------------------------------------------------------------------

static void player_doomfist2_think(edict_t *self);
static void player_doomfist3_think(edict_t *self);
static void player_doomfist4_think(edict_t *self);
static void player_doomfist5_think(edict_t *self);
static void player_doomfist6_think(edict_t *self);

// S_PUNCH1: windup.
static void player_doomfist1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, DOOM_4_TIC, player_doomfist2_think);
}
// S_PUNCH2: impact pose. Punch fires.
static void player_doomfist2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 2, DOOM_4_TIC, player_doomfist3_think);
    DoomFist_DoFire(self);
}
// S_PUNCH3: recoil.
static void player_doomfist3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 3, DOOM_5_TIC, player_doomfist4_think);
}
// S_PUNCH4: bounce-back through impact pose.
static void player_doomfist4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 2, DOOM_4_TIC, player_doomfist5_think);
}
// S_PUNCH5: settle through windup pose.
static void player_doomfist5_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, DOOM_5_TIC, player_doomfist6_think);
}
// Back to idle (frame 0 = PUNGA).
static void player_doomfist6_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_doomfist1(edict_t *self) {
    player_doomfist1_think(self);
}

// ---------------------------------------------------------------------------
// Doom chainsaw -- S_SAW1..S_SAW2 (4/4 tics ~= 0.114/0.114 s) loop.
// Both frames fire (A_Saw); A_ReFire gates the loop on button0.
// Frame layout in v_doomchainsaw.spr:
//   0 = SAWGA (attack pose A)  -- S_SAW1
//   1 = SAWGB (attack pose B)  -- S_SAW2
//   2 = SAWGC (idle alt -- unused, kept for completeness)
//   3 = SAWGD (idle alt -- unused)
// ---------------------------------------------------------------------------

static void player_doomsaw2_think(edict_t *self);
static void player_doomsaw3_think(edict_t *self);

// S_SAW1: pose A, fires.
static void player_doomsaw1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     0, DOOM_4_TIC, player_doomsaw2_think);
    DoomSaw_DoFire(self);
}
// S_SAW2: pose B, fires.
static void player_doomsaw2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 1, DOOM_4_TIC, player_doomsaw3_think);
    DoomSaw_DoFire(self);
}
// Back to idle. Refire is gated by attack_finished (set to time+DOOMSAW_CHAIN_S
// in W_FirePhase6_DoomChainsaw); engine re-enters W_Attack_Phase6 if button0
// is still held.
static void player_doomsaw3_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_doomsaw1(edict_t *self) {
    player_doomsaw1_think(self);
}

// ---------------------------------------------------------------------------
// Doom chaingun -- S_CHAIN1..S_CHAIN2 (4/4 tics) loop.
// Both frames fire; chain reloops via attack_finished while button0 held.
// Frame layout in v_doomchaingun.spr:
//   0 = CHGGA (no flash) — idle pose (weaponframe=0 default)
//   1 = CHGGA + CHGFA composited (fire pose A with flash A)
//   2 = CHGGB + CHGFB composited (fire pose B with flash B)
// ---------------------------------------------------------------------------

static void player_doomchaingun2_think(edict_t *self);
static void player_doomchaingun3_think(edict_t *self);

static void player_doomchaingun1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, DOOM_4_TIC, player_doomchaingun2_think);
    DoomChaingun_DoFire(self);
}
static void player_doomchaingun2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 2, DOOM_4_TIC, player_doomchaingun3_think);
    DoomChaingun_DoFire(self);
}
static void player_doomchaingun3_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_doomchaingun1(edict_t *self) {
    player_doomchaingun1_think(self);
}

// ---------------------------------------------------------------------------
// Doom shotgun -- S_SGUN1..S_SGUN8 (3/7/5/5/4/5/5/3 tics). S_SGUN9 (7 tic
// A_ReFire) folded into attack_finished.
// Frame layout in v_doomshotgun.spr:
//   0 = SHTGA (idle, no flash)                          -- weaponframe=0 idle, S_SGUN1, S_SGUN8
//   1 = SHTGA + SHTFA composited (fire pose with flash) -- S_SGUN2
//   2 = SHTGB (pump open)                               -- S_SGUN3, S_SGUN7
//   3 = SHTGC (pump back)                               -- S_SGUN4, S_SGUN6
//   4 = SHTGD (pump full back)                          -- S_SGUN5
// ---------------------------------------------------------------------------

static void player_doomshotgun2_think(edict_t *self);
static void player_doomshotgun3_think(edict_t *self);
static void player_doomshotgun4_think(edict_t *self);
static void player_doomshotgun5_think(edict_t *self);
static void player_doomshotgun6_think(edict_t *self);
static void player_doomshotgun7_think(edict_t *self);
static void player_doomshotgun8_think(edict_t *self);
static void player_doomshotgun9_think(edict_t *self);

// S_SGUN1: idle pose (3 tics).
static void player_doomshotgun1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     0, DOOM_3_TIC, player_doomshotgun2_think);
}
// S_SGUN2: fire (7 tics).
static void player_doomshotgun2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 1, DOOM_7_TIC, player_doomshotgun3_think);
    DoomShotgun_DoFire(self);
}
// S_SGUN3: pump open (5 tics).
static void player_doomshotgun3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 2, DOOM_5_TIC, player_doomshotgun4_think);
}
// S_SGUN4: pump back (5 tics).
static void player_doomshotgun4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 3, DOOM_5_TIC, player_doomshotgun5_think);
}
// S_SGUN5: pump full back (4 tics).
static void player_doomshotgun5_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     4, DOOM_4_TIC, player_doomshotgun6_think);
}
// S_SGUN6: pump back (return through C, 5 tics).
static void player_doomshotgun6_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 3, DOOM_5_TIC, player_doomshotgun7_think);
}
// S_SGUN7: pump open (return through B, 5 tics).
static void player_doomshotgun7_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 2, DOOM_5_TIC, player_doomshotgun8_think);
}
// S_SGUN8: idle pose return (3 tics).
static void player_doomshotgun8_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 0, DOOM_3_TIC, player_doomshotgun9_think);
}
// Back to idle.
static void player_doomshotgun9_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_doomshotgun1(edict_t *self) {
    player_doomshotgun1_think(self);
}

// ---------------------------------------------------------------------------
// Doom rocket launcher -- S_MISSILE1 (8 tics, A_GunFlash, no actual fire)
// then S_MISSILE2 (12 tics, A_FireMissile, fires). S_MISSILE3 0-tic A_ReFire
// folded into attack_finished. Total 20 tics + 7 tic extra settle = 27 tics
// ~= 0.771 s.
//
// Doom's flash layer (S_MISSILEFLASH1..4 = 3+4+4+4 = 15 tics) overlays only
// the first 15 tics: all of S_MISSILE1 (8 tics) plus the first 7 tics of
// S_MISSILE2. The last 5 tics of S_MISSILE2 show the gun without the flash.
// We approximate by splitting S_MISSILE2 into two sub-steps so the
// composited flash frame doesn't linger for the entire 20-tic firing
// animation -- it disappears at tic 15 like Doom.
//
// Frame layout in v_doomrocket.spr:
//   0 = MISGA (idle / lowered)         -- not used in attack chain
//   1 = MISGB + MISFA composited (fire pose with flash) -- S_MISSILE1 + early S_MISSILE2
//   2 = MISGB (settle, no flash) -- late S_MISSILE2
// ---------------------------------------------------------------------------

static void player_doomrocket2_think(edict_t *self);
static void player_doomrocket3_think(edict_t *self);
static void player_doomrocket4_think(edict_t *self);

// S_MISSILE1: flash already up; no fire yet (8 tics, flash present).
static void player_doomrocket1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, DOOM_8_TIC, player_doomrocket2_think);
}
// First 7 tics of S_MISSILE2: fire here, flash still present.
static void player_doomrocket2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 1, DOOM_7_TIC, player_doomrocket3_think);
    DoomRocket_DoFire(self);
}
// Last 5 tics of S_MISSILE2: flash has expired, switch to no-flash settle.
static void player_doomrocket3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 2, DOOM_5_TIC, player_doomrocket4_think);
}
// Back to idle.
static void player_doomrocket4_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_doomrocket1(edict_t *self) {
    player_doomrocket1_think(self);
}

// ---------------------------------------------------------------------------
// Wolf3D knife -- attackinfo {6,0,1},{6,2,2},{6,0,3},{6,-1,4}
// 4 steps x 6 tics at 70 Hz = 0.343 s total. Hit on step 2 (attack=2).
// Frame layout in v_wolfknife.spr (5 frames, indices 0..4):
//   0 = idle pose (not used in attack chain)
//   1 = pre-swing
//   2 = swing through (hit lands here)
//   3 = follow-through
//   4 = recover
// ---------------------------------------------------------------------------

static void player_wolfknife2_think(edict_t *self);
static void player_wolfknife3_think(edict_t *self);
static void player_wolfknife4_think(edict_t *self);
static void player_wolfknife5_think(edict_t *self);

static void player_wolfknife1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, WOLF_6_TIC, player_wolfknife2_think);
}
static void player_wolfknife2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 2, WOLF_6_TIC, player_wolfknife3_think);
    WolfKnife_DoHit(self);
}
static void player_wolfknife3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 3, WOLF_6_TIC, player_wolfknife4_think);
}
static void player_wolfknife4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 4, WOLF_6_TIC, player_wolfknife5_think);
}
static void player_wolfknife5_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_wolfknife1(edict_t *self) {
    player_wolfknife1_think(self);
}

// ---------------------------------------------------------------------------
// Wolf3D pistol -- attackinfo {6,0,1},{6,1,2},{6,0,3},{6,-1,4}.
// 4 steps x 6 tics @ 70 Hz = 0.343 s. Fires on step 2 (attack=1).
// Frame layout in v_wolfpistol.spr (5 frames, indices 0..4):
//   0 = idle (not used in attack chain)
//   1 = pre-fire pose
//   2 = fire pose with flash
//   3 = post-fire pose
//   4 = recover
// ---------------------------------------------------------------------------

static void player_wolfpistol2_think(edict_t *self);
static void player_wolfpistol3_think(edict_t *self);
static void player_wolfpistol4_think(edict_t *self);
static void player_wolfpistol5_think(edict_t *self);

static void player_wolfpistol1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     1, WOLF_6_TIC, player_wolfpistol2_think);
}
static void player_wolfpistol2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 2, WOLF_6_TIC, player_wolfpistol3_think);
    WolfPistol_DoFire(self);
}
static void player_wolfpistol3_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 2, 3, WOLF_6_TIC, player_wolfpistol4_think);
}
static void player_wolfpistol4_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 3, 4, WOLF_6_TIC, player_wolfpistol5_think);
}
static void player_wolfpistol5_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_wolfpistol1(edict_t *self) {
    player_wolfpistol1_think(self);
}

// ---------------------------------------------------------------------------
// Wolf3D MG -- 12-tic per-shot loop, 2-step chain (fire frame + post-fire).
// Wolf3D's 4-step attackinfo collapses to a 2-step here that reloops via
// attack_finished while button0 held.
// Frame layout in v_wolfmg.spr:
//   0 = idle (not used)
//   1 = pre-fire (only on first press)
//   2 = fire pose with flash
//   3 = post-fire
//   4 = recover (only on release)
// ---------------------------------------------------------------------------

static void player_wolfmg2_think(edict_t *self);
static void player_wolfmg3_think(edict_t *self);

static void player_wolfmg1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     2, WOLF_6_TIC, player_wolfmg2_think);
    WolfMG_DoFire(self);
}
static void player_wolfmg2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 3, WOLF_6_TIC, player_wolfmg3_think);
}
static void player_wolfmg3_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_wolfmg1(edict_t *self) {
    player_wolfmg1_think(self);
}

// ---------------------------------------------------------------------------
// Wolf3D chaingun -- single-step 6-tic chain. Refire is gated by
// attack_finished = time + WOLFCG_CHAIN_S (6 tics) so holding fire
// produces ~11.6 Hz sustained shots. Frame index alternates 2/3 across
// calls via a static toggle so the muzzle pose visibly changes.
// Frame layout in v_wolfchaingun.spr:
//   0 = idle (not used)
//   1 = pre-fire (only on first press)
//   2 = fire pose A with flash
//   3 = fire pose B with flash (alternates with 2)
//   4 = recover
// ---------------------------------------------------------------------------

static int p6_wolf_cg_toggle = 0;

static void player_wolfchaingun2_think(edict_t *self);

static void player_wolfchaingun1_think(edict_t *self) {
    int weap_fr = (p6_wolf_cg_toggle ^= 1) ? 2 : 3;
    P6_FRAME_STEP(FR_PHASE6_BODY, weap_fr, WOLF_6_TIC, player_wolfchaingun2_think);
    WolfChaingun_DoFire(self);
}
static void player_wolfchaingun2_think(edict_t *self) {
    g->self = self;
    g->self->v.weaponframe = 0;
    player_run(self);
}

void player_wolfchaingun1(edict_t *self) {
    player_wolfchaingun1_think(self);
}

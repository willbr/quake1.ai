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
//   0 = CHGGA + CHGFA composited (fire pose A with flash A)
//   1 = CHGGB + CHGFB composited (fire pose B with flash B)
// ---------------------------------------------------------------------------

static void player_doomchaingun2_think(edict_t *self);
static void player_doomchaingun3_think(edict_t *self);

static void player_doomchaingun1_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY,     0, DOOM_4_TIC, player_doomchaingun2_think);
    DoomChaingun_DoFire(self);
}
static void player_doomchaingun2_think(edict_t *self) {
    P6_FRAME_STEP(FR_PHASE6_BODY + 1, 1, DOOM_4_TIC, player_doomchaingun3_think);
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

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

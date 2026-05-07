// player_phase6.c -- Animation chains for Phase 6 viewmodels.
//
// Each chain runs `weaponframe` 1 → ... → final, then returns to player_run
// (which sets weaponframe back to 0 = idle sprite frame). Body-frame animation
// reuses Quake's shotgun-attack stance (FR_SHOTATT1) since we don't have
// Doom/Wolf-specific body poses. The held-sprite-frame index is what matters
// — that drives R_DrawViewModelSprite's frame selection.
//
// Tic timing: Doom runs at 35 Hz, so 1 tic ≈ 0.0286 s. Pistol is ~14 tics
// total → 0.4 s, distributed over 4 attack frames at ~0.1 s each.

#include "weapons_phase6.h"

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void player_run(edict_t *self);

#define FR_PHASE6_BODY  113   // FR_SHOTATT1 — generic shooting stance

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

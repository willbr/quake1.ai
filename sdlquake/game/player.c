// player.c -- Player animation, death, gib, bubbles. Source: player.qc

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern float modelindex_player;   // client.c

// ---------------------------------------------------------------------------
// Frame numbers (derived from $frame directives in player.qc)
// ---------------------------------------------------------------------------
#define FR_AXRUN1    0
#define FR_ROCKRUN1  6
#define FR_STAND1   12
#define FR_AXSTND1  17
#define FR_AXPAIN1  29
#define FR_PAIN1    35
#define FR_AXDETH1  41
#define FR_DEATHA1  50
#define FR_DEATHA11 60
#define FR_DEATHB1  61
#define FR_DEATHB9  69
#define FR_DEATHC1  70
#define FR_DEATHC15 84
#define FR_DEATHD1  85
#define FR_DEATHD9  93
#define FR_DEATHE1  94
#define FR_DEATHE9  102
#define FR_NAILATT1 103
#define FR_LIGHT1   105
#define FR_ROCKATT1 107
#define FR_SHOTATT1 113
#define FR_AXATT1   119
#define FR_AXATTB1  125
#define FR_AXATTC1  131
#define FR_AXATTD1  137

// ---------------------------------------------------------------------------
// Forward declarations for the animation think chain
// ---------------------------------------------------------------------------
static void player_stand1_think(edict_t *self);
static void player_run_think(edict_t *self);

static void player_shot2_think(edict_t *);
static void player_shot3_think(edict_t *);
static void player_shot4_think(edict_t *);
static void player_shot5_think(edict_t *);
static void player_shot6_think(edict_t *);

static void player_axe2_think(edict_t *);
static void player_axe3_think(edict_t *);
static void player_axe4_think(edict_t *);
static void player_axeb2_think(edict_t *);
static void player_axeb3_think(edict_t *);
static void player_axeb4_think(edict_t *);
static void player_axec2_think(edict_t *);
static void player_axec3_think(edict_t *);
static void player_axec4_think(edict_t *);
static void player_axed2_think(edict_t *);
static void player_axed3_think(edict_t *);
static void player_axed4_think(edict_t *);

static void player_nail2_think(edict_t *);
static void player_light2_think(edict_t *);

static void player_pain2_think(edict_t *);
static void player_pain3_think(edict_t *);
static void player_pain4_think(edict_t *);
static void player_pain5_think(edict_t *);
static void player_pain6_think(edict_t *);
static void player_axpain2_think(edict_t *);
static void player_axpain3_think(edict_t *);
static void player_axpain4_think(edict_t *);
static void player_axpain5_think(edict_t *);
static void player_axpain6_think(edict_t *);

static void player_diea2_think(edict_t *);
static void player_diea3_think(edict_t *);
static void player_diea4_think(edict_t *);
static void player_diea5_think(edict_t *);
static void player_diea6_think(edict_t *);
static void player_diea7_think(edict_t *);
static void player_diea8_think(edict_t *);
static void player_diea9_think(edict_t *);
static void player_diea10_think(edict_t *);
static void player_diea11_think(edict_t *);

static void player_dieb2_think(edict_t *);
static void player_dieb3_think(edict_t *);
static void player_dieb4_think(edict_t *);
static void player_dieb5_think(edict_t *);
static void player_dieb6_think(edict_t *);
static void player_dieb7_think(edict_t *);
static void player_dieb8_think(edict_t *);
static void player_dieb9_think(edict_t *);

static void player_diec2_think(edict_t *);
static void player_diec3_think(edict_t *);
static void player_diec4_think(edict_t *);
static void player_diec5_think(edict_t *);
static void player_diec6_think(edict_t *);
static void player_diec7_think(edict_t *);
static void player_diec8_think(edict_t *);
static void player_diec9_think(edict_t *);
static void player_diec10_think(edict_t *);
static void player_diec11_think(edict_t *);
static void player_diec12_think(edict_t *);
static void player_diec13_think(edict_t *);
static void player_diec14_think(edict_t *);
static void player_diec15_think(edict_t *);

static void player_died2_think(edict_t *);
static void player_died3_think(edict_t *);
static void player_died4_think(edict_t *);
static void player_died5_think(edict_t *);
static void player_died6_think(edict_t *);
static void player_died7_think(edict_t *);
static void player_died8_think(edict_t *);
static void player_died9_think(edict_t *);

static void player_diee2_think(edict_t *);
static void player_diee3_think(edict_t *);
static void player_diee4_think(edict_t *);
static void player_diee5_think(edict_t *);
static void player_diee6_think(edict_t *);
static void player_diee7_think(edict_t *);
static void player_diee8_think(edict_t *);
static void player_diee9_think(edict_t *);

static void player_die_ax2_think(edict_t *);
static void player_die_ax3_think(edict_t *);
static void player_die_ax4_think(edict_t *);
static void player_die_ax5_think(edict_t *);
static void player_die_ax6_think(edict_t *);
static void player_die_ax7_think(edict_t *);
static void player_die_ax8_think(edict_t *);
static void player_die_ax9_think(edict_t *);

// Forward declarations — defined in other files
__attribute__((weak)) void bubble_bob(edict_t *self)    { (void)self; }
__attribute__((weak)) void DropBackpack(void)           {}
__attribute__((weak)) void W_FireAxe(void)              {}
__attribute__((weak)) void W_FireSpikes(float offset)   { (void)offset; }
__attribute__((weak)) void W_FireLightning(void)        {}
__attribute__((weak)) void SuperDamageSound(void)       {}
void SUB_Remove(edict_t *self);   // subs.c

// ---------------------------------------------------------------------------
// Helper: set up a think-frame step
// ---------------------------------------------------------------------------
#define FRAME_STEP(fr, next_fn) do { \
    g->self = self; \
    g->self->v.frame     = (fr); \
    g->self->v.nextthink = g->self->v.ltime + 0.1f; \
    g->self->v.think     = (next_fn); \
} while (0)

// ---------------------------------------------------------------------------
// Stand / run
// ---------------------------------------------------------------------------

static void player_stand1_think(edict_t *self)
{
    FRAME_STEP(FR_AXSTND1, player_stand1_think);
    g->self->v.weaponframe = 0;
    if (g->self->v.velocity[0] || g->self->v.velocity[1]) {
        g->self->v.walkframe = 0;
        player_run_think(self);
        return;
    }
    if ((int)g->self->v.weapon == IT_AXE) {
        if (g->self->v.walkframe >= 12) g->self->v.walkframe = 0;
        g->self->v.frame = FR_AXSTND1 + (int)g->self->v.walkframe;
    } else {
        if (g->self->v.walkframe >= 5) g->self->v.walkframe = 0;
        g->self->v.frame = FR_STAND1 + (int)g->self->v.walkframe;
    }
    g->self->v.walkframe++;
}
void player_stand1(edict_t *self) { player_stand1_think(self); }

static void player_run_think(edict_t *self)
{
    FRAME_STEP(FR_ROCKRUN1, player_run_think);
    g->self->v.weaponframe = 0;
    if (!g->self->v.velocity[0] && !g->self->v.velocity[1]) {
        g->self->v.walkframe = 0;
        player_stand1_think(self);
        return;
    }
    if ((int)g->self->v.weapon == IT_AXE) {
        if (g->self->v.walkframe == 6) g->self->v.walkframe = 0;
        g->self->v.frame = FR_AXRUN1 + (int)g->self->v.walkframe;
    } else {
        if (g->self->v.walkframe == 6) g->self->v.walkframe = 0;
        g->self->v.frame = g->self->v.frame + (int)g->self->v.walkframe;
    }
    g->self->v.walkframe++;
}

// ---------------------------------------------------------------------------
// Shotgun attack
// ---------------------------------------------------------------------------

static void player_shot1_think(edict_t *self)
{
    FRAME_STEP(FR_SHOTATT1, player_shot2_think);
    g->self->v.weaponframe = 1;
    g->self->v.effects = (float)((int)g->self->v.effects | EF_MUZZLEFLASH);
}
void player_shot1(edict_t *self) { player_shot1_think(self); }

static void player_shot2_think(edict_t *self) { FRAME_STEP(FR_SHOTATT1+1, player_shot3_think); g->self->v.weaponframe=2; }
static void player_shot3_think(edict_t *self) { FRAME_STEP(FR_SHOTATT1+2, player_shot4_think); g->self->v.weaponframe=3; }
static void player_shot4_think(edict_t *self) { FRAME_STEP(FR_SHOTATT1+3, player_shot5_think); g->self->v.weaponframe=4; }
static void player_shot5_think(edict_t *self) { FRAME_STEP(FR_SHOTATT1+4, player_shot6_think); g->self->v.weaponframe=5; }
static void player_shot6_think(edict_t *self) { FRAME_STEP(FR_SHOTATT1+5, player_run_think);   g->self->v.weaponframe=6; }

// ---------------------------------------------------------------------------
// Axe attacks
// ---------------------------------------------------------------------------

static void player_axe1_think(edict_t *self) { FRAME_STEP(FR_AXATT1,   player_axe2_think);  g->self->v.weaponframe=1; }
static void player_axe2_think(edict_t *self) { FRAME_STEP(FR_AXATT1+1, player_axe3_think);  g->self->v.weaponframe=2; }
static void player_axe3_think(edict_t *self) { FRAME_STEP(FR_AXATT1+2, player_axe4_think);  g->self->v.weaponframe=3; W_FireAxe(); }
static void player_axe4_think(edict_t *self) { FRAME_STEP(FR_AXATT1+3, player_run_think);   g->self->v.weaponframe=4; }
void player_axe1(edict_t *self) { player_axe1_think(self); }

static void player_axeb1_think(edict_t *self) { FRAME_STEP(FR_AXATTB1,   player_axeb2_think); g->self->v.weaponframe=5; }
static void player_axeb2_think(edict_t *self) { FRAME_STEP(FR_AXATTB1+1, player_axeb3_think); g->self->v.weaponframe=6; }
static void player_axeb3_think(edict_t *self) { FRAME_STEP(FR_AXATTB1+2, player_axeb4_think); g->self->v.weaponframe=7; W_FireAxe(); }
static void player_axeb4_think(edict_t *self) { FRAME_STEP(FR_AXATTB1+3, player_run_think);   g->self->v.weaponframe=8; }
void player_axeb1(edict_t *self) { player_axeb1_think(self); }

static void player_axec1_think(edict_t *self) { FRAME_STEP(FR_AXATTC1,   player_axec2_think); g->self->v.weaponframe=1; }
static void player_axec2_think(edict_t *self) { FRAME_STEP(FR_AXATTC1+1, player_axec3_think); g->self->v.weaponframe=2; }
static void player_axec3_think(edict_t *self) { FRAME_STEP(FR_AXATTC1+2, player_axec4_think); g->self->v.weaponframe=3; W_FireAxe(); }
static void player_axec4_think(edict_t *self) { FRAME_STEP(FR_AXATTC1+3, player_run_think);   g->self->v.weaponframe=4; }
void player_axec1(edict_t *self) { player_axec1_think(self); }

static void player_axed1_think(edict_t *self) { FRAME_STEP(FR_AXATTD1,   player_axed2_think); g->self->v.weaponframe=5; }
static void player_axed2_think(edict_t *self) { FRAME_STEP(FR_AXATTD1+1, player_axed3_think); g->self->v.weaponframe=6; }
static void player_axed3_think(edict_t *self) { FRAME_STEP(FR_AXATTD1+2, player_axed4_think); g->self->v.weaponframe=7; W_FireAxe(); }
static void player_axed4_think(edict_t *self) { FRAME_STEP(FR_AXATTD1+3, player_run_think);   g->self->v.weaponframe=8; }
void player_axed1(edict_t *self) { player_axed1_think(self); }

// ---------------------------------------------------------------------------
// Nail / lightning attacks
// ---------------------------------------------------------------------------

static void player_nail1_think(edict_t *self)
{
    FRAME_STEP(FR_NAILATT1, player_nail2_think);
    g->self->v.effects = (float)((int)g->self->v.effects | EF_MUZZLEFLASH);
    if (!g->self->v.button0) { player_run_think(self); return; }
    g->self->v.weaponframe++;
    if (g->self->v.weaponframe == 9) g->self->v.weaponframe = 1;
    SuperDamageSound();
    W_FireSpikes(4);
    g->self->v.attack_finished = g->time + 0.2f;
}
void player_nail1(edict_t *self) { player_nail1_think(self); }

static void player_nail2_think(edict_t *self)
{
    FRAME_STEP(FR_NAILATT1+1, player_nail1_think);
    g->self->v.effects = (float)((int)g->self->v.effects | EF_MUZZLEFLASH);
    if (!g->self->v.button0) { player_run_think(self); return; }
    g->self->v.weaponframe++;
    if (g->self->v.weaponframe == 9) g->self->v.weaponframe = 1;
    SuperDamageSound();
    W_FireSpikes(-4);
    g->self->v.attack_finished = g->time + 0.2f;
}
void player_nail2(edict_t *self) { player_nail2_think(self); }

static void player_light1_think(edict_t *self)
{
    FRAME_STEP(FR_LIGHT1, player_light2_think);
    g->self->v.effects = (float)((int)g->self->v.effects | EF_MUZZLEFLASH);
    if (!g->self->v.button0) { player_run_think(self); return; }
    g->self->v.weaponframe++;
    if (g->self->v.weaponframe == 5) g->self->v.weaponframe = 1;
    SuperDamageSound();
    W_FireLightning();
    g->self->v.attack_finished = g->time + 0.2f;
}
void player_light1(edict_t *self) { player_light1_think(self); }

static void player_light2_think(edict_t *self)
{
    FRAME_STEP(FR_LIGHT1+1, player_light1_think);
    g->self->v.effects = (float)((int)g->self->v.effects | EF_MUZZLEFLASH);
    if (!g->self->v.button0) { player_run_think(self); return; }
    g->self->v.weaponframe++;
    if (g->self->v.weaponframe == 5) g->self->v.weaponframe = 1;
    SuperDamageSound();
    W_FireLightning();
    g->self->v.attack_finished = g->time + 0.2f;
}
void player_light2(edict_t *self) { player_light2_think(self); }

// ---------------------------------------------------------------------------
// Rocket attack
// ---------------------------------------------------------------------------

static void player_rocket2_think(edict_t *self) { FRAME_STEP(FR_ROCKATT1+1, player_rocket2_think); }

static void player_rocket1_think(edict_t *self)
{
    FRAME_STEP(FR_ROCKATT1, player_rocket2_think); // simplified — 6-frame sequence ending at player_run
    g->self->v.weaponframe = 1;
    g->self->v.effects = (float)((int)g->self->v.effects | EF_MUZZLEFLASH);
}
void player_rocket1(edict_t *self) { player_rocket1_think(self); }

// ---------------------------------------------------------------------------
// Pain sound helpers
// ---------------------------------------------------------------------------

static void DeathBubblesSpawn(edict_t *self);

static void DeathBubbles(float num_bubbles)
{
    edict_t *bs;
    bs = eng->ED_Alloc();
    eng->SV_SetOrigin(bs, g->self->v.origin);
    bs->v.movetype  = MOVETYPE_NONE;
    bs->v.solid     = SOLID_NOT;
    bs->v.nextthink = g->time + 0.1f;
    bs->v.think     = DeathBubblesSpawn;
    bs->v.air_finished = 0;
    bs->v.owner        = g->self;
    bs->v.bubble_count = num_bubbles;
}

static void DeathBubblesSpawn(edict_t *self)
{
    edict_t *bubble;
    vec3_t bpos, zero = {0,0,0}, bvel = {0,0,15}, bmin = {-8,-8,-8}, bmax = {8,8,8};

    g->self = self;
    if (g->self->v.owner->v.waterlevel != 3) return;

    bubble = eng->ED_Alloc();
    eng->SV_SetModel(bubble, "progs/s_bubble.spr");
    bpos[0] = g->self->v.owner->v.origin[0];
    bpos[1] = g->self->v.owner->v.origin[1];
    bpos[2] = g->self->v.owner->v.origin[2] + 24.0f;
    eng->SV_SetOrigin(bubble, bpos);
    bubble->v.movetype  = MOVETYPE_NOCLIP;
    bubble->v.solid     = SOLID_NOT;
    memcpy(bubble->v.velocity, bvel, sizeof(vec3_t));
    bubble->v.nextthink = g->time + 0.5f;
    bubble->v.think     = bubble_bob;
    bubble->v.classname = "bubble";
    bubble->v.frame     = 0;
    bubble->v.cnt       = 0;
    eng->SV_SetSize(bubble, bmin, bmax);

    g->self->v.nextthink = g->time + 0.1f;
    g->self->v.think     = DeathBubblesSpawn;
    g->self->v.air_finished++;
    if (g->self->v.air_finished >= g->self->v.bubble_count)
        eng->ED_Free(g->self);
    (void)zero;
}

static void PainSound(void)
{
    float rs;
    const char *snd;

    if (g->self->v.health < 0) return;

    if (g->damage_attacker && g->damage_attacker->v.classname &&
        strcmp(g->damage_attacker->v.classname, "teledeath") == 0) {
        eng->SV_StartSound(g->self, CHAN_VOICE, "player/teledth1.wav", 1, ATTN_NONE);
        return;
    }

    if (g->self->v.watertype == CONTENT_WATER && g->self->v.waterlevel == 3) {
        DeathBubbles(1);
        if (eng->Random() > 0.5f)
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/drown1.wav", 1, ATTN_NORM);
        else
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/drown2.wav", 1, ATTN_NORM);
        return;
    }

    if (g->self->v.watertype == CONTENT_SLIME || g->self->v.watertype == CONTENT_LAVA) {
        if (eng->Random() > 0.5f)
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/lburn1.wav", 1, ATTN_NORM);
        else
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/lburn2.wav", 1, ATTN_NORM);
        return;
    }

    if (g->self->v.pain_finished > g->time) {
        g->self->v.axhitme = 0;
        return;
    }
    g->self->v.pain_finished = g->time + 0.5f;

    if (g->self->v.axhitme == 1) {
        g->self->v.axhitme = 0;
        eng->SV_StartSound(g->self, CHAN_VOICE, "player/axhit1.wav", 1, ATTN_NORM);
        return;
    }

    rs = (float)(int)(eng->Random() * 5.0f + 1.5f);  // rint(random()*5 + 1) → 1..6
    if      (rs <= 1) snd = "player/pain1.wav";
    else if (rs <= 2) snd = "player/pain2.wav";
    else if (rs <= 3) snd = "player/pain3.wav";
    else if (rs <= 4) snd = "player/pain4.wav";
    else if (rs <= 5) snd = "player/pain5.wav";
    else              snd = "player/pain6.wav";
    eng->SV_StartSound(g->self, CHAN_VOICE, snd, 1, ATTN_NORM);
}

// ---------------------------------------------------------------------------
// Pain animation
// ---------------------------------------------------------------------------

static void player_pain1_think(edict_t *self) { FRAME_STEP(FR_PAIN1,   player_pain2_think);   PainSound(); g->self->v.weaponframe=0; }
static void player_pain2_think(edict_t *self) { FRAME_STEP(FR_PAIN1+1, player_pain3_think); }
static void player_pain3_think(edict_t *self) { FRAME_STEP(FR_PAIN1+2, player_pain4_think); }
static void player_pain4_think(edict_t *self) { FRAME_STEP(FR_PAIN1+3, player_pain5_think); }
static void player_pain5_think(edict_t *self) { FRAME_STEP(FR_PAIN1+4, player_pain6_think); }
static void player_pain6_think(edict_t *self) { FRAME_STEP(FR_PAIN1+5, player_run_think);   }

static void player_axpain1_think(edict_t *self) { FRAME_STEP(FR_AXPAIN1,   player_axpain2_think); PainSound(); g->self->v.weaponframe=0; }
static void player_axpain2_think(edict_t *self) { FRAME_STEP(FR_AXPAIN1+1, player_axpain3_think); }
static void player_axpain3_think(edict_t *self) { FRAME_STEP(FR_AXPAIN1+2, player_axpain4_think); }
static void player_axpain4_think(edict_t *self) { FRAME_STEP(FR_AXPAIN1+3, player_axpain5_think); }
static void player_axpain5_think(edict_t *self) { FRAME_STEP(FR_AXPAIN1+4, player_axpain6_think); }
static void player_axpain6_think(edict_t *self) { FRAME_STEP(FR_AXPAIN1+5, player_run_think);     }

// Public player_pain — called as th_pain(self, attacker, damage)
void player_pain(edict_t *self, edict_t *attacker, float damage)
{
    g->self = self;
    (void)attacker; (void)damage;
    if (g->self->v.weaponframe) return;
    if (g->self->v.invisible_finished > g->time) return;
    if ((int)g->self->v.weapon == IT_AXE)
        player_axpain1_think(self);
    else
        player_pain1_think(self);
}

// ---------------------------------------------------------------------------
// Death helpers
// ---------------------------------------------------------------------------

static void PlayerDead(edict_t *self)
{
    g->self = self;
    g->self->v.nextthink = -1;
    g->self->v.deadflag  = DEAD_DEAD;
}

static float crandom(void) { return eng->Random() * 2.0f - 1.0f; }

static void VelocityForDamage(float dm, vec3_t out)
{
    out[0] = 100.0f * crandom();
    out[1] = 100.0f * crandom();
    out[2] = 200.0f + 100.0f * eng->Random();
    if (dm > -50)        { out[0]*=0.7f; out[1]*=0.7f; out[2]*=0.7f; }
    else if (dm > -200)  { out[0]*=2.0f; out[1]*=2.0f; out[2]*=2.0f; }
    else                 { out[0]*=10.f; out[1]*=10.f; out[2]*=10.f; }
}

static void ThrowGib(const char *gibname, float dm)
{
    edict_t *gib;
    vec3_t zero = {0,0,0};

    gib = eng->ED_Alloc();
    memcpy(gib->v.origin, g->self->v.origin, sizeof(vec3_t));
    eng->SV_SetModel(gib, gibname);
    eng->SV_SetSize(gib, zero, zero);
    VelocityForDamage(dm, gib->v.velocity);
    gib->v.movetype   = MOVETYPE_BOUNCE;
    gib->v.solid      = SOLID_NOT;
    gib->v.avelocity[0] = eng->Random() * 600;
    gib->v.avelocity[1] = eng->Random() * 600;
    gib->v.avelocity[2] = eng->Random() * 600;
    gib->v.think      = SUB_Remove;
    gib->v.ltime      = g->time;
    gib->v.nextthink  = g->time + 10.0f + eng->Random() * 10.0f;
    gib->v.frame      = 0;
    gib->v.flags      = 0;
}

static void ThrowHead(const char *gibname, float dm)
{
    vec3_t hmin = {-16,-16, 0}, hmax = {16,16,56};
    eng->SV_SetModel(g->self, gibname);
    g->self->v.frame      = 0;
    g->self->v.nextthink  = -1;
    g->self->v.movetype   = MOVETYPE_BOUNCE;
    g->self->v.takedamage = DAMAGE_NO;
    g->self->v.solid      = SOLID_NOT;
    g->self->v.view_ofs[0] = g->self->v.view_ofs[1] = 0;
    g->self->v.view_ofs[2] = 8;
    eng->SV_SetSize(g->self, hmin, hmax);
    VelocityForDamage(dm, g->self->v.velocity);
    g->self->v.origin[2]  -= 24.0f;
    g->self->v.flags = (float)((int)g->self->v.flags & ~FL_ONGROUND);
    g->self->v.avelocity[0] = 0;
    g->self->v.avelocity[1] = crandom() * 600.0f;
    g->self->v.avelocity[2] = 0;
}

static void DeathSound(void)
{
    float rs;
    const char *snd;

    if (g->self->v.waterlevel == 3) {
        DeathBubbles(20);
        eng->SV_StartSound(g->self, CHAN_VOICE, "player/h2odeath.wav", 1, ATTN_NONE);
        return;
    }

    rs = (float)(int)(eng->Random() * 4.0f + 1.5f);
    if      (rs <= 1) snd = "player/death1.wav";
    else if (rs <= 2) snd = "player/death2.wav";
    else if (rs <= 3) snd = "player/death3.wav";
    else if (rs <= 4) snd = "player/death4.wav";
    else              snd = "player/death5.wav";
    eng->SV_StartSound(g->self, CHAN_VOICE, snd, 1, ATTN_NONE);
}

static void GibPlayer(void)
{
    ThrowHead("progs/h_player.mdl", g->self->v.health);
    ThrowGib("progs/gib1.mdl", g->self->v.health);
    ThrowGib("progs/gib2.mdl", g->self->v.health);
    ThrowGib("progs/gib3.mdl", g->self->v.health);
    g->self->v.deadflag = DEAD_DEAD;

    if (g->damage_attacker && g->damage_attacker->v.classname &&
        (strcmp(g->damage_attacker->v.classname, "teledeath") == 0 ||
         strcmp(g->damage_attacker->v.classname, "teledeath2") == 0)) {
        eng->SV_StartSound(g->self, CHAN_VOICE, "player/teledth1.wav", 1, ATTN_NONE);
        return;
    }
    if (eng->Random() < 0.5f)
        eng->SV_StartSound(g->self, CHAN_VOICE, "player/gib.wav", 1, ATTN_NONE);
    else
        eng->SV_StartSound(g->self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NONE);
}

// ---------------------------------------------------------------------------
// Death sequences
// ---------------------------------------------------------------------------

static void player_diea1_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1,    player_diea2_think);  }
static void player_diea2_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 1, player_diea3_think);  }
static void player_diea3_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 2, player_diea4_think);  }
static void player_diea4_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 3, player_diea5_think);  }
static void player_diea5_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 4, player_diea6_think);  }
static void player_diea6_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 5, player_diea7_think);  }
static void player_diea7_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 6, player_diea8_think);  }
static void player_diea8_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 7, player_diea9_think);  }
static void player_diea9_think(edict_t *self)  { FRAME_STEP(FR_DEATHA1+ 8, player_diea10_think); }
static void player_diea10_think(edict_t *self) { FRAME_STEP(FR_DEATHA1+ 9, player_diea11_think); }
static void player_diea11_think(edict_t *self) { FRAME_STEP(FR_DEATHA11,   player_diea11_think); PlayerDead(self); }

static void player_dieb1_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1,   player_dieb2_think); }
static void player_dieb2_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+1, player_dieb3_think); }
static void player_dieb3_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+2, player_dieb4_think); }
static void player_dieb4_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+3, player_dieb5_think); }
static void player_dieb5_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+4, player_dieb6_think); }
static void player_dieb6_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+5, player_dieb7_think); }
static void player_dieb7_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+6, player_dieb8_think); }
static void player_dieb8_think(edict_t *self)  { FRAME_STEP(FR_DEATHB1+7, player_dieb9_think); }
static void player_dieb9_think(edict_t *self)  { FRAME_STEP(FR_DEATHB9,   player_dieb9_think); PlayerDead(self); }

static void player_diec1_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1,    player_diec2_think);  }
static void player_diec2_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 1, player_diec3_think);  }
static void player_diec3_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 2, player_diec4_think);  }
static void player_diec4_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 3, player_diec5_think);  }
static void player_diec5_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 4, player_diec6_think);  }
static void player_diec6_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 5, player_diec7_think);  }
static void player_diec7_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 6, player_diec8_think);  }
static void player_diec8_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 7, player_diec9_think);  }
static void player_diec9_think(edict_t *self)  { FRAME_STEP(FR_DEATHC1+ 8, player_diec10_think); }
static void player_diec10_think(edict_t *self) { FRAME_STEP(FR_DEATHC1+ 9, player_diec11_think); }
static void player_diec11_think(edict_t *self) { FRAME_STEP(FR_DEATHC1+10, player_diec12_think); }
static void player_diec12_think(edict_t *self) { FRAME_STEP(FR_DEATHC1+11, player_diec13_think); }
static void player_diec13_think(edict_t *self) { FRAME_STEP(FR_DEATHC1+12, player_diec14_think); }
static void player_diec14_think(edict_t *self) { FRAME_STEP(FR_DEATHC1+13, player_diec15_think); }
static void player_diec15_think(edict_t *self) { FRAME_STEP(FR_DEATHC15,   player_diec15_think); PlayerDead(self); }

static void player_died1_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1,   player_died2_think); }
static void player_died2_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+1, player_died3_think); }
static void player_died3_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+2, player_died4_think); }
static void player_died4_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+3, player_died5_think); }
static void player_died5_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+4, player_died6_think); }
static void player_died6_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+5, player_died7_think); }
static void player_died7_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+6, player_died8_think); }
static void player_died8_think(edict_t *self)  { FRAME_STEP(FR_DEATHD1+7, player_died9_think); }
static void player_died9_think(edict_t *self)  { FRAME_STEP(FR_DEATHD9,   player_died9_think); PlayerDead(self); }

static void player_diee1_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1,   player_diee2_think); }
static void player_diee2_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+1, player_diee3_think); }
static void player_diee3_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+2, player_diee4_think); }
static void player_diee4_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+3, player_diee5_think); }
static void player_diee5_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+4, player_diee6_think); }
static void player_diee6_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+5, player_diee7_think); }
static void player_diee7_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+6, player_diee8_think); }
static void player_diee8_think(edict_t *self)  { FRAME_STEP(FR_DEATHE1+7, player_diee9_think); }
static void player_diee9_think(edict_t *self)  { FRAME_STEP(FR_DEATHE9,   player_diee9_think); PlayerDead(self); }

static void player_die_ax2_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+1, player_die_ax3_think); }
static void player_die_ax3_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+2, player_die_ax4_think); }
static void player_die_ax4_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+3, player_die_ax5_think); }
static void player_die_ax5_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+4, player_die_ax6_think); }
static void player_die_ax6_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+5, player_die_ax7_think); }
static void player_die_ax7_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+6, player_die_ax8_think); }
static void player_die_ax8_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+7, player_die_ax9_think); }
static void player_die_ax9_think(edict_t *self) { FRAME_STEP(FR_AXDETH1+8, player_die_ax9_think); PlayerDead(self); }
static void player_die_ax1_think(edict_t *self) { FRAME_STEP(FR_AXDETH1,   player_die_ax2_think); }

// ---------------------------------------------------------------------------
// PlayerDie — public entry point (th_die)
// ---------------------------------------------------------------------------

void PlayerDie(edict_t *self)
{
    float i;
    g->self = self;

    g->self->v.items = (float)((int)g->self->v.items & ~IT_INVISIBILITY);
    g->self->v.invisible_finished    = 0;
    g->self->v.invincible_finished   = 0;
    g->self->v.super_damage_finished = 0;
    g->self->v.radsuit_finished      = 0;
    g->self->v.modelindex = modelindex_player;

    if (g->deathmatch || g->coop) DropBackpack();

    g->self->v.weaponmodel = "";
    g->self->v.view_ofs[0] = g->self->v.view_ofs[1] = 0;
    g->self->v.view_ofs[2] = -8;
    g->self->v.deadflag    = DEAD_DYING;
    g->self->v.solid       = SOLID_NOT;
    g->self->v.flags = (float)((int)g->self->v.flags & ~FL_ONGROUND);
    g->self->v.movetype    = MOVETYPE_TOSS;

    if (g->self->v.velocity[2] < 10.0f)
        g->self->v.velocity[2] += eng->Random() * 300.0f;

    if (g->self->v.health < -40) { GibPlayer(); return; }

    DeathSound();
    g->self->v.angles[0] = 0;
    g->self->v.angles[2] = 0;

    if ((int)g->self->v.weapon == IT_AXE) {
        player_die_ax1_think(g->self);
        return;
    }

    i = eng->Cvar_VariableValue("temp1");
    if (!i) i = 1.0f + floorf(eng->Random() * 6.0f);

    if      (i <= 1) player_diea1_think(g->self);
    else if (i <= 2) player_dieb1_think(g->self);
    else if (i <= 3) player_diec1_think(g->self);
    else if (i <= 4) player_died1_think(g->self);
    else             player_diee1_think(g->self);
}

// ---------------------------------------------------------------------------
// set_suicide_frame
// ---------------------------------------------------------------------------

void set_suicide_frame(edict_t *self)
{
    g->self = self;
    if (!g->self->v.model || strcmp(g->self->v.model, "progs/player.mdl") != 0)
        return;
    g->self->v.frame     = FR_DEATHA11;
    g->self->v.solid     = SOLID_NOT;
    g->self->v.movetype  = MOVETYPE_TOSS;
    g->self->v.deadflag  = DEAD_DEAD;
    g->self->v.nextthink = -1;
}

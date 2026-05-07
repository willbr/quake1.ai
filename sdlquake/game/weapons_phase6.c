// weapons_phase6.c -- Wolf3D + Doom1 weapon fire functions + dispatch.
//
// All Phase 6 weapons live behind the (self->v.weapon2 != 0) guard. The main
// W_Attack and W_SetCurrentAmmo in weapons.c branch to W_Attack_Phase6 and
// W_SetCurrentAmmo_Phase6 here when that's true, leaving Quake's stock 8
// weapons untouched.

#include "weapons_phase6.h"
#include "game_defs.h"

#include <math.h>
#include <stddef.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *bomb, edict_t *attacker, float rad, edict_t *ignore);
extern void SpawnBlood(vec3_t org, vec3_t vel, float damage);
extern void player_run(edict_t *self);
extern void BecomeExplosion(void);
extern void SUB_Remove(edict_t *self);

// ---------------------------------------------------------------------------
// Local helpers — separate from weapons.c's static multi-damage state so
// Phase 6 fire functions can interleave with Quake's without mutual stomping.
// ---------------------------------------------------------------------------

static int rand_byte(void) {
    return (int)(eng->Random() * 256.0f) & 0xFF;
}

static float p6_crandom(void) {
    return 2.0f * (eng->Random() - 0.5f);
}

// Single hitscan with optional spread cone. Damage is applied directly
// (no multi-damage accumulator — single bullet, single hit, simple).
static void p6_fire_bullet(float damage, vec3_t aim, float spread_x, float spread_y) {
    edict_t *self = g->self;
    eng->MakeVectors(self->v.v_angle);

    vec3_t src;
    src[0] = self->v.origin[0] + g->v_forward[0]*10;
    src[1] = self->v.origin[1] + g->v_forward[1]*10;
    src[2] = self->v.absmin[2] + self->v.size[2]*0.7f;

    float cx = p6_crandom() * spread_x;
    float cy = p6_crandom() * spread_y;

    vec3_t dir;
    dir[0] = aim[0] + cx*g->v_right[0] + cy*g->v_up[0];
    dir[1] = aim[1] + cx*g->v_right[1] + cy*g->v_up[1];
    dir[2] = aim[2] + cx*g->v_right[2] + cy*g->v_up[2];

    vec3_t end;
    end[0] = src[0] + dir[0]*2048;
    end[1] = src[1] + dir[1]*2048;
    end[2] = src[2] + dir[2]*2048;
    eng->SV_Traceline(src, end, 0, self);

    if (g->trace_fraction == 1.0f)
        return;  // missed everything

    vec3_t hit_org;
    hit_org[0] = g->trace_endpos[0] - dir[0]*4;
    hit_org[1] = g->trace_endpos[1] - dir[1]*4;
    hit_org[2] = g->trace_endpos[2] - dir[2]*4;

    if (g->trace_ent && g->trace_ent->v.takedamage) {
        vec3_t blood_vel = {0, 0, 0};
        SpawnBlood(hit_org, blood_vel, damage);
        T_Damage(g->trace_ent, self, self, damage);
    } else {
        // Hit world geometry — bullet impact tempentity.
        eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte (MSG_BROADCAST, TE_GUNSHOT);
        eng->MSG_WriteCoord(MSG_BROADCAST, hit_org[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, hit_org[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, hit_org[2]);
    }
}

// Doom MELEERANGE: 64 fracunits in Doom = 64 map units in Quake. Used by
// p6_doom_melee_hit for fist + chainsaw line-attacks.
#define MELEERANGE_QU           64.0f

// ---------------------------------------------------------------------------
// Doom melee — A_Punch / A_Saw shared core. Returns 1 on hit, 0 on miss so
// the caller can switch sounds (saw plays a different sample on whiff vs hit).
//
// MELEERANGE in Doom is 64 fracunits = 64 map units in Quake's coordinate
// system (Quake doesn't use a fracunits scale). Damage formula and angle
// jitter are passed in so fist (rnd%10+1)*2 with optional 10x berserk and
// saw 2*(rnd%10+1) can share one body.
// ---------------------------------------------------------------------------
static int p6_doom_melee_hit(int damage, float angle_jitter_radians) {
    edict_t *self = g->self;
    eng->MakeVectors(self->v.v_angle);

    float jx = p6_crandom() * angle_jitter_radians;
    vec3_t aim;
    aim[0] = g->v_forward[0] + jx*g->v_right[0];
    aim[1] = g->v_forward[1] + jx*g->v_right[1];
    aim[2] = g->v_forward[2];

    vec3_t src;
    src[0] = self->v.origin[0];
    src[1] = self->v.origin[1];
    src[2] = self->v.absmin[2] + self->v.size[2]*0.7f;

    vec3_t end;
    end[0] = src[0] + aim[0]*MELEERANGE_QU;
    end[1] = src[1] + aim[1]*MELEERANGE_QU;
    end[2] = src[2] + aim[2]*MELEERANGE_QU;
    eng->SV_Traceline(src, end, 0, self);
    if (g->trace_fraction == 1.0f)
        return 0;

    if (g->trace_ent && g->trace_ent->v.takedamage) {
        vec3_t blood_vel = {0, 0, 0};
        SpawnBlood(g->trace_endpos, blood_vel, (float)damage);
        T_Damage(g->trace_ent, self, self, (float)damage);
        return 1;
    }
    return 0;
}

// Wolf3D damage falloff -- DO NOT change without re-checking WL_AGENT.C:1227-1240.
// US_RndT() returns 0..255 (modeled as rand_byte() & 0xFF here, same range).
#define WOLF_TILE_QU            64.0f   // 1 Wolf tile = 64 Quake units
#define WOLF_NEAR_TILES         2.0f    // dist < NEAR  -> rnd / NEAR_DIVISOR (max 63 dmg)
#define WOLF_MID_TILES          4.0f    // dist < MID   -> rnd / MID_DIVISOR  (max 42 dmg)
#define WOLF_NEAR_DIVISOR       4
#define WOLF_MID_DIVISOR        6
#define WOLF_FAR_MISS_DIVISOR   12      // (rnd / FAR_MISS_DIVISOR) < (int)dist -> miss

// ---------------------------------------------------------------------------
// Wolf3D hitscan — closest-visible-target front-cone + tile-distance damage
// falloff. Models WL_AGENT.C:1168-1243 (GunAttack).
//
// Algorithm:
//   1. ED_FindRadius enumerates all edicts within ~4096 units (plenty for any
//      Quake combat range). Walk the chain via head->v.chain.
//   2. For each candidate: must have health>0, takedamage!=DAMAGE_NO, must be
//      in front of the player (forward dot-product > 0), and within the cone
//      (lateral / forward < tan(cone)).
//   3. Pick the closest by squared distance.
//   4. SV_Traceline to that closest target's bbox centre — if obstructed by
//      world geometry, render a TE_GUNSHOT puff and return miss.
//   5. Apply Wolf's per-tile damage falloff (1 tile = 64 map units):
//        dist<2 -> rnd/4 (0..63), dist<4 -> rnd/6 (0..42),
//        else (rnd/12 < dist) miss, otherwise rnd/6.
//
// Returns 1 on hit, 0 on miss/whiff. Caller plays the sound regardless and
// owns animation; we just resolve the damage.
//
// Deviations from Wolf3D's GunAttack (intentional simplifications):
//   - Single-trace: if the closest in-cone target has world-blocked LOS,
//     we whiff and return 0. Wolf3D iterates to the next-closest target;
//     we don't. Acceptable because the player still gets visual feedback
//     (TE_GUNSHOT puff at the obstacle).
//   - Distance metric: Euclidean / 64 instead of Wolf's Chebyshev tile
//     distance (max(|dx|,|dy|)). Difference is at most ~40% on diagonals;
//     "Wolf-ish feel" rather than byte-for-byte parity is the goal.
// ---------------------------------------------------------------------------
static int p6_wolf_hitscan(float shoot_cone_radians) {
    edict_t *self = g->self;
    eng->MakeVectors(self->v.v_angle);

    edict_t *closest = NULL;
    float    closest_d2 = 1e18f;

    edict_t *head = eng->ED_FindRadius(self->v.origin, 4096.0f);
    while (head) {
        edict_t *e = head;
        head = head->v.chain;  // grab next before continuing — defensive, in case anything mutates
        if (e == self) continue;
        if (e->v.health <= 0) continue;
        if (e->v.takedamage == DAMAGE_NO) continue;

        vec3_t to;
        to[0] = e->v.origin[0] - self->v.origin[0];
        to[1] = e->v.origin[1] - self->v.origin[1];
        to[2] = e->v.origin[2] - self->v.origin[2];
        float fwd = to[0]*g->v_forward[0] + to[1]*g->v_forward[1] + to[2]*g->v_forward[2];
        if (fwd <= 0) continue;

        float lat_x = to[0] - fwd*g->v_forward[0];
        float lat_y = to[1] - fwd*g->v_forward[1];
        float lat_z = to[2] - fwd*g->v_forward[2];
        float lat   = sqrtf(lat_x*lat_x + lat_y*lat_y + lat_z*lat_z);
        if (lat / fwd > tanf(shoot_cone_radians)) continue;

        float d2 = to[0]*to[0] + to[1]*to[1] + to[2]*to[2];
        if (d2 < closest_d2) { closest_d2 = d2; closest = e; }
    }

    if (!closest) return 0;

    vec3_t src;
    src[0] = self->v.origin[0];
    src[1] = self->v.origin[1];
    src[2] = self->v.absmin[2] + self->v.size[2]*0.7f;
    vec3_t tgt;
    tgt[0] = (closest->v.absmin[0] + closest->v.absmax[0]) * 0.5f;
    tgt[1] = (closest->v.absmin[1] + closest->v.absmax[1]) * 0.5f;
    tgt[2] = (closest->v.absmin[2] + closest->v.absmax[2]) * 0.5f;
    // World-LOS check only -- nomonsters=1 skips every monster including the
    // target, matching Wolf3D's CheckLine semantics. The trace can still hit
    // non-monster takedamage entities (buttons, breakables); the
    // g->trace_ent != closest guard below covers the case where such an entity
    // is between us and the target.
    eng->SV_Traceline(src, tgt, 1, self);
    if (g->trace_fraction < 1.0f && g->trace_ent != closest) {
        eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte (MSG_BROADCAST, TE_GUNSHOT);
        eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[2]);
        return 0;
    }

    float dist = sqrtf(closest_d2) / WOLF_TILE_QU;
    int rnd = rand_byte();
    int damage;
    if (dist < WOLF_NEAR_TILES)      damage = rnd / WOLF_NEAR_DIVISOR;
    else if (dist < WOLF_MID_TILES)  damage = rnd / WOLF_MID_DIVISOR;
    else {
        if ((rnd / WOLF_FAR_MISS_DIVISOR) < (int)dist) return 0;
        damage = rnd / WOLF_MID_DIVISOR;
    }
    if (damage <= 0) return 0;

    vec3_t blood_vel = {0, 0, 0};
    SpawnBlood(tgt, blood_vel, (float)damage);
    T_Damage(closest, self, self, (float)damage);
    return 1;
}

// ---------------------------------------------------------------------------
// Doom pistol -- A_FirePistol from p_pspr.c
//   damage  = 5 * ((P_Random % 3) + 1)   = 5, 10, or 15
//   ammo    = 1 bullet
//   refire  = ~14 tics  (35 Hz Doom tic) ≈ 0.4 s
//   sound   = sfx_pistol  (= phase6/doom_pistol.wav after our extract)
// ---------------------------------------------------------------------------
#define DOOMPISTOL_CHAIN_S  (19.0f * (1.0f / 35.0f))  // matches DOOM_TIC_S; defined locally to avoid leaking from player_phase6.c

void W_FirePhase6_DoomPistol(void) {
    edict_t *self = g->self;

    if (self->v.ammo_bullets < 1) {
        // Doom-authentic: keep the pistol up, do nothing on press.
        // Player must switch weapons manually (impulse) or pick up another.
        return;
    }

    // Total chain = 4+6+4+5 = 19 tics. Constant DOOMPISTOL_CHAIN_S keeps this
    // in sync with the per-step DOOM_*_TIC values in player_phase6.c. The
    // bullet, sound, muzzleflash, punchangle, and ammo decrement happen on
    // entering S_PISTOL2 (the recoil pose) — see DoomPistol_DoFire, called
    // from player_doompistol2_think. Press → 4-tic idle hold → bullet leaves;
    // this lockout is intentional Doom feel.
    self->v.attack_finished = g->time + DOOMPISTOL_CHAIN_S;
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

// ---------------------------------------------------------------------------
// Doom fist -- A_Punch from p_pspr.c
//   damage  = (P_Random%10+1)<<1 = 2..20  (no berserk multiplier)
//   range   = MELEERANGE_QU = 64 map units
//   refire  = 22 tics (35 Hz Doom tic) ≈ 0.629 s
//   sound   = sfx_punch on hit only (silent on whiff)
//   chain   = S_PUNCH1..S_PUNCH5 (4/4/5/4/5 tics)
// ---------------------------------------------------------------------------
#define DOOMFIST_CHAIN_S  (22.0f * (1.0f / 35.0f))  // matches DOOM_TIC_S; defined locally to avoid leaking from player_phase6.c

void W_FirePhase6_DoomFist(void) {
    edict_t *self = g->self;
    // Total chain = 4+4+5+4+5 = 22 tics. Constant DOOMFIST_CHAIN_S keeps this in sync with the per-step DOOM_*_TIC values in player_phase6.c.
    self->v.attack_finished = g->time + DOOMFIST_CHAIN_S;
    player_doomfist1(self);
}

void DoomFist_DoFire(edict_t *self) {
    int dmg = ((rand_byte() % 10) + 1) << 1;  // 2..20
    int hit = p6_doom_melee_hit(dmg, 0.05f);
    if (hit)
        eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_punch.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -1;
}

// ---------------------------------------------------------------------------
// Doom chainsaw -- A_Saw from p_pspr.c
//   damage  = 2*(P_Random%10+1) = 2..20
//   range   = MELEERANGE_QU = 64 map units
//   refire  = ~0.229 s while held; both chain frames fire (S_SAW1+S_SAW2)
//   sound   = sfx_sawhit on flesh, sfx_sawful on whiff/wall
// ---------------------------------------------------------------------------
#define DOOMSAW_CHAIN_S  (8.0f * (1.0f / 35.0f))  // 8 tics = 2 frames * DOOM_4_TIC

void W_FirePhase6_DoomChainsaw(void) {
    edict_t *self = g->self;
    // 4+4+0 tics = 0.229s, plus the 0-tic A_ReFire which immediately re-checks
    // button0 + attack_finished. Hold fire = continuous saw.
    self->v.attack_finished = g->time + DOOMSAW_CHAIN_S;
    player_doomsaw1(self);
}

void DoomSaw_DoFire(edict_t *self) {
    int dmg = 2 * ((rand_byte() % 10) + 1);  // 2..20
    int hit = p6_doom_melee_hit(dmg, 0.05f);
    eng->SV_StartSound(self, CHAN_WEAPON,
                       hit ? "phase6/doom_sawhit.wav" : "phase6/doom_sawful.wav",
                       1, ATTN_NORM);
    if (hit) self->v.punchangle[0] = -1;
}

// ---------------------------------------------------------------------------
// Doom chaingun -- A_FireCGun from p_pspr.c
//   damage  = 5*(P_Random%3+1) = 5/10/15 (same as pistol)
//   ammo    = 1 bullet per shot; both chain frames fire = 2 bullets per chain
//   refire  = 4 tics per shot (35 Hz Doom tic) ~= 8.7 Hz sustained
//   sound   = sfx_pistol (= phase6/doom_pistol.wav, shared with pistol)
//   spread  = wider cone than pistol (0.04 each axis ~= 2.3 deg)
// ---------------------------------------------------------------------------
#define DOOMCG_CHAIN_S  (8.0f * (1.0f / 35.0f))  // 8 tics = 2 frames * DOOM_4_TIC

void W_FirePhase6_DoomChaingun(void) {
    edict_t *self = g->self;
    if (self->v.ammo_bullets < 1) return;  // silent stay (Doom-authentic)
    self->v.attack_finished = g->time + DOOMCG_CHAIN_S;
    player_doomchaingun1(self);
}

void DoomChaingun_DoFire(edict_t *self) {
    if (self->v.ammo_bullets < 1) return;

    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_pistol.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -1;
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);
    self->v.ammo_bullets -= 1;
    self->v.currentammo   = self->v.ammo_bullets;

    int dmg = 5 * ((rand_byte() % 3) + 1);
    vec3_t aim;
    eng->SV_Aim(self, 100000, aim);
    p6_fire_bullet((float)dmg, aim, 0.04f, 0.04f);
}

// ---------------------------------------------------------------------------
// Doom shotgun -- A_FireShotgun from p_pspr.c
//   damage  = 7 pellets, each 5*(P_Random%3+1) = 5/10/15 (same as pistol)
//   ammo    = 1 shell per shot
//   refire  = 44 tics (8 chain steps + 7-tic A_ReFire) ~= 1.257 s
//   sound   = sfx_shotgn (= phase6/doom_shotgn.wav)
//   spread  = 7 pellets in a horizontal fan (0.07/0.01) -- tighter than
//             Doom's 0.7 rad to suit Quake combat ranges
// ---------------------------------------------------------------------------
#define DOOMSGUN_CHAIN_S  (44.0f * (1.0f / 35.0f))   // 9 chain states (3+7+5+5+4+5+5+3+7) -- last 7-tic state is folded as refire delay

void W_FirePhase6_DoomShotgun(void) {
    edict_t *self = g->self;
    if (self->v.ammo_shells < 1) return;
    // 3+7+5+5+4+5+5+3 = 37 tics for chain steps, plus 7 tic A_ReFire delay
    // = 44 tics total. Use 44 tics for attack_finished so refire holds the
    // standard Doom shotgun 1.257s lockout.
    self->v.attack_finished = g->time + DOOMSGUN_CHAIN_S;
    player_doomshotgun1(self);
}

void DoomShotgun_DoFire(edict_t *self) {
    if (self->v.ammo_shells < 1) return;

    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_shotgn.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);
    self->v.ammo_shells -= 1;
    self->v.currentammo  = self->v.ammo_shells;

    vec3_t aim;
    eng->SV_Aim(self, 100000, aim);
    // 7 pellets, each 5/10/15 damage. Doom uses ~0.7 rad horizontal spread
    // (very wide); we use 0.07 for a tighter cone since Quake's combat
    // distances are larger than Doom's typical 4-tile shotgun range.
    for (int i = 0; i < 7; i++) {
        int dmg = 5 * ((rand_byte() % 3) + 1);
        p6_fire_bullet((float)dmg, aim, 0.07f, 0.01f);
    }
}

// ---------------------------------------------------------------------------
// Doom rocket launcher -- A_FireMissile from p_pspr.c
//   damage  = 20..160 direct (random per Doom MT_ROCKET base damage)
//   splash  = 128 unit radius (vs Quake's 120)
//   speed   = 660 ups (Doom MT_ROCKET ~700 fracunits/s, rounded down)
//   ammo    = 1 rocket per shot
//   refire  = 27 tics (8+12+7 settle) ~= 0.771 s
//   sound   = sfx_rlaunc (= phase6/doom_rlaunch.wav)
//   notes   = no shambler-resistance multiplier, no random Z gib bonus
// ---------------------------------------------------------------------------
#define DOOMROCKET_CHAIN_S  (27.0f * (1.0f / 35.0f))   // 8-tic flash hold + 12-tic fire + 7-tic settle

// Doom MT_ROCKET: 660 ups, 128 splash, 20-160 direct damage, no gibbing
// bonus, no shambler resistance multiplier. Fired by W_FirePhase6_DoomRocket
// via DoomRocket_DoFire on the second chain step.
static void DoomRocket_Touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) {
        eng->ED_Free(self);
        return;
    }

    if (other->v.health) {
        // Doom MT_ROCKET direct hit: mobjinfo damage (20) * ((P_Random()&7)+1)
        // = 20, 40, 60, 80, 100, 120, 140, or 160. Matches p_inter.c's
        // P_DamageMobj missile-damage calculation exactly.
        float damg = 20.0f * (float)((rand_byte() & 7) + 1);
        T_Damage(other, self, self->v.owner, damg);
    }
    T_RadiusDamage(self, self->v.owner, 128, other);

    // Pull back a bit so the explosion graphic doesn't z-fight the wall
    // (same trick as Quake's T_MissileTouch).
    float vlen = eng->VectorLength(self->v.velocity) + 0.0001f;
    self->v.origin[0] -= 8 * (self->v.velocity[0] / vlen);
    self->v.origin[1] -= 8 * (self->v.velocity[1] / vlen);
    self->v.origin[2] -= 8 * (self->v.velocity[2] / vlen);

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);

    BecomeExplosion();
}

void W_FirePhase6_DoomRocket(void) {
    edict_t *self = g->self;
    if (self->v.ammo_rockets < 1) return;
    // 8+12 tics = 0.571s for the chain; A_ReFire at the end has 0 tics so
    // refire is gated purely by attack_finished (we add a small extra
    // 7-tic settle so the player feels the rocket's weight).
    self->v.attack_finished = g->time + DOOMROCKET_CHAIN_S;
    player_doomrocket1(self);
}

void DoomRocket_DoFire(edict_t *self) {
    if (self->v.ammo_rockets < 1) return;

    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_rlaunch.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);
    self->v.ammo_rockets -= 1;
    self->v.currentammo   = self->v.ammo_rockets;

    edict_t *missile = eng->ED_Alloc();
    missile->v.owner    = self;
    missile->v.movetype = MOVETYPE_FLYMISSILE;
    missile->v.solid    = SOLID_BBOX;
    missile->v.classname = "rocket";  // for reverse-lookup / debugging

    eng->MakeVectors(self->v.v_angle);
    eng->SV_Aim(self, 1000, missile->v.velocity);
    // 660 ups -- matches Doom MT_ROCKET (700 fracunits/s in source, rounded
    // down for tactile feel).
    missile->v.velocity[0] *= 660.0f;
    missile->v.velocity[1] *= 660.0f;
    missile->v.velocity[2] *= 660.0f;
    eng->VectorToAngles(missile->v.velocity, missile->v.angles);

    missile->v.touch     = DoomRocket_Touch;
    missile->v.nextthink = g->time + 5;
    missile->v.think     = SUB_Remove;
    eng->SV_SetModel(missile, "progs/missile.mdl");
    vec3_t mzero = {0,0,0};
    eng->SV_SetSize(missile, mzero, mzero);
    vec3_t morg = {
        self->v.origin[0] + g->v_forward[0]*8,
        self->v.origin[1] + g->v_forward[1]*8,
        self->v.origin[2] + g->v_forward[2]*8 + 16
    };
    eng->SV_SetOrigin(missile, morg);
}

// ---------------------------------------------------------------------------
// Wolf3D knife -- KnifeAttack from WL_AGENT.C:1133-1164
//   damage  = US_RndT() >> 4 = 0..15
//   range   = MELEERANGE_QU = 64 map units (Wolf used 0x18000l ~= 96 units;
//             64 is fine because Quake combat distances are larger anyway)
//   refire  = 4 steps x 6 tics @ 70 Hz = 24 tics ~= 0.343 s
//   sound   = none -- shareware Wolf3D's knife is Adlib-only; manifest skips it
// ---------------------------------------------------------------------------
#define WOLFKNIFE_CHAIN_S  (24.0f * (1.0f / 70.0f))

void W_FirePhase6_WolfKnife(void) {
    edict_t *self = g->self;
    self->v.attack_finished = g->time + WOLFKNIFE_CHAIN_S;
    player_wolfknife1(self);
}

void WolfKnife_DoHit(edict_t *self) {
    int dmg = rand_byte() >> 4;  // 0..15
    if (dmg <= 0) return;
    // No sound -- shareware Wolf3D's knife is Adlib-only; no PCM extracted.
    p6_doom_melee_hit(dmg, 0.02f);
}

// ---------------------------------------------------------------------------
// Wolf3D pistol -- GunAttack from WL_AGENT.C:1168-1243
//   damage  = p6_wolf_hitscan tile-falloff (0..63 close, 0..42 mid, far chance to whiff)
//   ammo    = 1 bullet per shot; single-shot per press semantics
//   refire  = 24 chain tics + 7 tap-not-hold buffer = 31 tics @ 70 Hz ~= 0.443 s
//   sound   = phase6/wolf_pistol.wav
//   cone    = 0.05 rad ~= 2.9 deg (tighter than MG/chaingun)
// ---------------------------------------------------------------------------
#define WOLFPISTOL_CHAIN_S  (31.0f * (1.0f / 70.0f))

void W_FirePhase6_WolfPistol(void) {
    edict_t *self = g->self;
    if (self->v.ammo_bullets < 1) return;  // silent stay
    self->v.attack_finished = g->time + WOLFPISTOL_CHAIN_S;
    player_wolfpistol1(self);
}

void WolfPistol_DoFire(edict_t *self) {
    if (self->v.ammo_bullets < 1) return;
    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/wolf_pistol.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -1;
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);
    self->v.ammo_bullets -= 1;
    self->v.currentammo   = self->v.ammo_bullets;
    p6_wolf_hitscan(0.05f);
}

// ---------------------------------------------------------------------------
// Wolf3D MG -- attackinfo {6,0,1},{6,1,2},{6,3,3},{6,-1,4} folded
//   damage  = p6_wolf_hitscan tile-falloff
//   ammo    = 1 bullet per shot
//   refire  = 12 tics @ 70 Hz ~= 0.171s -> ~5.8 Hz sustained
//   sound   = phase6/wolf_mg.wav
//   cone    = 0.07 rad ~= 4 deg (slightly wider than pistol)
// ---------------------------------------------------------------------------
#define WOLFMG_CHAIN_S  (12.0f * (1.0f / 70.0f))

void W_FirePhase6_WolfMG(void) {
    edict_t *self = g->self;
    if (self->v.ammo_bullets < 1) return;
    self->v.attack_finished = g->time + WOLFMG_CHAIN_S;
    player_wolfmg1(self);
}

void WolfMG_DoFire(edict_t *self) {
    if (self->v.ammo_bullets < 1) return;
    eng->SV_StartSound(self, CHAN_WEAPON, "phase6/wolf_mg.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -1;
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);
    self->v.ammo_bullets -= 1;
    self->v.currentammo   = self->v.ammo_bullets;
    p6_wolf_hitscan(0.07f);
}

// ---------------------------------------------------------------------------
// Stubs for not-yet-implemented Phase 6 weapons (Phase D/E/F).
// They just return so dispatch is safe even if a weapon flag is granted
// before its fire function is written.
// ---------------------------------------------------------------------------
void W_FirePhase6_WolfChaingun (void) {}

// ---------------------------------------------------------------------------
// Top-level dispatchers — called from weapons.c when self->v.weapon2 != 0.
// ---------------------------------------------------------------------------
void W_Attack_Phase6(void) {
    int it2 = (int)g->self->v.weapon2;
    switch (it2) {
        case IT2_DOOM_FIST:      W_FirePhase6_DoomFist();     break;
        case IT2_DOOM_PISTOL:    W_FirePhase6_DoomPistol();   break;
        case IT2_DOOM_SHOTGUN:   W_FirePhase6_DoomShotgun();  break;
        case IT2_DOOM_CHAINGUN:  W_FirePhase6_DoomChaingun(); break;
        case IT2_DOOM_ROCKET:    W_FirePhase6_DoomRocket();   break;
        case IT2_DOOM_CHAINSAW:  W_FirePhase6_DoomChainsaw(); break;
        case IT2_WOLF_KNIFE:     W_FirePhase6_WolfKnife();    break;
        case IT2_WOLF_PISTOL:    W_FirePhase6_WolfPistol();   break;
        case IT2_WOLF_MACHINEGUN: W_FirePhase6_WolfMG();      break;
        case IT2_WOLF_CHAINGUN:  W_FirePhase6_WolfChaingun(); break;
        default: /* unknown — silently noop */                break;
    }
}

void W_SetCurrentAmmo_Phase6(int it2) {
    edict_t *self = g->self;
    self->v.weaponframe = 0;
    switch (it2) {
        case IT2_DOOM_FIST:
            self->v.weaponmodel = "progs/v_doomfist.spr";
            self->v.currentammo = 0;
            break;
        case IT2_DOOM_PISTOL:
            self->v.weaponmodel = "progs/v_doompistol.spr";
            self->v.currentammo = self->v.ammo_bullets;
            break;
        case IT2_DOOM_SHOTGUN:
            self->v.weaponmodel = "progs/v_doomshotgun.spr";
            self->v.currentammo = self->v.ammo_shells;
            break;
        case IT2_DOOM_CHAINGUN:
            self->v.weaponmodel = "progs/v_doomchaingun.spr";
            self->v.currentammo = self->v.ammo_bullets;
            break;
        case IT2_DOOM_ROCKET:
            self->v.weaponmodel = "progs/v_doomrocket.spr";
            self->v.currentammo = self->v.ammo_rockets;
            break;
        case IT2_DOOM_CHAINSAW:
            self->v.weaponmodel = "progs/v_doomchainsaw.spr";
            self->v.currentammo = 0;
            break;
        case IT2_WOLF_KNIFE:
            self->v.weaponmodel = "progs/v_wolfknife.spr";
            self->v.currentammo = 0;
            break;
        case IT2_WOLF_PISTOL:
            self->v.weaponmodel = "progs/v_wolfpistol.spr";
            self->v.currentammo = self->v.ammo_bullets;
            break;
        case IT2_WOLF_MACHINEGUN:
            self->v.weaponmodel = "progs/v_wolfmg.spr";
            self->v.currentammo = self->v.ammo_bullets;
            break;
        case IT2_WOLF_CHAINGUN:
            self->v.weaponmodel = "progs/v_wolfchaingun.spr";
            self->v.currentammo = self->v.ammo_bullets;
            break;
        default:
            self->v.weaponmodel = "";
            self->v.currentammo = 0;
            break;
    }
}

// ---------------------------------------------------------------------------
// Precache — call from worldspawn so all .spr + .wav assets are ready.
// ---------------------------------------------------------------------------
void Phase6_PrecacheCommon(void) {
    eng->PrecacheModel("progs/v_doomfist.spr");
    eng->PrecacheModel("progs/v_doompistol.spr");
    eng->PrecacheModel("progs/v_doomshotgun.spr");
    eng->PrecacheModel("progs/v_doomchaingun.spr");
    eng->PrecacheModel("progs/v_doomrocket.spr");
    eng->PrecacheModel("progs/v_doomchainsaw.spr");
    eng->PrecacheModel("progs/v_wolfknife.spr");
    eng->PrecacheModel("progs/v_wolfpistol.spr");
    eng->PrecacheModel("progs/v_wolfmg.spr");
    eng->PrecacheModel("progs/v_wolfchaingun.spr");

    eng->PrecacheSound("phase6/doom_pistol.wav");
    eng->PrecacheSound("phase6/doom_shotgn.wav");
    eng->PrecacheSound("phase6/doom_rlaunch.wav");
    eng->PrecacheSound("phase6/doom_punch.wav");
    eng->PrecacheSound("phase6/doom_sawhit.wav");
    eng->PrecacheSound("phase6/doom_sawful.wav");
    eng->PrecacheSound("phase6/wolf_pistol.wav");
    eng->PrecacheSound("phase6/wolf_mg.wav");
    eng->PrecacheSound("phase6/wolf_chaingun.wav");
}

// ---------------------------------------------------------------------------
// Impulse mapping — 30..39 → IT2_* flags, 100 = give all (Phase G1 cheat).
// (Impulse 12 is reserved for stock-Quake "previous weapon".)
// ---------------------------------------------------------------------------
void Phase6_ChangeWeapon(int impulse) {
    edict_t *self = g->self;

    int flag = 0;
    switch (impulse) {
        case 30: flag = IT2_DOOM_FIST;      break;
        case 31: flag = IT2_DOOM_PISTOL;    break;
        case 32: flag = IT2_DOOM_SHOTGUN;   break;
        case 33: flag = IT2_DOOM_CHAINGUN;  break;
        case 34: flag = IT2_DOOM_ROCKET;    break;
        case 35: flag = IT2_DOOM_CHAINSAW;  break;
        case 36: flag = IT2_WOLF_KNIFE;     break;
        case 37: flag = IT2_WOLF_PISTOL;    break;
        case 38: flag = IT2_WOLF_MACHINEGUN;break;
        case 39: flag = IT2_WOLF_CHAINGUN;  break;
        default: return;
    }

    if (!((int)self->v.items2 & flag)) {
        eng->Con_Print("no weapon\n");
        return;
    }

    self->v.weapon  = 0;
    self->v.weapon2 = (float)flag;
    W_SetCurrentAmmo_Phase6(flag);
}

void Phase6_CheatGiveAll(void) {
    edict_t *self = g->self;
    self->v.items2 = (float)(
        IT2_DOOM_FIST | IT2_DOOM_PISTOL | IT2_DOOM_SHOTGUN |
        IT2_DOOM_CHAINGUN | IT2_DOOM_ROCKET | IT2_DOOM_CHAINSAW |
        IT2_WOLF_KNIFE | IT2_WOLF_PISTOL | IT2_WOLF_MACHINEGUN | IT2_WOLF_CHAINGUN
    );
    if (self->v.ammo_bullets < 200) self->v.ammo_bullets = 200;
    if (self->v.ammo_shells  < 50 ) self->v.ammo_shells  = 50;
    if (self->v.ammo_rockets < 20 ) self->v.ammo_rockets = 20;
    eng->Con_Print("phase 6: all weapons granted\n");
}

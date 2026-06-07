// weapons_phase6.c -- Doom1 weapon fire functions + dispatch.
//
// All Phase 6 weapons live behind the (self->v.weapon2 != 0) guard. The main
// W_Attack and W_SetCurrentAmmo in weapons.c branch to W_Attack_Phase6 and
// W_SetCurrentAmmo_Phase6 here when that's true, leaving Quake's stock 8
// weapons untouched.

#include "weapons_phase6.h"
#include "weapons_fire.h"
#include "game_defs.h"

#include <stddef.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *bomb, edict_t *attacker, float rad, edict_t *ignore);
extern void SpawnBlood(vec3_t org, vec3_t vel, float damage);
extern void Corpse_BulletTrace(vec3_t start, vec3_t end, edict_t *skip);
extern void gib_apply_hit_impulse(edict_t *gib, vec3_t dir, float damage);

static int p6_is_gib(edict_t *e) {
    return e && e->v.classname && strcmp(e->v.classname, "gib") == 0;
}
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
    Corpse_BulletTrace(src, end, self);

    if (g->trace_fraction == 1.0f)
        return;  // missed everything

    vec3_t hit_org;
    hit_org[0] = g->trace_endpos[0] - dir[0]*4;
    hit_org[1] = g->trace_endpos[1] - dir[1]*4;
    hit_org[2] = g->trace_endpos[2] - dir[2]*4;

    if (g->trace_ent && g->trace_ent->v.takedamage) {
        vec3_t blood_vel = {0, 0, 0};
        SpawnBlood(hit_org, blood_vel, damage);
        // Hitscan impulse on gibs — T_Damage's gib branch suppresses its own
        // impulse for client inflictors, so apply along the bullet's path here.
        if (p6_is_gib(g->trace_ent)) {
            vec3_t ndir; eng->VectorNormalize(dir, ndir);
            gib_apply_hit_impulse(g->trace_ent, ndir, damage);
        }
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
    Corpse_BulletTrace(src, end, self);
    if (g->trace_fraction == 1.0f)
        return 0;

    if (g->trace_ent && g->trace_ent->v.takedamage) {
        vec3_t blood_vel = {0, 0, 0};
        SpawnBlood(g->trace_endpos, blood_vel, (float)damage);
        if (p6_is_gib(g->trace_ent)) {
            vec3_t ndir; eng->VectorNormalize(aim, ndir);
            gib_apply_hit_impulse(g->trace_ent, ndir, (float)damage);
        }
        T_Damage(g->trace_ent, self, self, (float)damage);
        return 1;
    }
    return 0;
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
// Doom chainsaw idle animation. Doom info.c states S_SAW (SAWGC, 4 tics) and
// S_SAWB (SAWGD, 4 tics) alternate while the chainsaw is up and idle, with
// A_WeaponReady playing sfx_sawidl every time the state machine enters S_SAW.
// One full alternation = 8 tics = 0.229 s, so the idle sound plays ~4.4 Hz.
//
// Called from player_run_think / player_stand1_think (which run on the
// player's nextthink slot only when no attack chain is active). Mid-attack
// the chain owns weaponframe directly, so the idle hook is correctly idle.
// ---------------------------------------------------------------------------
#define DOOMSAW_IDLE_PHASE_TICS  4    // tics per SAWGC↔SAWGD swap (matches Doom S_SAW/S_SAWB duration)

void Phase6_WeaponIdleFrame(edict_t *self) {
    int it2 = (int)self->v.weapon2;
    if (it2 != IT2_DOOM_CHAINSAW) {
        self->v.weaponframe = 0;
        return;
    }

    // While fire is held, the attack chain owns weaponframe. Skipping the
    // idle override here prevents a 1-tick flash to SAWGC/SAWGD between
    // attack cycles -- they have a 34-pixel-higher topoffset than SAWGA/B,
    // so any momentary hop into idle pose visibly jumps the saw vertically.
    // The chain keeps SAWGA after doomsaw3_think; the next refire then sets
    // SAWGA again via doomsaw1, so the swing stays at the attack y-position.
    if (self->v.button0) return;

    // Drive alternation off world time so the running-saw look is
    // independent of however often this hook fires.
    int phase = ((int)(g->time * 35.0f) / DOOMSAW_IDLE_PHASE_TICS) & 1;
    self->v.weaponframe = phase ? 3 : 2;   // 0 = SAWGC (S_SAW), 1 = SAWGD (S_SAWB)

    // Doom plays sfx_sawidl on each entry to S_SAW. We detect the 1→0 phase
    // transition. Single-player Quake, so a file-static is fine here; the
    // worst case across saw-equip cycles is one missed idle blip.
    static int last_phase = -1;
    if (phase == 0 && last_phase != 0) {
        eng->SV_StartSound(self, CHAN_WEAPON, "phase6/doom_sawidl.wav", 1, ATTN_NORM);
    }
    last_phase = phase;
}

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
        case IT2_OILGUN:       W_FireOilGun();       break;
        case IT2_FLAMETHROWER: W_FireFlamethrower(); break;
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
        case IT2_OILGUN:
            self->v.weaponmodel = "progs/v_rock.mdl";    // grenade launcher model = oil sprayer
            self->v.currentammo = self->v.ammo_cells;
            break;
        case IT2_FLAMETHROWER:
            self->v.weaponmodel = "progs/v_light.mdl";   // lightning gun model = flamethrower
            self->v.currentammo = self->v.ammo_cells;
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

    eng->PrecacheSound("phase6/doom_pistol.wav");
    eng->PrecacheSound("phase6/doom_shotgn.wav");
    eng->PrecacheSound("phase6/doom_rlaunch.wav");
    eng->PrecacheSound("phase6/doom_punch.wav");
    eng->PrecacheSound("phase6/doom_sawhit.wav");
    eng->PrecacheSound("phase6/doom_sawful.wav");
    eng->PrecacheSound("phase6/doom_sawidl.wav");
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
        case 40: flag = IT2_OILGUN;       break;
        case 41: flag = IT2_FLAMETHROWER; break;
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
        IT2_OILGUN | IT2_FLAMETHROWER
    );
    if (self->v.ammo_bullets < 200) self->v.ammo_bullets = 200;
    if (self->v.ammo_shells  < 50 ) self->v.ammo_shells  = 50;
    if (self->v.ammo_rockets < 20 ) self->v.ammo_rockets = 20;
    if (self->v.ammo_cells   < 200) self->v.ammo_cells   = 200;
    eng->Con_Print("phase 6: all weapons granted\n");
}

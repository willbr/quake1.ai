// weapons_phase6.c -- Wolf3D + Doom1 weapon fire functions + dispatch.
//
// All Phase 6 weapons live behind the (self->v.weapon2 != 0) guard. The main
// W_Attack and W_SetCurrentAmmo in weapons.c branch to W_Attack_Phase6 and
// W_SetCurrentAmmo_Phase6 here when that's true, leaving Quake's stock 8
// weapons untouched.

#include "weapons_phase6.h"
#include "game_defs.h"

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *bomb, edict_t *attacker, float rad, edict_t *ignore);
extern void SpawnBlood(vec3_t org, vec3_t vel, float damage);
extern void player_run(edict_t *self);

// W_BestWeapon lives in weapons.c — falls back to Quake roster on dry ammo.
extern float W_BestWeapon(void);
extern void  W_SetCurrentAmmo(void);

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

// ---------------------------------------------------------------------------
// Doom pistol -- A_FirePistol from p_pspr.c
//   damage  = 5 * ((P_Random % 3) + 1)   = 5, 10, or 15
//   ammo    = 1 bullet
//   refire  = ~14 tics  (35 Hz Doom tic) ≈ 0.4 s
//   sound   = sfx_pistol  (= phase6/doom_pistol.wav after our extract)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Stubs for not-yet-implemented Phase 6 weapons (Phase D/E/F).
// They just return so dispatch is safe even if a weapon flag is granted
// before its fire function is written.
// ---------------------------------------------------------------------------
void W_FirePhase6_DoomFist     (void) {}
void W_FirePhase6_DoomShotgun  (void) {}
void W_FirePhase6_DoomChaingun (void) {}
void W_FirePhase6_DoomRocket   (void) {}
void W_FirePhase6_DoomChainsaw (void) {}
void W_FirePhase6_WolfKnife    (void) {}
void W_FirePhase6_WolfPistol   (void) {}
void W_FirePhase6_WolfMG       (void) {}
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

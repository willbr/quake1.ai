// M8 / F3 fire weapons -- oil gun + flamethrower.
// Built on the Phase 6 weapon2 selector; held-fire cadence modeled on the
// lightning gun (player_light1/2_think, weapons.c:1485 + player.c:294-318).
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "game_types.h"
#include "weapons_fire.h"
#include "flammables.h"
#include "sim/sim.h"

extern engine_api_t  *eng;
extern game_globals_t *g;

// Non-static game functions defined elsewhere (called cross-file already).
extern void  player_run(edict_t *self);
extern float W_BestWeapon(void);
extern void  W_SetCurrentAmmo(void);

#define FR_FIRE_BODY 113   // FR_SHOTATT1 -- generic shooting stance (player.c:39)

void WeaponsFire_Init(void) {
    // Flamethrower cone + ignition.
    eng->Cvar_Register("fire_flame_range", "220");   // cone length (units)
    eng->Cvar_Register("fire_flame_cone",  "25");    // cone half-angle (degrees)
    eng->Cvar_Register("fire_flame_secs",  "3");     // ignite duration on a direct hit
    eng->Cvar_Register("fire_flame_tick",  "0.1");   // think interval = fire/drain rate
    eng->Cvar_Register("fire_flame_cost",  "1");     // cells drained per tick
    eng->Cvar_Register("fire_flame_backdraft", "40");  // wall within Nu point-blank ignites you; 0=off
    // Oil gun.
    eng->Cvar_Register("fire_oilgun_tick", "0.12");  // think interval
    eng->Cvar_Register("fire_oilgun_cost", "1");     // cells drained per deposit
    // (Flamethrower ignite DPS reuses the existing fire_dps cvar.)
}

// impulse 212: grant both fire weapons, top up cells, select the oil gun.
void Fire_GiveWeapons(edict_t *self) {
    self->v.items2 = (float)((int)self->v.items2 | IT2_OILGUN | IT2_FLAMETHROWER);
    if (self->v.ammo_cells < 200) self->v.ammo_cells = 200;
    self->v.weapon  = 0;
    self->v.weapon2 = (float)IT2_OILGUN;
    W_SetCurrentAmmo();
    eng->Con_Print("fire: granted oil gun + flamethrower (200 cells)\n");
}

// --- Oil gun implementation ---------------------------------------------------

// Emit a STIM_SOUND so the immersive-sim AI hears the player firing
// (mirrors weapons.c::emit_weapon_sound, which is static to that file).
static void weaponsfire_sound_stim(edict_t *shooter, float intensity) {
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_SOUND;
    s.origin[0]    = shooter->v.origin[0];
    s.origin[1]    = shooter->v.origin[1];
    s.origin[2]    = shooter->v.origin[2];
    s.intensity    = intensity;
    s.source_edict = eng->ED_GetNum(shooter);
    Stim_Emit(&s);
}

// One oil-gun tick. Returns 0 when out of fuel (caller ends the loop).
static int OilGun_DoFire(edict_t *self) {
    int cost = (int)eng->Cvar_VariableValue("fire_oilgun_cost");
    if (self->v.ammo_cells < cost) {
        self->v.weapon2 = 0;                  // out of fuel: drop to a stock weapon
        self->v.weapon  = W_BestWeapon();
        W_SetCurrentAmmo();
        return 0;
    }

    // Deposit oil at the crosshair floor-trace. Fire_PourOil also coats any
    // monster standing in the fresh patch (Fire_AddOil's coat loop). Only spend
    // fuel when oil actually lands (aiming at the sky deposits nothing).
    if (Fire_PourOil(self)) {
        self->v.ammo_cells -= cost;
        self->v.currentammo = self->v.ammo_cells;
    }

    if (self->v.t_width < g->time) {          // throttle the looping spray sound
        eng->SV_StartSound(self, CHAN_WEAPON, "misc/water1.wav", 1, ATTN_NORM);
        self->v.t_width = g->time + 0.3f;
    }
    weaponsfire_sound_stim(self, 0.4f);
    return 1;
}

// Self-perpetuating held-fire loop (modeled on player_light1/2_think). Runs
// while +attack is held and the oil gun stays selected.
static void oilgun_think(edict_t *self) {
    g->self = self;
    if (!self->v.button0 || (int)self->v.weapon2 != IT2_OILGUN) {
        player_run(self);
        return;
    }
    self->v.frame = FR_FIRE_BODY;
    self->v.weaponframe++;
    if (self->v.weaponframe > 4) self->v.weaponframe = 1;
    if (!OilGun_DoFire(self)) {               // out of fuel -> stop
        player_run(self);
        return;
    }
    self->v.nextthink       = g->time + eng->Cvar_VariableValue("fire_oilgun_tick");
    self->v.think           = oilgun_think;
    self->v.attack_finished = g->time + 0.2f; // keep W_WeaponFrame from re-entering W_Attack
}

void W_FireOilGun(void) {
    edict_t *self = g->self;
    if (self->v.ammo_cells < 1) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
        self->v.attack_finished = g->time + 0.5f;
        return;
    }
    self->v.attack_finished = g->time + 0.1f; // hand off to the think loop
    oilgun_think(self);
}

// Small vector length helper (gust_fire uses its own vlen; keep this local).
static float wf_vlen(const vec3_t v) {
    return (float)sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

// One flamethrower tick: ignite damageable edicts in a forward cone (with LOS)
// and light oil along the cone axis. Returns 0 when out of fuel.
static int Flamethrower_DoFire(edict_t *self) {
    int cost = (int)eng->Cvar_VariableValue("fire_flame_cost");
    if (self->v.ammo_cells < cost) {
        self->v.weapon2 = 0;
        self->v.weapon  = W_BestWeapon();
        W_SetCurrentAmmo();
        return 0;
    }
    self->v.ammo_cells -= cost;
    self->v.currentammo = self->v.ammo_cells;

    if (self->v.t_width < g->time) {          // throttled flame loop sound
        eng->SV_StartSound(self, CHAN_WEAPON, "ambience/fire1.wav", 1, ATTN_NORM);
        self->v.t_width = g->time + 0.5f;
    }
    weaponsfire_sound_stim(self, 0.7f);

    float range    = eng->Cvar_VariableValue("fire_flame_range");
    float cone_cos = (float)cos(eng->Cvar_VariableValue("fire_flame_cone") * 3.14159265f / 180.0f);
    float secs     = eng->Cvar_VariableValue("fire_flame_secs");
    float dps      = eng->Cvar_VariableValue("fire_dps");

    eng->MakeVectors(self->v.v_angle);
    vec3_t eye = { self->v.origin[0],
                   self->v.origin[1],
                   self->v.origin[2] + self->v.view_ofs[2] };
    vec3_t fwd = { g->v_forward[0], g->v_forward[1], g->v_forward[2] };

    // Ignite damageable edicts inside the cone (LOS-gated); relight extinguished
    // torches in the same pass. Mirrors gust_fire but drops the takedamage filter
    // so non-damageable torch edicts reach Torch_Relight (no-ops on non-torches).
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == self) continue;

        vec3_t to = { e->v.origin[0] - eye[0],
                      e->v.origin[1] - eye[1],
                      e->v.origin[2] - eye[2] };
        float d = wf_vlen(to);
        if (d > range || d < 1.0f) continue;

        vec3_t dirn = { to[0]/d, to[1]/d, to[2]/d };
        if (dirn[0]*fwd[0] + dirn[1]*fwd[1] + dirn[2]*fwd[2] < cone_cos) continue;

        eng->SV_Traceline(eye, e->v.origin, 1, self);
        if (g->trace_fraction != 1.0f && g->trace_ent != e) continue;

        if (e->v.takedamage)
            Fire_IgniteMaybeCoated(e, secs, dps, self);   // oil-coated targets burn longer
        else
            Torch_Relight(e, self);                        // no-op unless an extinguished torch
    }

    // Light oil patches the cone sweeps over (sample down the axis).
    for (int i = 1; i <= 4; i++) {
        float t = (float)i / 4.0f * range;
        vec3_t p = { eye[0] + fwd[0]*t, eye[1] + fwd[1]*t, eye[2] + fwd[2]*t };
        Fire_LightOilNear(p, 32.0f);
    }

    // Visible flame stream: fire-blobs along the near cone axis each tick.
    {
        vec3_t up = { 0.0f, 0.0f, 12.0f };
        float reach = range * 0.6f;          // flame visibly reaches ~60% of cone length
        for (int i = 1; i <= 3; i++) {
            float t = (float)i / 3.0f * reach;
            vec3_t fp = { eye[0] + fwd[0]*t,
                          eye[1] + fwd[1]*t,
                          eye[2] + fwd[2]*t };
            eng->SV_Fire(fp, up, 3.0f);
        }
    }

    // Backdraft: spraying point-blank into a wall ignites the player.
    {
        float bd = eng->Cvar_VariableValue("fire_flame_backdraft");
        if (bd > 0.0f) {
            vec3_t wall = { eye[0] + fwd[0]*bd, eye[1] + fwd[1]*bd, eye[2] + fwd[2]*bd };
            eng->SV_Traceline(eye, wall, 1, self);
            if (g->trace_fraction < 1.0f)              // solid within bd units
                Fire_IgniteMaybeCoated(self, secs, dps, self);
        }
    }
    return 1;
}

static void flamethrower_think(edict_t *self) {
    g->self = self;
    if (!self->v.button0 || (int)self->v.weapon2 != IT2_FLAMETHROWER) {
        player_run(self);
        return;
    }
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);   // muzzle glow
    self->v.frame = FR_FIRE_BODY;
    self->v.weaponframe++;
    if (self->v.weaponframe > 4) self->v.weaponframe = 1;
    if (!Flamethrower_DoFire(self)) {
        player_run(self);
        return;
    }
    self->v.nextthink       = g->time + eng->Cvar_VariableValue("fire_flame_tick");
    self->v.think           = flamethrower_think;
    self->v.attack_finished = g->time + 0.2f;
}

void W_FireFlamethrower(void) {
    edict_t *self = g->self;
    if (self->v.ammo_cells < 1) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
        self->v.attack_finished = g->time + 0.5f;
        return;
    }
    eng->SV_StartSound(self, CHAN_AUTO, "weapons/lstart.wav", 1, ATTN_NORM);   // spin-up, once
    self->v.attack_finished = g->time + 0.1f;
    flamethrower_think(self);
}

// ---------------------------------------------------------------------------
// weapon2 selector — dispatch, ammo/model setup, weapon switching, give-all.
// (Folded in from the retired Phase 6 module; Doom weapons removed, only the
// fire weapons remain.)
// ---------------------------------------------------------------------------

// Top-level fire dispatch — called from weapons.c W_Attack when weapon2 != 0.
void W_Attack_Weapon2(void) {
    int it2 = (int)g->self->v.weapon2;
    switch (it2) {
        case IT2_OILGUN:       W_FireOilGun();       break;
        case IT2_FLAMETHROWER: W_FireFlamethrower(); break;
        default: /* unknown — silently noop */       break;
    }
}

// Sets weaponmodel + currentammo for the active weapon2 selection.
void W_SetCurrentAmmo_Weapon2(int it2) {
    edict_t *self = g->self;
    self->v.weaponframe = 0;
    switch (it2) {
        case IT2_OILGUN:
            self->v.weaponmodel = "progs/v_rock.mdl";
            self->v.currentammo = self->v.ammo_cells;
            break;
        case IT2_FLAMETHROWER:
            self->v.weaponmodel = "progs/v_light.mdl";
            self->v.currentammo = self->v.ammo_cells;
            break;
        default:
            self->v.weaponmodel = "";
            self->v.currentammo = 0;
            break;
    }
}

// Impulse 40/41: switch to a fire weapon (if owned).
void Weapon2_ChangeWeapon(int impulse) {
    edict_t *self = g->self;
    int flag = 0;
    switch (impulse) {
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
    W_SetCurrentAmmo_Weapon2(flag);
}

// Impulse-100 cheat path: grant both fire weapons + cells.
void Weapon2_GiveAll(void) {
    edict_t *self = g->self;
    self->v.items2 = (float)(IT2_OILGUN | IT2_FLAMETHROWER);
    if (self->v.ammo_cells < 200) self->v.ammo_cells = 200;
    eng->Con_Print("weapon2: fire weapons granted\n");
}

// ---------------------------------------------------------------------------
// M8 / F3: World pickup spawn functions.
// ---------------------------------------------------------------------------
extern void StartItem(edict_t *e);
extern void weapon_touch_fire(edict_t *self, edict_t *other);

// Reuse existing world models (no new precache asset): grenade-launcher box for
// the oil gun, lightning box for the flamethrower.
void spawn_weapon_oilgun(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_rock.mdl");
    eng->SV_SetModel(e, "progs/g_rock.mdl");
    e->v.weapon  = 0;
    e->v.netname = "Oil Gun";
    e->v.touch   = weapon_touch_fire;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_flamethrower(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_light.mdl");
    eng->SV_SetModel(e, "progs/g_light.mdl");
    e->v.weapon  = 0;
    e->v.netname = "Flamethrower";
    e->v.touch   = weapon_touch_fire;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

// weapons.c -- Weapon firing, projectile logic, impulse commands (weapons.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "weapons_phase6.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// External dependencies
// ---------------------------------------------------------------------------
// combat.c
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *bomb, edict_t *attacker, float rad, edict_t *ignore);
// subs.c
extern void SUB_Remove(edict_t *self);
// player.c
extern void player_run(edict_t *self);
extern void player_axe1(edict_t *self);
extern void player_axeb1(edict_t *self);
extern void player_axec1(edict_t *self);
extern void player_axed1(edict_t *self);
extern void player_shot1(edict_t *self);
extern void player_nail1(edict_t *self);
extern void player_light1(edict_t *self);
extern void player_rocket1(edict_t *self);

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void  SpawnBlood(vec3_t org, vec3_t vel, float damage); // defined later
float W_BestWeapon(void);   // defined later
void  W_SetCurrentAmmo(void); // defined later

static void s_explode2(edict_t *self);
static void s_explode3(edict_t *self);
static void s_explode4(edict_t *self);
static void s_explode5(edict_t *self);
static void s_explode6(edict_t *self);
void BecomeExplosion(void);
static void GrenadeExplode(edict_t *self);
static void spike_touch(edict_t *self, edict_t *other);
void superspike_touch(edict_t *self, edict_t *other);
static void wall_velocity(vec3_t out);
static float crandom(void);
static void TraceAttack(float damage, vec3_t dir);
static void ClearMultiDamage(void);
static void ApplyMultiDamage(void);
static void AddMultiDamage(edict_t *hit, float damage);
static void W_FireShotgun(void);
static void W_FireSuperShotgun(void);
static void W_FireGrenade(void);
static void W_FireRocket(void);
static int  W_CheckNoAmmo(void);
static void W_ChangeWeapon(void);
static void CheatCommand(void);
static void CycleWeaponCommand(void);
static void ServerflagsCommand(void);
static void QuadCheat(void);
static void ImpulseCommands(void);
static void W_Attack(void);

// ---------------------------------------------------------------------------
// W_Precache -- called from worldspawn (world.c weak stub → replaced here)
// ---------------------------------------------------------------------------
void W_Precache(void) {
    eng->PrecacheSound("weapons/r_exp3.wav");
    eng->PrecacheSound("weapons/rocket1i.wav");
    eng->PrecacheSound("weapons/sgun1.wav");
    eng->PrecacheSound("weapons/guncock.wav");
    eng->PrecacheSound("weapons/ric1.wav");
    eng->PrecacheSound("weapons/ric2.wav");
    eng->PrecacheSound("weapons/ric3.wav");
    eng->PrecacheSound("weapons/spike2.wav");
    eng->PrecacheSound("weapons/tink1.wav");
    eng->PrecacheSound("weapons/grenade.wav");
    eng->PrecacheSound("weapons/bounce.wav");
    eng->PrecacheSound("weapons/shotgn2.wav");

    Phase6_PrecacheCommon();
}

static float crandom(void) {
    return 2.0f * (eng->Random() - 0.5f);
}

// ---------------------------------------------------------------------------
// W_FireAxe
// ---------------------------------------------------------------------------
void W_FireAxe(void) {
    edict_t *self = g->self;
    vec3_t source, org;
    source[0] = self->v.origin[0];
    source[1] = self->v.origin[1];
    source[2] = self->v.origin[2] + 16;

    vec3_t end;
    end[0] = source[0] + g->v_forward[0]*64;
    end[1] = source[1] + g->v_forward[1]*64;
    end[2] = source[2] + g->v_forward[2]*64;
    eng->SV_Traceline(source, end, 0, self);

    if (g->trace_fraction == 1.0f) return;

    org[0] = g->trace_endpos[0] - g->v_forward[0]*4;
    org[1] = g->trace_endpos[1] - g->v_forward[1]*4;
    org[2] = g->trace_endpos[2] - g->v_forward[2]*4;

    if (g->trace_ent->v.takedamage) {
        g->trace_ent->v.axhitme = 1;
        vec3_t zero = {0,0,0};
        SpawnBlood(org, zero, 20);
        T_Damage(g->trace_ent, self, self, 20);
    } else {
        eng->SV_StartSound(self, CHAN_WEAPON, "player/axhit2.wav", 1, ATTN_NORM);
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_GUNSHOT);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    }
}

// ---------------------------------------------------------------------------
// Utility: wall_velocity -- compute a bounce velocity off the hit surface
// ---------------------------------------------------------------------------
static void wall_velocity(vec3_t out) {
    edict_t *self = g->self;
    vec3_t tmp;
    eng->VectorNormalize(self->v.velocity, out);
    float r1 = eng->Random() - 0.5f;
    float r2 = eng->Random() - 0.5f;
    tmp[0] = out[0] + g->v_up[0]*r1 + g->v_right[0]*r2;
    tmp[1] = out[1] + g->v_up[1]*r1 + g->v_right[1]*r2;
    tmp[2] = out[2] + g->v_up[2]*r1 + g->v_right[2]*r2;
    eng->VectorNormalize(tmp, out);
    out[0] = (out[0] + 2*g->trace_plane_normal[0]) * 200;
    out[1] = (out[1] + 2*g->trace_plane_normal[1]) * 200;
    out[2] = (out[2] + 2*g->trace_plane_normal[2]) * 200;
}

// ---------------------------------------------------------------------------
// SpawnMeatSpray -- toss a meat chunk (for monster deaths)
// ---------------------------------------------------------------------------
void SpawnMeatSpray(vec3_t org, vec3_t vel) {
    edict_t *self = g->self;
    edict_t *missile = eng->ED_Alloc();
    missile->v.owner    = self;
    missile->v.movetype = MOVETYPE_BOUNCE;
    missile->v.solid    = SOLID_NOT;

    eng->MakeVectors(self->v.angles);

    missile->v.velocity[0] = vel[0];
    missile->v.velocity[1] = vel[1];
    missile->v.velocity[2] = vel[2] + 250 + 50*eng->Random();

    missile->v.avelocity[0] = 3000; missile->v.avelocity[1] = 1000; missile->v.avelocity[2] = 2000;

    missile->v.nextthink = g->time + 1;
    missile->v.think     = SUB_Remove;
    eng->SV_SetModel(missile, "progs/zom_gib.mdl");
    vec3_t zmin = {0,0,0}, zmax = {0,0,0};
    eng->SV_SetSize(missile, zmin, zmax);
    eng->SV_SetOrigin(missile, org);
}

// ---------------------------------------------------------------------------
// SpawnBlood / spawn_touchblood / SpawnChunk
// ---------------------------------------------------------------------------
void SpawnBlood(vec3_t org, vec3_t vel, float damage) {
    vec3_t scaled = {vel[0]*0.1f, vel[1]*0.1f, vel[2]*0.1f};
    eng->SV_Particle(org, scaled, 73, damage*2);
}

static void spawn_touchblood(float damage) {
    vec3_t vel;
    wall_velocity(vel);
    vec3_t origin;
    origin[0] = g->self->v.origin[0] + vel[0]*0.01f;
    origin[1] = g->self->v.origin[1] + vel[1]*0.01f;
    origin[2] = g->self->v.origin[2] + vel[2]*0.01f;
    vec3_t sv = {vel[0]*0.2f, vel[1]*0.2f, vel[2]*0.2f};
    SpawnBlood(origin, sv, damage);
}

void SpawnChunk(vec3_t org, vec3_t vel) {
    vec3_t scaled = {vel[0]*0.02f, vel[1]*0.02f, vel[2]*0.02f};
    eng->SV_Particle(org, scaled, 0, 10);
}

// ---------------------------------------------------------------------------
// Multi-damage accumulator (collect pellet hits, apply as one damage call)
// ---------------------------------------------------------------------------
static edict_t *multi_ent    = NULL;
static float    multi_damage = 0;

static void ClearMultiDamage(void) {
    multi_ent    = g->world;
    multi_damage = 0;
}

static void ApplyMultiDamage(void) {
    if (!multi_ent) return;
    T_Damage(multi_ent, g->self, g->self, multi_damage);
}

static void AddMultiDamage(edict_t *hit, float damage) {
    if (!hit) return;
    if (hit != multi_ent) {
        ApplyMultiDamage();
        multi_damage = damage;
        multi_ent    = hit;
    } else {
        multi_damage += damage;
    }
}

// ---------------------------------------------------------------------------
// TraceAttack / FireBullets
// ---------------------------------------------------------------------------
static void TraceAttack(float damage, vec3_t dir) {
    float c1 = crandom(), c2 = crandom();
    vec3_t vel, tmp, org;

    tmp[0] = dir[0] + g->v_up[0]*c1 + g->v_right[0]*c2;
    tmp[1] = dir[1] + g->v_up[1]*c1 + g->v_right[1]*c2;
    tmp[2] = dir[2] + g->v_up[2]*c1 + g->v_right[2]*c2;
    eng->VectorNormalize(tmp, vel);
    vel[0] = (vel[0] + 2*g->trace_plane_normal[0]) * 200;
    vel[1] = (vel[1] + 2*g->trace_plane_normal[1]) * 200;
    vel[2] = (vel[2] + 2*g->trace_plane_normal[2]) * 200;

    org[0] = g->trace_endpos[0] - dir[0]*4;
    org[1] = g->trace_endpos[1] - dir[1]*4;
    org[2] = g->trace_endpos[2] - dir[2]*4;

    if (g->trace_ent->v.takedamage) {
        vec3_t sv = {vel[0]*0.2f, vel[1]*0.2f, vel[2]*0.2f};
        SpawnBlood(org, sv, damage);
        AddMultiDamage(g->trace_ent, damage);
    } else {
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_GUNSHOT);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    }
}

static void FireBullets(float shotcount, vec3_t dir, vec3_t spread) {
    edict_t *self = g->self;
    eng->MakeVectors(self->v.v_angle);

    vec3_t src;
    src[0] = self->v.origin[0] + g->v_forward[0]*10;
    src[1] = self->v.origin[1] + g->v_forward[1]*10;
    src[2] = self->v.absmin[2] + self->v.size[2]*0.7f;

    ClearMultiDamage();
    while (shotcount > 0) {
        float c1 = crandom(), c2 = crandom();
        vec3_t direction;
        direction[0] = dir[0] + c1*spread[0]*g->v_right[0] + c2*spread[1]*g->v_up[0];
        direction[1] = dir[1] + c1*spread[0]*g->v_right[1] + c2*spread[1]*g->v_up[1];
        direction[2] = dir[2] + c1*spread[0]*g->v_right[2] + c2*spread[1]*g->v_up[2];

        vec3_t end;
        end[0] = src[0] + direction[0]*2048;
        end[1] = src[1] + direction[1]*2048;
        end[2] = src[2] + direction[2]*2048;
        eng->SV_Traceline(src, end, 0, self);
        if (g->trace_fraction != 1.0f)
            TraceAttack(4, direction);

        shotcount -= 1;
    }
    ApplyMultiDamage();
}

static void W_FireShotgun(void) {
    edict_t *self = g->self;
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;
    self->v.currentammo = self->v.ammo_shells = self->v.ammo_shells - 1;
    vec3_t dir;
    eng->SV_Aim(self, 100000, dir);
    vec3_t spread = {0.04f, 0.04f, 0};
    FireBullets(6, dir, spread);
}

static void W_FireSuperShotgun(void) {
    edict_t *self = g->self;
    if (self->v.currentammo == 1) { W_FireShotgun(); return; }
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/shotgn2.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -4;
    self->v.currentammo = self->v.ammo_shells = self->v.ammo_shells - 2;
    vec3_t dir;
    eng->SV_Aim(self, 100000, dir);
    vec3_t spread = {0.14f, 0.08f, 0};
    FireBullets(14, dir, spread);
}

// ---------------------------------------------------------------------------
// Rocket explosion animation
// ---------------------------------------------------------------------------
static void s_explode1(edict_t *self) { g->self=self; self->v.frame=0; self->v.nextthink=g->time+0.1f; self->v.think=s_explode2; }
static void s_explode2(edict_t *self) { g->self=self; self->v.frame=1; self->v.nextthink=g->time+0.1f; self->v.think=s_explode3; }
static void s_explode3(edict_t *self) { g->self=self; self->v.frame=2; self->v.nextthink=g->time+0.1f; self->v.think=s_explode4; }
static void s_explode4(edict_t *self) { g->self=self; self->v.frame=3; self->v.nextthink=g->time+0.1f; self->v.think=s_explode5; }
static void s_explode5(edict_t *self) { g->self=self; self->v.frame=4; self->v.nextthink=g->time+0.1f; self->v.think=s_explode6; }
static void s_explode6(edict_t *self) { g->self=self; self->v.frame=5; self->v.nextthink=g->time+0.1f; self->v.think=SUB_Remove; }

static void SUB_NullTouch(edict_t *self, edict_t *other) { (void)self; (void)other; }

void BecomeExplosion(void) {
    edict_t *self     = g->self;
    self->v.movetype  = MOVETYPE_NONE;
    self->v.velocity[0] = self->v.velocity[1] = self->v.velocity[2] = 0;
    self->v.touch     = SUB_NullTouch;
    eng->SV_SetModel(self, "progs/s_explod.spr");
    self->v.solid     = SOLID_NOT;
    s_explode1(self);
}

// ---------------------------------------------------------------------------
// Rocket missile
// ---------------------------------------------------------------------------
static void T_MissileTouch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;

    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) {
        eng->ED_Free(self);
        return;
    }

    float damg = 100 + eng->Random()*20;
    if (other->v.health) {
        if (other->v.classname && strcmp(other->v.classname, "monster_shambler") == 0)
            damg *= 0.5f;
        T_Damage(other, self, self->v.owner, damg);
    }
    T_RadiusDamage(self, self->v.owner, 120, other);

    self->v.origin[0] -= 8 * (self->v.velocity[0] / (eng->VectorLength(self->v.velocity) + 0.0001f));
    self->v.origin[1] -= 8 * (self->v.velocity[1] / (eng->VectorLength(self->v.velocity) + 0.0001f));
    self->v.origin[2] -= 8 * (self->v.velocity[2] / (eng->VectorLength(self->v.velocity) + 0.0001f));

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);

    BecomeExplosion();
}

static void W_FireRocket(void) {
    edict_t *self = g->self;
    self->v.currentammo = self->v.ammo_rockets = self->v.ammo_rockets - 1;
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/sgun1.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;

    edict_t *missile = eng->ED_Alloc();
    missile->v.owner    = self;
    missile->v.movetype = MOVETYPE_FLYMISSILE;
    missile->v.solid    = SOLID_BBOX;

    eng->MakeVectors(self->v.v_angle);
    eng->SV_Aim(self, 1000, missile->v.velocity);
    missile->v.velocity[0] *= 1000; missile->v.velocity[1] *= 1000; missile->v.velocity[2] *= 1000;
    eng->VectorToAngles(missile->v.velocity, missile->v.angles);

    missile->v.touch     = T_MissileTouch;
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
// Lightning
// ---------------------------------------------------------------------------
static void LightningDamage(vec3_t p1, vec3_t p2, edict_t *from, float damage) {
    edict_t *self = g->self;
    vec3_t f, foff;
    f[0] = p2[0]-p1[0]; f[1] = p2[1]-p1[1]; f[2] = p2[2]-p1[2];
    eng->VectorNormalize(f, f);
    // rotate 90° in XY to get a perpendicular spread offset
    float old_fy = f[1];
    f[0] = -old_fy;
    f[1] = f[0]; // = -old_fy
    f[2] = 0;
    foff[0] = f[0]*16; foff[1] = f[1]*16; foff[2] = 0;

    edict_t *e1 = g->world, *e2 = g->world;
    vec3_t par = {0,0,100};

    eng->SV_Traceline(p1, p2, 0, self);
    if (g->trace_ent->v.takedamage) {
        eng->SV_Particle(g->trace_endpos, par, 225, damage*4);
        T_Damage(g->trace_ent, from, from, damage);
        if (self->v.classname && strcmp(self->v.classname, "player") == 0)
            if (g->other->v.classname && strcmp(g->other->v.classname, "player") == 0)
                g->trace_ent->v.velocity[2] += 400;
    }
    e1 = g->trace_ent;

    vec3_t p1f = {p1[0]+foff[0], p1[1]+foff[1], p1[2]};
    vec3_t p2f = {p2[0]+foff[0], p2[1]+foff[1], p2[2]};
    eng->SV_Traceline(p1f, p2f, 0, self);
    if (g->trace_ent != e1 && g->trace_ent->v.takedamage) {
        eng->SV_Particle(g->trace_endpos, par, 225, damage*4);
        T_Damage(g->trace_ent, from, from, damage);
    }
    e2 = g->trace_ent;

    vec3_t p1b = {p1[0]-foff[0], p1[1]-foff[1], p1[2]};
    vec3_t p2b = {p2[0]-foff[0], p2[1]-foff[1], p2[2]};
    eng->SV_Traceline(p1b, p2b, 0, self);
    if (g->trace_ent != e1 && g->trace_ent != e2 && g->trace_ent->v.takedamage) {
        eng->SV_Particle(g->trace_endpos, par, 225, damage*4);
        T_Damage(g->trace_ent, from, from, damage);
    }
}

void W_FireLightning(void) {
    edict_t *self = g->self;
    if (self->v.ammo_cells < 1) {
        self->v.weapon = W_BestWeapon();
        W_SetCurrentAmmo();
        return;
    }
    if (self->v.waterlevel > 1) {
        T_RadiusDamage(self, self, 35*self->v.ammo_cells, g->world);
        self->v.ammo_cells = 0;
        W_SetCurrentAmmo();
        return;
    }
    if (self->v.t_width < g->time) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/lhit.wav", 1, ATTN_NORM);
        self->v.t_width = g->time + 0.6f;
    }
    self->v.punchangle[0] = -2;
    self->v.currentammo = self->v.ammo_cells = self->v.ammo_cells - 1;

    vec3_t org = {self->v.origin[0], self->v.origin[1], self->v.origin[2]+16};
    vec3_t end = {org[0]+g->v_forward[0]*600, org[1]+g->v_forward[1]*600, org[2]+g->v_forward[2]*600};
    eng->SV_Traceline(org, end, 1, self);  // TRUE = nomonsters

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_LIGHTNING2);
    eng->MSG_WriteEntity(MSG_BROADCAST, self);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[2]);

    vec3_t lend = {g->trace_endpos[0]+g->v_forward[0]*4,
                   g->trace_endpos[1]+g->v_forward[1]*4,
                   g->trace_endpos[2]+g->v_forward[2]*4};
    LightningDamage(self->v.origin, lend, self, 30);
}

// ---------------------------------------------------------------------------
// Grenade launcher
// ---------------------------------------------------------------------------
static void GrenadeExplode(edict_t *self) {
    g->self = self;
    T_RadiusDamage(self, self->v.owner, 120, g->world);
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    BecomeExplosion();
}

static void GrenadeTouch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    if (other->v.takedamage == DAMAGE_AIM) { GrenadeExplode(self); return; }
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/bounce.wav", 1, ATTN_NORM);
    if (self->v.velocity[0] == 0 && self->v.velocity[1] == 0 && self->v.velocity[2] == 0)
        self->v.avelocity[0] = self->v.avelocity[1] = self->v.avelocity[2] = 0;
}

static void W_FireGrenade(void) {
    edict_t *self = g->self;
    self->v.currentammo = self->v.ammo_rockets = self->v.ammo_rockets - 1;
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/grenade.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;

    edict_t *missile    = eng->ED_Alloc();
    missile->v.owner    = self;
    missile->v.movetype = MOVETYPE_BOUNCE;
    missile->v.solid    = SOLID_BBOX;
    missile->v.classname = "grenade";

    eng->MakeVectors(self->v.v_angle);

    if (self->v.v_angle[0]) {
        float c3 = crandom(), c4 = crandom();
        missile->v.velocity[0] = g->v_forward[0]*600 + g->v_up[0]*200 + c3*g->v_right[0]*10 + c4*g->v_up[0]*10;
        missile->v.velocity[1] = g->v_forward[1]*600 + g->v_up[1]*200 + c3*g->v_right[1]*10 + c4*g->v_up[1]*10;
        missile->v.velocity[2] = g->v_forward[2]*600 + g->v_up[2]*200 + c3*g->v_right[2]*10 + c4*g->v_up[2]*10;
    } else {
        eng->SV_Aim(self, 10000, missile->v.velocity);
        missile->v.velocity[0] *= 600; missile->v.velocity[1] *= 600; missile->v.velocity[2] *= 600;
        missile->v.velocity[2] = 200;
    }

    missile->v.avelocity[0] = missile->v.avelocity[1] = missile->v.avelocity[2] = 300;
    eng->VectorToAngles(missile->v.velocity, missile->v.angles);

    missile->v.touch     = GrenadeTouch;
    missile->v.nextthink = g->time + 2.5f;
    missile->v.think     = GrenadeExplode;

    eng->SV_SetModel(missile, "progs/grenade.mdl");
    vec3_t gzero = {0,0,0};
    eng->SV_SetSize(missile, gzero, gzero);
    eng->SV_SetOrigin(missile, self->v.origin);
}

// ---------------------------------------------------------------------------
// Spike projectiles
// ---------------------------------------------------------------------------
static void spike_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    if (other->v.solid == SOLID_TRIGGER) return;
    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) { eng->ED_Free(self); return; }

    if (other->v.takedamage) {
        spawn_touchblood(9);
        T_Damage(other, self, self->v.owner, 9);
    } else {
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        const char *cn = self->v.classname;
        if (cn && strcmp(cn, "wizspike") == 0)
            eng->MSG_WriteByte(MSG_BROADCAST, TE_WIZSPIKE);
        else if (cn && strcmp(cn, "knightspike") == 0)
            eng->MSG_WriteByte(MSG_BROADCAST, TE_KNIGHTSPIKE);
        else
            eng->MSG_WriteByte(MSG_BROADCAST, TE_SPIKE);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    }
    eng->ED_Free(self);
}

void superspike_touch(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    if (other->v.solid == SOLID_TRIGGER) return;
    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) { eng->ED_Free(self); return; }

    if (other->v.takedamage) {
        spawn_touchblood(18);
        T_Damage(other, self, self->v.owner, 18);
    } else {
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_SUPERSPIKE);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    }
    eng->ED_Free(self);
}

void launch_spike(vec3_t org, vec3_t dir) {
    edict_t *self = g->self;
    edict_t *newmis = eng->ED_Alloc();
    g->newmis = newmis;
    newmis->v.owner    = self;
    newmis->v.movetype = MOVETYPE_FLYMISSILE;
    newmis->v.solid    = SOLID_BBOX;
    eng->VectorToAngles(dir, newmis->v.angles);
    newmis->v.touch     = spike_touch;
    newmis->v.classname = "spike";
    newmis->v.think     = SUB_Remove;
    newmis->v.nextthink = g->time + 6;
    eng->SV_SetModel(newmis, "progs/spike.mdl");
    vec3_t szero = {0,0,0};
    eng->SV_SetSize(newmis, szero, szero);
    eng->SV_SetOrigin(newmis, org);
    newmis->v.velocity[0] = dir[0]*1000;
    newmis->v.velocity[1] = dir[1]*1000;
    newmis->v.velocity[2] = dir[2]*1000;
}

static void W_FireSuperSpikes(void) {
    edict_t *self = g->self;
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/spike2.wav", 1, ATTN_NORM);
    self->v.attack_finished = g->time + 0.2f;
    self->v.currentammo = self->v.ammo_nails = self->v.ammo_nails - 2;
    vec3_t dir;
    eng->SV_Aim(self, 1000, dir);
    vec3_t sorg = {self->v.origin[0], self->v.origin[1], self->v.origin[2]+16};
    launch_spike(sorg, dir);
    g->newmis->v.touch = superspike_touch;
    eng->SV_SetModel(g->newmis, "progs/s_spike.mdl");
    vec3_t szero = {0,0,0};
    eng->SV_SetSize(g->newmis, szero, szero);
    self->v.punchangle[0] = -2;
}

void W_FireSpikes(float ox) {
    edict_t *self = g->self;
    eng->MakeVectors(self->v.v_angle);
    if (self->v.ammo_nails >= 2 && self->v.weapon == IT_SUPER_NAILGUN) {
        W_FireSuperSpikes();
        return;
    }
    if (self->v.ammo_nails < 1) {
        self->v.weapon = W_BestWeapon();
        W_SetCurrentAmmo();
        return;
    }
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/rocket1i.wav", 1, ATTN_NORM);
    self->v.attack_finished = g->time + 0.2f;
    self->v.currentammo = self->v.ammo_nails = self->v.ammo_nails - 1;
    vec3_t dir;
    eng->SV_Aim(self, 1000, dir);
    vec3_t sorg = {
        self->v.origin[0] + g->v_right[0]*ox,
        self->v.origin[1] + g->v_right[1]*ox,
        self->v.origin[2] + 16
    };
    launch_spike(sorg, dir);
    self->v.punchangle[0] = -2;
}

// ---------------------------------------------------------------------------
// W_SetCurrentAmmo -- sets weapon model, ammo type, ammo count
// ---------------------------------------------------------------------------
void W_SetCurrentAmmo(void) {
    edict_t *self = g->self;
    player_run(self);

    // Phase 6 takes precedence: when weapon2 is set, set the sprite viewmodel
    // and skip the rest of Quake's IT_* / ammo-icon logic.
    if (self->v.weapon2 != 0) {
        W_SetCurrentAmmo_Phase6((int)self->v.weapon2);
        return;
    }

    int items = (int)self->v.items;
    items &= ~(IT_SHELLS | IT_NAILS | IT_ROCKETS | IT_CELLS);
    self->v.items = (float)items;

    int w = (int)self->v.weapon;
    if (w == IT_AXE) {
        self->v.currentammo  = 0;
        self->v.weaponmodel  = "progs/v_axe.mdl";
        self->v.weaponframe  = 0;
    } else if (w == IT_SHOTGUN) {
        self->v.currentammo  = self->v.ammo_shells;
        self->v.weaponmodel  = "progs/v_shot.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_SHELLS);
    } else if (w == IT_SUPER_SHOTGUN) {
        self->v.currentammo  = self->v.ammo_shells;
        self->v.weaponmodel  = "progs/v_shot2.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_SHELLS);
    } else if (w == IT_NAILGUN) {
        self->v.currentammo  = self->v.ammo_nails;
        self->v.weaponmodel  = "progs/v_nail.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_NAILS);
    } else if (w == IT_SUPER_NAILGUN) {
        self->v.currentammo  = self->v.ammo_nails;
        self->v.weaponmodel  = "progs/v_nail2.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_NAILS);
    } else if (w == IT_GRENADE_LAUNCHER) {
        self->v.currentammo  = self->v.ammo_rockets;
        self->v.weaponmodel  = "progs/v_rock.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_ROCKETS);
    } else if (w == IT_ROCKET_LAUNCHER) {
        self->v.currentammo  = self->v.ammo_rockets;
        self->v.weaponmodel  = "progs/v_rock2.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_ROCKETS);
    } else if (w == IT_LIGHTNING) {
        self->v.currentammo  = self->v.ammo_cells;
        self->v.weaponmodel  = "progs/v_light.mdl";
        self->v.weaponframe  = 0;
        self->v.items = (float)((int)self->v.items | IT_CELLS);
    } else {
        self->v.currentammo  = 0;
        self->v.weaponmodel  = "";
        self->v.weaponframe  = 0;
    }
}

// ---------------------------------------------------------------------------
// W_BestWeapon -- returns the best weapon the player can currently fire
// ---------------------------------------------------------------------------
float W_BestWeapon(void) {
    edict_t *self = g->self;
    int it = (int)self->v.items;
    if (self->v.ammo_cells  >= 1 && (it & IT_LIGHTNING))       return IT_LIGHTNING;
    if (self->v.ammo_nails  >= 2 && (it & IT_SUPER_NAILGUN))   return IT_SUPER_NAILGUN;
    if (self->v.ammo_shells >= 2 && (it & IT_SUPER_SHOTGUN))    return IT_SUPER_SHOTGUN;
    if (self->v.ammo_nails  >= 1 && (it & IT_NAILGUN))         return IT_NAILGUN;
    if (self->v.ammo_shells >= 1 && (it & IT_SHOTGUN))         return IT_SHOTGUN;
    return IT_AXE;
}

static int W_CheckNoAmmo(void) {
    edict_t *self = g->self;
    if (self->v.currentammo > 0) return 1;
    if (self->v.weapon == IT_AXE) return 1;
    self->v.weapon = W_BestWeapon();
    W_SetCurrentAmmo();
    return 0;
}

// ---------------------------------------------------------------------------
// W_Attack -- fire the current weapon
// ---------------------------------------------------------------------------
static void W_Attack(void) {
    edict_t *self = g->self;

    // Phase 6: route through the parallel dispatch when a Phase 6 weapon is
    // active. The Phase 6 fire functions handle their own ammo + sound + anim.
    if (self->v.weapon2 != 0) {
        eng->MakeVectors(self->v.v_angle);
        self->v.show_hostile = g->time + 1;
        W_Attack_Phase6();
        return;
    }

    if (!W_CheckNoAmmo()) return;

    eng->MakeVectors(self->v.v_angle);
    self->v.show_hostile = g->time + 1;

    int w = (int)self->v.weapon;
    if (w == IT_AXE) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/ax1.wav", 1, ATTN_NORM);
        float r = eng->Random();
        if      (r < 0.25f) player_axe1(self);
        else if (r < 0.5f)  player_axeb1(self);
        else if (r < 0.75f) player_axec1(self);
        else                player_axed1(self);
        self->v.attack_finished = g->time + 0.5f;
    } else if (w == IT_SHOTGUN) {
        player_shot1(self);
        W_FireShotgun();
        self->v.attack_finished = g->time + 0.5f;
    } else if (w == IT_SUPER_SHOTGUN) {
        player_shot1(self);
        W_FireSuperShotgun();
        self->v.attack_finished = g->time + 0.7f;
    } else if (w == IT_NAILGUN) {
        player_nail1(self);
    } else if (w == IT_SUPER_NAILGUN) {
        player_nail1(self);
    } else if (w == IT_GRENADE_LAUNCHER) {
        player_rocket1(self);
        W_FireGrenade();
        self->v.attack_finished = g->time + 0.6f;
    } else if (w == IT_ROCKET_LAUNCHER) {
        player_rocket1(self);
        W_FireRocket();
        self->v.attack_finished = g->time + 0.8f;
    } else if (w == IT_LIGHTNING) {
        player_light1(self);
        self->v.attack_finished = g->time + 0.1f;
        eng->SV_StartSound(self, CHAN_AUTO, "weapons/lstart.wav", 1, ATTN_NORM);
    }
}

// ---------------------------------------------------------------------------
// W_ChangeWeapon / CycleWeaponCommand / CheatCommand
// ---------------------------------------------------------------------------
static void W_ChangeWeapon(void) {
    edict_t *self = g->self;
    int it = (int)self->v.items;
    int am = 0;
    float fl = 0;
    int imp = (int)self->v.impulse;

    if      (imp == 1) { fl = IT_AXE; }
    else if (imp == 2) { fl = IT_SHOTGUN;          if (self->v.ammo_shells < 1) am = 1; }
    else if (imp == 3) { fl = IT_SUPER_SHOTGUN;    if (self->v.ammo_shells < 2) am = 1; }
    else if (imp == 4) { fl = IT_NAILGUN;          if (self->v.ammo_nails  < 1) am = 1; }
    else if (imp == 5) { fl = IT_SUPER_NAILGUN;    if (self->v.ammo_nails  < 2) am = 1; }
    else if (imp == 6) { fl = IT_GRENADE_LAUNCHER; if (self->v.ammo_rockets< 1) am = 1; }
    else if (imp == 7) { fl = IT_ROCKET_LAUNCHER;  if (self->v.ammo_rockets< 1) am = 1; }
    else if (imp == 8) { fl = IT_LIGHTNING;        if (self->v.ammo_cells  < 1) am = 1; }

    self->v.impulse = 0;
    if (!(it & (int)fl)) { eng->SV_SPrint(self, 0, "no weapon.\n"); return; }
    if (am)               { eng->SV_SPrint(self, 0, "not enough ammo.\n"); return; }

    self->v.weapon  = fl;
    self->v.weapon2 = 0;   // back to Quake roster
    W_SetCurrentAmmo();
}

static void CheatCommand(void) {
    edict_t *self = g->self;
    if (g->deathmatch || g->coop) return;
    self->v.ammo_rockets = 100;
    self->v.ammo_nails   = 200;
    self->v.ammo_shells  = 100;
    self->v.items = (float)((int)self->v.items |
        IT_AXE | IT_SHOTGUN | IT_SUPER_SHOTGUN | IT_NAILGUN | IT_SUPER_NAILGUN |
        IT_GRENADE_LAUNCHER | IT_ROCKET_LAUNCHER | IT_KEY1 | IT_KEY2);
    self->v.ammo_cells = 200;
    self->v.items = (float)((int)self->v.items | IT_LIGHTNING);
    self->v.weapon = IT_ROCKET_LAUNCHER;
    self->v.impulse = 0;
    W_SetCurrentAmmo();
}

static void CycleWeaponCommand(void) {
    edict_t *self = g->self;
    int it = (int)self->v.items;
    self->v.impulse = 0;
    while (1) {
        int am = 0;
        int w  = (int)self->v.weapon;
        if      (w == IT_LIGHTNING)        { self->v.weapon = IT_AXE; }
        else if (w == IT_AXE)              { self->v.weapon = IT_SHOTGUN;          if (self->v.ammo_shells < 1) am=1; }
        else if (w == IT_SHOTGUN)          { self->v.weapon = IT_SUPER_SHOTGUN;    if (self->v.ammo_shells < 2) am=1; }
        else if (w == IT_SUPER_SHOTGUN)    { self->v.weapon = IT_NAILGUN;          if (self->v.ammo_nails  < 1) am=1; }
        else if (w == IT_NAILGUN)          { self->v.weapon = IT_SUPER_NAILGUN;    if (self->v.ammo_nails  < 2) am=1; }
        else if (w == IT_SUPER_NAILGUN)    { self->v.weapon = IT_GRENADE_LAUNCHER; if (self->v.ammo_rockets< 1) am=1; }
        else if (w == IT_GRENADE_LAUNCHER) { self->v.weapon = IT_ROCKET_LAUNCHER;  if (self->v.ammo_rockets< 1) am=1; }
        else if (w == IT_ROCKET_LAUNCHER)  { self->v.weapon = IT_LIGHTNING;        if (self->v.ammo_cells  < 1) am=1; }
        if ((it & (int)self->v.weapon) && am == 0) { W_SetCurrentAmmo(); return; }
    }
}

static void ServerflagsCommand(void) {
    g->serverflags = g->serverflags * 2 + 1;
}

static void QuadCheat(void) {
    edict_t *self = g->self;
    if (g->deathmatch || g->coop) return;
    self->v.super_time            = 1;
    self->v.super_damage_finished = g->time + 30;
    self->v.items = (float)((int)self->v.items | IT_QUAD);
    eng->Con_DPrintf("quad cheat\n");
}

static void ImpulseCommands(void) {
    edict_t *self = g->self;
    int imp = (int)self->v.impulse;
    if (imp >= 1 && imp <= 8) W_ChangeWeapon();
    if (imp == 9)   CheatCommand();
    if (imp == 10)  CycleWeaponCommand();
    if (imp == 11)  ServerflagsCommand();
    if (imp >= 12 && imp <= 21) Phase6_ChangeWeapon(imp);   // Wolf3D + Doom1 roster
    if (imp == 100) Phase6_CheatGiveAll();
    if (imp == 255) QuadCheat();
    self->v.impulse = 0;
}

// ---------------------------------------------------------------------------
// SuperDamageSound -- plays quad damage sound periodically
// ---------------------------------------------------------------------------
void SuperDamageSound(void) {
    edict_t *self = g->self;
    if (self->v.super_damage_finished > g->time) {
        if (self->v.super_sound < g->time) {
            self->v.super_sound = g->time + 1;
            eng->SV_StartSound(self, CHAN_BODY, "items/damage3.wav", 1, ATTN_NORM);
        }
    }
}

// ---------------------------------------------------------------------------
// W_WeaponFrame -- called every player postthink; handles impulses + attack
// ---------------------------------------------------------------------------
void W_WeaponFrame(void) {
    edict_t *self = g->self;
    if (g->time < self->v.attack_finished) return;
    ImpulseCommands();
    if (self->v.button0) {
        SuperDamageSound();
        W_Attack();
    }
}

// weapons.c -- Weapon firing, projectile logic, impulse commands (weapons.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "weapons_phase6.h"
#include "weapons_fire.h"
#include "flammables.h"
#include "sim/sim.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static void emit_weapon_sound(edict_t *shooter, float intensity) {
    stimulus_t s = {0};
    s.kind          = STIM_SOUND;
    s.origin[0]     = shooter->v.origin[0];
    s.origin[1]     = shooter->v.origin[1];
    s.origin[2]     = shooter->v.origin[2];
    s.intensity     = intensity;
    s.source_edict  = eng->ED_GetNum(shooter);
    Stim_Emit(&s);
}

// ---------------------------------------------------------------------------
// External dependencies
// ---------------------------------------------------------------------------
// combat.c
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *bomb, edict_t *attacker, float rad, edict_t *ignore);
extern void Corpse_BulletTrace(vec3_t start, vec3_t end, edict_t *skip);
extern int  GibCorpse(edict_t *self);
// subs.c
extern void SUB_Remove(edict_t *self);

// Forward decls — used by Spike_GibPathScan below.
void superspike_touch(edict_t *self, edict_t *other);
void SpawnBlood(vec3_t org, vec3_t vel, float damage);
// Splash helpers defined further down — also called externally by client.c.
int  splash_at_water_entry(vec3_t start, vec3_t end, int strength_q4);
void splash_underwater_explosion(vec3_t org, int strength_q4);
int  splash_along_segment(vec3_t a, vec3_t b, int strength_q4);
static int is_liquid(int c);

// items_push.c -- pushable pickups
extern void Items_BulletSweep(vec3_t start, vec3_t end, vec3_t dir, float damage, edict_t *ignore);
extern void Items_RadiusPush(vec3_t origin, float radius, float base_impulse, edict_t *ignore);

// Apply hit-site impulse to a gib along `dir` (must be normalized). Used by
// hitscan and direct-projectile hits — both have the actual shot direction
// in scope, which gives better-feeling knockback than T_Damage's gib branch
// (which infers direction from inflictor midpoint, fine for radius damage).
// Does *not* shorten the gib's lifetime: the persistent-gib system already
// handles long-term cleanup (visibility-aware despawn + LRU cap).
void gib_apply_hit_impulse(edict_t *gib, vec3_t dir, float damage) {
    gib->v.velocity[0] += dir[0] * damage * 40.0f;
    gib->v.velocity[1] += dir[1] * damage * 40.0f;
    gib->v.velocity[2] += dir[2] * damage * 40.0f + 30.0f;
    gib->v.avelocity[0] = (eng->Random()*2.0f - 1.0f) * 400.0f;
    gib->v.avelocity[1] = (eng->Random()*2.0f - 1.0f) * 400.0f;
    gib->v.avelocity[2] = (eng->Random()*2.0f - 1.0f) * 400.0f;
    gib->v.flags = (float)((int)gib->v.flags & ~FL_ONGROUND);
}

static int is_gib(edict_t *e) {
    return e && e->v.classname && strcmp(e->v.classname, "gib") == 0;
}

// Returns true if the target is a player or monster (FL_CLIENT|FL_MONSTER).
// Used to gate blood vs. spark/puff: shooting a secret door (which sets
// takedamage=DAMAGE_YES so the player can trigger it) shouldn't spawn blood.
static int is_flesh(edict_t *e) {
    if (!e) return 0;
    return ((int)e->v.flags & (FL_MONSTER | FL_CLIENT)) != 0;
}

// Slab-method AABB-vs-segment. Returns 1 if the segment from `start` to `end`
// intersects the box [mins,maxs]; `*out_t` (if non-NULL) is the entry parameter
// in [0,1].
static int segment_hits_aabb(vec3_t start, vec3_t end, float *mins, float *maxs, float *out_t) {
    float d[3] = { end[0]-start[0], end[1]-start[1], end[2]-start[2] };
    float tmin = 0.0f, tmax = 1.0f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-6f) {
            if (start[i] < mins[i] || start[i] > maxs[i]) return 0;
            continue;
        }
        float inv = 1.0f / d[i];
        float t1 = (mins[i] - start[i]) * inv;
        float t2 = (maxs[i] - start[i]) * inv;
        if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return 0;
    }
    if (out_t) *out_t = tmin;
    return 1;
}

// Per-frame: for each in-flight spike, sweep its next-frame segment against
// gib AABBs. Gibs are SOLID_TRIGGER so the engine's missile trace ignores them
// and SV_TouchLinks at the spike's wall-impact position doesn't overlap a gib
// the spike passed *through* — so spike_touch never fires for gibs. We hijack
// here, before physics runs, to apply impulse and free the spike pre-emptively.
//
// dt is a generous over-estimate of one server tick; over-shoot means a spike
// can be freed up to ~1 frame before it would visually reach the gib (the
// spike is a tiny fast-moving sprite — imperceptible). Under-shoot would mean
// missing gibs at low fps, so we err high.
void Spike_GibPathScan(void) {
    const float dt = 0.02f;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e->v.movetype != MOVETYPE_FLYMISSILE) continue;
        const char *cn = e->v.classname;
        if (!cn || strcmp(cn, "spike") != 0) continue;
        float dmg = (e->v.touch == superspike_touch) ? 18.0f : 9.0f;

        vec3_t start = { e->v.origin[0], e->v.origin[1], e->v.origin[2] };
        vec3_t end   = { start[0] + e->v.velocity[0] * dt,
                         start[1] + e->v.velocity[1] * dt,
                         start[2] + e->v.velocity[2] * dt };

        // Push pickups the nail crosses this tick. The spike continues -- one
        // nail through a row of three ammo boxes pushes all three. Owner is
        // the player ent (avoids self-hit on dropped backpack the moment it
        // appears).
        {
            vec3_t vdir; eng->VectorNormalize(e->v.velocity, vdir);
            Items_BulletSweep(start, end, vdir, dmg, e->v.owner);
        }

        // Sweep against both gibs and settled corpses. Settled corpses are
        // SOLID_TRIGGER (Corpse_LayProne'd) so spike physics skips them, just
        // like gibs. Pre-LayProne (mid-death-animation) monsters are still
        // SOLID_BBOX and handled by spike_touch -- excluded here so they get
        // to finish their death animation before being gibbed.
        edict_t *best = NULL;
        float best_t = 1.0f;
        int best_is_corpse = 0;
        for (edict_t *gent = eng->ED_Next(g->world); gent; gent = eng->ED_Next(gent)) {
            if (!gent->v.takedamage) continue;
            int is_corpse = (gent->v.solid == SOLID_TRIGGER
                             && is_flesh(gent)
                             && gent->v.deadflag != DEAD_NO);
            if (!is_gib(gent) && !is_corpse) continue;
            float t;
            if (segment_hits_aabb(start, end, gent->v.absmin, gent->v.absmax, &t) && t < best_t) {
                best_t = t;
                best = gent;
                best_is_corpse = is_corpse;
            }
        }
        if (best) {
            vec3_t vdir; eng->VectorNormalize(e->v.velocity, vdir);
            vec3_t bvel = { vdir[0]*40, vdir[1]*40, vdir[2]*40 };
            SpawnBlood(best->v.origin, bvel, dmg);
            if (best_is_corpse) {
                if (!GibCorpse(best)) {
                    // No gib entry -- still apply spike's intended damage so
                    // T_Damage's deadflag branch tracks overkill normally.
                    T_Damage(best, e, e->v.owner, dmg);
                }
            } else {
                gib_apply_hit_impulse(best, vdir, dmg);
            }
            eng->ED_Free(e);
        }
    }
}
// ---------------------------------------------------------------------------
// W_FireGust -- every weapon discharge punches a brief small tunnel
// through any smoke crossing the muzzle's line of fire. Same mechanism
// as the rocket wake (Particles_PushTube), just one-shot and tighter --
// the hitscan beam isn't sustained, so particles get a single radial
// kick and drift back to ambient over the usual ~1s drag bleed-off.
//
// Assumes the caller already ran eng->MakeVectors(self->v.v_angle) so
// g->v_forward is populated. The tube is infinite along the axis, so
// any smoke in front of the muzzle within radius gets the same kick.
// ---------------------------------------------------------------------------
static void W_FireGust(void) {
    edict_t *self = g->self;
    vec3_t muzzle = {
        self->v.origin[0] + g->v_forward[0] * 16.0f,
        self->v.origin[1] + g->v_forward[1] * 16.0f,
        self->v.origin[2] + self->v.view_ofs[2] + g->v_forward[2] * 16.0f,
    };
    vec3_t axis = { g->v_forward[0], g->v_forward[1], g->v_forward[2] };
    // Single-frame impulse, so the magnitude has to do all the work --
    // unlike the rocket which kicks the same particles for many frames.
    // Rocket settings (250 / 48) are too gentle here; bump to 350 / 32
    // for a clearly visible one-shot punch that stays narrow.
    eng->Particles_PushTube(muzzle, axis, 350.0f, 32.0f);
}

// ---------------------------------------------------------------------------
// Missile_SmokeWake -- per-frame: flying missiles directly kick pt_smoke
// particles outward from their axis via Particles_PushTube. Bypasses the
// wind grid because rockets cross a smoke cloud in ~300ms -- far faster
// than wind-drag can transport particles visibly (k=1.5 + 0.85^tick velocity
// damping). The direct impulse adds radial velocity each frame the rocket
// is near a particle, so by the time the rocket exits the cloud particles
// in its path have visibly flown outward, leaving a tunnel.
// ---------------------------------------------------------------------------
void Missile_SmokeWake(void) {
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        // FLYMISSILE covers rockets + nail spikes. Grenades are
        // MOVETYPE_BOUNCE so we have to add them explicitly -- but only
        // grenade-style projectiles (classname filter), otherwise every
        // bouncing gib/backpack would also leave a smoke wake.
        const char *cn = e->v.classname;
        int is_grenade = (e->v.movetype == MOVETYPE_BOUNCE &&
                          cn && strcmp(cn, "grenade") == 0);
        if (e->v.movetype != MOVETYPE_FLYMISSILE && !is_grenade) continue;
        vec3_t axis = { e->v.velocity[0], e->v.velocity[1], e->v.velocity[2] };
        // Magnitude is now the *target* outward speed at the tube axis
        // (R_PushSmokeTube SETs velocity rather than adding). Once the
        // rocket has passed, the particle's vel is whatever the last
        // frame set; wind-drag in R_DrawParticles bleeds that down to
        // ambient over ~1s. No more "tunnel keeps widening forever".
        eng->Particles_PushTube(e->v.origin, axis, 250.0f, 48.0f);
    }
}

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
static void CycleWeaponReverseCommand(void);
static void ServerflagsCommand(void);
static void QuadCheat(void);
static void ImpulseCommands(void);
static void W_Attack(void);

// ---------------------------------------------------------------------------
// W_Precache -- called from worldspawn (world.c weak stub → replaced here)
// ---------------------------------------------------------------------------
// Registers weapon-related cvars at engine startup (called from game_init
// in game_main.c). Putting them here instead of in W_Precache means they
// are available from the console before any map has loaded.
void Weapons_RegisterCvars(void) {
    // Test cvar: when >0, the grenade launcher auto-equips and fires N
    // bouncing gibs (with decal_on_bounce = 1) per shot instead of grenades,
    // with infinite ammo. Lets you spam blood decals without monster kills.
    eng->Cvar_Register("g_test_gibgrenades", "0");
    // Test cvar: when >0, shotgun/super-shotgun pellets spawn blood at every
    // impact (walls, doors, anywhere) in addition to the normal gunshot puff.
    // Lets you splatter blood freely for tuning the wall-slide effect.
    eng->Cvar_Register("g_test_shotgunblood", "0");
}

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
    eng->PrecacheSound("ambience/fire1.wav");   // flamethrower loop (M8/F3)
    WeaponsFire_Init();
    Flammables_Init();

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
    emit_weapon_sound(self, 0.15f);
    vec3_t source, org;
    source[0] = self->v.origin[0];
    source[1] = self->v.origin[1];
    source[2] = self->v.origin[2] + 16;

    vec3_t end;
    end[0] = source[0] + g->v_forward[0]*64;
    end[1] = source[1] + g->v_forward[1]*64;
    end[2] = source[2] + g->v_forward[2]*64;
    eng->SV_Traceline(source, end, 0, self);
    Corpse_BulletTrace(source, end, self);

    // Axe sweep splashes when the swing touches a liquid surface — but
    // only when the player's head is above water. If they're fully
    // submerged (waterlevel 3, eyes underwater) any "surface splash" we
    // emit would appear detached above them; the swing is happening in
    // bulk water with no surface contact.
    int swing_splashed = 0;
    if (self->v.waterlevel < 3.0f) {
        // Shore-side downward hit (air->water entry), wading swing exiting
        // into air (water->air), or chest-deep swing with both ends in water
        // (splash above the swing midpoint). Strength 24 = 1.5x bullet — a
        // swing displaces more than a pellet.
        swing_splashed = splash_along_segment(source, end, 24);
    }
    // Wading fallback: the player's feet are in water but the axe arc is
    // entirely above the surface (waterlevel 1 with a horizontal swing).
    // Place the splash ~32 units ahead at foot height so it appears where
    // the player is looking (in front of them, visible) rather than under
    // their feet (occluded by the body). If that point isn't in liquid
    // (e.g. swinging toward the pool's edge from inside) fall back to the
    // feet seed so we still get a splash.
    if (!swing_splashed && self->v.waterlevel >= 1.0f && self->v.waterlevel < 3.0f) {
        vec3_t fwd_h, seed;
        fwd_h[0] = g->v_forward[0];
        fwd_h[1] = g->v_forward[1];
        fwd_h[2] = 0.0f;
        eng->VectorNormalize(fwd_h, fwd_h);
        seed[0] = self->v.origin[0] + fwd_h[0] * 32.0f;
        seed[1] = self->v.origin[1] + fwd_h[1] * 32.0f;
        seed[2] = self->v.origin[2] + self->v.mins[2] + 1.0f;
        if (!is_liquid(eng->SV_PointContents(seed))) {
            seed[0] = self->v.origin[0];
            seed[1] = self->v.origin[1];
        }
        splash_underwater_explosion(seed, 16);
    }

    if (g->trace_fraction == 1.0f) return;

    org[0] = g->trace_endpos[0] - g->v_forward[0]*4;
    org[1] = g->trace_endpos[1] - g->v_forward[1]*4;
    org[2] = g->trace_endpos[2] - g->v_forward[2]*4;

    if (is_flesh(g->trace_ent)) {
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
        // Shootable brushes (secret doors, breakables) still take damage so
        // they trigger / die — they just don't bleed.
        if (g->trace_ent->v.takedamage) {
            g->trace_ent->v.axhitme = 1;
            T_Damage(g->trace_ent, self, self, 20);
        }
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
    // Broadcast TE_BLOODSPRAY so the client spawns pt_grav blood that arcs and
    // falls under real gravity. svc_particle would force pt_slowgrav (~5% of
    // gravity) plus wind drag, which makes blood drift instead of falling.
    int count = (int)(damage * 2.0f);
    if (count < 1) count = 1;
    if (count > 255) count = 255;
    // Match svc_particle's vel encoding (dir*16 clamped to signed-char).
    int cvx = (int)(vel[0] * 0.1f * 16.0f);
    int cvy = (int)(vel[1] * 0.1f * 16.0f);
    int cvz = (int)(vel[2] * 0.1f * 16.0f);
    if (cvx >  127) cvx =  127; else if (cvx < -128) cvx = -128;
    if (cvy >  127) cvy =  127; else if (cvy < -128) cvy = -128;
    if (cvz >  127) cvz =  127; else if (cvz < -128) cvz = -128;
    eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte (MSG_BROADCAST, TE_BLOODSPRAY);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    eng->MSG_WriteChar (MSG_BROADCAST, cvx);
    eng->MSG_WriteChar (MSG_BROADCAST, cvy);
    eng->MSG_WriteChar (MSG_BROADCAST, cvz);
    eng->MSG_WriteByte (MSG_BROADCAST, count);
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
// Water-surface splash: if the segment start->end crosses an air→liquid
// boundary, binary-search for the crossing and emit a particle burst at the
// surface. Returns 1 if a splash was emitted.
// ---------------------------------------------------------------------------
static int is_liquid(int c) {
    return c == CONTENT_WATER || c == CONTENT_SLIME || c == CONTENT_LAVA;
}

int splash_at_water_entry(vec3_t start, vec3_t end, int strength_q4) {
    if (is_liquid(eng->SV_PointContents(start))) return 0;
    int end_c = eng->SV_PointContents(end);
    if (!is_liquid(end_c)) return 0;

    vec3_t lo, hi, mid;
    lo[0] = start[0]; lo[1] = start[1]; lo[2] = start[2];
    hi[0] = end[0];   hi[1] = end[1];   hi[2] = end[2];
    for (int i = 0; i < 10; i++) {
        mid[0] = (lo[0]+hi[0])*0.5f;
        mid[1] = (lo[1]+hi[1])*0.5f;
        mid[2] = (lo[2]+hi[2])*0.5f;
        if (is_liquid(eng->SV_PointContents(mid))) {
            hi[0] = mid[0]; hi[1] = mid[1]; hi[2] = mid[2];
        } else {
            lo[0] = mid[0]; lo[1] = mid[1]; lo[2] = mid[2];
        }
    }
    // Use TE_WATERSPLASH instead of SV_Particle so the client spawns pt_grav
    // particles with a ~1s lifetime — they rise, peak, then fall back. The
    // svc_particle path forces pt_slowgrav + 0..0.4s die and bleeds the burst
    // off via wind drag before gravity can pull it down.
    int kind = 0;
    if (end_c == CONTENT_SLIME) kind = 1;
    if (end_c == CONTENT_LAVA)  kind = 2;
    if (strength_q4 < 1)   strength_q4 = 1;
    if (strength_q4 > 255) strength_q4 = 255;
    eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte (MSG_BROADCAST, TE_WATERSPLASH);
    eng->MSG_WriteCoord(MSG_BROADCAST, hi[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, hi[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, hi[2]);
    eng->MSG_WriteByte (MSG_BROADCAST, kind);
    eng->MSG_WriteByte (MSG_BROADCAST, strength_q4);
    return 1;
}

// Backtrace from a projectile's impact point along its inverse velocity, then
// look for an air→liquid crossing. Used by spike/rocket touch handlers so a
// projectile that punches through water before hitting solid still leaves a
// surface splash. dist sets how far back to look (must exceed one frame of
// travel, but not so far that we re-enter air on the other side of a wall).
// Underwater explosion -> column-of-water splash at the surface above. Walks
// straight up from `org` looking for the air boundary; bails if the surface is
// further than 4096 units (deep submersion — no visible splash).
void splash_underwater_explosion(vec3_t org, int strength_q4) {
    int c0 = eng->SV_PointContents(org);
    if (!is_liquid(c0)) return;
    vec3_t hi = {org[0], org[1], org[2] + 4096.0f};
    if (is_liquid(eng->SV_PointContents(hi))) return;
    float lo_z = org[2], hi_z = org[2] + 4096.0f;
    for (int i = 0; i < 14; i++) {
        float mid_z = (lo_z + hi_z) * 0.5f;
        vec3_t mid = {org[0], org[1], mid_z};
        if (is_liquid(eng->SV_PointContents(mid))) lo_z = mid_z;
        else                                       hi_z = mid_z;
    }
    int kind = 0;
    if (c0 == CONTENT_SLIME) kind = 1;
    if (c0 == CONTENT_LAVA)  kind = 2;
    if (strength_q4 < 1)   strength_q4 = 1;
    if (strength_q4 > 255) strength_q4 = 255;
    eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte (MSG_BROADCAST, TE_WATERSPLASH);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, hi_z);
    eng->MSG_WriteByte (MSG_BROADCAST, kind);
    eng->MSG_WriteByte (MSG_BROADCAST, strength_q4);
}

// Generalised splash for an arbitrary line segment (e.g. an axe swing). Picks
// the right helper for whatever water/air configuration the endpoints land in:
//   - one end in air, other in liquid -> air->water entry crossing
//   - both ends in liquid             -> player swinging while submerged;
//                                         splash at the surface above the
//                                         midpoint
//   - both ends in air                -> no splash
int splash_along_segment(vec3_t a, vec3_t b, int strength_q4) {
    if (splash_at_water_entry(a, b, strength_q4)) return 1;
    if (splash_at_water_entry(b, a, strength_q4)) return 1;
    int ca = eng->SV_PointContents(a);
    int cb = eng->SV_PointContents(b);
    if (is_liquid(ca) && is_liquid(cb)) {
        vec3_t mid;
        mid[0] = (a[0] + b[0]) * 0.5f;
        mid[1] = (a[1] + b[1]) * 0.5f;
        mid[2] = (a[2] + b[2]) * 0.5f;
        splash_underwater_explosion(mid, strength_q4);
        return 1;
    }
    return 0;
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

    // Gib check first: ThrowGib spawns fresh edicts with flags=0, so they
    // fail is_flesh (no FL_MONSTER/FL_CLIENT) even though they bleed and
    // need impulse. ThrowHead reuses the monster edict and keeps FL_MONSTER,
    // which is why head gibs incidentally worked through the is_flesh branch
    // before this split.
    if (is_gib(g->trace_ent)) {
        vec3_t sv = {vel[0]*0.2f, vel[1]*0.2f, vel[2]*0.2f};
        SpawnBlood(org, sv, damage);
        AddMultiDamage(g->trace_ent, damage);
        // T_Damage's gib branch suppresses its own (inflictor-midpoint)
        // impulse when the inflictor is a client, so this is the sole
        // hitscan contribution.
        gib_apply_hit_impulse(g->trace_ent, dir, damage);
    } else if (is_flesh(g->trace_ent)) {
        vec3_t sv = {vel[0]*0.2f, vel[1]*0.2f, vel[2]*0.2f};
        SpawnBlood(org, sv, damage);
        AddMultiDamage(g->trace_ent, damage);
    } else {
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_GUNSHOT);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
        // Shootable brushes still take damage.
        if (g->trace_ent->v.takedamage)
            AddMultiDamage(g->trace_ent, damage);
        // A bullet striking a surface inside an oil patch sets it alight.
        Fire_LightOilNear(g->trace_endpos, 0.0f);
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
        Corpse_BulletTrace(src, end, self);
        splash_at_water_entry(src, g->trace_endpos, 16); // 1.0x bullet pellet
        // Push any pickups the pellet line crosses up to the wall hit. Each
        // pellet pushes independently, so a tight shotgun cluster on a single
        // box gets multiplied -- pleasingly heavy at point-blank.
        {
            vec3_t pend = { g->trace_endpos[0], g->trace_endpos[1], g->trace_endpos[2] };
            Items_BulletSweep(src, pend, direction, 4.0f, self);
        }
        if (g->trace_fraction != 1.0f)
            TraceAttack(4, direction);

        shotcount -= 1;
    }
    ApplyMultiDamage();
}

// Broadcast a TE_SHELLEJECT message: client spawns a brass-coloured pt_grav
// particle with PARTFL_BOUNCE at `org` with velocity `vel`.
static void eject_shell(vec3_t org, vec3_t vel) {
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_SHELLEJECT);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    eng->MSG_WriteCoord(MSG_BROADCAST, vel[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, vel[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, vel[2]);
}

// Eject a shell to the right of the player's view, with the given lateral
// nudge so super-shotgun can spawn two visually-distinct shells. Origin is
// at view height (origin + view_ofs[2]) so the arc is visible above the
// viewmodel sprite, not down at chest level where the gun is held.
static void player_eject_shell(edict_t *self, float lateral_offset) {
    eng->MakeVectors(self->v.v_angle);
    vec3_t org, vel;
    float ox = lateral_offset;
    org[0] = self->v.origin[0] + g->v_forward[0]*16.0f + g->v_right[0]*(6.0f + ox);
    org[1] = self->v.origin[1] + g->v_forward[1]*16.0f + g->v_right[1]*(6.0f + ox);
    org[2] = self->v.origin[2] + self->v.view_ofs[2] - 4.0f;
    float rspeed = 140.0f + eng->Random()*60.0f;	// 140..200 lateral
    float upspd  = 90.0f  + eng->Random()*40.0f;	// 90..130 up
    float fspeed = 60.0f  + eng->Random()*40.0f;	// 60..100 forward (clears the viewmodel)
    vel[0] = g->v_right[0]*rspeed + g->v_forward[0]*fspeed + (eng->Random()-0.5f)*30.0f;
    vel[1] = g->v_right[1]*rspeed + g->v_forward[1]*fspeed + (eng->Random()-0.5f)*30.0f;
    vel[2] = upspd + g->v_right[2]*rspeed + g->v_forward[2]*fspeed;
    eject_shell(org, vel);
}

static void W_FireShotgun(void) {
    edict_t *self = g->self;
    // Debug: replace the entire shotgun fire with N deterministic blood
    // droplets shot from the gun, where N = g_test_shotgunblood (capped
    // at 64). N==1 → one droplet straight forward; N>1 → small spread
    // cone around the forward direction.
    int blood_n = (int)eng->Cvar_VariableValue("g_test_shotgunblood");
    if (blood_n > 0) {
        if (blood_n > 64) blood_n = 64;
        eng->MakeVectors(self->v.v_angle);
        float mx = self->v.origin[0] + g->v_forward[0] * 16;
        float my = self->v.origin[1] + g->v_forward[1] * 16;
        float mz = self->v.absmin[2] + self->v.size[2] * 0.7f;
        int i;
        for (i = 0; i < blood_n; i++) {
            float c1 = (blood_n > 1) ? (eng->Random() - 0.5f) * 0.15f : 0.0f;
            float c2 = (blood_n > 1) ? (eng->Random() - 0.5f) * 0.15f : 0.0f;
            float dx = g->v_forward[0] + g->v_right[0]*c1 + g->v_up[0]*c2;
            float dy = g->v_forward[1] + g->v_right[1]*c1 + g->v_up[1]*c2;
            float dz = g->v_forward[2] + g->v_right[2]*c1 + g->v_up[2]*c2;
            float vx = dx * 500;
            float vy = dy * 500;
            float vz = dz * 500;
            eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
            eng->MSG_WriteByte (MSG_BROADCAST, TE_DEBUGBLOOD);
            eng->MSG_WriteCoord(MSG_BROADCAST, mx);
            eng->MSG_WriteCoord(MSG_BROADCAST, my);
            eng->MSG_WriteCoord(MSG_BROADCAST, mz);
            eng->MSG_WriteCoord(MSG_BROADCAST, vx);
            eng->MSG_WriteCoord(MSG_BROADCAST, vy);
            eng->MSG_WriteCoord(MSG_BROADCAST, vz);
        }
        return;
    }
    emit_weapon_sound(self, 0.7f);
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;
    self->v.currentammo = self->v.ammo_shells = self->v.ammo_shells - 1;
    vec3_t dir;
    eng->SV_Aim(self, 100000, dir);
    vec3_t spread = {0.04f, 0.04f, 0};
    FireBullets(6, dir, spread);
    player_eject_shell(self, 0.0f);
}

static void W_FireSuperShotgun(void) {
    edict_t *self = g->self;
    emit_weapon_sound(self, 0.85f);
    if (self->v.currentammo == 1) { W_FireShotgun(); return; }
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/shotgn2.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -4;
    self->v.currentammo = self->v.ammo_shells = self->v.ammo_shells - 2;
    vec3_t dir;
    eng->SV_Aim(self, 100000, dir);
    vec3_t spread = {0.14f, 0.08f, 0};
    FireBullets(14, dir, spread);
    player_eject_shell(self, -2.0f);
    player_eject_shell(self, +2.0f);
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
static void T_MissileTouch_impl(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;

    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) {
        eng->ED_Free(self);
        return;
    }
    // Entry splash now emitted engine-side by SV_CheckWaterTransition on
    // the physics step that crosses into liquid, so the timing matches
    // the actual water hit (was previously delayed until rocket reached
    // a solid behind/below the water). Detonation column stays here.
    splash_underwater_explosion(self->v.origin, 64); // 4.0x detonation column

    float damg = 100 + eng->Random()*20;
    if (other->v.health) {
        if (other->v.classname && strcmp(other->v.classname, "monster_shambler") == 0)
            damg *= 0.5f;
        T_Damage(other, self, self->v.owner, damg);
    }
    T_RadiusDamage(self, self->v.owner, 120, other);
    Items_RadiusPush(self->v.origin, 160.0f, 120.0f, NULL);

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
static void T_MissileTouch(edict_t *self, edict_t *other) {
    SIM_PERF("T_MissileTouch") T_MissileTouch_impl(self, other);
}

static void W_FireRocket(void) {
    edict_t *self = g->self;
    emit_weapon_sound(self, 0.9f);
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
    missile->v.classname = "missile";
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
    vec3_t beam_dir = { f[0], f[1], f[2] };  // saved before f is reused for spread
    // rotate 90° in XY to get a perpendicular spread offset
    float old_fy = f[1];
    f[0] = -old_fy;
    f[1] = f[0]; // = -old_fy
    f[2] = 0;
    foff[0] = f[0]*16; foff[1] = f[1]*16; foff[2] = 0;

    edict_t *e1 = g->world, *e2 = g->world;
    vec3_t par = {0,0,100};

    eng->SV_Traceline(p1, p2, 0, self);
    Corpse_BulletTrace(p1, p2, self);
    {
        vec3_t pend = { g->trace_endpos[0], g->trace_endpos[1], g->trace_endpos[2] };
        Items_BulletSweep(p1, pend, beam_dir, damage, self);
    }
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
    Corpse_BulletTrace(p1f, p2f, self);
    if (g->trace_ent != e1 && g->trace_ent->v.takedamage) {
        eng->SV_Particle(g->trace_endpos, par, 225, damage*4);
        T_Damage(g->trace_ent, from, from, damage);
    }
    e2 = g->trace_ent;

    vec3_t p1b = {p1[0]-foff[0], p1[1]-foff[1], p1[2]};
    vec3_t p2b = {p2[0]-foff[0], p2[1]-foff[1], p2[2]};
    eng->SV_Traceline(p1b, p2b, 0, self);
    Corpse_BulletTrace(p1b, p2b, self);
    if (g->trace_ent != e1 && g->trace_ent != e2 && g->trace_ent->v.takedamage) {
        eng->SV_Particle(g->trace_endpos, par, 225, damage*4);
        T_Damage(g->trace_ent, from, from, damage);
    }
}

void W_FireLightning(void) {
    edict_t *self = g->self;
    emit_weapon_sound(self, 0.6f);
    if (self->v.ammo_cells < 1) {
        self->v.weapon = W_BestWeapon();
        W_SetCurrentAmmo();
        return;
    }
    if (self->v.waterlevel > 1) {
        T_RadiusDamage(self, self, 35*self->v.ammo_cells, g->world);
        Items_RadiusPush(self->v.origin, 200.0f, 35.0f*self->v.ammo_cells, NULL);
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

    // Push smoke aside along the bolt itself, not just via the W_Attack
    // muzzle gust -- lightning re-fires at ~10 Hz from player_light1_think
    // while W_Attack only runs ~5 Hz, and the grenade spawns 80 puffs/sec.
    // Without this per-bolt push, fresh smoke refills the tunnel between
    // muzzle gusts and the player sees no effect.
    vec3_t axis = { g->v_forward[0], g->v_forward[1], g->v_forward[2] };
    eng->Particles_PushTube(org, axis, 350.0f, 32.0f);

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_LIGHTNING2);
    eng->MSG_WriteEntity(MSG_BROADCAST, self);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[2]);

    // Spark shower at the bolt's visible endpoint. TE_SPARKBURST carries
    // origin + surface normal + count to the client, which spawns 20
    // cyan-white pt_spark particles in the hemisphere oriented by the
    // normal. They bounce once (r_sparks_restitution), stick on the second
    // hit, ramp through ramp1[] (white -> yellow -> orange -> red -> dark),
    // and linger as embers for r_sparks_settle_dwell seconds.
    eng->MSG_WriteByte (MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte (MSG_BROADCAST, TE_SPARKBURST);
    eng->MSG_WriteCoord (MSG_BROADCAST, g->trace_endpos[0]);
    eng->MSG_WriteCoord (MSG_BROADCAST, g->trace_endpos[1]);
    eng->MSG_WriteCoord (MSG_BROADCAST, g->trace_endpos[2]);
    eng->MSG_WriteChar (MSG_BROADCAST, (int)(g->trace_plane_normal[0] * 127.0f));
    eng->MSG_WriteChar (MSG_BROADCAST, (int)(g->trace_plane_normal[1] * 127.0f));
    eng->MSG_WriteChar (MSG_BROADCAST, (int)(g->trace_plane_normal[2] * 127.0f));
    eng->MSG_WriteByte (MSG_BROADCAST, 20);

    vec3_t lend = {g->trace_endpos[0]+g->v_forward[0]*4,
                   g->trace_endpos[1]+g->v_forward[1]*4,
                   g->trace_endpos[2]+g->v_forward[2]*4};
    LightningDamage(self->v.origin, lend, self, 30);
}

// ---------------------------------------------------------------------------
// Grenade launcher
// ---------------------------------------------------------------------------
static void GrenadeExplode_impl(edict_t *self) {
    g->self = self;
    T_RadiusDamage(self, self->v.owner, 120, g->world);
    Items_RadiusPush(self->v.origin, 160.0f, 120.0f, NULL);
    splash_underwater_explosion(self->v.origin, 48); // 3.0x grenade detonation column
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    BecomeExplosion();
}
static void GrenadeExplode(edict_t *self) {
    SIM_PERF("GrenadeExplode") GrenadeExplode_impl(self);
}

static void GrenadeTouch_impl(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    // Entry splash handled engine-side by SV_CheckWaterTransition.
    if (other->v.takedamage == DAMAGE_AIM) { GrenadeExplode(self); return; }
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/bounce.wav", 1, ATTN_NORM);
    if (self->v.velocity[0] == 0 && self->v.velocity[1] == 0 && self->v.velocity[2] == 0)
        self->v.avelocity[0] = self->v.avelocity[1] = self->v.avelocity[2] = 0;
}
static void GrenadeTouch(edict_t *self, edict_t *other) {
    SIM_PERF("GrenadeTouch") GrenadeTouch_impl(self, other);
}

static void W_FireGrenade(void) {
    edict_t *self = g->self;
    static const char *gibmdl[3] = {
        "progs/gib1.mdl", "progs/gib2.mdl", "progs/gib3.mdl"
    };
    // Test cvar value is the gib count per shot (0 = off, normal grenade).
    int test_gib_count = (int)eng->Cvar_VariableValue("g_test_gibgrenades");
    if (test_gib_count < 0)  test_gib_count = 0;
    if (test_gib_count > 32) test_gib_count = 32;

    emit_weapon_sound(self, 0.8f);
    if (test_gib_count > 0) {
        self->v.ammo_rockets = 100;
        self->v.currentammo  = 100;
    } else {
        self->v.currentammo = self->v.ammo_rockets = self->v.ammo_rockets - 1;
    }
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/grenade.wav", 1, ATTN_NORM);
    self->v.punchangle[0] = -2;
    eng->MakeVectors(self->v.v_angle);

    // Base velocity matches stock W_FireGrenade.
    vec3_t base_vel;
    if (self->v.v_angle[0]) {
        float c3 = crandom(), c4 = crandom();
        base_vel[0] = g->v_forward[0]*600 + g->v_up[0]*200 + c3*g->v_right[0]*10 + c4*g->v_up[0]*10;
        base_vel[1] = g->v_forward[1]*600 + g->v_up[1]*200 + c3*g->v_right[1]*10 + c4*g->v_up[1]*10;
        base_vel[2] = g->v_forward[2]*600 + g->v_up[2]*200 + c3*g->v_right[2]*10 + c4*g->v_up[2]*10;
    } else {
        eng->SV_Aim(self, 10000, base_vel);
        base_vel[0] *= 600; base_vel[1] *= 600; base_vel[2] *= 600;
        base_vel[2] = 200;
    }

    int n = (test_gib_count > 0) ? test_gib_count : 1;
    for (int i = 0; i < n; i++) {
        edict_t *missile     = eng->ED_Alloc();
        missile->v.owner     = self;
        missile->v.movetype  = MOVETYPE_BOUNCE;
        missile->v.classname = (test_gib_count > 0) ? "blood_gib" : "grenade";

        // Multi-gib spread: first gib gets the base velocity, others fan
        // ~30 deg perpendicular to forward with random vertical kick so
        // they don't pile up on identical trajectories.
        if (i == 0 || test_gib_count == 0) {
            missile->v.velocity[0] = base_vel[0];
            missile->v.velocity[1] = base_vel[1];
            missile->v.velocity[2] = base_vel[2];
        } else {
            float sx = crandom() * 200.0f;   // sideways
            float sz = crandom() * 150.0f;   // vertical kick
            missile->v.velocity[0] = base_vel[0] + g->v_right[0]*sx;
            missile->v.velocity[1] = base_vel[1] + g->v_right[1]*sx;
            missile->v.velocity[2] = base_vel[2] + sz;
        }
        missile->v.avelocity[0] = 600.0f * crandom();
        missile->v.avelocity[1] = 600.0f * crandom();
        missile->v.avelocity[2] = 600.0f * crandom();
        eng->VectorToAngles(missile->v.velocity, missile->v.angles);

        if (test_gib_count > 0) {
            // Bouncing gib: no explosion, no touch (SOLID_NOT skips
            // SV_Impact), disappears after 10-20 s like a real ThrowGib.
            missile->v.solid           = SOLID_NOT;
            missile->v.touch           = NULL;
            missile->v.think           = SUB_Remove;
            missile->v.nextthink       = g->time + 10.0f + eng->Random()*10.0f;
            missile->v.decal_on_bounce = 1.0f;
            eng->SV_SetModel(missile, gibmdl[(int)(eng->Random()*3.0f) % 3]);
        } else {
            missile->v.solid     = SOLID_BBOX;
            missile->v.touch     = GrenadeTouch;
            missile->v.think     = GrenadeExplode;
            missile->v.nextthink = g->time + 2.5f;
            eng->SV_SetModel(missile, "progs/grenade.mdl");
        }
        vec3_t gzero = {0,0,0};
        eng->SV_SetSize(missile, gzero, gzero);
        eng->SV_SetOrigin(missile, self->v.origin);
    }
}

// ---------------------------------------------------------------------------
// Spike projectiles
// ---------------------------------------------------------------------------
static void spike_touch_impl(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    if (other->v.solid == SOLID_TRIGGER) return;
    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) { eng->ED_Free(self); return; }
    // Entry splash handled engine-side by SV_CheckWaterTransition.

    if (is_flesh(other)) {
        spawn_touchblood(9);
        T_Damage(other, self, self->v.owner, 9);
    } else {
        const char *cn = self->v.classname;
        if (cn && (strcmp(cn, "wizspike") == 0 || strcmp(cn, "knightspike") == 0)) {
            // Monster spikes keep their coloured-puff temp entity.
            eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
            if (strcmp(cn, "wizspike") == 0)
                eng->MSG_WriteByte(MSG_BROADCAST, TE_WIZSPIKE);
            else
                eng->MSG_WriteByte(MSG_BROADCAST, TE_KNIGHTSPIKE);
            eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
            eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
            eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
        } else {
            // Player nailgun: emit a small TE_SPARKBURST off the wall's
            // surface normal. Recover the normal by tracing a few units back
            // → forward along the nail's velocity; fall back to negated
            // velocity if the trace finds no surface (corner/sky edge case).
            float vx = self->v.velocity[0];
            float vy = self->v.velocity[1];
            float vz = self->v.velocity[2];
            float vlen2 = vx*vx + vy*vy + vz*vz;
            float nx, ny, nz;
            if (vlen2 > 1.0f) {
                float inv = 1.0f / (float)sqrt(vlen2);
                float dx = vx * inv * 4.0f;
                float dy = vy * inv * 4.0f;
                float dz = vz * inv * 4.0f;
                vec3_t back = {self->v.origin[0] - dx,
                               self->v.origin[1] - dy,
                               self->v.origin[2] - dz};
                vec3_t fwd  = {self->v.origin[0] + dx,
                               self->v.origin[1] + dy,
                               self->v.origin[2] + dz};
                eng->SV_Traceline(back, fwd, 0, self);
                if (g->trace_fraction < 1.0f) {
                    nx = g->trace_plane_normal[0];
                    ny = g->trace_plane_normal[1];
                    nz = g->trace_plane_normal[2];
                } else {
                    nx = -vx * inv;
                    ny = -vy * inv;
                    nz = -vz * inv;
                }
            } else {
                nx = 0; ny = 0; nz = 1;	// degenerate; just shoot sparks up
            }
            int cvx = (int)(nx * 127.0f);
            int cvy = (int)(ny * 127.0f);
            int cvz = (int)(nz * 127.0f);
            eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
            eng->MSG_WriteByte(MSG_BROADCAST, TE_SPARKBURST);
            eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
            eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
            eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
            eng->MSG_WriteChar(MSG_BROADCAST, cvx);
            eng->MSG_WriteChar(MSG_BROADCAST, cvy);
            eng->MSG_WriteChar(MSG_BROADCAST, cvz);
            eng->MSG_WriteByte(MSG_BROADCAST, 5);
        }
        if (other->v.takedamage)
            T_Damage(other, self, self->v.owner, 9);
    }
    eng->ED_Free(self);
}
static void spike_touch(edict_t *self, edict_t *other) {
    SIM_PERF("spike_touch") spike_touch_impl(self, other);
}

static void superspike_touch_impl(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (other == self->v.owner) return;
    if (other->v.solid == SOLID_TRIGGER) return;
    if (eng->SV_PointContents(self->v.origin) == CONTENT_SKY) { eng->ED_Free(self); return; }
    // Entry splash handled engine-side by SV_CheckWaterTransition.

    if (is_flesh(other)) {
        spawn_touchblood(18);
        T_Damage(other, self, self->v.owner, 18);
    } else {
        // Player super-nailgun: emit a TE_SPARKBURST off the wall's
        // surface normal. Recover the normal by tracing a few units back
        // → forward along the nail's velocity; fall back to negated
        // velocity if the trace finds no surface (corner/sky edge case).
        float vx = self->v.velocity[0];
        float vy = self->v.velocity[1];
        float vz = self->v.velocity[2];
        float vlen2 = vx*vx + vy*vy + vz*vz;
        float nx, ny, nz;
        if (vlen2 > 1.0f) {
            float inv = 1.0f / (float)sqrt(vlen2);
            float dx = vx * inv * 4.0f;
            float dy = vy * inv * 4.0f;
            float dz = vz * inv * 4.0f;
            vec3_t back = {self->v.origin[0] - dx,
                           self->v.origin[1] - dy,
                           self->v.origin[2] - dz};
            vec3_t fwd  = {self->v.origin[0] + dx,
                           self->v.origin[1] + dy,
                           self->v.origin[2] + dz};
            eng->SV_Traceline(back, fwd, 0, self);
            if (g->trace_fraction < 1.0f) {
                nx = g->trace_plane_normal[0];
                ny = g->trace_plane_normal[1];
                nz = g->trace_plane_normal[2];
            } else {
                nx = -vx * inv;
                ny = -vy * inv;
                nz = -vz * inv;
            }
        } else {
            nx = 0; ny = 0; nz = 1;	// degenerate; just shoot sparks up
        }
        int cvx = (int)(nx * 127.0f);
        int cvy = (int)(ny * 127.0f);
        int cvz = (int)(nz * 127.0f);
        eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
        eng->MSG_WriteByte(MSG_BROADCAST, TE_SPARKBURST);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
        eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
        eng->MSG_WriteChar(MSG_BROADCAST, cvx);
        eng->MSG_WriteChar(MSG_BROADCAST, cvy);
        eng->MSG_WriteChar(MSG_BROADCAST, cvz);
        eng->MSG_WriteByte(MSG_BROADCAST, 7);
        if (other->v.takedamage)
            T_Damage(other, self, self->v.owner, 18);
    }
    eng->ED_Free(self);
}
void superspike_touch(edict_t *self, edict_t *other) {
    SIM_PERF("superspike_touch") superspike_touch_impl(self, other);
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
    emit_weapon_sound(self, 0.55f);
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

    // Test cvar: when >0, force the grenade launcher (grant if missing,
    // make it the active weapon, refill rockets) before any ammo gating
    // so a single `g_test_gibgrenades N` console set is enough to start
    // spamming gib-grenades regardless of current loadout.
    int test_gib_count = (int)eng->Cvar_VariableValue("g_test_gibgrenades");
    if (test_gib_count > 0) {
        int items = (int)self->v.items;
        items |= IT_GRENADE_LAUNCHER;
        self->v.items   = (float)items;
        self->v.weapon  = IT_GRENADE_LAUNCHER;
        self->v.weapon2 = 0;                 // clear Phase 6 selector
        self->v.ammo_rockets = 100;
        W_SetCurrentAmmo();                  // refresh weaponmodel/currentammo
    }

    // Phase 6: route through the parallel dispatch when a Phase 6 weapon is
    // active. The Phase 6 fire functions handle their own ammo + sound + anim.
    if (self->v.weapon2 != 0) {
        eng->MakeVectors(self->v.v_angle);
        self->v.show_hostile = g->time + 1;
        W_FireGust();
        W_Attack_Phase6();
        return;
    }

    if (!W_CheckNoAmmo()) return;

    eng->MakeVectors(self->v.v_angle);
    self->v.show_hostile = g->time + 1;
    W_FireGust();

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

// ---------------------------------------------------------------------------
// Unified weapon cycle (impulse 10 / 12) — covers stock Quake + Phase 6.
// Order: 8 stock Quake, 6 Doom, 4 Wolf3D. Cycler walks forward/back through
// the array, skipping slots the player doesn't own or doesn't have enough
// ammo for, and wraps at either end.
// ---------------------------------------------------------------------------
typedef struct { int is_phase6; int flag; } weapon_slot_t;
static const weapon_slot_t WCYCLE[] = {
    {0, IT_AXE},
    {0, IT_SHOTGUN},
    {0, IT_SUPER_SHOTGUN},
    {0, IT_NAILGUN},
    {0, IT_SUPER_NAILGUN},
    {0, IT_GRENADE_LAUNCHER},
    {0, IT_ROCKET_LAUNCHER},
    {0, IT_LIGHTNING},
    {1, IT2_DOOM_FIST},
    {1, IT2_DOOM_PISTOL},
    {1, IT2_DOOM_SHOTGUN},
    {1, IT2_DOOM_CHAINGUN},
    {1, IT2_DOOM_ROCKET},
    {1, IT2_DOOM_CHAINSAW},
    {1, IT2_WOLF_KNIFE},
    {1, IT2_WOLF_PISTOL},
    {1, IT2_WOLF_MACHINEGUN},
    {1, IT2_WOLF_CHAINGUN},
};
#define WCYCLE_COUNT ((int)(sizeof(WCYCLE) / sizeof(WCYCLE[0])))

static int wcycle_usable(int idx) {
    edict_t *self = g->self;
    int items  = (int)self->v.items;
    int items2 = (int)self->v.items2;
    switch (idx) {
        case 0:  return  (items  & IT_AXE) != 0;
        case 1:  return ((items  & IT_SHOTGUN)          != 0) && self->v.ammo_shells  >= 1;
        case 2:  return ((items  & IT_SUPER_SHOTGUN)    != 0) && self->v.ammo_shells  >= 2;
        case 3:  return ((items  & IT_NAILGUN)          != 0) && self->v.ammo_nails   >= 1;
        case 4:  return ((items  & IT_SUPER_NAILGUN)    != 0) && self->v.ammo_nails   >= 2;
        case 5:  return ((items  & IT_GRENADE_LAUNCHER) != 0) && self->v.ammo_rockets >= 1;
        case 6:  return ((items  & IT_ROCKET_LAUNCHER)  != 0) && self->v.ammo_rockets >= 1;
        case 7:  return ((items  & IT_LIGHTNING)        != 0) && self->v.ammo_cells   >= 1;
        case 8:  return  (items2 & IT2_DOOM_FIST)       != 0;
        case 9:  return ((items2 & IT2_DOOM_PISTOL)     != 0) && self->v.ammo_bullets >= 1;
        case 10: return ((items2 & IT2_DOOM_SHOTGUN)    != 0) && self->v.ammo_shells  >= 1;
        case 11: return ((items2 & IT2_DOOM_CHAINGUN)   != 0) && self->v.ammo_bullets >= 1;
        case 12: return ((items2 & IT2_DOOM_ROCKET)     != 0) && self->v.ammo_rockets >= 1;
        case 13: return  (items2 & IT2_DOOM_CHAINSAW)   != 0;
        case 14: return  (items2 & IT2_WOLF_KNIFE)      != 0;
        case 15: return ((items2 & IT2_WOLF_PISTOL)     != 0) && self->v.ammo_bullets >= 1;
        case 16: return ((items2 & IT2_WOLF_MACHINEGUN) != 0) && self->v.ammo_bullets >= 1;
        case 17: return ((items2 & IT2_WOLF_CHAINGUN)   != 0) && self->v.ammo_bullets >= 1;
        default: return 0;
    }
}

static int wcycle_current_index(void) {
    edict_t *self = g->self;
    int w2 = (int)self->v.weapon2;
    int w  = (int)self->v.weapon;
    if (w2 != 0) {
        for (int i = 0; i < WCYCLE_COUNT; i++)
            if (WCYCLE[i].is_phase6 && WCYCLE[i].flag == w2) return i;
    }
    if (w != 0) {
        for (int i = 0; i < WCYCLE_COUNT; i++)
            if (!WCYCLE[i].is_phase6 && WCYCLE[i].flag == w) return i;
    }
    return -1;
}

static void wcycle_select(int idx) {
    edict_t *self = g->self;
    if (WCYCLE[idx].is_phase6) {
        self->v.weapon  = 0;
        self->v.weapon2 = (float)WCYCLE[idx].flag;
    } else {
        self->v.weapon  = (float)WCYCLE[idx].flag;
        self->v.weapon2 = 0;
    }
    W_SetCurrentAmmo();
}

static void CycleWeaponCommand(void) {
    g->self->v.impulse = 0;
    int cur = wcycle_current_index();
    int start = (cur < 0) ? -1 : cur;
    for (int i = 1; i <= WCYCLE_COUNT; i++) {
        int next = (start + i + WCYCLE_COUNT) % WCYCLE_COUNT;
        if (wcycle_usable(next)) { wcycle_select(next); return; }
    }
}

static void CycleWeaponReverseCommand(void) {
    g->self->v.impulse = 0;
    int cur = wcycle_current_index();
    int start = (cur < 0) ? WCYCLE_COUNT : cur;
    for (int i = 1; i <= WCYCLE_COUNT; i++) {
        int next = (start - i + WCYCLE_COUNT) % WCYCLE_COUNT;
        if (wcycle_usable(next)) { wcycle_select(next); return; }
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
    if (imp == 12)  CycleWeaponReverseCommand();
    if (imp >= 30 && imp <= 41) Phase6_ChangeWeapon(imp);   // Wolf3D + Doom1 roster + F3 fire weapons (40,41)
    if (imp == 100) Phase6_CheatGiveAll();
    if (imp == 210) Fire_IgniteTraced(self);   // debug: ignite entity under crosshair
    if (imp == 211) Fire_OilTraced(self);      // debug: deposit oil at crosshair
    if (imp == 212) Fire_GiveWeapons(self);    // M8/F3: grant fire weapons + cells
    if (imp == 213) Flammables_DebugSpawnBarrel(self);   // M8/F4 debug: oil barrel ahead
    if (imp == 214) Flammables_DebugSpawnBreakable(self);   // M8/F4 debug: breakable ahead
    if (imp == 215) Flammables_DebugToggleNearestTorch(self);   // M8/F4 debug: toggle nearest torch
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

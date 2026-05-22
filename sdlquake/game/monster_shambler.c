// monster_shambler.c -- Shambler (shambler.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <stddef.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_stand(edict_t *self);
extern void ai_walk(float dist);
extern void ai_run(float dist);
extern void ai_face(void);
extern void ai_charge(float dist);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void ThrowHead(const char *model, float damage);
extern void ThrowGib(const char *model, float damage);
extern void Corpse_LayProne(edict_t *self);
extern void walkmonster_start(edict_t *self);
extern void SpawnMeatSpray(vec3_t org, vec3_t vel);
extern void SUB_Remove(edict_t *e);
extern int CanDamage(edict_t *targ, edict_t *inflictor);

// Frame indices (sequential from $frame directives)
// stand1-17 (0-16), walk1-12 (17-28), run1-6 (29-34),
// smash1-12 (35-46), swingr1-9 (47-55), swingl1-9 (56-64),
// magic1-12 (65-76), pain1-6 (77-82), death1-11 (83-93)
enum {
    SM_STAND1=0,SM_STAND2,SM_STAND3,SM_STAND4,SM_STAND5,SM_STAND6,
    SM_STAND7,SM_STAND8,SM_STAND9,SM_STAND10,SM_STAND11,SM_STAND12,
    SM_STAND13,SM_STAND14,SM_STAND15,SM_STAND16,SM_STAND17,
    SM_WALK1,SM_WALK2,SM_WALK3,SM_WALK4,SM_WALK5,SM_WALK6,
    SM_WALK7,SM_WALK8,SM_WALK9,SM_WALK10,SM_WALK11,SM_WALK12,
    SM_RUN1,SM_RUN2,SM_RUN3,SM_RUN4,SM_RUN5,SM_RUN6,
    SM_SMASH1,SM_SMASH2,SM_SMASH3,SM_SMASH4,SM_SMASH5,SM_SMASH6,
    SM_SMASH7,SM_SMASH8,SM_SMASH9,SM_SMASH10,SM_SMASH11,SM_SMASH12,
    SM_SWINGR1,SM_SWINGR2,SM_SWINGR3,SM_SWINGR4,SM_SWINGR5,
    SM_SWINGR6,SM_SWINGR7,SM_SWINGR8,SM_SWINGR9,
    SM_SWINGL1,SM_SWINGL2,SM_SWINGL3,SM_SWINGL4,SM_SWINGL5,
    SM_SWINGL6,SM_SWINGL7,SM_SWINGL8,SM_SWINGL9,
    SM_MAGIC1,SM_MAGIC2,SM_MAGIC3,SM_MAGIC4,SM_MAGIC5,
    SM_MAGIC6,SM_MAGIC7,SM_MAGIC8,SM_MAGIC9,SM_MAGIC10,SM_MAGIC11,SM_MAGIC12,
    SM_PAIN1,SM_PAIN2,SM_PAIN3,SM_PAIN4,SM_PAIN5,SM_PAIN6,
    SM_DEATH1,SM_DEATH2,SM_DEATH3,SM_DEATH4,SM_DEATH5,SM_DEATH6,
    SM_DEATH7,SM_DEATH8,SM_DEATH9,SM_DEATH10,SM_DEATH11
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

static float crandom(void) { return eng->Random() * 2.0f - 1.0f; }

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void sham_stand1(edict_t *e),sham_stand2(edict_t *e),sham_stand3(edict_t *e),
            sham_stand4(edict_t *e),sham_stand5(edict_t *e),sham_stand6(edict_t *e),
            sham_stand7(edict_t *e),sham_stand8(edict_t *e),sham_stand9(edict_t *e),
            sham_stand10(edict_t *e),sham_stand11(edict_t *e),sham_stand12(edict_t *e),
            sham_stand13(edict_t *e),sham_stand14(edict_t *e),sham_stand15(edict_t *e),
            sham_stand16(edict_t *e),sham_stand17(edict_t *e);
static void sham_walk1(edict_t *e),sham_walk2(edict_t *e),sham_walk3(edict_t *e),
            sham_walk4(edict_t *e),sham_walk5(edict_t *e),sham_walk6(edict_t *e),
            sham_walk7(edict_t *e),sham_walk8(edict_t *e),sham_walk9(edict_t *e),
            sham_walk10(edict_t *e),sham_walk11(edict_t *e),sham_walk12(edict_t *e);
static void sham_run1(edict_t *e),sham_run2(edict_t *e),sham_run3(edict_t *e),
            sham_run4(edict_t *e),sham_run5(edict_t *e),sham_run6(edict_t *e);
static void sham_smash1(edict_t *e),sham_smash2(edict_t *e),sham_smash3(edict_t *e),
            sham_smash4(edict_t *e),sham_smash5(edict_t *e),sham_smash6(edict_t *e),
            sham_smash7(edict_t *e),sham_smash8(edict_t *e),sham_smash9(edict_t *e),
            sham_smash10(edict_t *e),sham_smash11(edict_t *e),sham_smash12(edict_t *e);
static void sham_swingr1(edict_t *e),sham_swingr2(edict_t *e),sham_swingr3(edict_t *e),
            sham_swingr4(edict_t *e),sham_swingr5(edict_t *e),sham_swingr6(edict_t *e),
            sham_swingr7(edict_t *e),sham_swingr8(edict_t *e),sham_swingr9(edict_t *e);
static void sham_swingl1(edict_t *e),sham_swingl2(edict_t *e),sham_swingl3(edict_t *e),
            sham_swingl4(edict_t *e),sham_swingl5(edict_t *e),sham_swingl6(edict_t *e),
            sham_swingl7(edict_t *e),sham_swingl8(edict_t *e),sham_swingl9(edict_t *e);
static void sham_magic1(edict_t *e),sham_magic2(edict_t *e),sham_magic3(edict_t *e),
            sham_magic4(edict_t *e),sham_magic5(edict_t *e),sham_magic6(edict_t *e),
            sham_magic9(edict_t *e),sham_magic10(edict_t *e),sham_magic11(edict_t *e),
            sham_magic12(edict_t *e);
static void sham_pain1(edict_t *e),sham_pain2(edict_t *e),sham_pain3(edict_t *e),
            sham_pain4(edict_t *e),sham_pain5(edict_t *e),sham_pain6(edict_t *e);
static void sham_death1(edict_t *e),sham_death2(edict_t *e),sham_death3(edict_t *e),
            sham_death4(edict_t *e),sham_death5(edict_t *e),sham_death6(edict_t *e),
            sham_death7(edict_t *e),sham_death8(edict_t *e),sham_death9(edict_t *e),
            sham_death10(edict_t *e),sham_death11(edict_t *e);

// ---------------------------------------------------------------------------
// Stand
// ---------------------------------------------------------------------------
static void sham_stand1(edict_t *e)  { FRAME(e,SM_STAND1, sham_stand2);  ai_stand(e); }
static void sham_stand2(edict_t *e)  { FRAME(e,SM_STAND2, sham_stand3);  ai_stand(e); }
static void sham_stand3(edict_t *e)  { FRAME(e,SM_STAND3, sham_stand4);  ai_stand(e); }
static void sham_stand4(edict_t *e)  { FRAME(e,SM_STAND4, sham_stand5);  ai_stand(e); }
static void sham_stand5(edict_t *e)  { FRAME(e,SM_STAND5, sham_stand6);  ai_stand(e); }
static void sham_stand6(edict_t *e)  { FRAME(e,SM_STAND6, sham_stand7);  ai_stand(e); }
static void sham_stand7(edict_t *e)  { FRAME(e,SM_STAND7, sham_stand8);  ai_stand(e); }
static void sham_stand8(edict_t *e)  { FRAME(e,SM_STAND8, sham_stand9);  ai_stand(e); }
static void sham_stand9(edict_t *e)  { FRAME(e,SM_STAND9, sham_stand10); ai_stand(e); }
static void sham_stand10(edict_t *e) { FRAME(e,SM_STAND10,sham_stand11); ai_stand(e); }
static void sham_stand11(edict_t *e) { FRAME(e,SM_STAND11,sham_stand12); ai_stand(e); }
static void sham_stand12(edict_t *e) { FRAME(e,SM_STAND12,sham_stand13); ai_stand(e); }
static void sham_stand13(edict_t *e) { FRAME(e,SM_STAND13,sham_stand14); ai_stand(e); }
static void sham_stand14(edict_t *e) { FRAME(e,SM_STAND14,sham_stand15); ai_stand(e); }
static void sham_stand15(edict_t *e) { FRAME(e,SM_STAND15,sham_stand16); ai_stand(e); }
static void sham_stand16(edict_t *e) { FRAME(e,SM_STAND16,sham_stand17); ai_stand(e); }
static void sham_stand17(edict_t *e) { FRAME(e,SM_STAND17,sham_stand1);  ai_stand(e); }

// ---------------------------------------------------------------------------
// Walk
// ---------------------------------------------------------------------------
static void sham_walk1(edict_t *e)  { FRAME(e,SM_WALK1, sham_walk2);  ai_walk(10); }
static void sham_walk2(edict_t *e)  { FRAME(e,SM_WALK2, sham_walk3);  ai_walk(9); }
static void sham_walk3(edict_t *e)  { FRAME(e,SM_WALK3, sham_walk4);  ai_walk(9); }
static void sham_walk4(edict_t *e)  { FRAME(e,SM_WALK4, sham_walk5);  ai_walk(5); }
static void sham_walk5(edict_t *e)  { FRAME(e,SM_WALK5, sham_walk6);  ai_walk(6); }
static void sham_walk6(edict_t *e)  { FRAME(e,SM_WALK6, sham_walk7);  ai_walk(12); }
static void sham_walk7(edict_t *e)  { FRAME(e,SM_WALK7, sham_walk8);  ai_walk(8); }
static void sham_walk8(edict_t *e)  { FRAME(e,SM_WALK8, sham_walk9);  ai_walk(3); }
static void sham_walk9(edict_t *e)  { FRAME(e,SM_WALK9, sham_walk10); ai_walk(13); }
static void sham_walk10(edict_t *e) { FRAME(e,SM_WALK10,sham_walk11); ai_walk(9); }
static void sham_walk11(edict_t *e) { FRAME(e,SM_WALK11,sham_walk12); ai_walk(7); }
static void sham_walk12(edict_t *e) {
    FRAME(e,SM_WALK12,sham_walk1); ai_walk(7);
    if (eng->Random() > 0.8f) eng->SV_StartSound(e, CHAN_VOICE, "shambler/sidle.wav", 1, ATTN_IDLE);
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------
static void sham_run1(edict_t *e) { FRAME(e,SM_RUN1,sham_run2); ai_run(20); }
static void sham_run2(edict_t *e) { FRAME(e,SM_RUN2,sham_run3); ai_run(24); }
static void sham_run3(edict_t *e) { FRAME(e,SM_RUN3,sham_run4); ai_run(20); }
static void sham_run4(edict_t *e) { FRAME(e,SM_RUN4,sham_run5); ai_run(20); }
static void sham_run5(edict_t *e) { FRAME(e,SM_RUN5,sham_run6); ai_run(24); }
static void sham_run6(edict_t *e) {
    FRAME(e,SM_RUN6,sham_run1); ai_run(20);
    if (eng->Random() > 0.8f) eng->SV_StartSound(e, CHAN_VOICE, "shambler/sidle.wav", 1, ATTN_IDLE);
}

// ---------------------------------------------------------------------------
// ShamClaw
// ---------------------------------------------------------------------------
static void ShamClaw(edict_t *e, float side) {
    if (!e->v.enemy) return;
    ai_charge(10);
    float dx = e->v.enemy->v.origin[0]-e->v.origin[0];
    float dy = e->v.enemy->v.origin[1]-e->v.origin[1];
    float dz = e->v.enemy->v.origin[2]-e->v.origin[2];
    float dist2 = dx*dx+dy*dy+dz*dz;
    if (dist2 > 100.0f*100.0f) return;
    float ldmg = (eng->Random()+eng->Random()+eng->Random()) * 20.0f;
    T_Damage(e->v.enemy, e, e, ldmg);
    eng->SV_StartSound(e, CHAN_VOICE, "shambler/smack.wav", 1, ATTN_NORM);
    if (side != 0.0f) {
        eng->MakeVectors(e->v.angles);
        vec3_t org, vel;
        org[0] = e->v.origin[0] + g->v_forward[0]*16.0f;
        org[1] = e->v.origin[1] + g->v_forward[1]*16.0f;
        org[2] = e->v.origin[2] + g->v_forward[2]*16.0f;
        vel[0] = side * g->v_right[0];
        vel[1] = side * g->v_right[1];
        vel[2] = side * g->v_right[2];
        SpawnMeatSpray(org, vel);
    }
}

// ---------------------------------------------------------------------------
// Smash
// ---------------------------------------------------------------------------
static void sham_smash1(edict_t *e) {
    FRAME(e,SM_SMASH1,sham_smash2);
    eng->SV_StartSound(e, CHAN_VOICE, "shambler/melee1.wav", 1, ATTN_NORM);
    ai_charge(2);
}
static void sham_smash2(edict_t *e)  { FRAME(e,SM_SMASH2, sham_smash3);  ai_charge(6); }
static void sham_smash3(edict_t *e)  { FRAME(e,SM_SMASH3, sham_smash4);  ai_charge(6); }
static void sham_smash4(edict_t *e)  { FRAME(e,SM_SMASH4, sham_smash5);  ai_charge(5); }
static void sham_smash5(edict_t *e)  { FRAME(e,SM_SMASH5, sham_smash6);  ai_charge(4); }
static void sham_smash6(edict_t *e)  { FRAME(e,SM_SMASH6, sham_smash7);  ai_charge(1); }
static void sham_smash7(edict_t *e)  { FRAME(e,SM_SMASH7, sham_smash8);  ai_charge(0); }
static void sham_smash8(edict_t *e)  { FRAME(e,SM_SMASH8, sham_smash9);  ai_charge(0); }
static void sham_smash9(edict_t *e)  { FRAME(e,SM_SMASH9, sham_smash10); ai_charge(0); }
static void sham_smash10(edict_t *e) {
    FRAME(e,SM_SMASH10,sham_smash11);
    if (!e->v.enemy) return;
    ai_charge(0);
    float dx = e->v.enemy->v.origin[0]-e->v.origin[0];
    float dy = e->v.enemy->v.origin[1]-e->v.origin[1];
    float dz = e->v.enemy->v.origin[2]-e->v.origin[2];
    float dist2 = dx*dx+dy*dy+dz*dz;
    if (dist2 > 100.0f*100.0f) return;
    if (!CanDamage(e->v.enemy, e)) return;
    float ldmg = (eng->Random()+eng->Random()+eng->Random()) * 40.0f;
    T_Damage(e->v.enemy, e, e, ldmg);
    eng->SV_StartSound(e, CHAN_VOICE, "shambler/smack.wav", 1, ATTN_NORM);
    eng->MakeVectors(e->v.angles);
    vec3_t org, vel;
    org[0] = e->v.origin[0] + g->v_forward[0]*16.0f;
    org[1] = e->v.origin[1] + g->v_forward[1]*16.0f;
    org[2] = e->v.origin[2] + g->v_forward[2]*16.0f;
    vel[0] = crandom()*100.0f*g->v_right[0];
    vel[1] = crandom()*100.0f*g->v_right[1];
    vel[2] = crandom()*100.0f*g->v_right[2];
    SpawnMeatSpray(org, vel);
    vec3_t vel2;
    vel2[0] = crandom()*100.0f*g->v_right[0];
    vel2[1] = crandom()*100.0f*g->v_right[1];
    vel2[2] = crandom()*100.0f*g->v_right[2];
    SpawnMeatSpray(org, vel2);
}
static void sham_smash11(edict_t *e) { FRAME(e,SM_SMASH11,sham_smash12); ai_charge(5); }
static void sham_smash12(edict_t *e) { FRAME(e,SM_SMASH12,sham_run1);    ai_charge(4); }

// ---------------------------------------------------------------------------
// Swing Left
// ---------------------------------------------------------------------------
static void sham_swingl1(edict_t *e) {
    FRAME(e,SM_SWINGL1,sham_swingl2);
    eng->SV_StartSound(e, CHAN_VOICE, "shambler/melee2.wav", 1, ATTN_NORM);
    ai_charge(5);
}
static void sham_swingl2(edict_t *e) { FRAME(e,SM_SWINGL2,sham_swingl3); ai_charge(3); }
static void sham_swingl3(edict_t *e) { FRAME(e,SM_SWINGL3,sham_swingl4); ai_charge(7); }
static void sham_swingl4(edict_t *e) { FRAME(e,SM_SWINGL4,sham_swingl5); ai_charge(3); }
static void sham_swingl5(edict_t *e) { FRAME(e,SM_SWINGL5,sham_swingl6); ai_charge(7); }
static void sham_swingl6(edict_t *e) { FRAME(e,SM_SWINGL6,sham_swingl7); ai_charge(9); }
static void sham_swingl7(edict_t *e) { FRAME(e,SM_SWINGL7,sham_swingl8); ai_charge(5); ShamClaw(e, 250.0f); }
static void sham_swingl8(edict_t *e) { FRAME(e,SM_SWINGL8,sham_swingl9); ai_charge(4); }
static void sham_swingl9(edict_t *e) {
    FRAME(e,SM_SWINGL9,sham_run1); ai_charge(8);
    if (eng->Random() < 0.5f) e->v.think = sham_swingr1;
}

// ---------------------------------------------------------------------------
// Swing Right
// ---------------------------------------------------------------------------
static void sham_swingr1(edict_t *e) {
    FRAME(e,SM_SWINGR1,sham_swingr2);
    eng->SV_StartSound(e, CHAN_VOICE, "shambler/melee1.wav", 1, ATTN_NORM);
    ai_charge(1);
}
static void sham_swingr2(edict_t *e) { FRAME(e,SM_SWINGR2,sham_swingr3); ai_charge(8); }
static void sham_swingr3(edict_t *e) { FRAME(e,SM_SWINGR3,sham_swingr4); ai_charge(14); }
static void sham_swingr4(edict_t *e) { FRAME(e,SM_SWINGR4,sham_swingr5); ai_charge(7); }
static void sham_swingr5(edict_t *e) { FRAME(e,SM_SWINGR5,sham_swingr6); ai_charge(3); }
static void sham_swingr6(edict_t *e) { FRAME(e,SM_SWINGR6,sham_swingr7); ai_charge(6); }
static void sham_swingr7(edict_t *e) { FRAME(e,SM_SWINGR7,sham_swingr8); ai_charge(6); ShamClaw(e, -250.0f); }
static void sham_swingr8(edict_t *e) { FRAME(e,SM_SWINGR8,sham_swingr9); ai_charge(3); }
static void sham_swingr9(edict_t *e) {
    FRAME(e,SM_SWINGR9,sham_run1); ai_charge(1); ai_charge(10);
    if (eng->Random() < 0.5f) e->v.think = sham_swingl1;
}

static void sham_melee(edict_t *e) {
    float chance = eng->Random();
    if (chance > 0.6f || e->v.health == 600.0f)
        sham_smash1(e);
    else if (chance > 0.3f)
        sham_swingr1(e);
    else
        sham_swingl1(e);
}

// ---------------------------------------------------------------------------
// Lightning (local CastLightning — LightningDamage is static in weapons.c)
// Shambler's lightning does traceline + TE_LIGHTNING1 + 10 damage per hit entity.
// We replicate T_RadiusDamage-style along the beam, simplified to direct damage.
// ---------------------------------------------------------------------------
static void CastLightning(edict_t *e) {
    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
    ai_face();

    vec3_t org;
    org[0] = e->v.origin[0];
    org[1] = e->v.origin[1];
    org[2] = e->v.origin[2] + 40.0f;

    if (!e->v.enemy) return;
    vec3_t dir;
    dir[0] = (e->v.enemy->v.origin[0]) - org[0];
    dir[1] = (e->v.enemy->v.origin[1]) - org[1];
    dir[2] = (e->v.enemy->v.origin[2] + 16.0f) - org[2];
    vec3_t ndir;
    eng->VectorNormalize(dir, ndir);

    vec3_t end;
    end[0] = e->v.origin[0] + ndir[0]*600.0f;
    end[1] = e->v.origin[1] + ndir[1]*600.0f;
    end[2] = e->v.origin[2] + ndir[2]*600.0f;

    eng->SV_Traceline(org, end, 0 /*nomonsters=FALSE*/, e);

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_LIGHTNING1);
    eng->MSG_WriteEntity(MSG_BROADCAST, e);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, org[2]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, g->trace_endpos[2]);

    /* Damage entities along the beam (simplified: just damage the trace_ent) */
    if (g->trace_ent && g->trace_ent != g->world && g->trace_ent->v.takedamage)
        T_Damage(g->trace_ent, e, e, 10.0f);
}

// ---------------------------------------------------------------------------
// Magic (lightning attack)
// ---------------------------------------------------------------------------
/* light entity stored in e->v.owner during magic animation */
static void sham_magic1(edict_t *e) {
    FRAME(e,SM_MAGIC1,sham_magic2); ai_face();
    eng->SV_StartSound(e, CHAN_WEAPON, "shambler/sattck1.wav", 1, ATTN_NORM);
}
static void sham_magic2(edict_t *e) { FRAME(e,SM_MAGIC2,sham_magic3); ai_face(); }
static void sham_magic3(edict_t *e) {
    FRAME(e,SM_MAGIC3,sham_magic4);
    e->v.nextthink += 0.2f; /* extra delay */
    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
    ai_face();
    /* spawn light entity */
    edict_t *o = eng->ED_Alloc();
    eng->SV_SetModel(o, "progs/s_light.mdl");
    eng->SV_SetOrigin(o, e->v.origin);
    o->v.angles[0] = e->v.angles[0];
    o->v.angles[1] = e->v.angles[1];
    o->v.angles[2] = e->v.angles[2];
    o->v.nextthink = g->time + 0.7f;
    o->v.think = SUB_Remove;
    e->v.owner = o;
}
static void sham_magic4(edict_t *e) {
    FRAME(e,SM_MAGIC4,sham_magic5);
    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
    if (e->v.owner) e->v.owner->v.frame = 1;
}
static void sham_magic5(edict_t *e) {
    FRAME(e,SM_MAGIC5,sham_magic6);
    e->v.effects = (float)((int)e->v.effects | EF_MUZZLEFLASH);
    if (e->v.owner) e->v.owner->v.frame = 2;
}
static void sham_magic6(edict_t *e) {
    FRAME(e,SM_MAGIC6,sham_magic9);
    if (e->v.owner) { eng->ED_Free(e->v.owner); e->v.owner = NULL; }
    CastLightning(e);
    eng->SV_StartSound(e, CHAN_WEAPON, "shambler/sboom.wav", 1, ATTN_NORM);
}
static void sham_magic9(edict_t *e)  { FRAME(e,SM_MAGIC9, sham_magic10); CastLightning(e); }
static void sham_magic10(edict_t *e) { FRAME(e,SM_MAGIC10,sham_magic11); CastLightning(e); }
static void sham_magic11(edict_t *e) {
    FRAME(e,SM_MAGIC11,sham_magic12);
    if (eng->Cvar_VariableValue("skill") >= 3.0f) CastLightning(e);
}
static void sham_magic12(edict_t *e) { FRAME(e,SM_MAGIC12,sham_run1); (void)e; }

// ---------------------------------------------------------------------------
// Pain
// ---------------------------------------------------------------------------
static void sham_pain1(edict_t *e) { FRAME(e,SM_PAIN1,sham_pain2); }
static void sham_pain2(edict_t *e) { FRAME(e,SM_PAIN2,sham_pain3); }
static void sham_pain3(edict_t *e) { FRAME(e,SM_PAIN3,sham_pain4); }
static void sham_pain4(edict_t *e) { FRAME(e,SM_PAIN4,sham_pain5); }
static void sham_pain5(edict_t *e) { FRAME(e,SM_PAIN5,sham_pain6); }
static void sham_pain6(edict_t *e) { FRAME(e,SM_PAIN6,sham_run1);  }

static void sham_pain(edict_t *self, edict_t *attacker, float damage) {
    (void)attacker;
    eng->SV_StartSound(self, CHAN_VOICE, "shambler/shurt2.wav", 1, ATTN_NORM);
    if (self->v.health <= 0.0f) return;
    if (eng->Random() * 400.0f > damage) return;
    if (self->v.pain_finished > g->time) return;
    self->v.pain_finished = g->time + 2.0f;
    sham_pain1(self);
}

// ---------------------------------------------------------------------------
// Death
// ---------------------------------------------------------------------------
static void sham_death1(edict_t *e)  { FRAME(e,SM_DEATH1, sham_death2);  }
static void sham_death2(edict_t *e)  { FRAME(e,SM_DEATH2, sham_death3);  }
static void sham_death3(edict_t *e)  { FRAME(e,SM_DEATH3, sham_death4);  Corpse_LayProne(e); }
static void sham_death4(edict_t *e)  { FRAME(e,SM_DEATH4, sham_death5);  }
static void sham_death5(edict_t *e)  { FRAME(e,SM_DEATH5, sham_death6);  }
static void sham_death6(edict_t *e)  { FRAME(e,SM_DEATH6, sham_death7);  }
static void sham_death7(edict_t *e)  { FRAME(e,SM_DEATH7, sham_death8);  }
static void sham_death8(edict_t *e)  { FRAME(e,SM_DEATH8, sham_death9);  }
static void sham_death9(edict_t *e)  { FRAME(e,SM_DEATH9, sham_death10); }
static void sham_death10(edict_t *e) { FRAME(e,SM_DEATH10,sham_death11); }
static void sham_death11(edict_t *e) { FRAME(e,SM_DEATH11,sham_death11); } /* loop */

static void sham_die(edict_t *self) {
    if (self->v.health < -60.0f) {
        eng->SV_StartSound(self, CHAN_VOICE, "player/udeath.wav", 1, ATTN_NORM);
        ThrowHead("progs/h_shams.mdl", self->v.health);
        ThrowGib("progs/gib1.mdl", self->v.health);
        ThrowGib("progs/gib2.mdl", self->v.health);
        ThrowGib("progs/gib3.mdl", self->v.health);
        return;
    }
    eng->SV_StartSound(self, CHAN_VOICE, "shambler/sdeath.wav", 1, ATTN_NORM);
    sham_death1(self);
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_shambler(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }

    eng->PrecacheModel("progs/shambler.mdl");
    eng->PrecacheModel("progs/s_light.mdl");
    eng->PrecacheModel("progs/h_shams.mdl");
    eng->PrecacheModel("progs/bolt.mdl");

    eng->PrecacheSound("shambler/sattck1.wav");
    eng->PrecacheSound("shambler/sboom.wav");
    eng->PrecacheSound("shambler/sdeath.wav");
    eng->PrecacheSound("shambler/shurt2.wav");
    eng->PrecacheSound("shambler/sidle.wav");
    eng->PrecacheSound("shambler/ssight.wav");
    eng->PrecacheSound("shambler/melee1.wav");
    eng->PrecacheSound("shambler/melee2.wav");
    eng->PrecacheSound("shambler/smack.wav");

    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/shambler.mdl");
    vec3_t mins = {VEC_HULL2_MIN_X, VEC_HULL2_MIN_Y, VEC_HULL2_MIN_Z};
    vec3_t maxs = {VEC_HULL2_MAX_X, VEC_HULL2_MAX_Y, VEC_HULL2_MAX_Z};
    eng->SV_SetSize(e, mins, maxs);
    e->v.health   = 600;

    e->v.th_stand   = sham_stand1;
    e->v.th_walk    = sham_walk1;
    e->v.th_run     = sham_run1;
    e->v.th_die     = sham_die;
    e->v.th_melee   = sham_melee;
    e->v.th_missile = sham_magic1;
    e->v.th_pain    = sham_pain;

    walkmonster_start(e);
}

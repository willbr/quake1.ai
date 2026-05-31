// monster_boss.c -- Chthon, the Boss (boss.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ai_face(void);
extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);
extern void SUB_UseTargets(void);
extern void launch_spike(vec3_t org, vec3_t dir);

// doors.c — needed for lightning_use
extern void door_go_down(edict_t *e);

// Frame indices (sequential from $frame directives)
// rise1-17 (0-16), walk1-31 (17-47), death1-9 (48-56),
// attack1-23 (57-79), shocka1-10 (80-89), shockb1-6 (90-95),
// shockc1-10 (96-105)
enum {
    BS_RISE1=0,BS_RISE2,BS_RISE3,BS_RISE4,BS_RISE5,BS_RISE6,
    BS_RISE7,BS_RISE8,BS_RISE9,BS_RISE10,BS_RISE11,BS_RISE12,
    BS_RISE13,BS_RISE14,BS_RISE15,BS_RISE16,BS_RISE17,
    BS_WALK1,BS_WALK2,BS_WALK3,BS_WALK4,BS_WALK5,BS_WALK6,BS_WALK7,BS_WALK8,
    BS_WALK9,BS_WALK10,BS_WALK11,BS_WALK12,BS_WALK13,BS_WALK14,BS_WALK15,
    BS_WALK16,BS_WALK17,BS_WALK18,BS_WALK19,BS_WALK20,BS_WALK21,BS_WALK22,
    BS_WALK23,BS_WALK24,BS_WALK25,BS_WALK26,BS_WALK27,BS_WALK28,BS_WALK29,
    BS_WALK30,BS_WALK31,
    BS_DEATH1,BS_DEATH2,BS_DEATH3,BS_DEATH4,BS_DEATH5,
    BS_DEATH6,BS_DEATH7,BS_DEATH8,BS_DEATH9,
    BS_ATK1,BS_ATK2,BS_ATK3,BS_ATK4,BS_ATK5,BS_ATK6,BS_ATK7,BS_ATK8,
    BS_ATK9,BS_ATK10,BS_ATK11,BS_ATK12,BS_ATK13,BS_ATK14,BS_ATK15,
    BS_ATK16,BS_ATK17,BS_ATK18,BS_ATK19,BS_ATK20,BS_ATK21,BS_ATK22,BS_ATK23,
    BS_SHOCKA1,BS_SHOCKA2,BS_SHOCKA3,BS_SHOCKA4,BS_SHOCKA5,
    BS_SHOCKA6,BS_SHOCKA7,BS_SHOCKA8,BS_SHOCKA9,BS_SHOCKA10,
    BS_SHOCKB1,BS_SHOCKB2,BS_SHOCKB3,BS_SHOCKB4,BS_SHOCKB5,BS_SHOCKB6,
    BS_SHOCKC1,BS_SHOCKC2,BS_SHOCKC3,BS_SHOCKC4,BS_SHOCKC5,
    BS_SHOCKC6,BS_SHOCKC7,BS_SHOCKC8,BS_SHOCKC9,BS_SHOCKC10
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void boss_rise1(edict_t *e);
static void boss_rise2(edict_t *e),boss_rise3(edict_t *e),boss_rise4(edict_t *e),
            boss_rise5(edict_t *e),boss_rise6(edict_t *e),boss_rise7(edict_t *e),
            boss_rise8(edict_t *e),boss_rise9(edict_t *e),boss_rise10(edict_t *e),
            boss_rise11(edict_t *e),boss_rise12(edict_t *e),boss_rise13(edict_t *e),
            boss_rise14(edict_t *e),boss_rise15(edict_t *e),boss_rise16(edict_t *e),
            boss_rise17(edict_t *e);
static void boss_idle1(edict_t *e);
static void boss_missile1(edict_t *e);
static void boss_shocka1(edict_t *e),boss_shockb1(edict_t *e),boss_shockc1(edict_t *e);
static void boss_death1(edict_t *e);

// ---------------------------------------------------------------------------
// boss_face: look for enemy / switch players in multiplayer
// ---------------------------------------------------------------------------
static void boss_face(edict_t *e) {
    if (!e->v.enemy || e->v.enemy->v.health <= 0 || eng->Random() < 0.02f) {
        edict_t *pl = eng->ED_Find(e->v.enemy, "classname", "player");
        if (pl == g->world) pl = eng->ED_Find(g->world, "classname", "player");
        if (pl != g->world) e->v.enemy = pl;
    }
    g->self = e;
    ai_face();
}

// ---------------------------------------------------------------------------
// boss_missile (helper): fire a lavaball from offset p
// T_MissileTouch is static in weapons.c — use T_RadiusDamage locally.
// ---------------------------------------------------------------------------
static void boss_missile_touch(edict_t *self, edict_t *other) {
    if (other == self->v.owner) return;
    T_RadiusDamage(self, self->v.owner, 120.0f, g->world);
    eng->SV_StartSound(self, CHAN_WEAPON, "weapons/r_exp3.wav", 1, ATTN_NORM);
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_EXPLOSION);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);
    eng->ED_Free(self);
}

static void boss_fire_missile(edict_t *e, float px, float py, float pz) {
    if (!e->v.enemy) return;

    vec3_t diff;
    diff[0] = e->v.enemy->v.origin[0] - e->v.origin[0];
    diff[1] = e->v.enemy->v.origin[1] - e->v.origin[1];
    diff[2] = e->v.enemy->v.origin[2] - e->v.origin[2];
    vec3_t offang;
    eng->VectorToAngles(diff, offang);
    eng->MakeVectors(offang);

    vec3_t org;
    org[0] = e->v.origin[0] + px*g->v_forward[0] + py*g->v_right[0];
    org[1] = e->v.origin[1] + px*g->v_forward[1] + py*g->v_right[1];
    org[2] = e->v.origin[2] + px*g->v_forward[2] + py*g->v_right[2] + pz;

    vec3_t d;
    float skill = eng->Cvar_VariableValue("skill");
    if (skill > 1.0f) {
        float t = eng->VectorLength(diff) / 300.0f;
        /* approximate t using VectorLength of diff */
        vec3_t ev; ev[0]=e->v.enemy->v.velocity[0]; ev[1]=e->v.enemy->v.velocity[1]; ev[2]=0.0f;
        d[0] = e->v.enemy->v.origin[0] + t * ev[0];
        d[1] = e->v.enemy->v.origin[1] + t * ev[1];
        d[2] = e->v.enemy->v.origin[2] + t * ev[2];
    } else {
        d[0] = e->v.enemy->v.origin[0];
        d[1] = e->v.enemy->v.origin[1];
        d[2] = e->v.enemy->v.origin[2];
    }

    vec3_t vec;
    vec[0] = d[0]-org[0]; vec[1] = d[1]-org[1]; vec[2] = d[2]-org[2];
    vec3_t nvec;
    eng->VectorNormalize(vec, nvec);

    launch_spike(org, nvec);
    edict_t *mis = g->newmis;
    if (!mis) return;
    eng->SV_SetModel(mis, "progs/lavaball.mdl");
    mis->v.classname = "boss_lavaball";   // override launch_spike's "spike"
    mis->v.avelocity[0] = 200.0f; mis->v.avelocity[1] = 100.0f; mis->v.avelocity[2] = 300.0f;
    vec3_t zero = {0,0,0};
    eng->SV_SetSize(mis, zero, zero);
    mis->v.velocity[0] = nvec[0]*300.0f;
    mis->v.velocity[1] = nvec[1]*300.0f;
    mis->v.velocity[2] = nvec[2]*300.0f;
    mis->v.touch = boss_missile_touch;
    eng->SV_StartSound(e, CHAN_WEAPON, "boss1/throw.wav", 1, ATTN_NORM);

    if (e->v.enemy->v.health <= 0) boss_idle1(e);
}

// ---------------------------------------------------------------------------
// Rise
// ---------------------------------------------------------------------------
static void boss_rise1(edict_t *e) {
    FRAME(e,BS_RISE1,boss_rise2);
    eng->SV_StartSound(e, CHAN_WEAPON, "boss1/out1.wav", 1, ATTN_NORM);
}
static void boss_rise2(edict_t *e) {
    FRAME(e,BS_RISE2,boss_rise3);
    eng->SV_StartSound(e, CHAN_VOICE, "boss1/sight1.wav", 1, ATTN_NORM);
}
static void boss_rise3(edict_t *e)  { FRAME(e,BS_RISE3, boss_rise4);  }
static void boss_rise4(edict_t *e)  { FRAME(e,BS_RISE4, boss_rise5);  }
static void boss_rise5(edict_t *e)  { FRAME(e,BS_RISE5, boss_rise6);  }
static void boss_rise6(edict_t *e)  { FRAME(e,BS_RISE6, boss_rise7);  }
static void boss_rise7(edict_t *e)  { FRAME(e,BS_RISE7, boss_rise8);  }
static void boss_rise8(edict_t *e)  { FRAME(e,BS_RISE8, boss_rise9);  }
static void boss_rise9(edict_t *e)  { FRAME(e,BS_RISE9, boss_rise10); }
static void boss_rise10(edict_t *e) { FRAME(e,BS_RISE10,boss_rise11); }
static void boss_rise11(edict_t *e) { FRAME(e,BS_RISE11,boss_rise12); }
static void boss_rise12(edict_t *e) { FRAME(e,BS_RISE12,boss_rise13); }
static void boss_rise13(edict_t *e) { FRAME(e,BS_RISE13,boss_rise14); }
static void boss_rise14(edict_t *e) { FRAME(e,BS_RISE14,boss_rise15); }
static void boss_rise15(edict_t *e) { FRAME(e,BS_RISE15,boss_rise16); }
static void boss_rise16(edict_t *e) { FRAME(e,BS_RISE16,boss_rise17); }
static void boss_rise17(edict_t *e) { FRAME(e,BS_RISE17,boss_missile1); }

// ---------------------------------------------------------------------------
// Idle
// ---------------------------------------------------------------------------
static void boss_idle2(edict_t *e),boss_idle3(edict_t *e),boss_idle4(edict_t *e),
            boss_idle5(edict_t *e),boss_idle6(edict_t *e),boss_idle7(edict_t *e),
            boss_idle8(edict_t *e),boss_idle9(edict_t *e),boss_idle10(edict_t *e),
            boss_idle11(edict_t *e),boss_idle12(edict_t *e),boss_idle13(edict_t *e),
            boss_idle14(edict_t *e),boss_idle15(edict_t *e),boss_idle16(edict_t *e),
            boss_idle17(edict_t *e),boss_idle18(edict_t *e),boss_idle19(edict_t *e),
            boss_idle20(edict_t *e),boss_idle21(edict_t *e),boss_idle22(edict_t *e),
            boss_idle23(edict_t *e),boss_idle24(edict_t *e),boss_idle25(edict_t *e),
            boss_idle26(edict_t *e),boss_idle27(edict_t *e),boss_idle28(edict_t *e),
            boss_idle29(edict_t *e),boss_idle30(edict_t *e),boss_idle31(edict_t *e);

static void boss_idle1(edict_t *e)  { FRAME(e,BS_WALK1, boss_idle2);  }
static void boss_idle2(edict_t *e)  { FRAME(e,BS_WALK2, boss_idle3);  boss_face(e); }
static void boss_idle3(edict_t *e)  { FRAME(e,BS_WALK3, boss_idle4);  boss_face(e); }
static void boss_idle4(edict_t *e)  { FRAME(e,BS_WALK4, boss_idle5);  boss_face(e); }
static void boss_idle5(edict_t *e)  { FRAME(e,BS_WALK5, boss_idle6);  boss_face(e); }
static void boss_idle6(edict_t *e)  { FRAME(e,BS_WALK6, boss_idle7);  boss_face(e); }
static void boss_idle7(edict_t *e)  { FRAME(e,BS_WALK7, boss_idle8);  boss_face(e); }
static void boss_idle8(edict_t *e)  { FRAME(e,BS_WALK8, boss_idle9);  boss_face(e); }
static void boss_idle9(edict_t *e)  { FRAME(e,BS_WALK9, boss_idle10); boss_face(e); }
static void boss_idle10(edict_t *e) { FRAME(e,BS_WALK10,boss_idle11); boss_face(e); }
static void boss_idle11(edict_t *e) { FRAME(e,BS_WALK11,boss_idle12); boss_face(e); }
static void boss_idle12(edict_t *e) { FRAME(e,BS_WALK12,boss_idle13); boss_face(e); }
static void boss_idle13(edict_t *e) { FRAME(e,BS_WALK13,boss_idle14); boss_face(e); }
static void boss_idle14(edict_t *e) { FRAME(e,BS_WALK14,boss_idle15); boss_face(e); }
static void boss_idle15(edict_t *e) { FRAME(e,BS_WALK15,boss_idle16); boss_face(e); }
static void boss_idle16(edict_t *e) { FRAME(e,BS_WALK16,boss_idle17); boss_face(e); }
static void boss_idle17(edict_t *e) { FRAME(e,BS_WALK17,boss_idle18); boss_face(e); }
static void boss_idle18(edict_t *e) { FRAME(e,BS_WALK18,boss_idle19); boss_face(e); }
static void boss_idle19(edict_t *e) { FRAME(e,BS_WALK19,boss_idle20); boss_face(e); }
static void boss_idle20(edict_t *e) { FRAME(e,BS_WALK20,boss_idle21); boss_face(e); }
static void boss_idle21(edict_t *e) { FRAME(e,BS_WALK21,boss_idle22); boss_face(e); }
static void boss_idle22(edict_t *e) { FRAME(e,BS_WALK22,boss_idle23); boss_face(e); }
static void boss_idle23(edict_t *e) { FRAME(e,BS_WALK23,boss_idle24); boss_face(e); }
static void boss_idle24(edict_t *e) { FRAME(e,BS_WALK24,boss_idle25); boss_face(e); }
static void boss_idle25(edict_t *e) { FRAME(e,BS_WALK25,boss_idle26); boss_face(e); }
static void boss_idle26(edict_t *e) { FRAME(e,BS_WALK26,boss_idle27); boss_face(e); }
static void boss_idle27(edict_t *e) { FRAME(e,BS_WALK27,boss_idle28); boss_face(e); }
static void boss_idle28(edict_t *e) { FRAME(e,BS_WALK28,boss_idle29); boss_face(e); }
static void boss_idle29(edict_t *e) { FRAME(e,BS_WALK29,boss_idle30); boss_face(e); }
static void boss_idle30(edict_t *e) { FRAME(e,BS_WALK30,boss_idle31); boss_face(e); }
static void boss_idle31(edict_t *e) { FRAME(e,BS_WALK31,boss_idle1);  boss_face(e); }

// ---------------------------------------------------------------------------
// Missile attack
// ---------------------------------------------------------------------------
static void boss_missile2(edict_t *e),boss_missile3(edict_t *e),boss_missile4(edict_t *e),
            boss_missile5(edict_t *e),boss_missile6(edict_t *e),boss_missile7(edict_t *e),
            boss_missile8(edict_t *e),boss_missile9(edict_t *e),boss_missile10(edict_t *e),
            boss_missile11(edict_t *e),boss_missile12(edict_t *e),boss_missile13(edict_t *e),
            boss_missile14(edict_t *e),boss_missile15(edict_t *e),boss_missile16(edict_t *e),
            boss_missile17(edict_t *e),boss_missile18(edict_t *e),boss_missile19(edict_t *e),
            boss_missile20(edict_t *e),boss_missile21(edict_t *e),boss_missile22(edict_t *e),
            boss_missile23(edict_t *e);

static void boss_missile1(edict_t *e)  { FRAME(e,BS_ATK1, boss_missile2);  boss_face(e); }
static void boss_missile2(edict_t *e)  { FRAME(e,BS_ATK2, boss_missile3);  boss_face(e); }
static void boss_missile3(edict_t *e)  { FRAME(e,BS_ATK3, boss_missile4);  boss_face(e); }
static void boss_missile4(edict_t *e)  { FRAME(e,BS_ATK4, boss_missile5);  boss_face(e); }
static void boss_missile5(edict_t *e)  { FRAME(e,BS_ATK5, boss_missile6);  boss_face(e); }
static void boss_missile6(edict_t *e)  { FRAME(e,BS_ATK6, boss_missile7);  boss_face(e); }
static void boss_missile7(edict_t *e)  { FRAME(e,BS_ATK7, boss_missile8);  boss_face(e); }
static void boss_missile8(edict_t *e)  { FRAME(e,BS_ATK8, boss_missile9);  boss_face(e); }
static void boss_missile9(edict_t *e)  { FRAME(e,BS_ATK9, boss_missile10); boss_fire_missile(e, 100.0f, 100.0f, 200.0f); }
static void boss_missile10(edict_t *e) { FRAME(e,BS_ATK10,boss_missile11); boss_face(e); }
static void boss_missile11(edict_t *e) { FRAME(e,BS_ATK11,boss_missile12); boss_face(e); }
static void boss_missile12(edict_t *e) { FRAME(e,BS_ATK12,boss_missile13); boss_face(e); }
static void boss_missile13(edict_t *e) { FRAME(e,BS_ATK13,boss_missile14); boss_face(e); }
static void boss_missile14(edict_t *e) { FRAME(e,BS_ATK14,boss_missile15); boss_face(e); }
static void boss_missile15(edict_t *e) { FRAME(e,BS_ATK15,boss_missile16); boss_face(e); }
static void boss_missile16(edict_t *e) { FRAME(e,BS_ATK16,boss_missile17); boss_face(e); }
static void boss_missile17(edict_t *e) { FRAME(e,BS_ATK17,boss_missile18); boss_face(e); }
static void boss_missile18(edict_t *e) { FRAME(e,BS_ATK18,boss_missile19); boss_face(e); }
static void boss_missile19(edict_t *e) { FRAME(e,BS_ATK19,boss_missile20); boss_face(e); }
static void boss_missile20(edict_t *e) { FRAME(e,BS_ATK20,boss_missile21); boss_fire_missile(e, 100.0f, -100.0f, 200.0f); }
static void boss_missile21(edict_t *e) { FRAME(e,BS_ATK21,boss_missile22); boss_face(e); }
static void boss_missile22(edict_t *e) { FRAME(e,BS_ATK22,boss_missile23); boss_face(e); }
static void boss_missile23(edict_t *e) { FRAME(e,BS_ATK23,boss_missile1);  boss_face(e); }

// ---------------------------------------------------------------------------
// Shock sequences (triggered by lightning electrodes)
// ---------------------------------------------------------------------------
static void boss_shocka2(edict_t *e),boss_shocka3(edict_t *e),boss_shocka4(edict_t *e),
            boss_shocka5(edict_t *e),boss_shocka6(edict_t *e),boss_shocka7(edict_t *e),
            boss_shocka8(edict_t *e),boss_shocka9(edict_t *e),boss_shocka10(edict_t *e);
static void boss_shockb2(edict_t *e),boss_shockb3(edict_t *e),boss_shockb4(edict_t *e),
            boss_shockb5(edict_t *e),boss_shockb6(edict_t *e),boss_shockb7(edict_t *e),
            boss_shockb8(edict_t *e),boss_shockb9(edict_t *e),boss_shockb10(edict_t *e);
static void boss_shockc2(edict_t *e),boss_shockc3(edict_t *e),boss_shockc4(edict_t *e),
            boss_shockc5(edict_t *e),boss_shockc6(edict_t *e),boss_shockc7(edict_t *e),
            boss_shockc8(edict_t *e),boss_shockc9(edict_t *e),boss_shockc10(edict_t *e);

static void boss_shocka1(edict_t *e)  { FRAME(e,BS_SHOCKA1, boss_shocka2);  }
static void boss_shocka2(edict_t *e)  { FRAME(e,BS_SHOCKA2, boss_shocka3);  }
static void boss_shocka3(edict_t *e)  { FRAME(e,BS_SHOCKA3, boss_shocka4);  }
static void boss_shocka4(edict_t *e)  { FRAME(e,BS_SHOCKA4, boss_shocka5);  }
static void boss_shocka5(edict_t *e)  { FRAME(e,BS_SHOCKA5, boss_shocka6);  }
static void boss_shocka6(edict_t *e)  { FRAME(e,BS_SHOCKA6, boss_shocka7);  }
static void boss_shocka7(edict_t *e)  { FRAME(e,BS_SHOCKA7, boss_shocka8);  }
static void boss_shocka8(edict_t *e)  { FRAME(e,BS_SHOCKA8, boss_shocka9);  }
static void boss_shocka9(edict_t *e)  { FRAME(e,BS_SHOCKA9, boss_shocka10); }
static void boss_shocka10(edict_t *e) { FRAME(e,BS_SHOCKA10,boss_missile1); }

/* shockb7-10 reuse shockb1-4 frame numbers (indices 90-93) */
static void boss_shockb1(edict_t *e)  { FRAME(e,BS_SHOCKB1,boss_shockb2);  }
static void boss_shockb2(edict_t *e)  { FRAME(e,BS_SHOCKB2,boss_shockb3);  }
static void boss_shockb3(edict_t *e)  { FRAME(e,BS_SHOCKB3,boss_shockb4);  }
static void boss_shockb4(edict_t *e)  { FRAME(e,BS_SHOCKB4,boss_shockb5);  }
static void boss_shockb5(edict_t *e)  { FRAME(e,BS_SHOCKB5,boss_shockb6);  }
static void boss_shockb6(edict_t *e)  { FRAME(e,BS_SHOCKB6,boss_shockb7);  }
static void boss_shockb7(edict_t *e)  { FRAME(e,BS_SHOCKB1,boss_shockb8);  } /* reuse shockb1 frame */
static void boss_shockb8(edict_t *e)  { FRAME(e,BS_SHOCKB2,boss_shockb9);  }
static void boss_shockb9(edict_t *e)  { FRAME(e,BS_SHOCKB3,boss_shockb10); }
static void boss_shockb10(edict_t *e) { FRAME(e,BS_SHOCKB4,boss_missile1); }

static void boss_shockc1(edict_t *e)  { FRAME(e,BS_SHOCKC1, boss_shockc2);  }
static void boss_shockc2(edict_t *e)  { FRAME(e,BS_SHOCKC2, boss_shockc3);  }
static void boss_shockc3(edict_t *e)  { FRAME(e,BS_SHOCKC3, boss_shockc4);  }
static void boss_shockc4(edict_t *e)  { FRAME(e,BS_SHOCKC4, boss_shockc5);  }
static void boss_shockc5(edict_t *e)  { FRAME(e,BS_SHOCKC5, boss_shockc6);  }
static void boss_shockc6(edict_t *e)  { FRAME(e,BS_SHOCKC6, boss_shockc7);  }
static void boss_shockc7(edict_t *e)  { FRAME(e,BS_SHOCKC7, boss_shockc8);  }
static void boss_shockc8(edict_t *e)  { FRAME(e,BS_SHOCKC8, boss_shockc9);  }
static void boss_shockc9(edict_t *e)  { FRAME(e,BS_SHOCKC9, boss_shockc10); }
static void boss_shockc10(edict_t *e) { FRAME(e,BS_SHOCKC10,boss_death1);   }

// ---------------------------------------------------------------------------
// Death
// ---------------------------------------------------------------------------
static void boss_death2(edict_t *e),boss_death3(edict_t *e),boss_death4(edict_t *e),
            boss_death5(edict_t *e),boss_death6(edict_t *e),boss_death7(edict_t *e),
            boss_death8(edict_t *e),boss_death9(edict_t *e),boss_death10(edict_t *e);

static void boss_death1(edict_t *e) {
    FRAME(e,BS_DEATH1,boss_death2);
    eng->SV_StartSound(e, CHAN_VOICE, "boss1/death.wav", 1, ATTN_NORM);
}
static void boss_death2(edict_t *e) { FRAME(e,BS_DEATH2,boss_death3); }
static void boss_death3(edict_t *e) { FRAME(e,BS_DEATH3,boss_death4); }
static void boss_death4(edict_t *e) { FRAME(e,BS_DEATH4,boss_death5); }
static void boss_death5(edict_t *e) { FRAME(e,BS_DEATH5,boss_death6); }
static void boss_death6(edict_t *e) { FRAME(e,BS_DEATH6,boss_death7); }
static void boss_death7(edict_t *e) { FRAME(e,BS_DEATH7,boss_death8); }
static void boss_death8(edict_t *e) { FRAME(e,BS_DEATH8,boss_death9); }
static void boss_death9(edict_t *e) {
    FRAME(e,BS_DEATH9,boss_death10);
    eng->SV_StartSound(e, CHAN_BODY, "boss1/out1.wav", 1, ATTN_NORM);
    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_LAVASPLASH);
    eng->MSG_WriteCoord(MSG_BROADCAST, e->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, e->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, e->v.origin[2]);
}
static void boss_death10(edict_t *e) {
    FRAME(e,BS_DEATH9,boss_death10); /* loop on death9 like QC */
    g->killed_monsters++;
    eng->MSG_WriteByte(MSG_ALL, SVC_KILLEDMONSTER);
    g->self = e;
    SUB_UseTargets();
    eng->ED_Free(e);
}

// ---------------------------------------------------------------------------
// boss_awake — use callback, fires when boss is triggered
// ---------------------------------------------------------------------------
static void boss_awake(edict_t *self, edict_t *activator) {
    self->v.solid    = SOLID_SLIDEBOX;
    self->v.movetype = MOVETYPE_STEP;
    self->v.takedamage = DAMAGE_NO;

    eng->SV_SetModel(self, "progs/boss.mdl");
    vec3_t mins = {-128,-128,-24}, maxs = {128,128,256};
    eng->SV_SetSize(self, mins, maxs);

    float skill = eng->Cvar_VariableValue("skill");
    self->v.health = (skill == 0.0f) ? 1.0f : 3.0f;

    self->v.enemy = activator;

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_LAVASPLASH);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, self->v.origin[2]);

    self->v.yaw_speed = 20.0f;
    boss_rise1(self);
}

// ---------------------------------------------------------------------------
// event_lightning + lightning_fire / lightning_use
// These are separate entities that drive the boss puzzle.
// le1/le2 stored as module-level statics.
// ---------------------------------------------------------------------------
static edict_t *le1_ent = NULL, *le2_ent = NULL;
static float    lightning_end_time = 0.0f;

static void lightning_fire(edict_t *e) {
    if (g->time >= lightning_end_time) {
        if (le1_ent) { g->self = le1_ent; door_go_down(le1_ent); }
        if (le2_ent) { g->self = le2_ent; door_go_down(le2_ent); }
        return;
    }

    if (!le1_ent || !le2_ent) return;

    float p1x = (le1_ent->v.absmin[0]+le1_ent->v.absmax[0])*0.5f;
    float p1y = (le1_ent->v.absmin[1]+le1_ent->v.absmax[1])*0.5f;
    float p1z = le1_ent->v.absmin[2] - 16.0f;

    float p2x = (le2_ent->v.absmin[0]+le2_ent->v.absmax[0])*0.5f;
    float p2y = (le2_ent->v.absmin[1]+le2_ent->v.absmax[1])*0.5f;
    float p2z = le2_ent->v.absmin[2] - 16.0f;

    /* compensate for bolt length */
    vec3_t v12; v12[0]=p2x-p1x; v12[1]=p2y-p1y; v12[2]=p2z-p1z;
    vec3_t nv12; eng->VectorNormalize(v12, nv12);
    p2x -= nv12[0]*100.0f; p2y -= nv12[1]*100.0f; p2z -= nv12[2]*100.0f;

    e->v.nextthink = g->time + 0.1f;
    e->v.think = lightning_fire;

    eng->MSG_WriteByte(MSG_ALL, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_ALL, TE_LIGHTNING3);
    eng->MSG_WriteEntity(MSG_ALL, g->world);
    eng->MSG_WriteCoord(MSG_ALL, p1x); eng->MSG_WriteCoord(MSG_ALL, p1y); eng->MSG_WriteCoord(MSG_ALL, p1z);
    eng->MSG_WriteCoord(MSG_ALL, p2x); eng->MSG_WriteCoord(MSG_ALL, p2y); eng->MSG_WriteCoord(MSG_ALL, p2z);
}

static void lightning_use(edict_t *self, edict_t *activator) {
    if (lightning_end_time >= g->time + 1.0f) return;

    le1_ent = eng->ED_Find(g->world, "target", "lightning");
    le2_ent = (le1_ent != g->world) ? eng->ED_Find(le1_ent, "target", "lightning") : g->world;
    if (le1_ent == g->world || le2_ent == g->world) {
        eng->Con_Print("missing lightning targets\n");
        return;
    }

    /* Both electrodes must be settled (top or bottom) and in same state */
    if ((le1_ent->v.state != STATE_TOP && le1_ent->v.state != STATE_BOTTOM) ||
        (le2_ent->v.state != STATE_TOP && le2_ent->v.state != STATE_BOTTOM) ||
        (le1_ent->v.state != le2_ent->v.state))
        return;

    le1_ent->v.nextthink = -1.0f;
    le2_ent->v.nextthink = -1.0f;
    lightning_end_time = g->time + 1.0f;

    eng->SV_StartSound(self, CHAN_VOICE, "misc/power.wav", 1, ATTN_NORM);
    lightning_fire(self);

    /* Advance boss pain if top state */
    edict_t *boss = eng->ED_Find(g->world, "classname", "monster_boss");
    if (boss == g->world) return;
    boss->v.enemy = activator;
    if (le1_ent->v.state == STATE_TOP && boss->v.health > 0.0f) {
        eng->SV_StartSound(boss, CHAN_VOICE, "boss1/pain.wav", 1, ATTN_NORM);
        boss->v.health -= 1.0f;
        if (boss->v.health >= 2.0f)
            boss_shocka1(boss);
        else if (boss->v.health == 1.0f)
            boss_shockb1(boss);
        else
            boss_shockc1(boss);
    }
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_boss(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }

    eng->PrecacheModel("progs/boss.mdl");
    eng->PrecacheModel("progs/lavaball.mdl");

    eng->PrecacheSound("weapons/rocket1i.wav");
    eng->PrecacheSound("boss1/out1.wav");
    eng->PrecacheSound("boss1/sight1.wav");
    eng->PrecacheSound("misc/power.wav");
    eng->PrecacheSound("boss1/throw.wav");
    eng->PrecacheSound("boss1/pain.wav");
    eng->PrecacheSound("boss1/death.wav");

    g->total_monsters++;

    e->v.use = boss_awake;
}

void spawn_event_lightning(edict_t *e) {
    e->v.use = lightning_use;
}

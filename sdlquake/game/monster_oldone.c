// monster_oldone.c -- Shub-Niggurath (oldone.qc port).

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void ThrowGib(const char *model, float damage);

// Frame indices (sequential from $frame directives)
// old1-46 (0-45)
// shake1-12 (46-57), shake12 repeated (58), shake13-20 (59-66)
// Total shake entries: 21 (indices 46-66)
enum {
    OO_OLD1=0,OO_OLD2,OO_OLD3,OO_OLD4,OO_OLD5,OO_OLD6,OO_OLD7,OO_OLD8,OO_OLD9,
    OO_OLD10,OO_OLD11,OO_OLD12,OO_OLD13,OO_OLD14,OO_OLD15,OO_OLD16,OO_OLD17,OO_OLD18,OO_OLD19,
    OO_OLD20,OO_OLD21,OO_OLD22,OO_OLD23,OO_OLD24,OO_OLD25,OO_OLD26,OO_OLD27,OO_OLD28,OO_OLD29,
    OO_OLD30,OO_OLD31,OO_OLD32,OO_OLD33,OO_OLD34,OO_OLD35,OO_OLD36,OO_OLD37,OO_OLD38,OO_OLD39,
    OO_OLD40,OO_OLD41,OO_OLD42,OO_OLD43,OO_OLD44,OO_OLD45,OO_OLD46,
    OO_SHAKE1,OO_SHAKE2,OO_SHAKE3,OO_SHAKE4,OO_SHAKE5,OO_SHAKE6,
    OO_SHAKE7,OO_SHAKE8,OO_SHAKE9,OO_SHAKE10,OO_SHAKE11,OO_SHAKE12,
    OO_SHAKE12B, /* repeated shake12 */
    OO_SHAKE13,OO_SHAKE14,OO_SHAKE15,OO_SHAKE16,OO_SHAKE17,OO_SHAKE18,OO_SHAKE19,OO_SHAKE20
};

#define FRAME(e,fr,nxt) do{g->self=(e);(e)->v.frame=(fr);(e)->v.nextthink=g->time+0.1f;(e)->v.think=(nxt);}while(0)

/* Module-level shub entity pointer */
static edict_t *shub_ent = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void old_idle1(edict_t *e);
static void old_thrash1(edict_t *e);
static void finale_1(edict_t *e);
static void finale_2(edict_t *e);
static void finale_3(edict_t *e);
static void finale_4(edict_t *e);

// ---------------------------------------------------------------------------
// Idle animation (loops on old1-46)
// ---------------------------------------------------------------------------
static void old_idle2(edict_t *e),old_idle3(edict_t *e),old_idle4(edict_t *e),
            old_idle5(edict_t *e),old_idle6(edict_t *e),old_idle7(edict_t *e),
            old_idle8(edict_t *e),old_idle9(edict_t *e),old_idle10(edict_t *e),
            old_idle11(edict_t *e),old_idle12(edict_t *e),old_idle13(edict_t *e),
            old_idle14(edict_t *e),old_idle15(edict_t *e),old_idle16(edict_t *e),
            old_idle17(edict_t *e),old_idle18(edict_t *e),old_idle19(edict_t *e),
            old_idle20(edict_t *e),old_idle21(edict_t *e),old_idle22(edict_t *e),
            old_idle23(edict_t *e),old_idle24(edict_t *e),old_idle25(edict_t *e),
            old_idle26(edict_t *e),old_idle27(edict_t *e),old_idle28(edict_t *e),
            old_idle29(edict_t *e),old_idle30(edict_t *e),old_idle31(edict_t *e),
            old_idle32(edict_t *e),old_idle33(edict_t *e),old_idle34(edict_t *e),
            old_idle35(edict_t *e),old_idle36(edict_t *e),old_idle37(edict_t *e),
            old_idle38(edict_t *e),old_idle39(edict_t *e),old_idle40(edict_t *e),
            old_idle41(edict_t *e),old_idle42(edict_t *e),old_idle43(edict_t *e),
            old_idle44(edict_t *e),old_idle45(edict_t *e),old_idle46(edict_t *e);

static void old_idle1(edict_t *e)  { FRAME(e,OO_OLD1, old_idle2);  }
static void old_idle2(edict_t *e)  { FRAME(e,OO_OLD2, old_idle3);  }
static void old_idle3(edict_t *e)  { FRAME(e,OO_OLD3, old_idle4);  }
static void old_idle4(edict_t *e)  { FRAME(e,OO_OLD4, old_idle5);  }
static void old_idle5(edict_t *e)  { FRAME(e,OO_OLD5, old_idle6);  }
static void old_idle6(edict_t *e)  { FRAME(e,OO_OLD6, old_idle7);  }
static void old_idle7(edict_t *e)  { FRAME(e,OO_OLD7, old_idle8);  }
static void old_idle8(edict_t *e)  { FRAME(e,OO_OLD8, old_idle9);  }
static void old_idle9(edict_t *e)  { FRAME(e,OO_OLD9, old_idle10); }
static void old_idle10(edict_t *e) { FRAME(e,OO_OLD10,old_idle11); }
static void old_idle11(edict_t *e) { FRAME(e,OO_OLD11,old_idle12); }
static void old_idle12(edict_t *e) { FRAME(e,OO_OLD12,old_idle13); }
static void old_idle13(edict_t *e) { FRAME(e,OO_OLD13,old_idle14); }
static void old_idle14(edict_t *e) { FRAME(e,OO_OLD14,old_idle15); }
static void old_idle15(edict_t *e) { FRAME(e,OO_OLD15,old_idle16); }
static void old_idle16(edict_t *e) { FRAME(e,OO_OLD16,old_idle17); }
static void old_idle17(edict_t *e) { FRAME(e,OO_OLD17,old_idle18); }
static void old_idle18(edict_t *e) { FRAME(e,OO_OLD18,old_idle19); }
static void old_idle19(edict_t *e) { FRAME(e,OO_OLD19,old_idle20); }
static void old_idle20(edict_t *e) { FRAME(e,OO_OLD20,old_idle21); }
static void old_idle21(edict_t *e) { FRAME(e,OO_OLD21,old_idle22); }
static void old_idle22(edict_t *e) { FRAME(e,OO_OLD22,old_idle23); }
static void old_idle23(edict_t *e) { FRAME(e,OO_OLD23,old_idle24); }
static void old_idle24(edict_t *e) { FRAME(e,OO_OLD24,old_idle25); }
static void old_idle25(edict_t *e) { FRAME(e,OO_OLD25,old_idle26); }
static void old_idle26(edict_t *e) { FRAME(e,OO_OLD26,old_idle27); }
static void old_idle27(edict_t *e) { FRAME(e,OO_OLD27,old_idle28); }
static void old_idle28(edict_t *e) { FRAME(e,OO_OLD28,old_idle29); }
static void old_idle29(edict_t *e) { FRAME(e,OO_OLD29,old_idle30); }
static void old_idle30(edict_t *e) { FRAME(e,OO_OLD30,old_idle31); }
static void old_idle31(edict_t *e) { FRAME(e,OO_OLD31,old_idle32); }
static void old_idle32(edict_t *e) { FRAME(e,OO_OLD32,old_idle33); }
static void old_idle33(edict_t *e) { FRAME(e,OO_OLD33,old_idle34); }
static void old_idle34(edict_t *e) { FRAME(e,OO_OLD34,old_idle35); }
static void old_idle35(edict_t *e) { FRAME(e,OO_OLD35,old_idle36); }
static void old_idle36(edict_t *e) { FRAME(e,OO_OLD36,old_idle37); }
static void old_idle37(edict_t *e) { FRAME(e,OO_OLD37,old_idle38); }
static void old_idle38(edict_t *e) { FRAME(e,OO_OLD38,old_idle39); }
static void old_idle39(edict_t *e) { FRAME(e,OO_OLD39,old_idle40); }
static void old_idle40(edict_t *e) { FRAME(e,OO_OLD40,old_idle41); }
static void old_idle41(edict_t *e) { FRAME(e,OO_OLD41,old_idle42); }
static void old_idle42(edict_t *e) { FRAME(e,OO_OLD42,old_idle43); }
static void old_idle43(edict_t *e) { FRAME(e,OO_OLD43,old_idle44); }
static void old_idle44(edict_t *e) { FRAME(e,OO_OLD44,old_idle45); }
static void old_idle45(edict_t *e) { FRAME(e,OO_OLD45,old_idle46); }
static void old_idle46(edict_t *e) { FRAME(e,OO_OLD46,old_idle1);  }

// ---------------------------------------------------------------------------
// Thrash animation (death sequence)
// cnt counts how many times we've looped (loop 3 times via shake1-15)
// ---------------------------------------------------------------------------
static void old_thrash2(edict_t *e),old_thrash3(edict_t *e),old_thrash4(edict_t *e),
            old_thrash5(edict_t *e),old_thrash6(edict_t *e),old_thrash7(edict_t *e),
            old_thrash8(edict_t *e),old_thrash9(edict_t *e),old_thrash10(edict_t *e),
            old_thrash11(edict_t *e),old_thrash12(edict_t *e),old_thrash13(edict_t *e),
            old_thrash14(edict_t *e),old_thrash15(edict_t *e),old_thrash16(edict_t *e),
            old_thrash17(edict_t *e),old_thrash18(edict_t *e),old_thrash19(edict_t *e),
            old_thrash20(edict_t *e);

static void old_thrash1(edict_t *e)  { FRAME(e,OO_SHAKE1, old_thrash2);  eng->SV_LightStyle(0,"m"); }
static void old_thrash2(edict_t *e)  { FRAME(e,OO_SHAKE2, old_thrash3);  eng->SV_LightStyle(0,"k"); }
static void old_thrash3(edict_t *e)  { FRAME(e,OO_SHAKE3, old_thrash4);  eng->SV_LightStyle(0,"k"); }
static void old_thrash4(edict_t *e)  { FRAME(e,OO_SHAKE4, old_thrash5);  eng->SV_LightStyle(0,"i"); }
static void old_thrash5(edict_t *e)  { FRAME(e,OO_SHAKE5, old_thrash6);  eng->SV_LightStyle(0,"g"); }
static void old_thrash6(edict_t *e)  { FRAME(e,OO_SHAKE6, old_thrash7);  eng->SV_LightStyle(0,"e"); }
static void old_thrash7(edict_t *e)  { FRAME(e,OO_SHAKE7, old_thrash8);  eng->SV_LightStyle(0,"c"); }
static void old_thrash8(edict_t *e)  { FRAME(e,OO_SHAKE8, old_thrash9);  eng->SV_LightStyle(0,"a"); }
static void old_thrash9(edict_t *e)  { FRAME(e,OO_SHAKE9, old_thrash10); eng->SV_LightStyle(0,"c"); }
static void old_thrash10(edict_t *e) { FRAME(e,OO_SHAKE10,old_thrash11); eng->SV_LightStyle(0,"e"); }
static void old_thrash11(edict_t *e) { FRAME(e,OO_SHAKE11,old_thrash12); eng->SV_LightStyle(0,"g"); }
static void old_thrash12(edict_t *e) { FRAME(e,OO_SHAKE12,old_thrash13); eng->SV_LightStyle(0,"i"); }
static void old_thrash13(edict_t *e) { FRAME(e,OO_SHAKE13,old_thrash14); eng->SV_LightStyle(0,"k"); }
static void old_thrash14(edict_t *e) { FRAME(e,OO_SHAKE14,old_thrash15); eng->SV_LightStyle(0,"m"); }
static void old_thrash15(edict_t *e) {
    FRAME(e,OO_SHAKE15,old_thrash16);
    eng->SV_LightStyle(0,"m");
    e->v.cnt += 1.0f;
    if (e->v.cnt != 3.0f) e->v.think = old_thrash1; /* loop back */
}
static void old_thrash16(edict_t *e) { FRAME(e,OO_SHAKE16,old_thrash17); eng->SV_LightStyle(0,"g"); }
static void old_thrash17(edict_t *e) { FRAME(e,OO_SHAKE17,old_thrash18); eng->SV_LightStyle(0,"c"); }
static void old_thrash18(edict_t *e) { FRAME(e,OO_SHAKE18,old_thrash19); eng->SV_LightStyle(0,"b"); }
static void old_thrash19(edict_t *e) { FRAME(e,OO_SHAKE19,old_thrash20); eng->SV_LightStyle(0,"a"); }
static void old_thrash20(edict_t *e) { FRAME(e,OO_SHAKE20,old_thrash20); finale_4(e); }

// ---------------------------------------------------------------------------
// Finale sequence
// ---------------------------------------------------------------------------
static void finale_1(edict_t *e) {
    /* Lock intermission — use Cvar as substitute for intermission_exittime/running */
    eng->Cvar_SetValue("sv_intermission_time", 10000000.0f);

    /* Find intermission spot */
    edict_t *pos = eng->ED_Find(g->world, "classname", "info_intermission");
    if (pos == g->world) {
        eng->Con_Print("no info_intermission\n");
        return;
    }

    /* Remove misc_teleporttrain */
    edict_t *pl = eng->ED_Find(g->world, "classname", "misc_teleporttrain");
    if (pl != g->world) eng->ED_Free(pl);

    eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
    eng->MSG_WriteString(MSG_ALL, "");

    /* Move all players to intermission spot */
    pl = eng->ED_Find(g->world, "classname", "player");
    while (pl != g->world) {
        pl->v.view_ofs[0] = 0; pl->v.view_ofs[1] = 0; pl->v.view_ofs[2] = 0;
        pl->v.angles[0] = pos->v.mangle[0];
        pl->v.angles[1] = pos->v.mangle[1];
        pl->v.angles[2] = pos->v.mangle[2];
        pl->v.fixangle  = 1;
        pl->v.nextthink = g->time + 0.5f;
        pl->v.takedamage = DAMAGE_NO;
        pl->v.solid      = SOLID_NOT;
        pl->v.movetype   = MOVETYPE_NONE;
        pl->v.modelindex = 0;
        eng->SV_SetOrigin(pl, pos->v.origin);
        pl = eng->ED_Find(pl, "classname", "player");
    }

    /* Spawn a timer entity for finale_2 */
    edict_t *timer = eng->ED_Alloc();
    timer->v.nextthink = g->time + 1.0f;
    timer->v.think = finale_2;
}

static void finale_2(edict_t *e) {
    if (!shub_ent) { FRAME(e,0,finale_2); return; }
    vec3_t o;
    o[0] = shub_ent->v.origin[0];
    o[1] = shub_ent->v.origin[1] - 100.0f;
    o[2] = shub_ent->v.origin[2];

    eng->MSG_WriteByte(MSG_BROADCAST, SVC_TEMPENTITY);
    eng->MSG_WriteByte(MSG_BROADCAST, TE_TELEPORT);
    eng->MSG_WriteCoord(MSG_BROADCAST, o[0]);
    eng->MSG_WriteCoord(MSG_BROADCAST, o[1]);
    eng->MSG_WriteCoord(MSG_BROADCAST, o[2]);

    eng->SV_StartSound(shub_ent, CHAN_VOICE, "misc/r_tele1.wav", 1, ATTN_NORM);

    e->v.nextthink = g->time + 2.0f;
    e->v.think = finale_3;
}

static void finale_3(edict_t *e) {
    if (!shub_ent) return;
    shub_ent->v.think = old_thrash1;
    shub_ent->v.nextthink = g->time + 0.1f;
    eng->SV_StartSound(shub_ent, CHAN_VOICE, "boss2/death.wav", 1, ATTN_NORM);
    eng->SV_LightStyle(0, "abcdefghijklmlkjihgfedcb");
    eng->ED_Free(e); /* remove timer */
}

static void finale_4(edict_t *e) {
    eng->SV_StartSound(e, CHAN_VOICE, "boss2/pop2.wav", 1, ATTN_NORM);

    float ox = e->v.origin[0];
    float oy = e->v.origin[1];
    float oz = e->v.origin[2];

    /* Spawn gibs in 3D grid */
    for (float z = 16.0f; z <= 144.0f; z += 96.0f) {
        for (float x = -64.0f; x <= 64.0f; x += 32.0f) {
            for (float y = -64.0f; y <= 64.0f; y += 32.0f) {
                e->v.origin[0] = ox + x;
                e->v.origin[1] = oy + y;
                e->v.origin[2] = oz + z;
                float r = eng->Random();
                if (r < 0.3f)
                    ThrowGib("progs/gib1.mdl", -999.0f);
                else if (r < 0.6f)
                    ThrowGib("progs/gib2.mdl", -999.0f);
                else
                    ThrowGib("progs/gib3.mdl", -999.0f);
            }
        }
    }

    eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
    eng->MSG_WriteString(MSG_ALL,
        "Congratulations and well done! You have\n"
        "beaten the hideous Shub-Niggurath, and\n"
        "her hundreds of ugly changelings and\n"
        "monsters. You have proven that your\n"
        "skill and your cunning are greater than\n"
        "all the powers of Quake. You are the\n"
        "master now. Id Software salutes you.");

    /* Spawn a player model decoration */
    edict_t *n = eng->ED_Alloc();
    eng->SV_SetModel(n, "progs/player.mdl");
    vec3_t norg;
    norg[0] = ox - 32.0f;
    norg[1] = oy - 264.0f;
    norg[2] = oz;
    eng->SV_SetOrigin(n, norg);
    n->v.angles[1] = 290.0f;
    n->v.frame = 1;

    eng->ED_Free(e);

    eng->MSG_WriteByte(MSG_ALL, SVC_CDTRACK);
    eng->MSG_WriteByte(MSG_ALL, 3);
    eng->MSG_WriteByte(MSG_ALL, 3);
    eng->SV_LightStyle(0, "m");
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------
void spawn_monster_oldone(edict_t *e) {
    g->self = e;
    if (g->deathmatch) { eng->ED_Free(e); return; }

    eng->PrecacheModel("progs/oldone.mdl");
    eng->PrecacheModel("progs/player.mdl");

    eng->PrecacheSound("boss2/death.wav");
    eng->PrecacheSound("boss2/idle.wav");
    eng->PrecacheSound("boss2/sight.wav");
    eng->PrecacheSound("boss2/pop2.wav");
    eng->PrecacheSound("misc/r_tele1.wav");

    e->v.solid    = SOLID_SLIDEBOX;
    e->v.movetype = MOVETYPE_STEP;
    eng->SV_SetModel(e, "progs/oldone.mdl");
    vec3_t mins = {-160,-128,-24}, maxs = {160,128,256};
    eng->SV_SetSize(e, mins, maxs);

    e->v.health      = 40000.0f;
    e->v.takedamage  = DAMAGE_YES;
    e->v.think       = old_idle1;
    e->v.nextthink   = g->time + 0.1f;
    e->v.th_die      = finale_1;

    shub_ent = e;
    g->total_monsters++;
}

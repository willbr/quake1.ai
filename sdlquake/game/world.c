// world.c -- World entity, body queue, frame start. Source: world.qc

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include "sim/sim.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// Defined in weapons.c (Task 14) — precaches all weapon assets
__attribute__((weak)) void W_Precache(void) {}

edict_t *lastspawn;              // used by client.c spawn selection

static edict_t *bodyque_head;   // circular ring of 4 body-copy entities

// ---------------------------------------------------------------------------
// Body queue
// ---------------------------------------------------------------------------

static void InitBodyQue(void)
{
    edict_t *e0, *e1, *e2, *e3;

    e0 = eng->ED_Alloc(); e0->v.classname = "bodyque";
    e1 = eng->ED_Alloc(); e1->v.classname = "bodyque";
    e2 = eng->ED_Alloc(); e2->v.classname = "bodyque";
    e3 = eng->ED_Alloc(); e3->v.classname = "bodyque";

    e0->v.owner = e1;
    e1->v.owner = e2;
    e2->v.owner = e3;
    e3->v.owner = e0;

    bodyque_head = e0;
}

void CopyToBodyQue(edict_t *ent)
{
    memcpy(bodyque_head->v.angles,   ent->v.angles,   sizeof(vec3_t));
    memcpy(bodyque_head->v.velocity, ent->v.velocity, sizeof(vec3_t));
    bodyque_head->v.model      = ent->v.model;
    bodyque_head->v.modelindex = ent->v.modelindex;
    bodyque_head->v.frame      = ent->v.frame;
    bodyque_head->v.colormap   = ent->v.colormap;
    bodyque_head->v.movetype   = ent->v.movetype;
    bodyque_head->v.flags      = 0;
    eng->SV_SetOrigin(bodyque_head, ent->v.origin);
    eng->SV_SetSize(bodyque_head, ent->v.mins, ent->v.maxs);
    bodyque_head = bodyque_head->v.owner;
}

// ---------------------------------------------------------------------------
// StartFrame — called every server frame
// ---------------------------------------------------------------------------

void StartFrame(void)
{
    g->teamplay  = eng->Cvar_VariableValue("teamplay");
    g->framecount++;

    // Emit a sight stimulus from the player every frame so AI can see them.
    if (g->world && g->time > 0) {
        edict_t *player = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == 1) { player = cur; break; }
        }
        if (player && player->v.health > 0
                && !((int)player->v.flags & FL_NOTARGET)) {
            stimulus_t s = {0};
            s.kind = STIM_SIGHT_ENTITY;
            s.origin[0] = player->v.origin[0];
            s.origin[1] = player->v.origin[1];
            s.origin[2] = player->v.origin[2];
            s.intensity = 1.0f;
            s.source_edict = 1;
            Stim_Emit(&s);
        }
    }
}

// ---------------------------------------------------------------------------
// Spawn functions
// ---------------------------------------------------------------------------

void spawn_bodyque(edict_t *self)
{
    // empty — bodyque entities are created programmatically by InitBodyQue
    g->self = self;
}

extern void Client_LevelInit(void);

void spawn_worldspawn(edict_t *self)
{
    g->self  = self;
    lastspawn = g->world;
    Client_LevelInit();
    InitBodyQue();

    if (g->self->v.model && strcmp(g->self->v.model, "maps/e1m8.bsp") == 0)
        eng->Cvar_SetValue("sv_gravity", 100.0f);
    else
        eng->Cvar_SetValue("sv_gravity", 800.0f);

    W_Precache();

    // sounds used by C physics code
    eng->PrecacheSound("demon/dland2.wav");
    eng->PrecacheSound("misc/h2ohit1.wav");

    // general purpose
    eng->PrecacheSound("items/itembk2.wav");
    eng->PrecacheSound("player/plyrjmp8.wav");
    eng->PrecacheSound("player/land.wav");
    eng->PrecacheSound("player/land2.wav");
    eng->PrecacheSound("player/drown1.wav");
    eng->PrecacheSound("player/drown2.wav");
    eng->PrecacheSound("player/gasp1.wav");
    eng->PrecacheSound("player/gasp2.wav");
    eng->PrecacheSound("player/h2odeath.wav");
    eng->PrecacheSound("misc/talk.wav");
    eng->PrecacheSound("player/teledth1.wav");
    eng->PrecacheSound("misc/r_tele1.wav");
    eng->PrecacheSound("misc/r_tele2.wav");
    eng->PrecacheSound("misc/r_tele3.wav");
    eng->PrecacheSound("misc/r_tele4.wav");
    eng->PrecacheSound("misc/r_tele5.wav");
    eng->PrecacheSound("weapons/lock4.wav");
    eng->PrecacheSound("weapons/pkup.wav");
    eng->PrecacheSound("items/armor1.wav");
    eng->PrecacheSound("weapons/lhit.wav");
    eng->PrecacheSound("weapons/lstart.wav");
    eng->PrecacheSound("items/damage3.wav");
    eng->PrecacheSound("misc/power.wav");

    // player gib
    eng->PrecacheSound("player/gib.wav");
    eng->PrecacheSound("player/udeath.wav");
    eng->PrecacheSound("player/tornoff2.wav");

    // player pain
    eng->PrecacheSound("player/pain1.wav");
    eng->PrecacheSound("player/pain2.wav");
    eng->PrecacheSound("player/pain3.wav");
    eng->PrecacheSound("player/pain4.wav");
    eng->PrecacheSound("player/pain5.wav");
    eng->PrecacheSound("player/pain6.wav");

    // player death
    eng->PrecacheSound("player/death1.wav");
    eng->PrecacheSound("player/death2.wav");
    eng->PrecacheSound("player/death3.wav");
    eng->PrecacheSound("player/death4.wav");
    eng->PrecacheSound("player/death5.wav");

    // axe
    eng->PrecacheSound("weapons/ax1.wav");
    eng->PrecacheSound("player/axhit1.wav");
    eng->PrecacheSound("player/axhit2.wav");

    // water
    eng->PrecacheSound("player/h2ojump.wav");
    eng->PrecacheSound("player/slimbrn2.wav");
    eng->PrecacheSound("player/inh2o.wav");
    eng->PrecacheSound("player/inlava.wav");
    eng->PrecacheSound("misc/outwater.wav");
    eng->PrecacheSound("player/lburn1.wav");
    eng->PrecacheSound("player/lburn2.wav");
    eng->PrecacheSound("misc/water1.wav");
    eng->PrecacheSound("misc/water2.wav");

    // models
    eng->PrecacheModel("progs/player.mdl");
    eng->PrecacheModel("progs/eyes.mdl");
    eng->PrecacheModel("progs/h_player.mdl");
    eng->PrecacheModel("progs/gib1.mdl");
    eng->PrecacheModel("progs/gib2.mdl");
    eng->PrecacheModel("progs/gib3.mdl");
    eng->PrecacheModel("progs/s_bubble.spr");
    eng->PrecacheModel("progs/s_explod.spr");
    eng->PrecacheModel("progs/v_axe.mdl");
    eng->PrecacheModel("progs/v_shot.mdl");
    eng->PrecacheModel("progs/v_nail.mdl");
    eng->PrecacheModel("progs/v_rock.mdl");
    eng->PrecacheModel("progs/v_shot2.mdl");
    eng->PrecacheModel("progs/v_nail2.mdl");
    eng->PrecacheModel("progs/v_rock2.mdl");
    eng->PrecacheModel("progs/bolt.mdl");
    eng->PrecacheModel("progs/bolt2.mdl");
    eng->PrecacheModel("progs/bolt3.mdl");
    eng->PrecacheModel("progs/lavaball.mdl");
    eng->PrecacheModel("progs/missile.mdl");
    eng->PrecacheModel("progs/grenade.mdl");
    eng->PrecacheModel("progs/spike.mdl");
    eng->PrecacheModel("progs/s_spike.mdl");
    eng->PrecacheModel("progs/backpack.mdl");
    eng->PrecacheModel("progs/zom_gib.mdl");
    eng->PrecacheModel("progs/v_light.mdl");

    // light style animation tables ('a' = dark, 'z' = bright)
    eng->SV_LightStyle( 0, "m");
    eng->SV_LightStyle( 1, "mmnmmommommnonmmonqnmmo");
    eng->SV_LightStyle( 2, "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba");
    eng->SV_LightStyle( 3, "mmmmmaaaaammmmmaaaaaabcdefgabcdefg");
    eng->SV_LightStyle( 4, "mamamamamama");
    eng->SV_LightStyle( 5, "jklmnopqrstuvwxyzyxwvutsrqponmlkj");
    eng->SV_LightStyle( 6, "nmonqnmomnmomomno");
    eng->SV_LightStyle( 7, "mmmaaaabcdefgmmmmaaaammmaamm");
    eng->SV_LightStyle( 8, "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa");
    eng->SV_LightStyle( 9, "aaaaaaaazzzzzzzz");
    eng->SV_LightStyle(10, "mmamammmmammamamaaamammma");
    eng->SV_LightStyle(11, "abcdefghijklmnopqrrqponmlkjihgfedcba");
    eng->SV_LightStyle(63, "a");
}

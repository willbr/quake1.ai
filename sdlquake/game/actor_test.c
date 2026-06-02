// Skeletal-actor in-game integration test (C1-lite). Spawns a server-side
// entity whose model is the IQM dummy actor, proving the procedural expression
// (look-at / breathing / ponytail) works on a real networked entity, not just
// the engine dev-spawn. The expression is all client-side (R3/R4 run in
// R_IQMDrawModel for any mod_iqm entity), so this needs no protocol/ABI change.
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "game_types.h"
#include "actor_test.h"

extern engine_api_t   *eng;
extern game_globals_t *g;

#define ACTOR_TEST_MODEL "actors/dummy.iqm"

// Precache at level init so the model is in the precache list sent to clients at
// connect; a mid-game precache would add a slot the connected client never sees.
void Actor_TestPrecache(void)
{
    eng->PrecacheModel(ACTOR_TEST_MODEL);
}

// impulse 217: spawn a static IQM actor ~96u ahead of the player, facing it.
void Actor_TestDebugSpawn(edict_t *player)
{
    edict_t *e = eng->ED_Alloc();
    float    fx, fy, fl;

    eng->MakeVectors(player->v.v_angle);
    fx = g->v_forward[0];
    fy = g->v_forward[1];
    fl = (float)sqrt(fx * fx + fy * fy);
    if (fl > 0.001f) { fx /= fl; fy /= fl; }

    e->v.classname = "actor_test";
    e->v.solid     = SOLID_NOT;
    e->v.movetype  = MOVETYPE_NONE;
    e->v.angles[1] = player->v.v_angle[1] + 180.0f;   // face the player
    e->v.origin[0] = player->v.origin[0] + fx * 96.0f;
    e->v.origin[1] = player->v.origin[1] + fy * 96.0f;
    e->v.origin[2] = player->v.origin[2];

    eng->SV_SetModel(e, ACTOR_TEST_MODEL);
    eng->SV_SetOrigin(e, e->v.origin);   // links the edict so it is sent to clients

    eng->Con_Print("actor: spawned IQM test actor ahead (look-at/breath/ponytail active)\n");
}

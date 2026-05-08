// client.c -- Player lifecycle, intermission, water, powerups. Source: client.qc

#include "game_api.h"
#include "game_types.h"
#include "game_defs.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// Forward declarations for functions defined in other files
// ---------------------------------------------------------------------------
void  InitTrigger(void);        // subs.c
void  SUB_UseTargets(void);     // subs.c
void  CopyToBodyQue(edict_t *); // world.c
extern edict_t *lastspawn;      // world.c

void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage); // combat.c

// player.c (Task 12) — weak stubs until ported
__attribute__((weak)) void player_pain(edict_t *self, edict_t *attacker, float damage)
    { (void)self; (void)attacker; (void)damage; }
__attribute__((weak)) void player_stand1(edict_t *self) { (void)self; }
__attribute__((weak)) void PlayerDie(edict_t *self)     { (void)self; }
__attribute__((weak)) void set_suicide_frame(edict_t *self) { (void)self; }

// forward declaration — defined after CheckRules
void NextLevel(void);

// weapons.c (Task 14) — weak stubs until ported
__attribute__((weak)) void W_WeaponFrame(void)    {}
__attribute__((weak)) void W_SetCurrentAmmo(void) {}

// misc.c (Task 17) — weak stubs until ported
__attribute__((weak)) void spawn_tfog(vec3_t org)                        { (void)org; }
__attribute__((weak)) void spawn_tdeath(vec3_t org, edict_t *death_owner)
    { (void)org; (void)death_owner; }

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
float modelindex_eyes;   // set in PutClientInServer; read in CheckPowerups
float modelindex_player;

static float       intermission_running  = 0;
static float       intermission_exittime = 0;
static const char *nextmap               = NULL;

// Reset per-level state. Called from spawn_worldspawn (world.c) at the start
// of every map. In original QuakeC these were progs globals that PR_LoadProgs
// implicitly cleared on each SV_SpawnServer; we need to clear them by hand
// because the DLL's static storage persists across levels and `nextmap` would
// otherwise dangle into the previous level's hunk after Host_ClearMemory.
void Client_LevelInit(void)
{
    intermission_running  = 0;
    intermission_exittime = 0;
    nextmap               = NULL;
}

// ---------------------------------------------------------------------------
// Intermission
// ---------------------------------------------------------------------------

static edict_t *FindIntermission(void)
{
    edict_t *spot;
    float    cyc;

    spot = eng->ED_Find(g->world, "classname", "info_intermission");
    if (spot != g->world) {
        cyc = eng->Random() * 4.0f;
        while (cyc > 1.0f) {
            spot = eng->ED_Find(spot, "classname", "info_intermission");
            if (spot == g->world)
                spot = eng->ED_Find(spot, "classname", "info_intermission");
            cyc -= 1.0f;
        }
        return spot;
    }

    spot = eng->ED_Find(g->world, "classname", "info_player_start");
    if (spot != g->world) return spot;

    spot = eng->ED_Find(g->world, "classname", "testplayerstart");
    if (spot != g->world) return spot;

    eng->Host_Error("FindIntermission: no spot");
    return g->world;
}

static void GotoNextMap(void)
{
    if (eng->Cvar_VariableValue("samelevel"))
        eng->SV_ChangeLevel(g->mapname);
    else
        eng->SV_ChangeLevel(nextmap);
}

static void ExitIntermission(void)
{
    if (g->deathmatch) { GotoNextMap(); return; }

    intermission_exittime  = g->time + 1.0f;
    intermission_running  += 1.0f;

    if (intermission_running == 2.0f) {
        const char *wm = g->world->v.model;

        if (wm && strcmp(wm, "maps/e1m7.bsp") == 0) {
            eng->MSG_WriteByte(MSG_ALL, SVC_CDTRACK);
            eng->MSG_WriteByte(MSG_ALL, 2);
            eng->MSG_WriteByte(MSG_ALL, 3);
            if (!eng->Cvar_VariableValue("registered")) {
                eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
                eng->MSG_WriteString(MSG_ALL,
                    "As the corpse of the monstrous entity\nChthon sinks back into the lava whence\n"
                    "it rose, you grip the Rune of Earth\nMagic tightly. Now that you have\n"
                    "conquered the Dimension of the Doomed,\nrealm of Earth Magic, you are ready to\n"
                    "complete your task in the other three\nhaunted lands of Quake. Or are you? If\n"
                    "you don't register Quake, you'll never\nknow what awaits you in the Realm of\n"
                    "Black Magic, the Netherworld, and the\nElder World!");
            } else {
                eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
                eng->MSG_WriteString(MSG_ALL,
                    "As the corpse of the monstrous entity\nChthon sinks back into the lava whence\n"
                    "it rose, you grip the Rune of Earth\nMagic tightly. Now that you have\n"
                    "conquered the Dimension of the Doomed,\nrealm of Earth Magic, you are ready to\n"
                    "complete your task. A Rune of magic\npower lies at the end of each haunted\n"
                    "land of Quake. Go forth, seek the\ntotality of the four Runes!");
            }
            return;
        } else if (wm && strcmp(wm, "maps/e2m6.bsp") == 0) {
            eng->MSG_WriteByte(MSG_ALL, SVC_CDTRACK);
            eng->MSG_WriteByte(MSG_ALL, 2);
            eng->MSG_WriteByte(MSG_ALL, 3);
            eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
            eng->MSG_WriteString(MSG_ALL,
                "The Rune of Black Magic throbs evilly in\nyour hand and whispers dark thoughts\n"
                "into your brain. You learn the inmost\nlore of the Hell-Mother; Shub-Niggurath!\n"
                "You now know that she is behind all the\nterrible plotting which has led to so\n"
                "much death and horror. But she is not\ninviolate! Armed with this Rune, you\n"
                "realize that once all four Runes are\ncombined, the gate to Shub-Niggurath's\n"
                "Pit will open, and you can face the\nWitch-Goddess herself in her frightful\notherworld cathedral.");
            return;
        } else if (wm && strcmp(wm, "maps/e3m6.bsp") == 0) {
            eng->MSG_WriteByte(MSG_ALL, SVC_CDTRACK);
            eng->MSG_WriteByte(MSG_ALL, 2);
            eng->MSG_WriteByte(MSG_ALL, 3);
            eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
            eng->MSG_WriteString(MSG_ALL,
                "The charred viscera of diabolic horrors\nbubble viscously as you seize the Rune\n"
                "of Hell Magic. Its heat scorches your\nhand, and its terrible secrets blight\n"
                "your mind. Gathering the shreds of your\ncourage, you shake the devil's shackles\n"
                "from your soul, and become ever more\nhard and determined to destroy the\n"
                "hideous creatures whose mere existence\nthreatens the souls and psyches of all\n"
                "the population of Earth.");
            return;
        } else if (wm && strcmp(wm, "maps/e4m7.bsp") == 0) {
            eng->MSG_WriteByte(MSG_ALL, SVC_CDTRACK);
            eng->MSG_WriteByte(MSG_ALL, 2);
            eng->MSG_WriteByte(MSG_ALL, 3);
            eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
            eng->MSG_WriteString(MSG_ALL,
                "Despite the awful might of the Elder\nWorld, you have achieved the Rune of\n"
                "Elder Magic, capstone of all types of\narcane wisdom. Beyond good and evil,\n"
                "beyond life and death, the Rune\npulsates, heavy with import. Patient and\n"
                "potent, the Elder Being Shub-Niggurath\nweaves her dire plans to clear off all\n"
                "life from the Earth, and bring her own\nfoul offspring to our world! For all the\n"
                "dwellers in these nightmare dimensions\nare her descendants! Once all Runes of\n"
                "magic power are united, the energy\nbehind them will blast open the Gateway\n"
                "to Shub-Niggurath, and you can travel\nthere to foil the Hell-Mother's plots\nin person.");
            return;
        }
        GotoNextMap();
    }

    if (intermission_running == 3.0f) {
        if (!eng->Cvar_VariableValue("registered")) {
            eng->MSG_WriteByte(MSG_ALL, SVC_SELLSCREEN);
            return;
        }
        if (((int)g->serverflags & 15) == 15) {
            eng->MSG_WriteByte(MSG_ALL, SVC_FINALE);
            eng->MSG_WriteString(MSG_ALL,
                "Now, you have all four Runes. You sense\ntremendous invisible forces moving to\n"
                "unseal ancient barriers. Shub-Niggurath\nhad hoped to use the Runes Herself to\n"
                "clear off the Earth, but now instead,\nyou will use them to enter her home and\n"
                "confront her as an avatar of avenging\nEarth-life. If you defeat her, you will\n"
                "be remembered forever as the savior of\nthe planet. If she conquers, it will be\n"
                "as if you had never been born.");
            return;
        }
    }

    GotoNextMap();
}

static void IntermissionThink(void)
{
    if (g->time < intermission_exittime) return;
    if (!g->self->v.button0 && !g->self->v.button1 && !g->self->v.button2) return;
    ExitIntermission();
}

static void execute_changelevel(edict_t *self)
{
    edict_t *pos, *other;
    g->self = self;

    intermission_running = 1.0f;
    intermission_exittime = g->time + (g->deathmatch ? 5.0f : 2.0f);

    eng->MSG_WriteByte(MSG_ALL, SVC_CDTRACK);
    eng->MSG_WriteByte(MSG_ALL, 3);
    eng->MSG_WriteByte(MSG_ALL, 3);

    pos = FindIntermission();

    other = eng->ED_Find(g->world, "classname", "player");
    while (other != g->world) {
        other->v.view_ofs[0] = other->v.view_ofs[1] = other->v.view_ofs[2] = 0;
        memcpy(other->v.angles, pos->v.mangle, sizeof(vec3_t));
        memcpy(other->v.v_angle, pos->v.mangle, sizeof(vec3_t));
        other->v.fixangle  = 1;
        other->v.nextthink = g->time + 0.5f;
        other->v.takedamage = DAMAGE_NO;
        other->v.solid     = SOLID_NOT;
        other->v.movetype  = MOVETYPE_NONE;
        other->v.modelindex = 0;
        eng->SV_SetOrigin(other, pos->v.origin);
        other = eng->ED_Find(other, "classname", "player");
    }

    eng->MSG_WriteByte(MSG_ALL, SVC_INTERMISSION);
}

static void changelevel_touch(edict_t *self, edict_t *other)
{
    g->self  = self;
    g->other = other;

    if (!other->v.classname || strcmp(other->v.classname, "player") != 0)
        return;

    if (eng->Cvar_VariableValue("noexit")) {
        T_Damage(other, g->self, g->self, 50000);
        return;
    }

    eng->SV_BPrint(MSG_BROADCAST, other->v.netname);
    eng->SV_BPrint(MSG_BROADCAST, " exited the level\n");

    nextmap = g->self->v.map;
    SUB_UseTargets();

    if ((int)g->self->v.spawnflags & 1 && (int)g->deathmatch == 0) {
        GotoNextMap();
        return;
    }

    g->self->v.touch    = NULL;
    g->self->v.think    = execute_changelevel;
    g->self->v.nextthink = g->time + 0.1f;
}

// ---------------------------------------------------------------------------
// Parms — carry inventory across level changes
// ---------------------------------------------------------------------------

void SetChangeParms(edict_t *client)
{
    g->self = client;
    g->self->v.items = (float)((int)g->self->v.items &
        ~(IT_KEY1 | IT_KEY2 | IT_INVISIBILITY | IT_INVULNERABILITY | IT_SUIT | IT_QUAD));

    if (g->self->v.health > 100) g->self->v.health = 100;
    if (g->self->v.health < 50)  g->self->v.health = 50;

    g->parm[0]  = g->self->v.items;
    g->parm[1]  = g->self->v.health;
    g->parm[2]  = g->self->v.armorvalue;
    g->parm[3]  = (g->self->v.ammo_shells < 25) ? 25 : g->self->v.ammo_shells;
    g->parm[4]  = g->self->v.ammo_nails;
    g->parm[5]  = g->self->v.ammo_rockets;
    g->parm[6]  = g->self->v.ammo_cells;
    g->parm[7]  = g->self->v.weapon;
    g->parm[8]  = g->self->v.armortype * 100.0f;
    // Phase 6 carry-over: items2 (Doom + Wolf weapon-owned bitmask),
    // weapon2 (currently active Phase 6 weapon, 0 if a stock Quake weapon
    // is active), ammo_bullets (Phase 6's own ammo type used by Doom
    // pistol/chaingun and all three Wolf hitscan weapons).
    g->parm[9]  = g->self->v.items2;
    g->parm[10] = g->self->v.weapon2;
    g->parm[11] = g->self->v.ammo_bullets;
}

void SetNewParms(void)
{
    g->parm[0] = IT_SHOTGUN | IT_AXE;
    g->parm[1] = 100;
    g->parm[2] = 0;
    g->parm[3] = 25;
    g->parm[4] = 0;
    g->parm[5] = 0;
    g->parm[6] = 0;
    g->parm[7] = 1;
    g->parm[8] = 0;
    g->parm[9]  = 0;  // items2: no Phase 6 weapons owned at fresh start
    g->parm[10] = 0;  // weapon2: stock Quake weapon active
    g->parm[11] = 0;  // ammo_bullets: Phase 6 bullet ammo
}

static void DecodeLevelParms(void)
{
    if (g->serverflags) {
        if (g->world->v.model && strcmp(g->world->v.model, "maps/start.bsp") == 0)
            SetNewParms();
    }
    g->self->v.items       = g->parm[0];
    g->self->v.health      = g->parm[1];
    g->self->v.armorvalue  = g->parm[2];
    g->self->v.ammo_shells = g->parm[3];
    g->self->v.ammo_nails  = g->parm[4];
    g->self->v.ammo_rockets= g->parm[5];
    g->self->v.ammo_cells  = g->parm[6];
    g->self->v.weapon      = g->parm[7];
    g->self->v.armortype   = g->parm[8] * 0.01f;
    g->self->v.items2       = g->parm[9];
    g->self->v.weapon2      = g->parm[10];
    g->self->v.ammo_bullets = g->parm[11];
}

// ---------------------------------------------------------------------------
// Spawn point selection
// ---------------------------------------------------------------------------

static edict_t *SelectSpawnPoint(void)
{
    edict_t *spot;

    spot = eng->ED_Find(g->world, "classname", "testplayerstart");
    if (spot != g->world) return spot;

    if (g->coop) {
        lastspawn = eng->ED_Find(lastspawn, "classname", "info_player_coop");
        if (lastspawn == g->world)
            lastspawn = eng->ED_Find(lastspawn, "classname", "info_player_start");
        if (lastspawn != g->world) return lastspawn;
    } else if (g->deathmatch) {
        lastspawn = eng->ED_Find(lastspawn, "classname", "info_player_deathmatch");
        if (lastspawn == g->world)
            lastspawn = eng->ED_Find(lastspawn, "classname", "info_player_deathmatch");
        if (lastspawn != g->world) return lastspawn;
    }

    if (g->serverflags) {
        spot = eng->ED_Find(g->world, "classname", "info_player_start2");
        if (spot != g->world) return spot;
    }

    spot = eng->ED_Find(g->world, "classname", "info_player_start");
    if (spot == g->world)
        eng->Host_Error("PutClientInServer: no info_player_start on level");
    return spot;
}

// ---------------------------------------------------------------------------
// Player client lifecycle
// ---------------------------------------------------------------------------

void PutClientInServer(edict_t *client)
{
    edict_t *spot;
    vec3_t origin_up;

    g->self = client;
    g->self->v.classname   = "player";
    g->self->v.health      = 100;
    g->self->v.takedamage  = DAMAGE_AIM;
    g->self->v.solid       = SOLID_SLIDEBOX;
    g->self->v.movetype    = MOVETYPE_WALK;
    g->self->v.show_hostile = 0;
    g->self->v.max_health  = 100;
    g->self->v.flags       = FL_CLIENT;
    g->self->v.air_finished = g->time + 12.0f;
    g->self->v.dmg         = 2;
    g->self->v.super_damage_finished = 0;
    g->self->v.radsuit_finished      = 0;
    g->self->v.invisible_finished    = 0;
    g->self->v.invincible_finished   = 0;
    g->self->v.effects     = 0;
    g->self->v.invincible_time = 0;

    DecodeLevelParms();
    W_SetCurrentAmmo();

    g->self->v.attack_finished = g->time;
    g->self->v.th_pain         = player_pain;
    g->self->v.th_die          = PlayerDie;
    g->self->v.deadflag        = DEAD_NO;
    g->self->v.pausetime       = 0;

    spot = SelectSpawnPoint();

    origin_up[0] = spot->v.origin[0];
    origin_up[1] = spot->v.origin[1];
    origin_up[2] = spot->v.origin[2] + 1.0f;
    memcpy(g->self->v.origin, origin_up, sizeof(vec3_t));
    memcpy(g->self->v.angles, spot->v.angles, sizeof(vec3_t));
    g->self->v.fixangle = 1;

    eng->SV_SetModel(g->self, "progs/eyes.mdl");
    modelindex_eyes = g->self->v.modelindex;

    eng->SV_SetModel(g->self, "progs/player.mdl");
    modelindex_player = g->self->v.modelindex;

    {
        vec3_t hull_min = {VEC_HULL_MIN_X, VEC_HULL_MIN_Y, VEC_HULL_MIN_Z};
        vec3_t hull_max = {VEC_HULL_MAX_X, VEC_HULL_MAX_Y, VEC_HULL_MAX_Z};
        eng->SV_SetSize(g->self, hull_min, hull_max);
    }

    g->self->v.view_ofs[0] = 0;
    g->self->v.view_ofs[1] = 0;
    g->self->v.view_ofs[2] = 22;

    player_stand1(g->self);

    if (g->deathmatch || g->coop) {
        eng->MakeVectors(g->self->v.angles);
        vec3_t tfog_org;
        tfog_org[0] = g->self->v.origin[0] + g->v_forward[0] * 20.0f;
        tfog_org[1] = g->self->v.origin[1] + g->v_forward[1] * 20.0f;
        tfog_org[2] = g->self->v.origin[2] + g->v_forward[2] * 20.0f;
        spawn_tfog(tfog_org);
    }

    spawn_tdeath(g->self->v.origin, g->self);
}

void ClientConnect(edict_t *client)
{
    g->self = client;
    eng->SV_BPrint(MSG_BROADCAST, g->self->v.netname);
    eng->SV_BPrint(MSG_BROADCAST, " entered the game\n");
    if (intermission_running)
        ExitIntermission();
}

void ClientDisconnect(edict_t *client)
{
    g->self = client;
    if (g->gameover) return;
    eng->SV_BPrint(MSG_BROADCAST, g->self->v.netname);
    eng->SV_BPrint(MSG_BROADCAST, " left the game with ");
    eng->SV_BPrint(MSG_BROADCAST, eng->FToS(g->self->v.frags));
    eng->SV_BPrint(MSG_BROADCAST, " frags\n");
    eng->SV_StartSound(g->self, CHAN_BODY, "player/tornoff2.wav", 1, ATTN_NONE);
    set_suicide_frame(g->self);
}

// ---------------------------------------------------------------------------
// Respawn / death handling
// ---------------------------------------------------------------------------

static void respawn(void)
{
    if (g->coop) {
        CopyToBodyQue(g->self);
        eng->SV_SetSpawnParms(g->self);
        PutClientInServer(g->self);
    } else if (g->deathmatch) {
        CopyToBodyQue(g->self);
        SetNewParms();
        PutClientInServer(g->self);
    } else {
        eng->Cbuf_AddText("restart\n");
    }
}

void ClientKill(edict_t *client)
{
    g->self = client;
    eng->SV_BPrint(MSG_BROADCAST, g->self->v.netname);
    eng->SV_BPrint(MSG_BROADCAST, " suicides\n");
    set_suicide_frame(g->self);
    g->self->v.modelindex = modelindex_player;
    g->self->v.frags     -= 2.0f;
    respawn();
}

// ---------------------------------------------------------------------------
// Death think / jump
// ---------------------------------------------------------------------------

static void PlayerDeathThink(void)
{
    float forward;

    if ((int)g->self->v.flags & FL_ONGROUND) {
        forward = eng->VectorLength(g->self->v.velocity);
        forward -= 20.0f;
        if (forward <= 0.0f) {
            g->self->v.velocity[0] = g->self->v.velocity[1] = g->self->v.velocity[2] = 0;
        } else {
            vec3_t dir;
            eng->VectorNormalize(g->self->v.velocity, dir);
            g->self->v.velocity[0] = dir[0] * forward;
            g->self->v.velocity[1] = dir[1] * forward;
            g->self->v.velocity[2] = dir[2] * forward;
        }
    }

    if (g->self->v.deadflag == DEAD_DEAD) {
        if (g->self->v.button2 || g->self->v.button1 || g->self->v.button0) return;
        g->self->v.deadflag = DEAD_RESPAWNABLE;
        return;
    }

    if (!g->self->v.button2 && !g->self->v.button1 && !g->self->v.button0) return;

    g->self->v.button0 = 0;
    g->self->v.button1 = 0;
    g->self->v.button2 = 0;
    respawn();
}

static void PlayerJump(void)
{
    if ((int)g->self->v.flags & FL_WATERJUMP) return;

    if (g->self->v.waterlevel >= 2.0f) {
        if (g->self->v.watertype == CONTENT_WATER)
            g->self->v.velocity[2] = 100;
        else if (g->self->v.watertype == CONTENT_SLIME)
            g->self->v.velocity[2] = 80;
        else
            g->self->v.velocity[2] = 50;

        if (g->self->v.swim_flag < g->time) {
            g->self->v.swim_flag = g->time + 1.0f;
            if (eng->Random() < 0.5f)
                eng->SV_StartSound(g->self, CHAN_BODY, "misc/water1.wav", 1, ATTN_NORM);
            else
                eng->SV_StartSound(g->self, CHAN_BODY, "misc/water2.wav", 1, ATTN_NORM);
        }
        return;
    }

    if (!((int)g->self->v.flags & FL_ONGROUND))     return;
    if (!((int)g->self->v.flags & FL_JUMPRELEASED))  return;

    g->self->v.flags = (float)((int)g->self->v.flags & ~FL_JUMPRELEASED);
    g->self->v.flags = (float)((int)g->self->v.flags & ~FL_ONGROUND);
    g->self->v.button2 = 0;
    eng->SV_StartSound(g->self, CHAN_BODY, "player/plyrjmp8.wav", 1, ATTN_NORM);
    g->self->v.velocity[2] += 270.0f;
}

static void WaterMove(void)
{
    if (g->self->v.movetype == MOVETYPE_NOCLIP) return;
    if (g->self->v.health < 0) return;

    if (g->self->v.waterlevel != 3.0f) {
        if (g->self->v.air_finished < g->time)
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/gasp2.wav", 1, ATTN_NORM);
        else if (g->self->v.air_finished < g->time + 9.0f)
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/gasp1.wav", 1, ATTN_NORM);
        g->self->v.air_finished = g->time + 12.0f;
        g->self->v.dmg = 2;
    } else if (g->self->v.air_finished < g->time) {
        if (g->self->v.pain_finished < g->time) {
            g->self->v.dmg += 2.0f;
            if (g->self->v.dmg > 15.0f) g->self->v.dmg = 10.0f;
            T_Damage(g->self, g->world, g->world, g->self->v.dmg);
            g->self->v.pain_finished = g->time + 1.0f;
        }
    }

    if (!g->self->v.waterlevel) {
        if ((int)g->self->v.flags & FL_INWATER) {
            eng->SV_StartSound(g->self, CHAN_BODY, "misc/outwater.wav", 1, ATTN_NORM);
            g->self->v.flags = (float)((int)g->self->v.flags & ~FL_INWATER);
        }
        return;
    }

    if (g->self->v.watertype == CONTENT_LAVA) {
        if (g->self->v.dmgtime < g->time) {
            g->self->v.dmgtime = g->time +
                (g->self->v.radsuit_finished > g->time ? 1.0f : 0.2f);
            T_Damage(g->self, g->world, g->world, 10.0f * g->self->v.waterlevel);
        }
    } else if (g->self->v.watertype == CONTENT_SLIME) {
        if (g->self->v.dmgtime < g->time && g->self->v.radsuit_finished < g->time) {
            g->self->v.dmgtime = g->time + 1.0f;
            T_Damage(g->self, g->world, g->world, 4.0f * g->self->v.waterlevel);
        }
    }

    if (!((int)g->self->v.flags & FL_INWATER)) {
        if (g->self->v.watertype == CONTENT_LAVA)
            eng->SV_StartSound(g->self, CHAN_BODY, "player/inlava.wav", 1, ATTN_NORM);
        if (g->self->v.watertype == CONTENT_WATER)
            eng->SV_StartSound(g->self, CHAN_BODY, "player/inh2o.wav", 1, ATTN_NORM);
        if (g->self->v.watertype == CONTENT_SLIME)
            eng->SV_StartSound(g->self, CHAN_BODY, "player/slimbrn2.wav", 1, ATTN_NORM);
        g->self->v.flags = (float)((int)g->self->v.flags | FL_INWATER);
        g->self->v.dmgtime = 0;
    }

    if (!((int)g->self->v.flags & FL_WATERJUMP)) {
        float drag = 0.8f * g->self->v.waterlevel * g->frametime;
        g->self->v.velocity[0] -= drag * g->self->v.velocity[0];
        g->self->v.velocity[1] -= drag * g->self->v.velocity[1];
        g->self->v.velocity[2] -= drag * g->self->v.velocity[2];
    }
}

static void CheckWaterJump(void)
{
    vec3_t start, end, fwd;

    eng->MakeVectors(g->self->v.angles);
    memcpy(start, g->self->v.origin, sizeof(vec3_t));
    start[2] += 8.0f;

    memcpy(fwd, g->v_forward, sizeof(vec3_t));
    fwd[2] = 0;
    eng->VectorNormalize(fwd, fwd);

    end[0] = start[0] + fwd[0] * 24.0f;
    end[1] = start[1] + fwd[1] * 24.0f;
    end[2] = start[2];

    eng->SV_Traceline(start, end, 1, g->self);
    if (g->trace_fraction < 1.0f) {
        start[2] += g->self->v.maxs[2] - 8.0f;
        end[0] = start[0] + fwd[0] * 24.0f;
        end[1] = start[1] + fwd[1] * 24.0f;
        end[2] = start[2];
        g->self->v.movedir[0] = g->trace_plane_normal[0] * -50.0f;
        g->self->v.movedir[1] = g->trace_plane_normal[1] * -50.0f;
        g->self->v.movedir[2] = g->trace_plane_normal[2] * -50.0f;
        eng->SV_Traceline(start, end, 1, g->self);
        if (g->trace_fraction == 1.0f) {
            g->self->v.flags = (float)((int)g->self->v.flags | FL_WATERJUMP);
            g->self->v.velocity[2] = 225.0f;
            g->self->v.flags = (float)((int)g->self->v.flags & ~FL_JUMPRELEASED);
            g->self->v.teleport_time = g->time + 2.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// PlayerPreThink / PlayerPostThink
// ---------------------------------------------------------------------------

static void CheckRules(void)
{
    float timelimit, fraglimit;

    if (g->gameover) return;

    timelimit = eng->Cvar_VariableValue("timelimit") * 60.0f;
    fraglimit  = eng->Cvar_VariableValue("fraglimit");

    if (timelimit && g->time >= timelimit) { NextLevel(); return; }
    if (fraglimit  && g->self->v.frags >= fraglimit) { NextLevel(); return; }
}

void PlayerPreThink(edict_t *client)
{
    g->self = client;

    if (intermission_running) { IntermissionThink(); return; }

    if (!g->self->v.view_ofs[0] && !g->self->v.view_ofs[1] && !g->self->v.view_ofs[2])
        return;

    eng->MakeVectors(g->self->v.v_angle);
    CheckRules();
    WaterMove();
    if (g->self->v.waterlevel == 2.0f) CheckWaterJump();

    if (g->self->v.deadflag >= DEAD_DEAD) { PlayerDeathThink(); return; }
    if (g->self->v.deadflag == DEAD_DYING) return;

    if (g->self->v.button2)
        PlayerJump();
    else
        g->self->v.flags = (float)((int)g->self->v.flags | FL_JUMPRELEASED);

    if (g->time < g->self->v.pausetime)
        g->self->v.velocity[0] = g->self->v.velocity[1] = g->self->v.velocity[2] = 0;
}

static void CheckPowerups(void)
{
    if (g->self->v.health <= 0) return;

    if (g->self->v.invisible_finished) {
        if (g->self->v.invisible_sound < g->time) {
            eng->SV_StartSound(g->self, CHAN_AUTO, "items/inv3.wav", 0.5f, ATTN_IDLE);
            g->self->v.invisible_sound = g->time + (eng->Random() * 3.0f + 1.0f);
        }
        if (g->self->v.invisible_finished < g->time + 3.0f) {
            if (g->self->v.invisible_time == 1.0f) {
                eng->SV_SPrint(g->self, MSG_ONE, "Ring of Shadows magic is fading\n");
                eng->SV_StuffCmd(g->self, "bf\n");
                eng->SV_StartSound(g->self, CHAN_AUTO, "items/inv2.wav", 1, ATTN_NORM);
                g->self->v.invisible_time = g->time + 1.0f;
            }
            if (g->self->v.invisible_time < g->time) {
                g->self->v.invisible_time = g->time + 1.0f;
                eng->SV_StuffCmd(g->self, "bf\n");
            }
        }
        if (g->self->v.invisible_finished < g->time) {
            g->self->v.items = (float)((int)g->self->v.items & ~IT_INVISIBILITY);
            g->self->v.invisible_finished = 0;
            g->self->v.invisible_time     = 0;
        }
        g->self->v.frame       = 0;
        g->self->v.modelindex  = modelindex_eyes;
    } else {
        g->self->v.modelindex = modelindex_player;
    }

    if (g->self->v.invincible_finished) {
        if (g->self->v.invincible_finished < g->time + 3.0f) {
            if (g->self->v.invincible_time == 1.0f) {
                eng->SV_SPrint(g->self, MSG_ONE, "Protection is almost burned out\n");
                eng->SV_StuffCmd(g->self, "bf\n");
                eng->SV_StartSound(g->self, CHAN_AUTO, "items/protect2.wav", 1, ATTN_NORM);
                g->self->v.invincible_time = g->time + 1.0f;
            }
            if (g->self->v.invincible_time < g->time) {
                g->self->v.invincible_time = g->time + 1.0f;
                eng->SV_StuffCmd(g->self, "bf\n");
            }
        }
        if (g->self->v.invincible_finished < g->time) {
            g->self->v.items = (float)((int)g->self->v.items & ~IT_INVULNERABILITY);
            g->self->v.invincible_time     = 0;
            g->self->v.invincible_finished = 0;
        }
        if (g->self->v.invincible_finished > g->time)
            g->self->v.effects = (float)((int)g->self->v.effects | EF_DIMLIGHT);
        else
            g->self->v.effects = (float)((int)g->self->v.effects & ~EF_DIMLIGHT);
    }

    if (g->self->v.super_damage_finished) {
        if (g->self->v.super_damage_finished < g->time + 3.0f) {
            if (g->self->v.super_time == 1.0f) {
                eng->SV_SPrint(g->self, MSG_ONE, "Quad Damage is wearing off\n");
                eng->SV_StuffCmd(g->self, "bf\n");
                eng->SV_StartSound(g->self, CHAN_AUTO, "items/damage2.wav", 1, ATTN_NORM);
                g->self->v.super_time = g->time + 1.0f;
            }
            if (g->self->v.super_time < g->time) {
                g->self->v.super_time = g->time + 1.0f;
                eng->SV_StuffCmd(g->self, "bf\n");
            }
        }
        if (g->self->v.super_damage_finished < g->time) {
            g->self->v.items = (float)((int)g->self->v.items & ~IT_QUAD);
            g->self->v.super_damage_finished = 0;
            g->self->v.super_time = 0;
        }
        if (g->self->v.super_damage_finished > g->time)
            g->self->v.effects = (float)((int)g->self->v.effects | EF_DIMLIGHT);
        else
            g->self->v.effects = (float)((int)g->self->v.effects & ~EF_DIMLIGHT);
    }

    if (g->self->v.radsuit_finished) {
        g->self->v.air_finished = g->time + 12.0f;
        if (g->self->v.radsuit_finished < g->time + 3.0f) {
            if (g->self->v.rad_time == 1.0f) {
                eng->SV_SPrint(g->self, MSG_ONE, "Air supply in Biosuit expiring\n");
                eng->SV_StuffCmd(g->self, "bf\n");
                eng->SV_StartSound(g->self, CHAN_AUTO, "items/suit2.wav", 1, ATTN_NORM);
                g->self->v.rad_time = g->time + 1.0f;
            }
            if (g->self->v.rad_time < g->time) {
                g->self->v.rad_time = g->time + 1.0f;
                eng->SV_StuffCmd(g->self, "bf\n");
            }
        }
        if (g->self->v.radsuit_finished < g->time) {
            g->self->v.items = (float)((int)g->self->v.items & ~IT_SUIT);
            g->self->v.rad_time       = 0;
            g->self->v.radsuit_finished = 0;
        }
    }
}

void PlayerPostThink(edict_t *client)
{
    g->self = client;
    if (!g->self->v.view_ofs[0] && !g->self->v.view_ofs[1] && !g->self->v.view_ofs[2])
        return;
    if (g->self->v.deadflag) return;

    W_WeaponFrame();

    if (g->self->v.jump_flag < -300.0f &&
        ((int)g->self->v.flags & FL_ONGROUND) &&
        g->self->v.health > 0)
    {
        if (g->self->v.watertype == CONTENT_WATER)
            eng->SV_StartSound(g->self, CHAN_BODY, "player/h2ojump.wav", 1, ATTN_NORM);
        else if (g->self->v.jump_flag < -650.0f) {
            T_Damage(g->self, g->world, g->world, 5);
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/land2.wav", 1, ATTN_NORM);
            g->self->v.deathtype = "falling";
        } else {
            eng->SV_StartSound(g->self, CHAN_VOICE, "player/land.wav", 1, ATTN_NORM);
        }
        g->self->v.jump_flag = 0;
    }

    if (!((int)g->self->v.flags & FL_ONGROUND))
        g->self->v.jump_flag = g->self->v.velocity[2];

    CheckPowerups();
}

// ---------------------------------------------------------------------------
// NextLevel helper
// ---------------------------------------------------------------------------

void NextLevel(void)
{
    edict_t *o;

    o = eng->ED_Find(g->world, "classname", "trigger_changelevel");
    if (o == g->world || (g->mapname && strcmp(g->mapname, "start") == 0)) {
        o = eng->ED_Alloc();
        o->v.map = g->mapname;
    }

    nextmap = o->v.map;
    if (o->v.nextthink < g->time) {
        o->v.think    = execute_changelevel;
        o->v.nextthink = g->time + 0.1f;
    }
}

// ---------------------------------------------------------------------------
// Obituary
// ---------------------------------------------------------------------------

void ClientObituary(edict_t *targ, edict_t *attacker)
{
    float rnum;
    const char *deathstring, *deathstring2;

    rnum = eng->Random();

    if (!targ->v.classname || strcmp(targ->v.classname, "player") != 0) return;

    if (attacker->v.classname && strcmp(attacker->v.classname, "teledeath") == 0) {
        eng->SV_BPrint(MSG_BROADCAST, targ->v.netname);
        eng->SV_BPrint(MSG_BROADCAST, " was telefragged by ");
        eng->SV_BPrint(MSG_BROADCAST, attacker->v.owner ? attacker->v.owner->v.netname : "");
        eng->SV_BPrint(MSG_BROADCAST, "\n");
        if (attacker->v.owner) attacker->v.owner->v.frags += 1.0f;
        return;
    }

    if (attacker->v.classname && strcmp(attacker->v.classname, "teledeath2") == 0) {
        eng->SV_BPrint(MSG_BROADCAST, "Satan's power deflects ");
        eng->SV_BPrint(MSG_BROADCAST, targ->v.netname);
        eng->SV_BPrint(MSG_BROADCAST, "'s telefrag\n");
        targ->v.frags -= 1.0f;
        return;
    }

    if (attacker->v.classname && strcmp(attacker->v.classname, "player") == 0) {
        if (targ == attacker) {
            attacker->v.frags -= 1.0f;
            eng->SV_BPrint(MSG_BROADCAST, targ->v.netname);
            if ((int)targ->v.weapon == 64 && targ->v.waterlevel > 1)
                { eng->SV_BPrint(MSG_BROADCAST, " discharges into the water.\n"); return; }
            if ((int)targ->v.weapon == 16)
                eng->SV_BPrint(MSG_BROADCAST, " tries to put the pin back in\n");
            else if (rnum)
                eng->SV_BPrint(MSG_BROADCAST, " becomes bored with life\n");
            else
                eng->SV_BPrint(MSG_BROADCAST, " checks if his weapon is loaded\n");
            return;
        } else {
            attacker->v.frags += 1.0f;
            rnum = attacker->v.weapon;
            deathstring = deathstring2 = "";
            if ((int)rnum == IT_AXE)
                { deathstring = " was ax-murdered by "; deathstring2 = "\n"; }
            if ((int)rnum == IT_SHOTGUN)
                { deathstring = " chewed on "; deathstring2 = "'s boomstick\n"; }
            if ((int)rnum == IT_SUPER_SHOTGUN)
                { deathstring = " ate 2 loads of "; deathstring2 = "'s buckshot\n"; }
            if ((int)rnum == IT_NAILGUN)
                { deathstring = " was nailed by "; deathstring2 = "\n"; }
            if ((int)rnum == IT_SUPER_NAILGUN)
                { deathstring = " was punctured by "; deathstring2 = "\n"; }
            if ((int)rnum == IT_GRENADE_LAUNCHER) {
                deathstring = " eats "; deathstring2 = "'s pineapple\n";
                if (targ->v.health < -40)
                    { deathstring = " was gibbed by "; deathstring2 = "'s grenade\n"; }
            }
            if ((int)rnum == IT_ROCKET_LAUNCHER) {
                deathstring = " rides "; deathstring2 = "'s rocket\n";
                if (targ->v.health < -40)
                    { deathstring = " was gibbed by "; deathstring2 = "'s rocket\n"; }
            }
            if ((int)rnum == IT_LIGHTNING) {
                deathstring = " accepts ";
                deathstring2 = (attacker->v.waterlevel > 1) ? "'s discharge\n" : "'s shaft\n";
            }
            eng->SV_BPrint(MSG_BROADCAST, targ->v.netname);
            eng->SV_BPrint(MSG_BROADCAST, deathstring);
            eng->SV_BPrint(MSG_BROADCAST, attacker->v.netname);
            eng->SV_BPrint(MSG_BROADCAST, deathstring2);
            return;
        }
    }

    targ->v.frags -= 1.0f;
    rnum = targ->v.watertype;

    eng->SV_BPrint(MSG_BROADCAST, targ->v.netname);
    if ((int)rnum == CONTENT_WATER) {
        if (eng->Random() < 0.5f)
            eng->SV_BPrint(MSG_BROADCAST, " sleeps with the fishes\n");
        else
            eng->SV_BPrint(MSG_BROADCAST, " sucks it down\n");
        return;
    } else if ((int)rnum == CONTENT_SLIME) {
        if (eng->Random() < 0.5f)
            eng->SV_BPrint(MSG_BROADCAST, " gulped a load of slime\n");
        else
            eng->SV_BPrint(MSG_BROADCAST, " can't exist on slime alone\n");
        return;
    } else if ((int)rnum == CONTENT_LAVA) {
        if (targ->v.health < -15)
            { eng->SV_BPrint(MSG_BROADCAST, " burst into flames\n"); return; }
        if (eng->Random() < 0.5f)
            eng->SV_BPrint(MSG_BROADCAST, " turned into hot slag\n");
        else
            eng->SV_BPrint(MSG_BROADCAST, " visits the Volcano God\n");
        return;
    }

    if (attacker->v.classname && ((int)attacker->v.flags & FL_MONSTER)) {
        const char *cn = attacker->v.classname;
        if      (!strcmp(cn,"monster_army"))       eng->SV_BPrint(MSG_BROADCAST," was shot by a Grunt\n");
        else if (!strcmp(cn,"monster_demon1"))     eng->SV_BPrint(MSG_BROADCAST," was eviscerated by a Fiend\n");
        else if (!strcmp(cn,"monster_dog"))        eng->SV_BPrint(MSG_BROADCAST," was mauled by a Rottweiler\n");
        else if (!strcmp(cn,"monster_dragon"))     eng->SV_BPrint(MSG_BROADCAST," was fried by a Dragon\n");
        else if (!strcmp(cn,"monster_enforcer"))   eng->SV_BPrint(MSG_BROADCAST," was blasted by an Enforcer\n");
        else if (!strcmp(cn,"monster_fish"))       eng->SV_BPrint(MSG_BROADCAST," was fed to the Rotfish\n");
        else if (!strcmp(cn,"monster_hell_knight"))eng->SV_BPrint(MSG_BROADCAST," was slain by a Death Knight\n");
        else if (!strcmp(cn,"monster_knight"))     eng->SV_BPrint(MSG_BROADCAST," was slashed by a Knight\n");
        else if (!strcmp(cn,"monster_ogre"))       eng->SV_BPrint(MSG_BROADCAST," was destroyed by an Ogre\n");
        else if (!strcmp(cn,"monster_oldone"))     eng->SV_BPrint(MSG_BROADCAST," became one with Shub-Niggurath\n");
        else if (!strcmp(cn,"monster_shalrath"))   eng->SV_BPrint(MSG_BROADCAST," was exploded by a Vore\n");
        else if (!strcmp(cn,"monster_shambler"))   eng->SV_BPrint(MSG_BROADCAST," was smashed by a Shambler\n");
        else if (!strcmp(cn,"monster_tarbaby"))    eng->SV_BPrint(MSG_BROADCAST," was slimed by a Spawn\n");
        else if (!strcmp(cn,"monster_wizard"))     eng->SV_BPrint(MSG_BROADCAST," was scragged by a Scrag\n");
        else if (!strcmp(cn,"monster_zombie"))     eng->SV_BPrint(MSG_BROADCAST," joins the Zombies\n");
        return;
    }
    if (attacker->v.classname && !strcmp(attacker->v.classname, "explo_box"))
        { eng->SV_BPrint(MSG_BROADCAST, " blew up\n"); return; }
    if (attacker->v.solid == SOLID_BSP && attacker != g->world)
        { eng->SV_BPrint(MSG_BROADCAST, " was squished\n"); return; }
    if (targ->v.deathtype && !strcmp(targ->v.deathtype, "falling")) {
        targ->v.deathtype = "";
        eng->SV_BPrint(MSG_BROADCAST, " fell to his death\n");
        return;
    }
    if (attacker->v.classname &&
        (!strcmp(attacker->v.classname, "trap_shooter") ||
         !strcmp(attacker->v.classname, "trap_spikeshooter")))
        { eng->SV_BPrint(MSG_BROADCAST, " was spiked\n"); return; }
    if (attacker->v.classname && !strcmp(attacker->v.classname, "fireball"))
        { eng->SV_BPrint(MSG_BROADCAST, " ate a lavaball\n"); return; }
    if (attacker->v.classname && !strcmp(attacker->v.classname, "trigger_changelevel"))
        { eng->SV_BPrint(MSG_BROADCAST, " tried to leave\n"); return; }

    eng->SV_BPrint(MSG_BROADCAST, " died\n");
}

// ---------------------------------------------------------------------------
// Spawn functions
// ---------------------------------------------------------------------------

void spawn_info_intermission(edict_t *self)   { g->self = self; }
void spawn_info_player_start(edict_t *self)   { g->self = self; }
void spawn_info_player_start2(edict_t *self)  { g->self = self; }
void spawn_testplayerstart(edict_t *self)     { g->self = self; }
void spawn_info_player_deathmatch(edict_t *self) { g->self = self; }
void spawn_info_player_coop(edict_t *self)    { g->self = self; }

void spawn_trigger_changelevel(edict_t *self)
{
    g->self = self;
    if (!g->self->v.map) {
        eng->Con_Print("trigger_changelevel: no map\n");
        return;
    }
    InitTrigger();
    g->self->v.touch = changelevel_touch;
}

// bot.c -- self-driving player. Reads the live server state and writes
// the outgoing usercmd_t each frame, replacing keyboard/mouse input.
//
// The bot is strictly navmesh-driven: it picks a goal, asks for a path,
// and walks waypoints. There is no fallback / reactive recovery — if
// the navmesh says "no path" the bot just stands still, which is the
// behaviour we want while iterating on the navmesh itself.

#include "quakedef.h"
#include "hotreload.h"      // g_game_api
#include "../game/game_api.h"

// ---------------------------------------------------------------------------
// Cvars + state
// ---------------------------------------------------------------------------

cvar_t bot              = { "bot",              "0", true };
cvar_t bot_aware_radius = { "bot_aware_radius", "1024" };
cvar_t bot_pickup_radius= { "bot_pickup_radius","640" };
cvar_t bot_turn_speed   = { "bot_turn_speed",   "540" };
cvar_t bot_combat_dist  = { "bot_combat_dist",  "768" };
cvar_t bot_debug        = { "bot_debug",        "0", true };
cvar_t bot_debug_viz    = { "bot_debug_viz",    "0", true };

typedef enum {
    BS_IDLE = 0,
    BS_COMBAT,
    BS_GOTO_ITEM,
    BS_GOTO_KEY,
    BS_GOTO_BUTTON,
    BS_GOTO_EXIT,
    BS_DEAD,
} bot_state_t;

// STATE_BOTTOM in the game DLL — a func_button at this state is ready to
// be pressed. Anything else (UP/TOP/DOWN) means it's mid-cycle.
#define BOT_BUTTON_STATE_READY 1

#define BOT_MAX_WAYPOINTS 32

// Kept in sync with sim_nav.c's NAV_EDGE_* enum. The drive layer uses
// this to press jump for jump-up edges and wait on plat-ride edges.
enum {
    BOT_EDGE_WALK       = 0,
    BOT_EDGE_JUMP_UP    = 1,
    BOT_EDGE_DROP_DOWN  = 2,
    BOT_EDGE_PLAT_RIDE  = 3,
    BOT_EDGE_TELEPORT   = 4,
    BOT_EDGE_PLAT_LINK  = 5,
};

static struct {
    bot_state_t  state;
    int          target_edict;     // current goal edict, or -1
    vec3_t       target_pos;       // edict origin or static (exit / plat center)
    vec3_t       waypoints[BOT_MAX_WAYPOINTS];
    unsigned char waypoint_kinds[BOT_MAX_WAYPOINTS];   // entry-edge kind
    int          waypoint_count;
    int          waypoint_idx;

    float        next_replan;      // host_time of next goal re-evaluation

    char         map_at_init[64];  // detect map change for state reset
} b;

// in_attack / in_jump live in cl_input.c (kbutton_t globals)
extern kbutton_t in_attack, in_jump;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void Bot_VecCopy(const vec3_t in, vec3_t out) { out[0]=in[0]; out[1]=in[1]; out[2]=in[2]; }

static float Bot_AngleNorm(float a)
{
    while (a >  180.f) a -= 360.f;
    while (a < -180.f) a += 360.f;
    return a;
}

// Edict accessor — sv.edicts is byte-strided by pr_edict_size, but in the
// native-C build all edicts are sizeof(edict_t). EDICT_NUM is the standard.
static edict_t *Bot_Edict(int i)
{
    if (i < 0 || i >= sv.num_edicts) return NULL;
    return EDICT_NUM(i);
}

static int Bot_LineOfSight(const vec3_t a, const vec3_t b_pos)
{
    trace_t tr = SV_Move((float *)a, vec3_origin, vec3_origin, (float *)b_pos,
                         MOVE_NOMONSTERS, sv_player);
    return tr.fraction >= 0.999f;
}

static int Bot_IsEnemy(edict_t *e)
{
    if (!e || e->free) return 0;
    if (e == sv_player) return 0;
    if ((int)e->v.flags & FL_MONSTER) {
        if (e->v.health > 0) return 1;
    }
    return 0;
}

static int Bot_IsExit(edict_t *e)
{
    if (!e || e->free || !e->v.classname) return 0;
    return strcmp(e->v.classname, "trigger_changelevel") == 0;
}

static int Bot_IsItem(edict_t *e)
{
    if (!e || e->free || !e->v.classname) return 0;
    if (!((int)e->v.flags & FL_ITEM)) return 0;
    return 1;
}

// Only chase items that actually improve us — saves the bot from
// orbiting full-ammo pickups that won't help.
static int Bot_ItemUseful(edict_t *e)
{
    const char *cls;
    if (!e || !e->v.classname) return 0;
    cls = e->v.classname;
    if (sv_player->v.health < 75 && strncmp(cls, "item_health", 11) == 0) return 1;
    if (strncmp(cls, "item_armor",  10) == 0 && sv_player->v.armortype == 0) return 1;
    if (strncmp(cls, "weapon_",      7) == 0) {
        int new_bits = (int)e->v.items & ~(int)sv_player->v.items;
        return new_bits != 0;
    }
    return 0;
}

static int Bot_IsKey(edict_t *e)
{
    if (!e || e->free || !e->v.classname) return 0;
    return strncmp(e->v.classname, "item_key", 8) == 0;
}

// Touchable func_button that hasn't been pressed yet. Shoot-only buttons
// (health != 0) are skipped — we don't have button-shooting logic.
static int Bot_IsButton(edict_t *e)
{
    if (!e || e->free || !e->v.classname) return 0;
    if (strcmp(e->v.classname, "func_button") != 0) return 0;
    if ((int)e->v.health != 0) return 0;
    if ((int)e->v.state != BOT_BUTTON_STATE_READY) return 0;
    return 1;
}

static float Bot_Dist2(const vec3_t a, const vec3_t bv)
{
    float dx = a[0]-bv[0], dy = a[1]-bv[1], dz = a[2]-bv[2];
    return dx*dx + dy*dy + dz*dz;
}

static void Bot_EntityCenter(edict_t *e, vec3_t out)
{
    out[0] = e->v.origin[0] + 0.5f*(e->v.mins[0]+e->v.maxs[0]);
    out[1] = e->v.origin[1] + 0.5f*(e->v.mins[1]+e->v.maxs[1]);
    out[2] = e->v.origin[2] + 0.5f*(e->v.mins[2]+e->v.maxs[2]);
}

// ---------------------------------------------------------------------------
// Path request
// ---------------------------------------------------------------------------

static const char *Bot_EdgeKindName(int k)
{
    switch (k) {
        case BOT_EDGE_WALK:      return "walk";
        case BOT_EDGE_JUMP_UP:   return "jump";
        case BOT_EDGE_DROP_DOWN: return "drop";
        case BOT_EDGE_PLAT_RIDE: return "ride";
        case BOT_EDGE_TELEPORT:  return "tele";
        case BOT_EDGE_PLAT_LINK: return "plat";
    }
    return "?";
}

static int Bot_RequestPath(const vec3_t to)
{
    int n;
    unsigned int items;
    if (!g_game_api || !g_game_api->nav_path) return 0;
    items = (unsigned int)sv_player->v.items;
    n = g_game_api->nav_path((float *)sv_player->v.origin, (float *)to,
                             (float *)b.waypoints,
                             b.waypoint_kinds,
                             items,
                             BOT_MAX_WAYPOINTS);
    if (n <= 0) {
        b.waypoint_count = 0;
        b.waypoint_idx = 0;
        if (bot_debug.value)
            Con_Printf("[bot] nav_path 0 (no path) to (%.0f %.0f %.0f) items=0x%x\n",
                       to[0], to[1], to[2], items);
        return 0;
    }
    b.waypoint_count = n;
    b.waypoint_idx = 0;
    if (bot_debug.value) {
        int i;
        Con_Printf("[bot] path %d wps to (%.0f %.0f %.0f):\n",
                   n, to[0], to[1], to[2]);
        for (i = 0; i < n; i++) {
            Con_Printf("  [%2d] %s (%.0f %.0f %.0f)\n",
                       i, Bot_EdgeKindName(b.waypoint_kinds[i]),
                       b.waypoints[i][0], b.waypoints[i][1], b.waypoints[i][2]);
        }
    }
    return 1;
}

static int Bot_CurrentWaypointKind(void)
{
    if (b.waypoint_count <= 0 || b.waypoint_idx >= b.waypoint_count)
        return BOT_EDGE_WALK;
    return b.waypoint_kinds[b.waypoint_idx];
}

// ---------------------------------------------------------------------------
// Perception + goal selection
// ---------------------------------------------------------------------------

static void Bot_DecideGoal(void)
{
    int i;
    edict_t *best_enemy = NULL, *best_key = NULL, *best_item = NULL;
    edict_t *best_exit = NULL, *best_button = NULL;
    float best_enemy_d2 = 1e30f, best_key_d2 = 1e30f;
    float best_item_d2  = 1e30f, best_exit_d2 = 1e30f;
    float best_button_d2 = 1e30f;
    float aware2  = bot_aware_radius.value  * bot_aware_radius.value;
    float pickup2 = bot_pickup_radius.value * bot_pickup_radius.value;
    vec3_t ppos;

    if (!sv_player) return;
    Bot_VecCopy(sv_player->v.origin, ppos);

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = Bot_Edict(i);
        float d2;
        if (!e || e->free) continue;
        d2 = Bot_Dist2(ppos, e->v.origin);

        if (Bot_IsEnemy(e)) {
            if (d2 < aware2 && d2 < best_enemy_d2 && Bot_LineOfSight(ppos, e->v.origin)) {
                best_enemy = e;
                best_enemy_d2 = d2;
            }
        } else if (Bot_IsKey(e)) {
            if (d2 < best_key_d2) {
                best_key = e;
                best_key_d2 = d2;
            }
        } else if (Bot_IsItem(e)) {
            if (d2 < pickup2 && d2 < best_item_d2) {
                best_item = e;
                best_item_d2 = d2;
            }
        } else if (Bot_IsExit(e)) {
            vec3_t c;
            Bot_EntityCenter(e, c);
            d2 = Bot_Dist2(ppos, c);
            if (d2 < best_exit_d2) {
                best_exit = e;
                best_exit_d2 = d2;
            }
        } else if (Bot_IsButton(e)) {
            vec3_t c;
            Bot_EntityCenter(e, c);
            d2 = Bot_Dist2(ppos, c);
            if (d2 < aware2 && d2 < best_button_d2) {
                best_button = e;
                best_button_d2 = d2;
            }
        }
    }

    if (best_item && !Bot_ItemUseful(best_item)) best_item = NULL;

    if (best_enemy) {
        b.state = BS_COMBAT;
        b.target_edict = NUM_FOR_EDICT(best_enemy);
        Bot_VecCopy(best_enemy->v.origin, b.target_pos);
        return;
    }
    if (best_button) {
        b.state = BS_GOTO_BUTTON;
        b.target_edict = NUM_FOR_EDICT(best_button);
        Bot_EntityCenter(best_button, b.target_pos);
        Bot_RequestPath(b.target_pos);
        return;
    }
    if (best_key) {
        b.state = BS_GOTO_KEY;
        b.target_edict = NUM_FOR_EDICT(best_key);
        Bot_VecCopy(best_key->v.origin, b.target_pos);
        Bot_RequestPath(b.target_pos);
        return;
    }
    if (best_exit) {
        b.state = BS_GOTO_EXIT;
        b.target_edict = NUM_FOR_EDICT(best_exit);
        Bot_EntityCenter(best_exit, b.target_pos);
        Bot_RequestPath(b.target_pos);
        return;
    }
    if (best_item) {
        b.state = BS_GOTO_ITEM;
        b.target_edict = NUM_FOR_EDICT(best_item);
        Bot_VecCopy(best_item->v.origin, b.target_pos);
        Bot_RequestPath(b.target_pos);
        return;
    }

    if (bot_debug.value) Con_Printf("[bot] no goal -> IDLE\n");
    b.state = BS_IDLE;
    b.target_edict = -1;
    b.waypoint_count = 0;
}

// ---------------------------------------------------------------------------
// Drive layer
// ---------------------------------------------------------------------------

static const float *Bot_CurrentWaypoint(void)
{
    if (b.waypoint_count <= 0) return NULL;
    if (b.waypoint_idx >= b.waypoint_count) return NULL;
    return b.waypoints[b.waypoint_idx];
}

static void Bot_AdvanceWaypoint(void)
{
    const float *wp = Bot_CurrentWaypoint();
    float dx, dy, dz;
    if (!wp) return;
    // Never auto-advance the final waypoint — keep walking into it so
    // the bot's bbox physically reaches the goal entity (button → touch
    // fires, key/item → pickup fires). The 48-unit XY radius is wider
    // than the player bbox, so without this the bot stops short.
    if (b.waypoint_idx >= b.waypoint_count - 1) return;
    dx = wp[0] - sv_player->v.origin[0];
    dy = wp[1] - sv_player->v.origin[1];
    dz = wp[2] - sv_player->v.origin[2];
    // Require XY proximity AND vertical proximity; otherwise lift
    // waypoints (same XY, two heights) auto-advance instantly and the
    // path skips the ride.
    if (dx*dx + dy*dy < (48.f*48.f) && dz > -64.f && dz < 64.f) {
        b.waypoint_idx++;
    }
}

// combat_mode: 0 = navigate (pitch decays to 0), 1 = combat (aim Y vs Z).
static void Bot_AimAt(const vec3_t target, int combat_mode)
{
    float dx = target[0] - sv_player->v.origin[0];
    float dy = target[1] - sv_player->v.origin[1];
    float yaw = atan2f(dy, dx) * 180.f / (float)M_PI;
    float diff = Bot_AngleNorm(yaw - cl.viewangles[YAW]);
    float max_step = bot_turn_speed.value * host_frametime;
    float want_pitch = 0.f, pdiff;
    if (diff >  max_step) diff = max_step;
    if (diff < -max_step) diff = -max_step;
    cl.viewangles[YAW] = anglemod(cl.viewangles[YAW] + diff);

    if (combat_mode) {
        float dz = target[2] - (sv_player->v.origin[2] + sv_player->v.view_ofs[2]);
        float horiz = sqrtf(dx*dx + dy*dy);
        if (horiz > 32.f)
            want_pitch = -(atan2f(dz, horiz) * 180.f / (float)M_PI);
    }
    pdiff = want_pitch - cl.viewangles[PITCH];
    if (pdiff >  max_step) pdiff = max_step;
    if (pdiff < -max_step) pdiff = -max_step;
    cl.viewangles[PITCH] += pdiff;
    if (cl.viewangles[PITCH] > 80.f) cl.viewangles[PITCH] = 80.f;
    if (cl.viewangles[PITCH] < -70.f) cl.viewangles[PITCH] = -70.f;
}

static int Bot_AimError(const vec3_t target)
{
    float dx = target[0] - sv_player->v.origin[0];
    float dy = target[1] - sv_player->v.origin[1];
    float yaw = atan2f(dy, dx) * 180.f / (float)M_PI;
    return (int)fabsf(Bot_AngleNorm(yaw - cl.viewangles[YAW]));
}

static void Bot_SetButtons(int attack, int jump)
{
    if (attack) in_attack.state |= 1; else in_attack.state &= ~1;
    if (jump)   in_jump.state   |= 1; else in_jump.state   &= ~1;
}

// ---------------------------------------------------------------------------
// Main per-frame entry
// ---------------------------------------------------------------------------

static void Bot_DriveFrame(usercmd_t *cmd)
{
    extern cvar_t cl_forwardspeed, cl_sidespeed, cl_upspeed;
    edict_t *target = NULL;
    const float *wp;
    vec3_t aim_target;
    int do_attack = 0, do_jump = 0;

    // Map change — reset all per-map state (waypoints, target, etc.)
    if (strncmp(b.map_at_init, sv.name, sizeof(b.map_at_init)) != 0) {
        if (bot_debug.value)
            Con_Printf("[bot] map change %s -> %s, resetting\n",
                       b.map_at_init, sv.name);
        b.state = BS_IDLE;
        b.target_edict = -1;
        b.waypoint_count = 0;
        b.waypoint_idx = 0;
        b.next_replan = 0.f;
        Q_strncpy(b.map_at_init, sv.name, sizeof(b.map_at_init) - 1);
        b.map_at_init[sizeof(b.map_at_init) - 1] = '\0';
        return;
    }

    // Dead — leave the corpse alone so cause of death is inspectable.
    if (sv_player->v.health <= 0) {
        b.state = BS_DEAD;
        return;
    } else if (b.state == BS_DEAD) {
        b.state = BS_IDLE;
        b.next_replan = 0;
    }

    if (b.target_edict >= 0) target = Bot_Edict(b.target_edict);

    // Drop the target if it's gone or no longer matches our goal kind.
    if (b.target_edict >= 0) {
        int still_valid = 0;
        if (target && !target->free) {
            switch (b.state) {
                case BS_COMBAT:      still_valid = Bot_IsEnemy(target);  break;
                case BS_GOTO_KEY:    still_valid = Bot_IsKey(target);    break;
                case BS_GOTO_ITEM:   still_valid = Bot_IsItem(target);   break;
                case BS_GOTO_EXIT:   still_valid = Bot_IsExit(target);   break;
                case BS_GOTO_BUTTON: still_valid = Bot_IsButton(target); break;
                default:             still_valid = 1;
            }
        }
        if (!still_valid) {
            b.next_replan = 0;
            b.target_edict = -1;
        }
    }

    if (host_time >= b.next_replan) {
        Bot_DecideGoal();
        b.next_replan = host_time + 0.5f;
        target = b.target_edict >= 0 ? Bot_Edict(b.target_edict) : NULL;
    }

    // Pick the point we're aiming at this tick.
    aim_target[0] = aim_target[1] = aim_target[2] = 0.f;
    if (b.state == BS_COMBAT && target) {
        Bot_EntityCenter(target, aim_target);
    } else {
        wp = Bot_CurrentWaypoint();
        if (wp) {
            aim_target[0] = wp[0]; aim_target[1] = wp[1]; aim_target[2] = wp[2];
        } else if (target) {
            // No path — still face the unreachable target so the
            // failure is visible. forwardmove stays 0 below, so the
            // bot turns but doesn't walk into a wall.
            Bot_EntityCenter(target, aim_target);
        }
    }
    if (aim_target[0] || aim_target[1] || aim_target[2])
        Bot_AimAt(aim_target, b.state == BS_COMBAT);

    cmd->forwardmove = 0;
    cmd->sidemove    = 0;
    cmd->upmove      = 0;

    if (b.state == BS_COMBAT && target) {
        int yaw_err = Bot_AimError(aim_target);
        float d2 = Bot_Dist2(sv_player->v.origin, target->v.origin);
        do_attack = (yaw_err < 8);
        cmd->sidemove = (sinf((float)host_time * 4.f) > 0.f ? 1.f : -1.f) * cl_sidespeed.value * 0.6f;
        if (d2 > (bot_combat_dist.value * bot_combat_dist.value))
            cmd->forwardmove = cl_forwardspeed.value;
        else if (d2 < (200.f*200.f))
            cmd->forwardmove = -cl_forwardspeed.value * 0.5f;
    } else if (Bot_CurrentWaypoint()) {
        // Walk forward only when roughly aligned with the waypoint.
        // Above 60° error, turn in place; below, scale by cos so the
        // bot strafes smoothly through gentle bends.
        int yaw_err = Bot_AimError(aim_target);
        if (yaw_err <= 60) {
            float cosw = cosf(yaw_err * (float)M_PI / 180.f);
            cmd->forwardmove = cl_forwardspeed.value * cosw;
        }

        // Lift wait: standing on plat_bottom with plat_top above us —
        // hold position so we don't walk off before the lift rises.
        {
            const float *cur = Bot_CurrentWaypoint();
            int kind = Bot_CurrentWaypointKind();
            float dx = cur[0] - sv_player->v.origin[0];
            float dy = cur[1] - sv_player->v.origin[1];
            float dz = cur[2] - sv_player->v.origin[2];
            float xy2 = dx*dx + dy*dy;
            if (xy2 < (32.f*32.f) && dz > 64.f) {
                cmd->forwardmove = 0;
                cmd->sidemove    = 0;
            }
            // Jump-up edge: press jump as we close on the ledge.
            if (kind == BOT_EDGE_JUMP_UP &&
                xy2 < (64.f*64.f) && dz > 18.f && dz < 48.f &&
                ((int)sv_player->v.flags & FL_ONGROUND))
            {
                do_jump = 1;
            }
        }

        Bot_AdvanceWaypoint();
    }
    // else: no waypoint (no path or IDLE). Stand still.

    Bot_SetButtons(do_attack, do_jump);

    if (bot_debug.value && b.state != BS_IDLE &&
        ((int)(host_time*2.f) != (int)((host_time - host_frametime)*2.f)))
    {
        const char *stname = "?";
        switch (b.state) {
            case BS_COMBAT:      stname = "COMBAT";      break;
            case BS_GOTO_KEY:    stname = "GOTO_KEY";    break;
            case BS_GOTO_ITEM:   stname = "GOTO_ITEM";   break;
            case BS_GOTO_EXIT:   stname = "GOTO_EXIT";   break;
            case BS_GOTO_BUTTON: stname = "GOTO_BUTTON"; break;
            case BS_IDLE:        stname = "IDLE";        break;
            case BS_DEAD:        stname = "DEAD";        break;
        }
        Con_Printf("[bot] %s tgt=%d wp=%d/%d hp=%d\n",
                   stname, b.target_edict, b.waypoint_idx, b.waypoint_count,
                   (int)sv_player->v.health);
    }
}

// ---------------------------------------------------------------------------
// Debug viz
// ---------------------------------------------------------------------------

static void Bot_DrawDebug(void)
{
    extern void DebugLines_Add(const float start[3], const float end[3], int color, int ztest);
    vec3_t eye, fwd, ahead, aim;
    float yaw_r;
    int i;

    if (!bot_debug_viz.value || !sv_player || sv_player->free) return;

    eye[0] = sv_player->v.origin[0];
    eye[1] = sv_player->v.origin[1];
    eye[2] = sv_player->v.origin[2] + sv_player->v.view_ofs[2];

    // Waypoint chain — colour each segment by its entry-edge kind.
    {
        static const int kind_color[6] = { 244, 251, 79, 192, 13, 254 };
        for (i = b.waypoint_idx; i + 1 < b.waypoint_count; i++) {
            int k = b.waypoint_kinds[i+1];
            if (k < 0 || k >= 6) k = 0;
            DebugLines_Add(b.waypoints[i], b.waypoints[i+1], kind_color[k], 0);
        }
        if (b.waypoint_count > 0 && b.waypoint_idx < b.waypoint_count) {
            int k = b.waypoint_kinds[b.waypoint_idx];
            if (k < 0 || k >= 6) k = 0;
            DebugLines_Add(eye, b.waypoints[b.waypoint_idx], kind_color[k], 0);
        }
    }

    // Target marker (red cross).
    if (b.target_edict >= 0) {
        vec3_t tp;
        const float k = 24.f;
        vec3_t a, b2;
        Bot_VecCopy(b.target_pos, tp);
        a[0]=tp[0]-k; a[1]=tp[1]; a[2]=tp[2]; b2[0]=tp[0]+k; b2[1]=tp[1]; b2[2]=tp[2];
        DebugLines_Add(a, b2, 251, 0);
        a[0]=tp[0]; a[1]=tp[1]-k; a[2]=tp[2]; b2[0]=tp[0]; b2[1]=tp[1]+k; b2[2]=tp[2];
        DebugLines_Add(a, b2, 251, 0);
        a[0]=tp[0]; a[1]=tp[1]; a[2]=tp[2]-k; b2[0]=tp[0]; b2[1]=tp[1]; b2[2]=tp[2]+k;
        DebugLines_Add(a, b2, 251, 0);
    }

    // Aim ray (yellow).
    yaw_r = cl.viewangles[YAW] * (float)M_PI / 180.f;
    fwd[0] = cosf(yaw_r); fwd[1] = sinf(yaw_r); fwd[2] = 0.f;
    aim[0] = eye[0] + fwd[0] * 256.f;
    aim[1] = eye[1] + fwd[1] * 256.f;
    aim[2] = eye[2];
    DebugLines_Add(eye, aim, 192, 0);

    // Wall-probe stub (magenta when something close in front).
    ahead[0] = eye[0] + fwd[0] * 48.f;
    ahead[1] = eye[1] + fwd[1] * 48.f;
    ahead[2] = eye[2];
    {
        trace_t tr = SV_Move(eye, vec3_origin, vec3_origin, ahead, MOVE_NORMAL, sv_player);
        if (tr.fraction < 0.95f) {
            vec3_t hit;
            hit[0] = eye[0] + (ahead[0]-eye[0]) * tr.fraction;
            hit[1] = eye[1] + (ahead[1]-eye[1]) * tr.fraction;
            hit[2] = eye[2];
            DebugLines_Add(eye, hit, 254, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API + console commands
// ---------------------------------------------------------------------------

int Bot_Active(void) { return bot.value != 0.f && sv.active && cls.signon == SIGNONS; }

void Bot_Frame(usercmd_t *cmd)
{
    if (!Bot_Active()) return;
    if (!sv_player || sv_player->free) return;
    Bot_DriveFrame(cmd);
    Bot_DrawDebug();
}

static void Bot_Status_f(void)
{
    const char *stname = "?";
    if (!Bot_Active()) { Con_Printf("[bot] inactive\n"); return; }
    switch (b.state) {
        case BS_COMBAT:      stname = "COMBAT";      break;
        case BS_GOTO_KEY:    stname = "GOTO_KEY";    break;
        case BS_GOTO_ITEM:   stname = "GOTO_ITEM";   break;
        case BS_GOTO_EXIT:   stname = "GOTO_EXIT";   break;
        case BS_GOTO_BUTTON: stname = "GOTO_BUTTON"; break;
        case BS_IDLE:        stname = "IDLE";        break;
        case BS_DEAD:        stname = "DEAD";        break;
    }
    Con_Printf("[bot] state=%s target_edict=%d waypoints=%d/%d\n",
               stname, b.target_edict, b.waypoint_idx, b.waypoint_count);
}

static void Bot_PathDump_f(void)
{
    int i;
    if (b.waypoint_count <= 0) {
        Con_Printf("[bot] no active path\n");
        return;
    }
    Con_Printf("[bot] path target_pos=(%.0f %.0f %.0f) idx=%d/%d:\n",
               b.target_pos[0], b.target_pos[1], b.target_pos[2],
               b.waypoint_idx, b.waypoint_count);
    Con_Printf("  player @ (%.0f %.0f %.0f)\n",
               sv_player->v.origin[0], sv_player->v.origin[1], sv_player->v.origin[2]);
    for (i = 0; i < b.waypoint_count; i++) {
        const char *mark = (i == b.waypoint_idx) ? " <-- current" : "";
        Con_Printf("  [%2d] %s (%.0f %.0f %.0f)%s\n",
                   i, Bot_EdgeKindName(b.waypoint_kinds[i]),
                   b.waypoints[i][0], b.waypoints[i][1], b.waypoints[i][2],
                   mark);
    }
}

void Bot_Init(void)
{
    Cvar_RegisterVariable(&bot);
    Cvar_RegisterVariable(&bot_aware_radius);
    Cvar_RegisterVariable(&bot_pickup_radius);
    Cvar_RegisterVariable(&bot_turn_speed);
    Cvar_RegisterVariable(&bot_combat_dist);
    Cvar_RegisterVariable(&bot_debug);
    Cvar_RegisterVariable(&bot_debug_viz);
    Cmd_AddCommand("bot_status", Bot_Status_f);
    Cmd_AddCommand("bot_path_dump", Bot_PathDump_f);

    b.state = BS_IDLE;
    b.target_edict = -1;
}

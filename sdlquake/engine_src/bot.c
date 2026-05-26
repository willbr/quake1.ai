// bot.c -- self-driving player. Reads the live server state and writes
// the outgoing usercmd_t each frame, replacing keyboard/mouse input.

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

typedef enum {
    BS_IDLE = 0,
    BS_COMBAT,
    BS_GOTO_ITEM,
    BS_GOTO_KEY,
    BS_GOTO_EXIT,
    BS_WANDER,    // randomized exploration when no path available
    BS_DEAD,
} bot_state_t;

#define BOT_MAX_WAYPOINTS 32
#define BOT_BLOCKLIST_MAX 16

typedef struct {
    int    edict_index;     // -1 == positional goal (no edict pin)
    float  cooldown_until;  // host_time when re-eligible
} bot_blocklist_entry_t;

static struct {
    bot_state_t  state;
    int          target_edict;     // current goal edict, or -1
    vec3_t       target_pos;       // either edict origin or static (exit center)
    vec3_t       waypoints[BOT_MAX_WAYPOINTS];
    int          waypoint_count;
    int          waypoint_idx;
    float        waypoint_started; // host_time when current waypoint became active

    float        next_replan;      // host_time of next forced replan
    int          stuck_fails;      // consecutive stuck replans
    float        jump_until;       // host_time hold-jump countdown
    float        last_attack_toggle;

    vec3_t       progress_pos;     // last position checkpoint
    float        progress_time;    // host_time when progress_pos last updated

    // Wander state: pick a random yaw heading, keep it for wander_until
    // seconds; if blocked, pick a new one.
    float        wander_yaw;
    float        wander_until;     // host_time when current heading expires
    float        wander_replan;    // host_time of next goal re-check during wander

    bot_blocklist_entry_t blocklist[BOT_BLOCKLIST_MAX];
    int                   blocklist_n;

    float        smoothed_yaw;     // for damping
    char         map_at_init[64];  // detect map change for state reset
} b;

// in_attack / in_jump live in cl_input.c (kbutton_t globals)
extern kbutton_t in_attack, in_jump, in_forward, in_back, in_moveleft, in_moveright, in_up, in_down;

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

static int Bot_Blocked(int edict_index)
{
    int i;
    if (edict_index < 0) return 0;
    for (i = 0; i < b.blocklist_n; i++) {
        if (b.blocklist[i].edict_index == edict_index &&
            b.blocklist[i].cooldown_until > host_time)
            return 1;
    }
    return 0;
}

static void Bot_AddBlocklist(int edict_index, float secs)
{
    int i;
    if (edict_index < 0) return;
    // reuse slot if already present
    for (i = 0; i < b.blocklist_n; i++) {
        if (b.blocklist[i].edict_index == edict_index) {
            b.blocklist[i].cooldown_until = host_time + secs;
            return;
        }
    }
    if (b.blocklist_n >= BOT_BLOCKLIST_MAX) {
        // overwrite oldest (lowest cooldown_until)
        int oldest = 0;
        for (i = 1; i < BOT_BLOCKLIST_MAX; i++)
            if (b.blocklist[i].cooldown_until < b.blocklist[oldest].cooldown_until) oldest = i;
        b.blocklist[oldest].edict_index = edict_index;
        b.blocklist[oldest].cooldown_until = host_time + secs;
        return;
    }
    b.blocklist[b.blocklist_n].edict_index = edict_index;
    b.blocklist[b.blocklist_n].cooldown_until = host_time + secs;
    b.blocklist_n++;
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
    trace_t tr = SV_Move((float *)a, vec3_origin, vec3_origin, (float *)b_pos, MOVE_NOMONSTERS, sv_player);
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

// Is this item actually worth picking up given our current loadout?
// Conservative: only chase items that improve us, so we don't waste
// time pursuing already-maxed ammo (the most common pickup type).
static int Bot_ItemUseful(edict_t *e)
{
    const char *cls;
    if (!e || !e->v.classname) return 0;
    cls = e->v.classname;
    if (sv_player->v.health < 75 && strncmp(cls, "item_health", 11) == 0) return 1;
    if (strncmp(cls, "item_armor",  10) == 0 && sv_player->v.armortype == 0) return 1;
    if (strncmp(cls, "weapon_",      7) == 0) {
        // New weapon iff we don't have it yet — entity carries the bit in v.items.
        int new_bits = (int)e->v.items & ~(int)sv_player->v.items;
        return new_bits != 0;
    }
    // Keys handled separately; ammo / artifacts skipped.
    return 0;
}

static int Bot_IsKey(edict_t *e)
{
    if (!e || e->free || !e->v.classname) return 0;
    // Quake stock key classnames: item_key1, item_key2 (silver/gold).
    return strncmp(e->v.classname, "item_key", 8) == 0;
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

static int Bot_RequestPath(const vec3_t to)
{
    int n;
    if (!g_game_api || !g_game_api->nav_path) return 0;
    n = g_game_api->nav_path((float *)sv_player->v.origin, (float *)to,
                             (float *)b.waypoints, BOT_MAX_WAYPOINTS);
    if (n <= 0) {
        b.waypoint_count = 0;
        b.waypoint_idx = 0;
        return 0;
    }
    b.waypoint_count = n;
    b.waypoint_idx = 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Perception + goal selection
// ---------------------------------------------------------------------------

static void Bot_DecideGoal(void)
{
    int i;
    edict_t *best_enemy = NULL, *best_key = NULL, *best_item = NULL, *best_exit = NULL;
    float best_enemy_d2 = 1e30f, best_key_d2 = 1e30f, best_item_d2 = 1e30f, best_exit_d2 = 1e30f;
    float aware2 = bot_aware_radius.value * bot_aware_radius.value;
    float pickup2 = bot_pickup_radius.value * bot_pickup_radius.value;
    vec3_t ppos;

    if (!sv_player) return;
    Bot_VecCopy(sv_player->v.origin, ppos);

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = Bot_Edict(i);
        float d2;
        if (!e || e->free) continue;
        if (Bot_Blocked(NUM_FOR_EDICT(e))) continue;
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
            // Use entity centre rather than origin (triggers are brush ents).
            vec3_t c;
            Bot_EntityCenter(e, c);
            d2 = Bot_Dist2(ppos, c);
            if (d2 < best_exit_d2) {
                best_exit = e;
                best_exit_d2 = d2;
            }
        }
    }

    // Filter best_item by usefulness (don't waste time on full-ammo pickups).
    if (best_item && !Bot_ItemUseful(best_item)) best_item = NULL;

    if (best_enemy) {
        b.state = BS_COMBAT;
        b.target_edict = NUM_FOR_EDICT(best_enemy);
        Bot_VecCopy(best_enemy->v.origin, b.target_pos);
        return;
    }
    // Set goal in priority order. Path-finding failure no longer
    // blocklists — the drive layer can still aim directly at the
    // target position. Blocklisting is reserved for actual stuck loops.
    if (best_key) {
        b.state = BS_GOTO_KEY;
        b.target_edict = NUM_FOR_EDICT(best_key);
        Bot_VecCopy(best_key->v.origin, b.target_pos);
        b.waypoint_started = (float)host_time;
        Bot_RequestPath(b.target_pos);
        return;
    }
    if (best_exit) {
        b.state = BS_GOTO_EXIT;
        b.target_edict = NUM_FOR_EDICT(best_exit);
        Bot_EntityCenter(best_exit, b.target_pos);
        b.waypoint_started = (float)host_time;
        Bot_RequestPath(b.target_pos);
        return;
    }
    if (best_item) {
        b.state = BS_GOTO_ITEM;
        b.target_edict = NUM_FOR_EDICT(best_item);
        Bot_VecCopy(best_item->v.origin, b.target_pos);
        b.waypoint_started = (float)host_time;
        Bot_RequestPath(b.target_pos);
        return;
    }

    // Nothing actionable — wander forward.
    if (bot_debug.value) Con_Printf("[bot] IDLE: no goal found\n");
    b.state = BS_IDLE;
    b.target_edict = -1;
    b.waypoint_count = 0;
}

// ---------------------------------------------------------------------------
// Drive layer -- turns goal + waypoints into usercmd_t
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
    float dx, dy;
    if (!wp) return;
    dx = wp[0] - sv_player->v.origin[0];
    dy = wp[1] - sv_player->v.origin[1];
    if (dx*dx + dy*dy < (48.f*48.f)) {
        b.waypoint_idx++;
        b.waypoint_started = (float)host_time;
    }
}

// aim_pitch: 0 = navigate (pitch decays to 0), nonzero = combat (aim Y vs Z).
static void Bot_AimAt(const vec3_t target, int combat_mode)
{
    float dx = target[0] - sv_player->v.origin[0];
    float dy = target[1] - sv_player->v.origin[1];
    float yaw = (atan2f(dy, dx) * 180.f / (float)M_PI);
    float diff = Bot_AngleNorm(yaw - cl.viewangles[YAW]);
    float max_step = bot_turn_speed.value * host_frametime;
    float want_pitch = 0.f;
    float pdiff;
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
    // KeyDown sets the down bit (1) and edge bit (2). For continuous
    // attack/jump just hold bit 1.
    if (attack) in_attack.state |= 1;
    else        in_attack.state &= ~1;
    if (jump)   in_jump.state |= 1;
    else        in_jump.state &= ~1;
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
    float speed = sv_player->v.velocity[0]*sv_player->v.velocity[0]
                + sv_player->v.velocity[1]*sv_player->v.velocity[1];

    // Reset state if the map changed under us (slipgate took the bot
    // to a new level — old waypoints / target_edict / blocklist are
    // referring to the previous map's entities).
    if (strncmp(b.map_at_init, sv.name, sizeof(b.map_at_init)) != 0) {
        if (bot_debug.value)
            Con_Printf("[bot] map change %s -> %s, resetting\n",
                       b.map_at_init, sv.name);
        b.state = BS_IDLE;
        b.target_edict = -1;
        b.waypoint_count = 0;
        b.waypoint_idx = 0;
        b.progress_time = 0.f;
        b.stuck_fails = 0;
        b.next_replan = 0.f;
        b.blocklist_n = 0;
        b.waypoint_started = 0.f;
        Q_strncpy(b.map_at_init, sv.name, sizeof(b.map_at_init) - 1);
        b.map_at_init[sizeof(b.map_at_init) - 1] = '\0';
        return;  // skip this frame, let server settle
    }

    // Death — issue restart and reset.
    if (sv_player->v.health <= 0) {
        if (b.state != BS_DEAD) {
            b.state = BS_DEAD;
            Cbuf_AddText("restart\n");
        }
        return;
    } else if (b.state == BS_DEAD) {
        b.state = BS_IDLE;
        b.next_replan = 0;
    }

    if (b.target_edict >= 0) target = Bot_Edict(b.target_edict);

    // If our target edict died, freed or became invalid → replan now.
    if (b.target_edict >= 0) {
        int still_valid = 0;
        if (target && !target->free) {
            switch (b.state) {
                case BS_COMBAT:   still_valid = Bot_IsEnemy(target); break;
                case BS_GOTO_KEY: still_valid = Bot_IsKey(target);   break;
                case BS_GOTO_ITEM:still_valid = Bot_IsItem(target);  break;
                case BS_GOTO_EXIT:still_valid = Bot_IsExit(target);  break;
                default: still_valid = 1;
            }
        }
        if (!still_valid) {
            b.next_replan = 0;
            b.target_edict = -1;
        }
    }

    // Replan periodically. While WANDER, the replan check is deferred to
    // wander_replan so the random walk has time to actually move us.
    if (b.state == BS_WANDER) {
        if ((float)host_time >= b.wander_replan) {
            // Exit wander mode and re-decide goal from new position.
            b.state = BS_IDLE;
            b.stuck_fails = 0;
            b.progress_time = 0.f;
            Bot_DecideGoal();
            b.next_replan = host_time + 0.5f;
            target = b.target_edict >= 0 ? Bot_Edict(b.target_edict) : NULL;
        }
    } else if (host_time >= b.next_replan) {
        Bot_DecideGoal();
        b.next_replan = host_time + 0.5f;
        target = b.target_edict >= 0 ? Bot_Edict(b.target_edict) : NULL;
    }

    // Aim + movement target.
    aim_target[0] = aim_target[1] = aim_target[2] = 0.f;
    if (b.state == BS_COMBAT && target) {
        Bot_EntityCenter(target, aim_target);
    } else if (b.state == BS_WANDER) {
        // Re-randomize heading every few seconds.
        if ((float)host_time >= b.wander_until) {
            b.wander_yaw   = (float)(rand() % 360);
            b.wander_until = (float)host_time + 1.0f + ((rand() & 7) * 0.25f);
        }
        // Compute a forward aim point from heading.
        {
            float r = b.wander_yaw * (float)M_PI / 180.f;
            aim_target[0] = sv_player->v.origin[0] + cosf(r) * 256.f;
            aim_target[1] = sv_player->v.origin[1] + sinf(r) * 256.f;
            aim_target[2] = sv_player->v.origin[2];
        }
    } else {
        wp = Bot_CurrentWaypoint();
        if (wp) {
            aim_target[0] = wp[0]; aim_target[1] = wp[1]; aim_target[2] = wp[2];
        } else if (target) {
            // No path; aim straight at target, walk forward (better than nothing).
            Bot_EntityCenter(target, aim_target);
        }
    }

    if (aim_target[0] || aim_target[1] || aim_target[2]) {
        Bot_AimAt(aim_target, b.state == BS_COMBAT);
    }

    // Decide forward / strafe / jump / attack.
    cmd->forwardmove = 0;
    cmd->sidemove    = 0;
    cmd->upmove      = 0;

    if (b.state == BS_COMBAT && target) {
        int yaw_err = Bot_AimError(aim_target);
        float d2 = Bot_Dist2(sv_player->v.origin, target->v.origin);
        do_attack = (yaw_err < 8);
        // Strafe back/forth based on host_time parity to dodge.
        cmd->sidemove = (sinf((float)host_time * 4.f) > 0.f ? 1.f : -1.f) * cl_sidespeed.value * 0.6f;
        // Close to combat distance.
        if (d2 > (bot_combat_dist.value * bot_combat_dist.value))
            cmd->forwardmove = cl_forwardspeed.value;
        else if (d2 < (200.f*200.f))
            cmd->forwardmove = -cl_forwardspeed.value * 0.5f; // back up
    } else if (Bot_CurrentWaypoint() || target || b.state == BS_WANDER) {
        // Always walk forward, scaled by cos of remaining yaw error so the
        // bot crab-walks through turns instead of standing still.
        int yaw_err = Bot_AimError(aim_target);
        float cosw = cosf(yaw_err * (float)M_PI / 180.f);
        if (cosw < 0.3f) cosw = 0.3f;  // never fully stop
        cmd->forwardmove = cl_forwardspeed.value * cosw;

        // Wall avoidance — trace forward from eye height. If something
        // close in front, add sidemove to swerve and bias jump.
        {
            vec3_t eye, fwd, ahead;
            float yaw_r = cl.viewangles[YAW] * (float)M_PI / 180.f;
            eye[0] = sv_player->v.origin[0];
            eye[1] = sv_player->v.origin[1];
            eye[2] = sv_player->v.origin[2] + sv_player->v.view_ofs[2];
            fwd[0] = cosf(yaw_r); fwd[1] = sinf(yaw_r); fwd[2] = 0.f;
            ahead[0] = eye[0] + fwd[0] * 48.f;
            ahead[1] = eye[1] + fwd[1] * 48.f;
            ahead[2] = eye[2];
            {
                trace_t tr = SV_Move(eye, vec3_origin, vec3_origin, ahead, MOVE_NORMAL, sv_player);
                if (tr.fraction < 0.6f) {
                    cmd->sidemove = ((((int)(host_time*2.f)) & 1) ? 1.f : -1.f) * cl_sidespeed.value;
                    if (sv_player->v.flags && ((int)sv_player->v.flags & FL_ONGROUND))
                        do_jump = 1;
                }
            }
        }
        Bot_AdvanceWaypoint();
    } else {
        // Idle wander — small forward nudge to bump into stuff and trigger sight events.
        cmd->forwardmove = cl_forwardspeed.value * 0.25f;
    }

    // Stuck detection — position-based. Every check_interval seconds, if
    // we haven't moved enough, escalate (jump → replan → wander). Skip
    // during WANDER itself (its random heading swap handles unsticking).
    if (b.state != BS_IDLE && b.state != BS_DEAD && b.state != BS_WANDER &&
        b.target_edict >= 0) {
        const float check_interval = 1.5f;
        const float min_movement   = 48.f;
        float dx = sv_player->v.origin[0] - b.progress_pos[0];
        float dy = sv_player->v.origin[1] - b.progress_pos[1];
        float moved2 = dx*dx + dy*dy;

        if (b.progress_time == 0.f) {
            Bot_VecCopy(sv_player->v.origin, b.progress_pos);
            b.progress_time = (float)host_time;
        } else if ((float)host_time - b.progress_time > check_interval) {
            if (moved2 < min_movement * min_movement) {
                // No real progress — escalate.
                b.stuck_fails++;
                b.jump_until = (float)host_time + 0.35f;
                if (bot_debug.value)
                    Con_Printf("[bot] stuck #%d at (%.0f %.0f) tgt=%d wp=%d/%d\n",
                               b.stuck_fails,
                               sv_player->v.origin[0], sv_player->v.origin[1],
                               b.target_edict, b.waypoint_idx, b.waypoint_count);
                // SCRAMBLE — twist yaw by 60-ish degrees so wall avoidance
                // picks a different direction next frame.
                cl.viewangles[YAW] = anglemod(cl.viewangles[YAW] +
                                              ((b.stuck_fails & 1) ? 75.f : -75.f));
                if (b.stuck_fails >= 3) {
                    // Switch to WANDER for ~6s and replan after.
                    b.state = BS_WANDER;
                    b.wander_yaw   = cl.viewangles[YAW];
                    b.wander_until = (float)host_time + 1.0f;  // re-randomize quickly
                    b.wander_replan= (float)host_time + 6.0f;
                    b.waypoint_count = 0;
                    if (bot_debug.value)
                        Con_Printf("[bot] -> WANDER (stuck on tgt=%d)\n", b.target_edict);
                } else {
                    // Try advancing waypoint manually first (might be unreachable),
                    // then replan from current pos.
                    if (b.waypoint_idx + 1 < b.waypoint_count) b.waypoint_idx++;
                    else                                       Bot_RequestPath(b.target_pos);
                }
            } else {
                b.stuck_fails = 0;
            }
            Bot_VecCopy(sv_player->v.origin, b.progress_pos);
            b.progress_time = (float)host_time;
        }

        // Per-waypoint timeout — abandon a waypoint that's not getting reached.
        if (b.waypoint_count > 0 && b.waypoint_started > 0.f &&
            (float)host_time - b.waypoint_started > 4.0f) {
            if (b.waypoint_idx + 1 < b.waypoint_count) {
                b.waypoint_idx++;
                b.waypoint_started = (float)host_time;
            } else {
                Bot_RequestPath(b.target_pos);
            }
        }
    } else {
        b.progress_time = 0.f;
        b.stuck_fails = 0;
    }

    if (host_time < b.jump_until) do_jump = 1;

    Bot_SetButtons(do_attack, do_jump);

    if (bot_debug.value && b.state != BS_IDLE && ((int)(host_time*2.f) != (int)((host_time - host_frametime)*2.f))) {
        const char *stname = "?";
        switch (b.state) {
            case BS_COMBAT:   stname = "COMBAT"; break;
            case BS_GOTO_KEY: stname = "GOTO_KEY"; break;
            case BS_GOTO_ITEM:stname = "GOTO_ITEM"; break;
            case BS_GOTO_EXIT:stname = "GOTO_EXIT"; break;
            case BS_IDLE:     stname = "IDLE"; break;
            case BS_DEAD:     stname = "DEAD"; break;
        }
        Con_Printf("[bot] %s tgt=%d wp=%d/%d hp=%d\n",
                   stname, b.target_edict, b.waypoint_idx, b.waypoint_count,
                   (int)sv_player->v.health);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int Bot_Active(void) { return bot.value != 0.f && sv.active && cls.signon == SIGNONS; }

void Bot_Frame(usercmd_t *cmd)
{
    if (!Bot_Active()) return;
    if (!sv_player || sv_player->free) return;
    Bot_DriveFrame(cmd);
}

static void Bot_Status_f(void)
{
    const char *stname = "?";
    if (!Bot_Active()) { Con_Printf("[bot] inactive\n"); return; }
    switch (b.state) {
        case BS_COMBAT:   stname = "COMBAT"; break;
        case BS_GOTO_KEY: stname = "GOTO_KEY"; break;
        case BS_GOTO_ITEM:stname = "GOTO_ITEM"; break;
        case BS_GOTO_EXIT:stname = "GOTO_EXIT"; break;
        case BS_IDLE:     stname = "IDLE"; break;
        case BS_DEAD:     stname = "DEAD"; break;
        case BS_WANDER:   stname = "WANDER"; break;
    }
    Con_Printf("[bot] state=%s target_edict=%d waypoints=%d/%d blocklist=%d\n",
               stname, b.target_edict, b.waypoint_idx, b.waypoint_count, b.blocklist_n);
}

void Bot_Init(void)
{
    Cvar_RegisterVariable(&bot);
    Cvar_RegisterVariable(&bot_aware_radius);
    Cvar_RegisterVariable(&bot_pickup_radius);
    Cvar_RegisterVariable(&bot_turn_speed);
    Cvar_RegisterVariable(&bot_combat_dist);
    Cvar_RegisterVariable(&bot_debug);
    Cmd_AddCommand("bot_status", Bot_Status_f);

    b.state = BS_IDLE;
    b.target_edict = -1;
}

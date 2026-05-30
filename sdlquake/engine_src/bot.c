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

cvar_t bot              = { "bot",              "0" };
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
    BS_WANDER,     // nothing path-reachable — roam to explore/find new goals
    BS_DEAD,
} bot_state_t;

// STATE_BOTTOM in the game DLL — a func_button at this state is ready to
// be pressed. Anything else (UP/TOP/DOWN) means it's mid-cycle.
#define BOT_BUTTON_STATE_READY 1

#define BOT_MAX_WAYPOINTS 32

// Kept in sync with sim_nav.c's NAV_EDGE_* enum. The drive layer uses
// this to press jump for jump-up edges, wait on plat-ride edges, and
// aim+fire on shoot-link edges.
enum {
    BOT_EDGE_WALK        = 0,
    BOT_EDGE_JUMP_UP     = 1,
    BOT_EDGE_DROP_DOWN   = 2,
    BOT_EDGE_PLAT_RIDE   = 3,
    BOT_EDGE_TELEPORT    = 4,
    BOT_EDGE_PLAT_LINK   = 5,
    BOT_EDGE_SHOOT_LINK  = 6,
    BOT_EDGE_BUTTON_LINK = 7,
    BOT_EDGE_TRAIN_RIDE  = 8,
    BOT_EDGE_TRAIN_LINK  = 9,
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

    // Stuck recovery: sampled displacement; when the bot keeps trying to walk
    // but isn't moving, briefly reverse+strafe+hop to unwedge.
    vec3_t       last_pos;
    float        stuck_sample_time;
    int          stuck_count;
    float        recover_until;
    int          recover_sign;

    // Wander/explore: ring of recently chosen roam targets, so the fallback
    // doesn't ping-pong between the same two spots.
    vec3_t       visited[8];
    int          visited_head;

    // Combat give-up: the bot has no combat pathing, so an enemy across a gap
    // (or an unkillable one) would pin it forever. Track how long we've fought
    // one target and whether we're hurting it; if not, blacklist it and move
    // on so navigation resumes.
    int          combat_target;     // edict num currently engaged, or -1
    float        combat_since;      // host_time we locked onto it
    float        combat_tgt_hp0;    // its health when we engaged
    int          bl_ent[8];         // blacklisted enemy edict nums
    float        bl_until[8];       // host_time the blacklist entry expires
    int          bl_head;
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
        case BOT_EDGE_WALK:       return "walk";
        case BOT_EDGE_JUMP_UP:    return "jump";
        case BOT_EDGE_DROP_DOWN:  return "drop";
        case BOT_EDGE_PLAT_RIDE:  return "ride";
        case BOT_EDGE_TELEPORT:   return "tele";
        case BOT_EDGE_PLAT_LINK:   return "plat";
        case BOT_EDGE_SHOOT_LINK:  return "shoot";
        case BOT_EDGE_BUTTON_LINK: return "btn";
        case BOT_EDGE_TRAIN_RIDE:  return "train";
        case BOT_EDGE_TRAIN_LINK:  return "tlink";
    }
    return "?";
}

// Is there a func_button within `radius` of `pos` that's still ready
// to be pressed? Used to gate the BUTTON_LINK "don't advance" logic —
// once the button's state leaves BOTTOM (we've touched it) the bot is
// allowed to move on.
static int Bot_NearbyButtonReady(const vec3_t pos, float radius)
{
    int i;
    float r2 = radius * radius;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = Bot_Edict(i);
        vec3_t c;
        if (!e || e->free || !e->v.classname) continue;
        if (strcmp(e->v.classname, "func_button") != 0) continue;
        if ((int)e->v.health != 0) continue;
        if ((int)e->v.state != BOT_BUTTON_STATE_READY) continue;
        Bot_EntityCenter(e, c);
        if (Bot_Dist2(pos, c) < r2) return 1;
    }
    return 0;
}

// STATE_TOP in the game DLL — a func_door at this state is fully open.
#define BOT_DOOR_STATE_TOP 0

// After pressing a button that drives a HORIZONTAL bridge-door, hold until
// that door has finished extending. A bridge that slides toward the bot
// (e.g. out of a side wall) covers its near boarding end LAST, so stepping
// on the instant the button is pressed drops the bot into the pit. Vertical
// lift-doors are NOT gated here — their ride logic boards-then-rides, and
// waiting for their travel end would strand the bot at the wrong level.
// Returns 1 (clear to board) when no bridge-door is involved.
static int Bot_TriggeredBridgeOpen(const vec3_t pos, float radius)
{
    int i, j;
    float r2 = radius * radius;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *bt = Bot_Edict(i);
        vec3_t c;
        if (!bt || bt->free || !bt->v.classname) continue;
        if (strcmp(bt->v.classname, "func_button") != 0) continue;
        if (!bt->v.target) continue;
        Bot_EntityCenter(bt, c);
        if (Bot_Dist2(pos, c) >= r2) continue;
        for (j = 1; j < sv.num_edicts; j++) {
            edict_t *d = Bot_Edict(j);
            if (!d || d->free || !d->v.classname) continue;
            if (strcmp(d->v.classname, "door") != 0 &&
                strcmp(d->v.classname, "func_door") != 0) continue;
            if (!d->v.targetname ||
                strcmp(d->v.targetname, bt->v.target) != 0) continue;
            float adz = fabsf(d->v.pos2[2] - d->v.pos1[2]);
            float adx = fabsf(d->v.pos2[0] - d->v.pos1[0]);
            float ady = fabsf(d->v.pos2[1] - d->v.pos1[1]);
            float sz  = d->v.maxs[2] - d->v.mins[2];
            int horizontal = (adz < 24.f) && (adx > 24.f || ady > 24.f);
            int flat       = sz < 32.f;
            if (horizontal && flat)
                return ((int)d->v.state == BOT_DOOR_STATE_TOP);
        }
    }
    return 1;
}

// Is a func_train currently parked (~stationary) with its footprint over
// `pos`? Gates boarding: the bot must not step toward a train's standing
// anchor until the train is actually there, or it walks into the gap.
static int Bot_TrainParkedAt(const vec3_t pos, float pad)
{
    int i;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = Bot_Edict(i);
        if (!e || e->free || !e->v.classname) continue;
        if (strcmp(e->v.classname, "train") != 0 &&
            strcmp(e->v.classname, "func_train") != 0) continue;
        float vx = e->v.velocity[0], vy = e->v.velocity[1], vz = e->v.velocity[2];
        if (vx*vx + vy*vy + vz*vz > 1.0f) continue;   // still moving
        float minx = e->v.origin[0] + e->v.mins[0] - pad;
        float maxx = e->v.origin[0] + e->v.maxs[0] + pad;
        float miny = e->v.origin[1] + e->v.mins[1] - pad;
        float maxy = e->v.origin[1] + e->v.maxs[1] + pad;
        if (pos[0] >= minx && pos[0] <= maxx &&
            pos[1] >= miny && pos[1] <= maxy)
            return 1;
    }
    return 0;
}

// Returns the func_train the player is currently standing on (footprint XY
// over the player, train top ~ player feet), else NULL. Distinguishes
// "boarding from a ledge" from "already riding".
static edict_t *Bot_OnTrain(void)
{
    int i;
    if (!sv_player) return NULL;
    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = Bot_Edict(i);
        if (!e || e->free || !e->v.classname) continue;
        if (strcmp(e->v.classname, "train") != 0 &&
            strcmp(e->v.classname, "func_train") != 0) continue;
        float px   = sv_player->v.origin[0];
        float py   = sv_player->v.origin[1];
        float feet = sv_player->v.origin[2] + sv_player->v.mins[2];
        float top  = e->v.origin[2] + e->v.maxs[2];
        float minx = e->v.origin[0] + e->v.mins[0] - 8.f;
        float maxx = e->v.origin[0] + e->v.maxs[0] + 8.f;
        float miny = e->v.origin[1] + e->v.mins[1] - 8.f;
        float maxy = e->v.origin[1] + e->v.maxs[1] + 8.f;
        if (px >= minx && px <= maxx && py >= miny && py <= maxy &&
            feet >= top - 8.f && feet <= top + 24.f)
            return e;
    }
    return NULL;
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

typedef struct { int ent; float d2; } bot_cand_t;

// Sort candidates ascending by squared distance (insertion sort — n is tiny),
// then return the edict number of the NEAREST one the navmesh can actually
// reach, having committed that path into b.waypoints / b.target_pos. This is
// what stops the bot locking onto the closest goal and freezing when it has no
// path to it — e.g. start.bsp's four episode slipgates where only e1m1 is
// reachable (e2/e3/e4 sit behind registered-only gates), or a far key that is
// itself walled off behind the door it would open. `use_center` paths to the
// bbox centre (exits are brush triggers with no origin) vs the origin
// (keys/items). Returns -1 if none are reachable.
static int Bot_PickReachable(bot_cand_t *c, int n, int use_center, vec3_t out_pos)
{
    int i, j;
    for (i = 1; i < n; i++) {
        bot_cand_t key = c[i];
        for (j = i - 1; j >= 0 && c[j].d2 > key.d2; j--) c[j+1] = c[j];
        c[j+1] = key;
    }
    for (i = 0; i < n; i++) {
        edict_t *e = Bot_Edict(c[i].ent);
        vec3_t to;
        if (!e || e->free) continue;
        if (use_center) Bot_EntityCenter(e, to); else Bot_VecCopy(e->v.origin, to);
        if (Bot_RequestPath(to)) {
            Bot_VecCopy(to, out_pos);
            return c[i].ent;
        }
    }
    return -1;
}

#define BOT_MAX_CAND 64

static int Bot_IsBlacklisted(int ent)
{
    int i;
    for (i = 0; i < 8; i++)
        if (b.bl_ent[i] == ent && host_time < b.bl_until[i]) return 1;
    return 0;
}

static void Bot_Blacklist(int ent, float secs)
{
    b.bl_ent[b.bl_head]   = ent;
    b.bl_until[b.bl_head] = host_time + secs;
    b.bl_head = (b.bl_head + 1) & 7;
}

// When no real goal is path-reachable, roam: probe several compass directions
// at a couple of ranges, nav_path to each, and head for the best reachable
// end-point. If we know where the level exit is (`lure`, even when it's not yet
// path-reachable), bias strongly toward reachable frontier nodes nearest it —
// so exploration trends toward progress and tends to cross the one-way drop /
// lift that finally puts the exit region within pathing range. Otherwise just
// favour long fresh paths. Recently-visited spots are penalised so the fallback
// doesn't ping-pong. This keeps the bot moving and fighting on large maps
// instead of freezing when the only known goal sits behind a navmesh gap.
static int Bot_PickWander(vec3_t out_pos, const float *lure)
{
    static const float dirs[8][2] = {
        { 1.f, 0.f}, { 0.f, 1.f}, {-1.f, 0.f}, { 0.f,-1.f},
        { 0.7f,0.7f},{-0.7f,0.7f},{0.7f,-0.7f},{-0.7f,-0.7f} };
    static const float ranges[2] = { 448.f, 960.f };
    static unsigned wander_tick = 0;
    vec3_t scratch[BOT_MAX_WAYPOINTS];
    unsigned char skind[BOT_MAX_WAYPOINTS];
    unsigned int items = (unsigned int)sv_player->v.items;
    float best_score = -1e30f;
    int found = 0, di, ri, h;
    vec3_t best; best[0] = best[1] = best[2] = 0.f;

    if (!g_game_api || !g_game_api->nav_path) return 0;

    for (di = 0; di < 8; di++) for (ri = 0; ri < 2; ri++) {
        vec3_t cand, end;
        float score;
        int n;
        cand[0] = sv_player->v.origin[0] + dirs[di][0] * ranges[ri];
        cand[1] = sv_player->v.origin[1] + dirs[di][1] * ranges[ri];
        cand[2] = sv_player->v.origin[2];
        n = g_game_api->nav_path((float *)sv_player->v.origin, cand,
                                 (float *)scratch, skind, items, BOT_MAX_WAYPOINTS);
        if (n <= 1) continue;                          // no/trivial path
        Bot_VecCopy(scratch[n - 1], end);
        // Coverage-primary (path length + revisit penalty) with a gentle pull
        // toward the exit. A strong pull beelines toward the exit's coordinates,
        // which backfires when the topological entry to the exit region is in a
        // different direction (true on real maps) — so keep the lure weak.
        score = (float)n;
        if (lure) score -= 0.012f * sqrtf(Bot_Dist2(end, lure));
        for (h = 0; h < 8; h++)
            if (Bot_Dist2(end, b.visited[h]) < (320.f * 320.f)) score -= 30.f;
        score += (float)((wander_tick + di) & 3) * 0.25f;   // break ties
        if (score > best_score) { best_score = score; Bot_VecCopy(end, best); found = 1; }
    }
    wander_tick++;
    if (found) { Bot_VecCopy(best, out_pos); return 1; }
    return 0;
}

static void Bot_DecideGoal(void)
{
    int i, ent;
    edict_t *best_enemy = NULL;
    float best_enemy_d2 = 1e30f;
    float aware2  = bot_aware_radius.value  * bot_aware_radius.value;
    float pickup2 = bot_pickup_radius.value * bot_pickup_radius.value;
    vec3_t ppos;

    bot_cand_t keys[BOT_MAX_CAND], items[BOT_MAX_CAND], exits[16];
    int nkeys = 0, nitems = 0, nexits = 0;

    if (!sv_player) return;
    Bot_VecCopy(sv_player->v.origin, ppos);

    for (i = 1; i < sv.num_edicts; i++) {
        edict_t *e = Bot_Edict(i);
        float d2;
        if (!e || e->free) continue;
        /* Items / keys turn SOLID_NOT on pickup (key_touch + h_touch),
         * but the entity sticks around with classname intact. Without
         * this filter the goal cascade keeps re-targeting a consumed
         * key and the bot loops on its origin instead of progressing. */
        if (e->v.solid == SOLID_NOT && !Bot_IsEnemy(e) && !Bot_IsExit(e))
            continue;
        d2 = Bot_Dist2(ppos, e->v.origin);

        if (Bot_IsEnemy(e)) {
            if (Bot_IsBlacklisted(i)) continue;     // gave up on this one recently
            if (d2 < aware2 && d2 < best_enemy_d2 && Bot_LineOfSight(ppos, e->v.origin)) {
                best_enemy = e;
                best_enemy_d2 = d2;
            }
        } else if (Bot_IsKey(e)) {
            if (nkeys < BOT_MAX_CAND) { keys[nkeys].ent = i; keys[nkeys].d2 = d2; nkeys++; }
        } else if (Bot_IsItem(e)) {
            if (d2 < pickup2 && Bot_ItemUseful(e) && nitems < BOT_MAX_CAND) {
                items[nitems].ent = i; items[nitems].d2 = d2; nitems++;
            }
        } else if (Bot_IsExit(e)) {
            vec3_t c;
            Bot_EntityCenter(e, c);
            if (nexits < 16) {
                exits[nexits].ent = i;
                exits[nexits].d2  = Bot_Dist2(ppos, c);
                nexits++;
            }
        }
    }

    // Enemies first (combat is straight-line, no nav path needed). Then the
    // nearest REACHABLE key > exit > useful item.
    if (best_enemy) {
        int en = NUM_FOR_EDICT(best_enemy);
        if (en == b.combat_target) {
            // Still locked on the same enemy. If we've fought it a while
            // without drawing blood (it's across a gap / behind a grate) or
            // far too long overall, give up: blacklist it and resume nav so a
            // single unreachable monster can't pin the bot for the whole map.
            float dmg = b.combat_tgt_hp0 - best_enemy->v.health;
            if ((host_time - b.combat_since > 6.0f && dmg <= 0.f) ||
                (host_time - b.combat_since > 14.0f)) {
                Bot_Blacklist(en, 12.0f);
                if (bot_debug.value) Con_Printf("[bot] give up enemy %d\n", en);
                best_enemy = NULL;
            }
        } else {
            b.combat_target  = en;
            b.combat_since   = host_time;
            b.combat_tgt_hp0 = best_enemy->v.health;
        }
    }
    if (best_enemy) {
        b.state = BS_COMBAT;
        b.target_edict = NUM_FOR_EDICT(best_enemy);
        Bot_VecCopy(best_enemy->v.origin, b.target_pos);
        return;
    }
    b.combat_target = -1;   // not fighting — reset the engage timer
    if ((ent = Bot_PickReachable(keys, nkeys, 0, b.target_pos)) >= 0) {
        b.state = BS_GOTO_KEY; b.target_edict = ent; return;
    }
    if ((ent = Bot_PickReachable(exits, nexits, 1, b.target_pos)) >= 0) {
        b.state = BS_GOTO_EXIT; b.target_edict = ent; return;
    }
    if ((ent = Bot_PickReachable(items, nitems, 0, b.target_pos)) >= 0) {
        b.state = BS_GOTO_ITEM; b.target_edict = ent; return;
    }

    // Nothing reachable to pursue — wander instead of freezing, biased toward
    // the nearest known exit (exits[] was sorted ascending by Bot_PickReachable)
    // so exploration trends toward the level's goal.
    {
        vec3_t wp, lurepos;
        const float *lure = NULL;
        if (nexits > 0) {
            edict_t *xe = Bot_Edict(exits[0].ent);
            if (xe && !xe->free) { Bot_EntityCenter(xe, lurepos); lure = lurepos; }
        }
        if (Bot_PickWander(wp, lure) && Bot_RequestPath(wp)) {
            b.state = BS_WANDER;
            b.target_edict = -1;
            Bot_VecCopy(wp, b.target_pos);
            Bot_VecCopy(wp, b.visited[b.visited_head]);
            b.visited_head = (b.visited_head + 1) & 7;
            if (bot_debug.value)
                Con_Printf("[bot] WANDER -> (%.0f %.0f %.0f)\n", wp[0], wp[1], wp[2]);
            return;
        }
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
    // the bot's bbox physically reaches the goal entity (key/item →
    // pickup fires). The 48-unit XY radius is wider than the player
    // bbox, so without this the bot stops short.
    if (b.waypoint_idx >= b.waypoint_count - 1) return;
    // If the next edge is a button-link, the current waypoint is the
    // button-anchor and we must stay here until the bbox actually
    // overlaps the button (state leaves BOTTOM). Otherwise the bot
    // sails past at 48 units without ever triggering it.
    if (b.waypoint_kinds[b.waypoint_idx + 1] == BOT_EDGE_BUTTON_LINK) {
        if (Bot_NearbyButtonReady(wp, 96.f)) return;       // not pressed yet
        if (!Bot_TriggeredBridgeOpen(wp, 96.f)) return;    // bridge still extending
    }
    // Train board-gate: when the NEXT edge steps onto a train (TRAIN_LINK
    // boarding, or a TRAIN_RIDE leaving this ledge) and we're not already
    // aboard, hold until the train is parked at that standing anchor — else
    // we step into the gap.
    if ((b.waypoint_kinds[b.waypoint_idx + 1] == BOT_EDGE_TRAIN_LINK ||
         b.waypoint_kinds[b.waypoint_idx + 1] == BOT_EDGE_TRAIN_RIDE) &&
        !Bot_OnTrain()) {
        const float *next = b.waypoints[b.waypoint_idx + 1];
        if (!Bot_TrainParkedAt(next, 8.f)) return;
    }
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
        int q;
        if (bot_debug.value)
            Con_Printf("[bot] map change %s -> %s, resetting\n",
                       b.map_at_init, sv.name);
        b.state = BS_IDLE;
        b.target_edict = -1;
        b.waypoint_count = 0;
        b.waypoint_idx = 0;
        b.next_replan = 0.f;
        b.combat_target = -1;
        for (q = 0; q < 8; q++) b.bl_until[q] = 0.f;   // edict nums differ per map
        b.recover_until = 0.f;
        b.stuck_count = 0;
        b.visited_head = 0;
        Bot_VecCopy(sv_player->v.origin, b.last_pos);
        b.stuck_sample_time = host_time;
        Bot_SetButtons(0, 0);   // release any attack held from intermission
        Q_strncpy(b.map_at_init, sv.name, sizeof(b.map_at_init) - 1);
        b.map_at_init[sizeof(b.map_at_init) - 1] = '\0';
        return;
    }

    // Intermission: a real (non-spawnflags-1) trigger_changelevel freezes the
    // player on the end-of-level camera until a button is pressed
    // (IntermissionThink in game.dll waits on button0/1/2 after exittime).
    // The test maps skip this via spawnflags 1, but every id1 episode map goes
    // through it — so without pressing attack here the bot sits on the
    // scoreboard forever and never advances start->e1m1->...  key_dest stays
    // key_game during intermission, so Bot_Frame still reaches us.
    if (cl.intermission) {
        cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;
        Bot_SetButtons(1, 0);
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

        // Lift wait: when the next waypoint is directly above or below
        // us (same XY, big dz) we're standing on a lift — hold still
        // so we don't walk off before it moves. Above = waiting for
        // lift to rise; below = riding the lift down.
        {
            const float *cur = Bot_CurrentWaypoint();
            int kind = Bot_CurrentWaypointKind();
            float dx = cur[0] - sv_player->v.origin[0];
            float dy = cur[1] - sv_player->v.origin[1];
            float dz = cur[2] - sv_player->v.origin[2];
            float xy2 = dx*dx + dy*dy;
            if (xy2 < (32.f*32.f) && (dz > 64.f || dz < -64.f)) {
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
            // Train ride: standing on the ferry, the next stop is the far
            // corner (same Z, big XY). Hold still and let SV_PushMove carry
            // us; Bot_AdvanceWaypoint fires on XY proximity when the train
            // parks at the far corner, stepping us off onto the ledge.
            if (kind == BOT_EDGE_TRAIN_RIDE && Bot_OnTrain()) {
                cmd->forwardmove = 0;
                cmd->sidemove    = 0;
            }
        }

        Bot_AdvanceWaypoint();
    }
    // else: no waypoint (no path or IDLE). Stand still.

    // Stuck recovery: if we've been trying to walk but not displacing, override
    // with a brief reverse + strafe + hop to break free of a geometry snag.
    if (host_time < b.recover_until) {
        cmd->forwardmove = -cl_forwardspeed.value * 0.8f;
        cmd->sidemove    = (float)b.recover_sign * cl_sidespeed.value;
        cl.viewangles[YAW] = anglemod(cl.viewangles[YAW] + (float)b.recover_sign * 8.f);
        do_jump = 1;
    }

    Bot_SetButtons(do_attack, do_jump);

    // Stuck bookkeeping (sampled ~3x/sec). Only counts while actually trying to
    // move, so deliberate holds (lift wait, train ride, button wait with zeroed
    // movement) don't false-trigger recovery.
    if (host_time - b.stuck_sample_time > 0.34f) {
        float dd = Bot_Dist2(sv_player->v.origin, b.last_pos);
        int tried = (cmd->forwardmove != 0.f || cmd->sidemove != 0.f);
        if (tried && dd < (18.f * 18.f)) b.stuck_count++;
        else                             b.stuck_count = 0;
        Bot_VecCopy(sv_player->v.origin, b.last_pos);
        b.stuck_sample_time = host_time;
        if (b.stuck_count >= 3 && host_time >= b.recover_until) {   // ~1s wedged
            b.recover_until = host_time + 0.5f;
            b.recover_sign  = (b.recover_sign <= 0) ? 1 : -1;
            b.stuck_count   = 0;
            if (bot_debug.value) Con_Printf("[bot] stuck -> recover\n");
        }
    }

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
            case BS_WANDER:      stname = "WANDER";      break;
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
    // Hand controls back to the user when the menu/console is up, the
    // game is paused, or the in-game editor is open — otherwise the
    // bot keeps driving inputs and the player can't take over without
    // disabling the cvar.
    {
        extern int Editor_IsOpen(void);
        if (key_dest != key_game || cl.paused || Editor_IsOpen()) {
            Bot_SetButtons(0, 0);
            return;
        }
    }
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

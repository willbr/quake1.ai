// sim_ai.c -- AI brain side-table, sense filter, FSM.
// State for each monster lives in s_brains[edict_num], not in edict_t.

#include "sim.h"
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static ai_brain_t s_brains[SIM_MAX_BRAINS];

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_AI_Init(void) {
    memset(s_brains, 0, sizeof(s_brains));
}

void Sim_AI_LevelInit(void) {
    memset(s_brains, 0, sizeof(s_brains));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
ai_brain_t *Sim_AI_GetBrain(edict_t *e) {
    if (!e) return 0;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= SIM_MAX_BRAINS) return 0;
    if (!s_brains[n].in_use) return 0;
    return &s_brains[n];
}

ai_brain_t *Sim_AI_RegisterMonster(edict_t *e) {
    if (!e) return 0;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= SIM_MAX_BRAINS) return 0;
    ai_brain_t *b = &s_brains[n];
    if (b->in_use) return b;
    memset(b, 0, sizeof(*b));
    b->in_use             = 1;
    b->edict_num          = n;
    b->state              = AI_IDLE;
    b->state_entered_time = g->time;
    b->target_edict       = -1;
    b->patrol_route_id    = -1;
    b->sense_sight_range  = 1024.0f;
    b->sense_hearing_mult = 1.0f;
    b->next_tick_time     = g->time;
    return b;
}

void Sim_AI_UnregisterByEdictNum(int n) {
    if (n < 0 || n >= SIM_MAX_BRAINS) return;
    s_brains[n].in_use = 0;
}

// ---------------------------------------------------------------------------
// Iteration (for the imgui panel)
// ---------------------------------------------------------------------------
ai_brain_t *Sim_AI_IterFirst(void) {
    for (int i = 0; i < SIM_MAX_BRAINS; i++)
        if (s_brains[i].in_use) return &s_brains[i];
    return 0;
}

ai_brain_t *Sim_AI_IterNext(ai_brain_t *prev) {
    if (!prev) return 0;
    int i = (int)(prev - s_brains) + 1;
    for (; i < SIM_MAX_BRAINS; i++)
        if (s_brains[i].in_use) return &s_brains[i];
    return 0;
}

// ---------------------------------------------------------------------------
// Sense filter
// ---------------------------------------------------------------------------
static float falloff(float distance, float ref_radius) {
    float f = 1.0f - (distance / (2.0f * ref_radius));
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

static int los_clear(const vec3_t a, const vec3_t b) {
    eng->SV_Traceline(a, b, /*nomonsters=*/1, /*skip=*/0);
    return g->trace_fraction == 1.0f;
}

static float sense_intensity(ai_brain_t *b, edict_t *e, const stimulus_t *s) {
    float d = 0;
    {
        float dx = s->origin[0] - e->v.origin[0];
        float dy = s->origin[1] - e->v.origin[1];
        float dz = s->origin[2] - e->v.origin[2];
        d = (float)sqrt(dx*dx + dy*dy + dz*dz);
    }
    float ref = 0;
    float los = 1.0f;
    switch (s->kind) {
        case STIM_SOUND:
            ref = 1024.0f * b->sense_hearing_mult; break;
        case STIM_SIGHT_ENTITY:
            ref = b->sense_sight_range;
            los = los_clear(e->v.origin, s->origin) ? 1.0f : 0.0f;
            break;
        case STIM_CORPSE:
            ref = 512.0f;
            los = los_clear(e->v.origin, s->origin) ? 1.0f : 0.0f;
            break;
        case STIM_PROP_BROKEN:
            ref = 768.0f; break;
        case STIM_LIGHT_CHANGE:
            ref = 384.0f; break;
        case STIM_SMOKE:
            ref = b->sense_sight_range; break;
        default:
            return 0.0f;
    }
    return s->intensity * falloff(d, ref) * los;
}

static void sense_tick(ai_brain_t *b, edict_t *e) {
    stimulus_t recents[16];
    float since = b->next_tick_time - 1.0f;     // 1s lookback overlap
    int n = Stim_QueryNear(e->v.origin,
                           b->sense_sight_range * 2.0f,
                           since, recents, 16);

    for (int i = 0; i < n; i++) {
        // Don't react to your own emissions.
        if (recents[i].source_edict == b->edict_num) continue;
        float eff = sense_intensity(b, e, &recents[i]);
        b->alert_level += eff * 0.5f;
    }

    // Decay
    b->alert_level *= 0.95f;
    if (b->alert_level < 0)    b->alert_level = 0;
    if (b->alert_level > 1.0f) b->alert_level = 1.0f;

    // Track strongest sight stim this tick for FSM bookkeeping.
    int    saw_player_full = 0;
    vec3_t player_pos = {0, 0, 0};

    for (int i = 0; i < n; i++) {
        if (recents[i].source_edict == b->edict_num) continue;
        if (recents[i].kind != STIM_SIGHT_ENTITY) continue;
        if (recents[i].source_edict != 1) continue;   // edict 1 = player in single-player
        float eff = sense_intensity(b, e, &recents[i]);
        if (eff > 0.7f) {
            saw_player_full = 1;
            player_pos[0] = recents[i].origin[0];
            player_pos[1] = recents[i].origin[1];
            player_pos[2] = recents[i].origin[2];
        }
    }

    if (saw_player_full) {
        b->last_known_pos[0] = player_pos[0];
        b->last_known_pos[1] = player_pos[1];
        b->last_known_pos[2] = player_pos[2];
        b->target_edict = 1;
    }

    // FSM transitions
    float since_state = g->time - b->state_entered_time;
    ai_state_t prev = b->state;

    switch (b->state) {
    case AI_IDLE:
        if (b->alert_level > 0.25f) b->state = AI_SUSPICIOUS;
        break;
    case AI_SUSPICIOUS:
        if (saw_player_full || b->alert_level > 0.6f) b->state = AI_SEARCHING;
        else if (n == 0 && since_state > 8.0f && b->alert_level < 0.05f) b->state = AI_IDLE;
        break;
    case AI_SEARCHING:
        if (saw_player_full && los_clear(e->v.origin, b->last_known_pos))
            b->state = AI_COMBAT;
        else if (n == 0 && since_state > 20.0f) b->state = AI_IDLE;
        break;
    case AI_COMBAT:
        if (!saw_player_full && since_state > 3.0f) b->state = AI_SEARCHING;
        break;
    }

    if (b->state != prev) b->state_entered_time = g->time;
}

// ---------------------------------------------------------------------------
// Behavior helpers
// ---------------------------------------------------------------------------
static void face_point(edict_t *e, const vec3_t target) {
    float dx = target[0] - e->v.origin[0];
    float dy = target[1] - e->v.origin[1];
    vec3_t v = {dx, dy, 0};
    e->v.ideal_yaw = eng->VectorToYaw(v);
    eng->SV_ChangeYaw(e);
}

static void behavior_tick(ai_brain_t *b, edict_t *e) {
    switch (b->state) {
    case AI_IDLE:
        // Fall through to vanilla Quake AI (handled elsewhere).
        break;
    case AI_SUSPICIOUS:
    case AI_SEARCHING:
        face_point(e, b->last_known_pos);
        eng->SV_WalkMove(e, e->v.angles[1], 8.0f);
        break;
    case AI_COMBAT:
        // Fall through to vanilla Quake combat AI.
        break;
    }
}

// ---------------------------------------------------------------------------
// Frame tick — 10 Hz per brain; pushes live data to the imgui AI panel.
// ---------------------------------------------------------------------------
void Sim_AI_Frame(void) {
    float now = g->time;

    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (now < b->next_tick_time) continue;

        // Find the edict for this brain by walking entity list.
        // O(N^2) is acceptable for M1 with typical monster counts.
        edict_t *e = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == b->edict_num) { e = cur; break; }
        }
        if (!e) { b->in_use = 0; continue; }
        if (e->v.health <= 0) {
            b->state = AI_IDLE;
            b->alert_level = 0;
            continue;
        }

        sense_tick(b, e);
        behavior_tick(b, e);
        b->next_tick_time = now + (1.0f / SIM_AI_TICK_HZ);
    }

    // Push to imgui panel.
    if (eng->ImguiAI_Active && eng->ImguiAI_Active()) {
        eng->ImguiAI_Clear();
        for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
            eng->ImguiAI_Push(b->edict_num, (int)b->state, b->alert_level,
                              b->last_known_pos, b->target_edict);
        }
    }
}

// ---------------------------------------------------------------------------
// Patrol routes (stubs until Task 13)
// ---------------------------------------------------------------------------
void Sim_Patrol_RegisterNode(edict_t *e) { (void)e; }
void Sim_Patrol_Resolve(void) {}
edict_t *Sim_Patrol_FindByTargetname(const char *n) { (void)n; return 0; }

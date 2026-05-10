// sim_stimulus.c -- Central event bus. All systems emit; AI consumes.

#include "sim.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static stimulus_t s_ring[SIM_STIM_RING_SIZE];
static int        s_head;        // next write index
static int        s_count;       // number of valid entries (<= ring size)

void Stim_Init(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_head  = 0;
    s_count = 0;
}

void Stim_LevelInit(void) {
    Stim_Init();
}

void Stim_Emit(const stimulus_t *s) {
    if (!s) return;
    s_ring[s_head] = *s;
    s_ring[s_head].time = g->time;
    s_head = (s_head + 1) % SIM_STIM_RING_SIZE;
    if (s_count < SIM_STIM_RING_SIZE) s_count++;
}

static float vec_dist(const vec3_t a, const vec3_t b) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return (float)sqrt(dx*dx + dy*dy + dz*dz);
}

int Stim_QueryNear(const vec3_t pos,
                   float radius,
                   float since_time,
                   stimulus_t *out,
                   int max_out)
{
    if (!out || max_out <= 0) return 0;

    int written = 0;
    for (int i = 0; i < s_count && written < max_out; i++) {
        // Walk newest-to-oldest so the caller sees most-recent first.
        int idx = (s_head - 1 - i + SIM_STIM_RING_SIZE) % SIM_STIM_RING_SIZE;
        const stimulus_t *s = &s_ring[idx];
        if (s->time < since_time) continue;
        if (g->time - s->time > SIM_STIM_MAX_AGE_S) continue;
        if (vec_dist(s->origin, pos) > radius) continue;
        out[written++] = *s;
    }
    return written;
}

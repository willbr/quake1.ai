// perf.c -- scoped per-frame timing.
//
// Storage model:
//   - One bump arena per frame, holding scope nodes for that frame.
//   - A ring of N recent frames (frame_ring), 0 = current, 1 = last completed.
//   - When capturing, completed frames are also appended to a separate
//     capture buffer until N captured frames are accumulated, then flushed
//     to a Chrome-trace JSON.
//
// Time source: SDL_GetPerformanceCounter / Frequency (CPU cycle counter on
// most platforms). Sys_FloatTime would also work but performance counters
// give us a ~ns resolution without the double-precision conversion in the
// hot path.
//
// Thread model: single-threaded. All push/pop calls must come from the main
// thread. Sound and bake worker threads don't push scopes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "quakedef.h"
#include "perf.h"

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define PERF_FRAME_HISTORY     256   // for the live frametime graph
#define PERF_MAX_NODES_FRAME   2048  // hard cap per frame (overrun -> truncate)
#define PERF_MAX_DEPTH         32
#define PERF_MAX_CAPTURE_FRAMES 1200 // sanity cap on `profile <n>`

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------
struct perf_scope_view_s {
    const char *name;       // assumed static-lifetime
    double      start_s;    // seconds since frame begin
    double      dur_s;
    int         depth;
    int         parent;     // index into the same arena, -1 for root
    int         first_child;
    int         next_sibling;
};

typedef struct {
    perf_scope_view_t nodes[PERF_MAX_NODES_FRAME];
    int               n_nodes;
    double            frame_start_s;
    double            frame_end_s;
    int               valid;  // 0 if push/pop mismatch
} perf_frame_t;

// Push/pop stack for the in-progress frame.
static int     s_stack[PERF_MAX_DEPTH];
static int     s_stack_top;
static int     s_in_frame;

static perf_frame_t s_frame_ring[PERF_FRAME_HISTORY];
static int          s_ring_head;        // index of current frame
static int          s_ring_count;       // number of completed frames in ring

// Per-frame total ms history (decoupled from the ring so the live graph keeps
// rolling even when individual scope storage is dropped).
static float        s_frametime_ms[PERF_FRAME_HISTORY];
static int          s_ft_head;          // next slot to write (newest is head-1)
static int          s_ft_count;

// Capture state. A capture can be bounded by frame count, wall-clock time,
// or run until the end of the current level — `s_capture_target` is the
// remaining frame budget (0 = no frame cap), `s_capture_deadline_s` is the
// absolute end time (0 = no time cap), and `s_capture_level_end` marks the
// "profile level" mode (stops on map change / intermission / disconnect).
// Whichever fires first wins.
static int          s_capture_active;
static int          s_capture_target;
static double       s_capture_deadline_s;
static int          s_capture_level_end;        // "profile level": run to level end
static char         s_capture_level_name[64];   // snapshot of the level at start
static int          s_capture_level_intermission; // snapshot of cl.intermission
static int          s_capture_done;

// When set, Perf_BeginFrame is a no-op so the live overlay stays frozen on
// the last completed frame. Hooked from ImguiLayer_Toggle so F3 freezes the
// graphs and flame chart for inspection.
static int          s_paused;

// ---------------------------------------------------------------------------
// Replay
//
// Loaded capture sits in dedicated heap so the live ring buffer is undisturbed.
// We store one perf_scope_view_t per event (depth reconstructed at load time)
// and a separate frame-boundary array. The live Perf_LastFrame* accessors
// transparently switch to replay state when active.
// ---------------------------------------------------------------------------

typedef struct {
    int   first_event;
    int   event_count;
    float total_ms;
} replay_frame_t;

static struct {
    int                 active;
    char                path[512];
    char               *name_pool;
    int                 name_pool_used, name_pool_cap;
    perf_scope_view_t  *events;          // flat array, frames pack consecutively
    int                 n_events, events_cap;
    replay_frame_t     *frames;
    int                 n_frames, frames_cap;
    int                 current_frame;
} s_replay;

static char         s_last_capture_path[512];

// Cached time source.
static Uint64       s_perf_freq;

// Cvar — drives the always-on perf overlay (independent of the F3 dev panel
// so the graph keeps ticking while the game runs unpaused).
//   0  off
//   1  Perf panel (graphs + flame chart)
cvar_t scr_showperf = {"showperf", "0", true};

// Cvar — when the F3 dev overlay is open, keep the profiler (and the game
// sim) running live instead of freezing on the last frame. Toggled by the
// Profile panel's Play/Pause button. Default on ("play"): opening F3 shows
// live, updating graphs; set 0 to restore the classic freeze-on-open so the
// flame graph / sparkline stay still for hovering.
cvar_t perf_live = {"perf_live", "1", true};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline double now_seconds(void) {
    return (double)SDL_GetPerformanceCounter() / (double)s_perf_freq;
}

static perf_frame_t *current_frame(void) {
    return &s_frame_ring[s_ring_head];
}

static const perf_frame_t *last_completed_frame(void) {
    if (s_ring_count == 0) return NULL;
    int idx = s_ring_head - 1;
    if (idx < 0) idx += PERF_FRAME_HISTORY;
    return &s_frame_ring[idx];
}

// ---------------------------------------------------------------------------
// Capture buffer
// ---------------------------------------------------------------------------
//
// Captured frames are stored as a flat list of events: (name_ptr, start_us,
// dur_us). Storing pointers is fine because we require names to be static
// literals. On capture end we walk this and emit the Chrome trace plus the
// summary aggregate.

typedef struct {
    const char *name;
    double      start_s;        // relative to capture start
    double      dur_s;
    int         tid;            // always 1 today; reserved for future per-thread capture
} capture_event_t;

#define PERF_CAPTURE_MAX_EVENTS  (PERF_MAX_NODES_FRAME * 128)  // ~256k events
static capture_event_t *s_capture_events;
static int              s_capture_event_count;
static int              s_capture_frames_seen;
static double           s_capture_t0;

static void capture_alloc(void) {
    if (s_capture_events) return;
    s_capture_events = (capture_event_t *)calloc(PERF_CAPTURE_MAX_EVENTS, sizeof(*s_capture_events));
}

static void capture_reset(void) {
    s_capture_event_count = 0;
    s_capture_frames_seen = 0;
    s_capture_t0 = 0.0;
}

// Identity of the currently-loaded level, for the "profile level" mode. The
// BSP name (e.g. "maps/e1m1.bsp") changes on every map load and goes empty on
// disconnect, so it's a reliable end-of-level edge. Empty when not in a level.
static void capture_level_name(char *out, size_t cap) {
    if (cls.state == ca_connected && cl.worldmodel && cl.worldmodel->name[0])
        snprintf(out, cap, "%s", cl.worldmodel->name);
    else
        out[0] = 0;
}

static void capture_add_frame(const perf_frame_t *f) {
    if (!s_capture_events) return;
    if (s_capture_frames_seen == 0) {
        s_capture_t0 = f->frame_start_s;
    }
    double base = f->frame_start_s - s_capture_t0;
    for (int i = 0; i < f->n_nodes && s_capture_event_count < PERF_CAPTURE_MAX_EVENTS; i++) {
        const perf_scope_view_t *n = &f->nodes[i];
        capture_event_t *e = &s_capture_events[s_capture_event_count++];
        e->name    = n->name;
        e->start_s = base + n->start_s;
        e->dur_s   = n->dur_s;
        e->tid     = 1;
    }
    s_capture_frames_seen++;
}

// ---------------------------------------------------------------------------
// Capture flush — Chrome trace JSON + summary aggregate
// ---------------------------------------------------------------------------

// Aggregate slot keyed by name pointer (since names are interned string
// literals, pointer equality is identity equality).
typedef struct {
    const char *name;
    int         calls;
    double      total_s;
    double      max_s;
    // For percentiles we keep all durations; bounded by event count.
    double     *durations;
    int         dur_count;
    int         dur_cap;
} agg_slot_t;

static int slot_cmp_total_desc(const void *a, const void *b) {
    const agg_slot_t *sa = (const agg_slot_t *)a;
    const agg_slot_t *sb = (const agg_slot_t *)b;
    if (sa->total_s < sb->total_s) return 1;
    if (sa->total_s > sb->total_s) return -1;
    return 0;
}

static int dcmp(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

static void ensure_profiles_dir(void) {
    SDL_CreateDirectory("profiles");
}

static void format_timestamp(char *out, size_t cap) {
    time_t t = time(NULL);
    struct tm *tm_local = localtime(&t);
    if (tm_local) {
        strftime(out, cap, "%Y%m%d_%H%M%S", tm_local);
    } else {
        snprintf(out, cap, "0");
    }
}

static void capture_flush(void) {
    if (s_capture_event_count == 0) {
        Con_Printf("perf: capture had no events\n");
        return;
    }

    ensure_profiles_dir();

    char stamp[64];
    format_timestamp(stamp, sizeof(stamp));
    char trace_path  [600];
    char summary_path[600];
    snprintf(trace_path,   sizeof(trace_path),   "profiles/perf_%s.json",          stamp);
    snprintf(summary_path, sizeof(summary_path), "profiles/perf_%s_summary.json",  stamp);

    // -- 1. Chrome trace events.
    FILE *fp = fopen(trace_path, "wb");
    if (!fp) {
        Con_Printf("perf: failed to open %s\n", trace_path);
        return;
    }
    fputs("{\"traceEvents\":[\n", fp);
    for (int i = 0; i < s_capture_event_count; i++) {
        const capture_event_t *e = &s_capture_events[i];
        // Trace ts/dur are integer microseconds.
        double ts_us  = e->start_s * 1e6;
        double dur_us = e->dur_s   * 1e6;
        // Escape only " and \ in names (assume sane symbol-like inputs).
        const char *name = e->name ? e->name : "?";
        fprintf(fp,
            "{\"name\":\"%s\",\"cat\":\"perf\",\"ph\":\"X\",\"ts\":%.1f,\"dur\":%.1f,\"pid\":1,\"tid\":%d}%s\n",
            name, ts_us, dur_us, e->tid,
            (i + 1 < s_capture_event_count) ? "," : "");
    }
    fputs("]}\n", fp);
    fclose(fp);

    // -- 2. Summary aggregate (per-scope stats over the capture).
    // Small hash-by-pointer; events are sparse so linear is fine for typical sizes.
    enum { MAX_SLOTS = 256 };
    agg_slot_t slots[MAX_SLOTS];
    int n_slots = 0;
    for (int i = 0; i < s_capture_event_count; i++) {
        const capture_event_t *e = &s_capture_events[i];
        agg_slot_t *slot = NULL;
        for (int j = 0; j < n_slots; j++) {
            if (slots[j].name == e->name) { slot = &slots[j]; break; }
        }
        if (!slot) {
            if (n_slots >= MAX_SLOTS) continue;
            slot = &slots[n_slots++];
            slot->name      = e->name;
            slot->calls     = 0;
            slot->total_s   = 0.0;
            slot->max_s     = 0.0;
            slot->durations = NULL;
            slot->dur_count = 0;
            slot->dur_cap   = 0;
        }
        slot->calls++;
        slot->total_s += e->dur_s;
        if (e->dur_s > slot->max_s) slot->max_s = e->dur_s;
        if (slot->dur_count == slot->dur_cap) {
            int nc = slot->dur_cap ? slot->dur_cap * 2 : 64;
            slot->durations = (double *)realloc(slot->durations, nc * sizeof(double));
            slot->dur_cap   = nc;
        }
        slot->durations[slot->dur_count++] = e->dur_s;
    }

    qsort(slots, n_slots, sizeof(*slots), slot_cmp_total_desc);

    fp = fopen(summary_path, "wb");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"frames\": %d,\n", s_capture_frames_seen);
        fprintf(fp, "  \"events\": %d,\n", s_capture_event_count);
        fprintf(fp, "  \"scopes\": {\n");
        for (int i = 0; i < n_slots; i++) {
            agg_slot_t *s = &slots[i];
            qsort(s->durations, s->dur_count, sizeof(double), dcmp);
            double p50 = s->durations[s->dur_count / 2];
            double p95 = s->durations[(int)(s->dur_count * 0.95)];
            double avg = s->total_s / s->calls;
            fprintf(fp,
                "    \"%s\": {\"calls\":%d,\"avg_ms\":%.4f,\"p50_ms\":%.4f,\"p95_ms\":%.4f,\"max_ms\":%.4f,\"total_ms\":%.3f}%s\n",
                s->name, s->calls,
                avg * 1000.0, p50 * 1000.0, p95 * 1000.0, s->max_s * 1000.0,
                s->total_s * 1000.0,
                (i + 1 < n_slots) ? "," : "");
        }
        fprintf(fp, "  }\n}\n");
        fclose(fp);
    }

    for (int i = 0; i < n_slots; i++) free(slots[i].durations);

    snprintf(s_last_capture_path, sizeof(s_last_capture_path), "%s", trace_path);
    Con_Printf("perf: captured %d frames, %d events -> %s (+ summary)\n",
               s_capture_frames_seen, s_capture_event_count, trace_path);
}

// ---------------------------------------------------------------------------
// Aggregate cache — rebuilt by Perf_AggregateUpdate from either the live
// frame ring or the loaded replay events. Names are interned so we hash by
// pointer equality.
// ---------------------------------------------------------------------------
#define PERF_AGG_MAX_ROWS  256

static perf_agg_row_t   s_agg_rows[PERF_AGG_MAX_ROWS];
static int              s_agg_count;
static float            s_agg_window_ms;     // sum of frame totals in the window
static int              s_agg_frame_count;   // frames contributing to the window

static int agg_cmp_total_desc(const void *a, const void *b) {
    const perf_agg_row_t *ra = (const perf_agg_row_t *)a;
    const perf_agg_row_t *rb = (const perf_agg_row_t *)b;
    if (ra->total_ms < rb->total_ms) return  1;
    if (ra->total_ms > rb->total_ms) return -1;
    return 0;
}
static int agg_cmp_avg_desc(const void *a, const void *b) {
    const perf_agg_row_t *ra = (const perf_agg_row_t *)a;
    const perf_agg_row_t *rb = (const perf_agg_row_t *)b;
    if (ra->avg_ms < rb->avg_ms) return  1;
    if (ra->avg_ms > rb->avg_ms) return -1;
    return 0;
}
static int agg_cmp_max_desc(const void *a, const void *b) {
    const perf_agg_row_t *ra = (const perf_agg_row_t *)a;
    const perf_agg_row_t *rb = (const perf_agg_row_t *)b;
    if (ra->max_ms < rb->max_ms) return  1;
    if (ra->max_ms > rb->max_ms) return -1;
    return 0;
}
static int agg_cmp_calls_desc(const void *a, const void *b) {
    const perf_agg_row_t *ra = (const perf_agg_row_t *)a;
    const perf_agg_row_t *rb = (const perf_agg_row_t *)b;
    return rb->calls - ra->calls;
}
static int agg_cmp_name_asc(const void *a, const void *b) {
    const perf_agg_row_t *ra = (const perf_agg_row_t *)a;
    const perf_agg_row_t *rb = (const perf_agg_row_t *)b;
    return strcmp(ra->name ? ra->name : "", rb->name ? rb->name : "");
}

static perf_agg_row_t *agg_find_or_add(const char *name) {
    for (int i = 0; i < s_agg_count; i++) {
        if (s_agg_rows[i].name == name) return &s_agg_rows[i];
    }
    if (s_agg_count >= PERF_AGG_MAX_ROWS) return NULL;
    perf_agg_row_t *r = &s_agg_rows[s_agg_count++];
    r->name     = name;
    r->calls    = 0;
    r->total_ms = 0;
    r->avg_ms   = 0;
    r->max_ms   = 0;
    return r;
}

static void agg_accumulate_event(const char *name, float dur_ms) {
    perf_agg_row_t *r = agg_find_or_add(name);
    if (!r) return;
    r->calls++;
    r->total_ms += dur_ms;
    if (dur_ms > r->max_ms) r->max_ms = dur_ms;
}

void Perf_AggregateUpdate(perf_agg_sort_t sort) {
    s_agg_count       = 0;
    s_agg_window_ms   = 0;
    s_agg_frame_count = 0;

    if (s_replay.active && s_replay.n_frames > 0) {
        for (int fi = 0; fi < s_replay.n_frames; fi++) {
            const replay_frame_t *rf = &s_replay.frames[fi];
            s_agg_window_ms += rf->total_ms;
            s_agg_frame_count++;
            for (int j = 0; j < rf->event_count; j++) {
                const perf_scope_view_t *e = &s_replay.events[rf->first_event + j];
                agg_accumulate_event(e->name, (float)(e->dur_s * 1000.0));
            }
        }
    } else {
        // Walk the live ring of completed frames (skip the in-progress one
        // at s_ring_head — its dur fields aren't finalized).
        for (int k = 0; k < s_ring_count; k++) {
            int idx = s_ring_head - 1 - k;
            if (idx < 0) idx += PERF_FRAME_HISTORY;
            const perf_frame_t *f = &s_frame_ring[idx];
            if (!f->valid) continue;
            float frame_ms = (float)((f->frame_end_s - f->frame_start_s) * 1000.0);
            s_agg_window_ms += frame_ms;
            s_agg_frame_count++;
            for (int j = 0; j < f->n_nodes; j++) {
                const perf_scope_view_t *n = &f->nodes[j];
                agg_accumulate_event(n->name, (float)(n->dur_s * 1000.0));
            }
        }
    }

    for (int i = 0; i < s_agg_count; i++) {
        perf_agg_row_t *r = &s_agg_rows[i];
        r->avg_ms = r->calls ? r->total_ms / (float)r->calls : 0.0f;
    }

    int (*cmp)(const void *, const void *) = agg_cmp_total_desc;
    switch (sort) {
        case PERF_AGG_SORT_AVG:   cmp = agg_cmp_avg_desc;   break;
        case PERF_AGG_SORT_MAX:   cmp = agg_cmp_max_desc;   break;
        case PERF_AGG_SORT_CALLS: cmp = agg_cmp_calls_desc; break;
        case PERF_AGG_SORT_NAME:  cmp = agg_cmp_name_asc;   break;
        case PERF_AGG_SORT_TOTAL: default: break;
    }
    qsort(s_agg_rows, s_agg_count, sizeof(s_agg_rows[0]), cmp);
}

int Perf_AggregateRowCount(void) { return s_agg_count; }
const perf_agg_row_t *Perf_AggregateRow(int i) {
    if (i < 0 || i >= s_agg_count) return NULL;
    return &s_agg_rows[i];
}
float Perf_AggregateWindowMs(void)  { return s_agg_window_ms; }
int   Perf_AggregateFrameCount(void){ return s_agg_frame_count; }

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Perf_Init(void) {
    s_perf_freq = SDL_GetPerformanceFrequency();
    if (s_perf_freq == 0) s_perf_freq = 1;  // defensive
    memset(s_frame_ring, 0, sizeof(s_frame_ring));
    memset(s_frametime_ms, 0, sizeof(s_frametime_ms));
    s_ring_head      = 0;
    s_ring_count     = 0;
    s_ft_head        = 0;
    s_ft_count       = 0;
    s_stack_top      = 0;
    s_in_frame       = 0;
    s_capture_active = 0;
    s_capture_target = 0;
    s_capture_level_end = 0;
    s_capture_done   = 0;
    capture_reset();
}

void Perf_BeginFrame(void) {
    // While paused, drop the frame entirely — leaves the last completed
    // frame's data in place for the live overlay to keep displaying.
    // Captures continue regardless (s_capture_active is checked in EndFrame
    // and won't fire because s_in_frame stays 0).
    if (s_paused) return;

    perf_frame_t *f = current_frame();
    f->n_nodes      = 0;
    f->valid        = 1;
    f->frame_start_s = now_seconds();
    f->frame_end_s   = f->frame_start_s;
    s_stack_top     = 0;
    s_in_frame      = 1;

    // Auto-push a synthetic "frame" root so all user scopes nest under one
    // bar and the flame graph can scale to the frame total. Popped by
    // Perf_EndFrame before history bookkeeping runs.
    Perf_PushScope("frame");
}

void Perf_SetPaused(int paused) {
    // If we're transitioning into paused mid-frame, drop the in-flight frame
    // cleanly (don't record a half-measured frametime).
    if (paused && s_in_frame) {
        s_in_frame = 0;
        s_stack_top = 0;
    }
    s_paused = paused;
}

int Perf_IsPaused(void) { return s_paused; }

void Perf_EndFrame(void) {
    if (!s_in_frame) return;

    // Close the synthetic root pushed by Perf_BeginFrame.
    Perf_PopScope();

    perf_frame_t *f = current_frame();
    f->frame_end_s = now_seconds();

    if (s_stack_top != 0) {
        // Stack-mismatch -- drop this frame's scope data; keep the frametime.
        f->valid   = 0;
        f->n_nodes = 0;
        s_stack_top = 0;
    }

    double total_s = f->frame_end_s - f->frame_start_s;

    // Push into the frametime history ring.
    s_frametime_ms[s_ft_head] = (float)(total_s * 1000.0);
    s_ft_head = (s_ft_head + 1) % PERF_FRAME_HISTORY;
    if (s_ft_count < PERF_FRAME_HISTORY) s_ft_count++;

    if (s_capture_active && f->valid) {
        capture_add_frame(f);
        int stop = 0;
        if (s_capture_target > 0 && --s_capture_target <= 0)  stop = 1;
        if (s_capture_deadline_s > 0 && f->frame_end_s >= s_capture_deadline_s) stop = 1;
        if (s_capture_level_end) {
            // Stop at the end of the level: the map changed / we disconnected,
            // or the player reached intermission (level complete).
            char now_name[64];
            capture_level_name(now_name, sizeof(now_name));
            if (strcmp(now_name, s_capture_level_name) != 0) stop = 1;
            if (cl.intermission && !s_capture_level_intermission) stop = 1;
            // Guard against the event buffer silently truncating on a very long
            // level — flush what we have rather than dropping the tail unseen.
            if (s_capture_event_count >= PERF_CAPTURE_MAX_EVENTS) {
                Con_Printf("perf: capture buffer full (%d frames) — stopping early\n",
                           s_capture_frames_seen);
                stop = 1;
            }
        }
        if (stop) {
            s_capture_active = 0;
            s_capture_done   = 1;
            capture_flush();
            capture_reset();
        }
    }

    // Advance ring.
    s_ring_head = (s_ring_head + 1) % PERF_FRAME_HISTORY;
    if (s_ring_count < PERF_FRAME_HISTORY) s_ring_count++;
    s_in_frame = 0;
}

void Perf_PushScope(const char *name) {
    if (!s_in_frame) return;
    perf_frame_t *f = current_frame();
    if (!f->valid) return;
    if (f->n_nodes >= PERF_MAX_NODES_FRAME) { f->valid = 0; return; }
    if (s_stack_top >= PERF_MAX_DEPTH)      { f->valid = 0; return; }

    int idx = f->n_nodes++;
    perf_scope_view_t *n = &f->nodes[idx];
    n->name         = name;
    n->start_s      = now_seconds() - f->frame_start_s;
    n->dur_s        = 0;
    n->depth        = s_stack_top;
    n->parent       = (s_stack_top > 0) ? s_stack[s_stack_top - 1] : -1;
    n->first_child  = -1;
    n->next_sibling = -1;

    // Wire into parent's child list.
    if (n->parent >= 0) {
        perf_scope_view_t *p = &f->nodes[n->parent];
        if (p->first_child < 0) {
            p->first_child = idx;
        } else {
            int sib = p->first_child;
            while (f->nodes[sib].next_sibling >= 0) sib = f->nodes[sib].next_sibling;
            f->nodes[sib].next_sibling = idx;
        }
    }

    s_stack[s_stack_top++] = idx;
}

void Perf_PopScope(void) {
    if (!s_in_frame) return;
    perf_frame_t *f = current_frame();
    if (!f->valid) return;
    if (s_stack_top <= 0) { f->valid = 0; return; }
    int idx = s_stack[--s_stack_top];
    perf_scope_view_t *n = &f->nodes[idx];
    n->dur_s = (now_seconds() - f->frame_start_s) - n->start_s;
}

// ---------------------------------------------------------------------------
// Read-side accessors
// ---------------------------------------------------------------------------

int Perf_GetFrametimeHistoryMs(float *out, int max) {
    // Frametime history is always live — even when a capture is loaded,
    // the sparkline reflects real-time engine load. The flame graph below
    // is what shows the replay frame.
    int n = s_ft_count < max ? s_ft_count : max;
    for (int i = 0; i < n; i++) {
        int idx = s_ft_head - 1 - i;
        if (idx < 0) idx += PERF_FRAME_HISTORY;
        out[i] = s_frametime_ms[idx];
    }
    return n;
}

float Perf_LastFrameMs(void) {
    if (s_replay.active && s_replay.n_frames > 0) {
        return s_replay.frames[s_replay.current_frame].total_ms;
    }
    if (s_ft_count == 0) return 0;
    int idx = s_ft_head - 1;
    if (idx < 0) idx += PERF_FRAME_HISTORY;
    return s_frametime_ms[idx];
}

float Perf_SmoothedFps(void) {
    if (s_ft_count == 0) return 0;
    // EWMA over last ~30 samples.
    float ms = 0;
    int n = s_ft_count < 30 ? s_ft_count : 30;
    int idx = s_ft_head - 1;
    if (idx < 0) idx += PERF_FRAME_HISTORY;
    float alpha = 0.18f;
    ms = s_frametime_ms[idx];
    for (int i = 1; i < n; i++) {
        int j = idx - i;
        if (j < 0) j += PERF_FRAME_HISTORY;
        ms = alpha * s_frametime_ms[j] + (1 - alpha) * ms;
    }
    if (ms < 0.0001f) return 0;
    return 1000.0f / ms;
}

const perf_scope_view_t *Perf_LastFrameTree(void) {
    const perf_frame_t *f = last_completed_frame();
    if (!f || !f->valid || f->n_nodes == 0) return NULL;
    return &f->nodes[0];
}

const char *Perf_ScopeName(const perf_scope_view_t *s) { return s ? s->name : ""; }
float Perf_ScopeMs       (const perf_scope_view_t *s) { return s ? (float)(s->dur_s * 1000.0) : 0; }
float Perf_ScopeStartMs  (const perf_scope_view_t *s) { return s ? (float)(s->start_s * 1000.0) : 0; }
int   Perf_ScopeDepth    (const perf_scope_view_t *s) { return s ? s->depth : 0; }

int Perf_ScopeChildCount(const perf_scope_view_t *s) {
    if (!s) return 0;
    const perf_frame_t *f = last_completed_frame();
    if (!f) return 0;
    int n = 0;
    for (int c = s->first_child; c >= 0; c = f->nodes[c].next_sibling) n++;
    return n;
}

const perf_scope_view_t *Perf_ScopeChild(const perf_scope_view_t *s, int i) {
    if (!s) return NULL;
    const perf_frame_t *f = last_completed_frame();
    if (!f) return NULL;
    int n = 0;
    for (int c = s->first_child; c >= 0; c = f->nodes[c].next_sibling) {
        if (n == i) return &f->nodes[c];
        n++;
    }
    return NULL;
}

int Perf_LastFrameNodeCount(void) {
    if (s_replay.active && s_replay.n_frames > 0) {
        return s_replay.frames[s_replay.current_frame].event_count;
    }
    const perf_frame_t *f = last_completed_frame();
    return (f && f->valid) ? f->n_nodes : 0;
}

const perf_scope_view_t *Perf_LastFrameNode(int i) {
    if (s_replay.active && s_replay.n_frames > 0) {
        const replay_frame_t *rf = &s_replay.frames[s_replay.current_frame];
        if (i < 0 || i >= rf->event_count) return NULL;
        return &s_replay.events[rf->first_event + i];
    }
    const perf_frame_t *f = last_completed_frame();
    if (!f || !f->valid) return NULL;
    if (i < 0 || i >= f->n_nodes) return NULL;
    return &f->nodes[i];
}

// ---------------------------------------------------------------------------
// Capture control
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Replay — file loader
// ---------------------------------------------------------------------------

static const char *replay_intern_name(const char *s, int len) {
    // Linear search; scope-name pool is tiny (a few dozen distinct strings).
    int pos = 0;
    while (pos < s_replay.name_pool_used) {
        int el = (int)strlen(s_replay.name_pool + pos);
        if (el == len && memcmp(s_replay.name_pool + pos, s, len) == 0)
            return s_replay.name_pool + pos;
        pos += el + 1;
    }
    if (s_replay.name_pool_used + len + 1 > s_replay.name_pool_cap) {
        int nc = s_replay.name_pool_cap ? s_replay.name_pool_cap * 2 : 4096;
        while (nc < s_replay.name_pool_used + len + 1) nc *= 2;
        char *np = (char *)realloc(s_replay.name_pool, nc);
        if (!np) return NULL;
        s_replay.name_pool = np;
        s_replay.name_pool_cap = nc;
    }
    char *dst = s_replay.name_pool + s_replay.name_pool_used;
    memcpy(dst, s, len);
    dst[len] = 0;
    s_replay.name_pool_used += len + 1;
    return dst;
}

// Extract one event from a single-line {"name":"...","ts":..,"dur":..,...}.
// Returns 1 on success.
static int parse_event_line(const char *line,
                            const char **name_p, int *name_len,
                            double *ts_us, double *dur_us)
{
    const char *p = strstr(line, "\"name\":\"");
    if (!p) return 0;
    p += 8;
    const char *q = strchr(p, '"');
    if (!q) return 0;
    *name_p   = p;
    *name_len = (int)(q - p);

    p = strstr(line, "\"ts\":");
    if (!p) return 0;
    *ts_us = atof(p + 5);

    p = strstr(line, "\"dur\":");
    if (!p) return 0;
    *dur_us = atof(p + 6);

    return 1;
}

void Perf_UnloadCapture(void) {
    if (!s_replay.active) return;
    free(s_replay.events);      s_replay.events = NULL;
    free(s_replay.frames);      s_replay.frames = NULL;
    free(s_replay.name_pool);   s_replay.name_pool = NULL;
    s_replay.events_cap = s_replay.n_events = 0;
    s_replay.frames_cap = s_replay.n_frames = 0;
    s_replay.name_pool_used = s_replay.name_pool_cap = 0;
    s_replay.current_frame = 0;
    s_replay.path[0] = 0;
    s_replay.active = 0;
}

int Perf_LoadCapture(const char *path) {
    Perf_UnloadCapture();

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        Con_Printf("perf: can't open %s\n", path);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 0) { fclose(fp); return 2; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return 3; }
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fclose(fp); free(buf); return 4;
    }
    fclose(fp);
    buf[len] = 0;

    // Walk the file line by line. The format we write is one event per line
    // wrapped between `{"traceEvents":[\n` and `]}\n`; anything not matching
    // the event schema is skipped (including the wrapper lines).
    typedef struct { const char *name; float start_ms, dur_ms; } raw_event_t;
    raw_event_t *raw = NULL;
    int raw_n = 0, raw_cap = 0;

    char *line = buf;
    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;

        const char *name_p; int name_len;
        double ts_us, dur_us;
        if (parse_event_line(line, &name_p, &name_len, &ts_us, &dur_us)) {
            if (raw_n == raw_cap) {
                int nc = raw_cap ? raw_cap * 2 : 1024;
                raw = (raw_event_t *)realloc(raw, nc * sizeof(*raw));
                raw_cap = nc;
            }
            const char *interned = replay_intern_name(name_p, name_len);
            if (!interned) { free(buf); free(raw); return 5; }
            raw[raw_n].name      = interned;
            raw[raw_n].start_ms  = (float)(ts_us  / 1000.0);
            raw[raw_n].dur_ms    = (float)(dur_us / 1000.0);
            raw_n++;
        }

        if (!eol) break;
        line = eol + 1;
    }
    free(buf);

    if (raw_n == 0) {
        Con_Printf("perf: no events in %s\n", path);
        free(raw);
        Perf_UnloadCapture();
        return 6;
    }

    // Allocate the flat scope_view array. Frame boundaries are demarcated by
    // events whose name is "frame" (the synthetic root emitted by
    // Perf_BeginFrame). Within a frame, events are already in start-time
    // order. Reconstruct depth via a stack of (end_ms) tuples; reset the
    // stack at each frame boundary.
    s_replay.events     = (perf_scope_view_t *)calloc(raw_n, sizeof(perf_scope_view_t));
    s_replay.events_cap = raw_n;
    s_replay.frames     = NULL;
    s_replay.frames_cap = 0;

    float stack_end[PERF_MAX_DEPTH];
    int   stack_top = 0;
    float frame_origin_ms = 0;       // start of current frame in capture-space
    int   in_frame = 0;

    for (int i = 0; i < raw_n; i++) {
        int is_frame_root = (strcmp(raw[i].name, "frame") == 0);

        if (is_frame_root) {
            // Close out any previous frame, start a new one.
            if (s_replay.n_frames > 0) {
                replay_frame_t *prev = &s_replay.frames[s_replay.n_frames - 1];
                prev->event_count = s_replay.n_events - prev->first_event;
            }
            if (s_replay.n_frames == s_replay.frames_cap) {
                int nc = s_replay.frames_cap ? s_replay.frames_cap * 2 : 128;
                s_replay.frames = (replay_frame_t *)realloc(
                    s_replay.frames, nc * sizeof(*s_replay.frames));
                s_replay.frames_cap = nc;
            }
            s_replay.frames[s_replay.n_frames].first_event = s_replay.n_events;
            s_replay.frames[s_replay.n_frames].event_count = 0;
            s_replay.frames[s_replay.n_frames].total_ms    = raw[i].dur_ms;
            s_replay.n_frames++;
            frame_origin_ms = raw[i].start_ms;
            stack_top  = 0;
            in_frame   = 1;
        }
        if (!in_frame) continue;  // events before first "frame" root — skip

        // Pop closed scopes (those whose end is <= this event's start).
        float ev_start = raw[i].start_ms;
        while (stack_top > 0 && stack_end[stack_top - 1] <= ev_start)
            stack_top--;
        int depth = stack_top;

        perf_scope_view_t *ev = &s_replay.events[s_replay.n_events++];
        ev->name         = raw[i].name;
        ev->start_s      = (raw[i].start_ms - frame_origin_ms) * 0.001;
        ev->dur_s        = raw[i].dur_ms * 0.001;
        ev->depth        = depth;
        ev->parent       = -1;
        ev->first_child  = -1;
        ev->next_sibling = -1;

        if (stack_top < PERF_MAX_DEPTH)
            stack_end[stack_top++] = ev_start + raw[i].dur_ms;
    }
    // Close out the final frame.
    if (s_replay.n_frames > 0) {
        replay_frame_t *prev = &s_replay.frames[s_replay.n_frames - 1];
        prev->event_count = s_replay.n_events - prev->first_event;
    }
    free(raw);

    if (s_replay.n_frames == 0) {
        Con_Printf("perf: %s has no 'frame' markers (older capture?)\n", path);
        Perf_UnloadCapture();
        return 7;
    }

    snprintf(s_replay.path, sizeof(s_replay.path), "%s", path);
    s_replay.current_frame = 0;
    s_replay.active = 1;

    Con_Printf("perf: loaded %s (%d frames, %d events)\n",
               path, s_replay.n_frames, s_replay.n_events);
    return 0;
}

int  Perf_IsReplaying     (void) { return s_replay.active; }
int  Perf_ReplayFrameCount(void) { return s_replay.active ? s_replay.n_frames : 0; }
int  Perf_ReplayFrameIdx  (void) { return s_replay.active ? s_replay.current_frame : 0; }

void Perf_SetReplayFrame(int idx) {
    if (!s_replay.active || s_replay.n_frames == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= s_replay.n_frames) idx = s_replay.n_frames - 1;
    s_replay.current_frame = idx;
}

const char *Perf_ReplayPath(void) {
    return s_replay.active ? s_replay.path : NULL;
}

float Perf_ReplayFrameMs(int idx) {
    if (!s_replay.active || idx < 0 || idx >= s_replay.n_frames) return 0.0f;
    return s_replay.frames[idx].total_ms;
}

float Perf_ReplayMaxMs(void) {
    if (!s_replay.active) return 0.0f;
    float m = 0.0f;
    for (int i = 0; i < s_replay.n_frames; i++) {
        if (s_replay.frames[i].total_ms > m) m = s_replay.frames[i].total_ms;
    }
    return m;
}

int Perf_StartCapture(int n_frames) {
    if (s_capture_active) return 1;
    if (n_frames <= 0)    return 2;
    if (n_frames > PERF_MAX_CAPTURE_FRAMES) n_frames = PERF_MAX_CAPTURE_FRAMES;
    capture_alloc();
    if (!s_capture_events) return 3;
    capture_reset();
    s_capture_target     = n_frames;
    s_capture_deadline_s = 0.0;
    s_capture_level_end  = 0;
    s_capture_active     = 1;
    s_capture_done       = 0;
    s_last_capture_path[0] = 0;
    Con_Printf("perf: capturing %d frames...\n", n_frames);
    return 0;
}

int Perf_StartCaptureSeconds(float seconds) {
    if (s_capture_active) return 1;
    if (seconds <= 0)     return 2;
    capture_alloc();
    if (!s_capture_events) return 3;
    capture_reset();
    s_capture_target     = 0;   // no frame cap — time-bounded
    s_capture_deadline_s = now_seconds() + seconds;
    s_capture_level_end  = 0;
    s_capture_active     = 1;
    s_capture_done       = 0;
    s_last_capture_path[0] = 0;
    Con_Printf("perf: capturing for %.1fs...\n", (double)seconds);
    return 0;
}

// "profile level": capture from now until the current level ends — i.e. the
// map changes, the player disconnects, or intermission begins. Unbounded by
// frames or time (subject only to the event-buffer guard in Perf_EndFrame).
int Perf_StartCaptureLevel(void) {
    if (s_capture_active) return 1;
    capture_alloc();
    if (!s_capture_events) return 3;
    capture_reset();
    s_capture_target     = 0;   // no frame cap
    s_capture_deadline_s = 0.0; // no time cap
    s_capture_level_end  = 1;
    capture_level_name(s_capture_level_name, sizeof(s_capture_level_name));
    s_capture_level_intermission = cl.intermission;
    s_capture_active     = 1;
    s_capture_done       = 0;
    s_last_capture_path[0] = 0;
    if (s_capture_level_name[0])
        Con_Printf("perf: capturing until end of %s...\n", s_capture_level_name);
    else
        Con_Printf("perf: capturing until end of level...\n");
    return 0;
}

// Finalize the in-progress capture early (the Profile panel's Stop button).
// Flushes whatever frames were collected, mirroring the normal completion path
// in Perf_EndFrame. No-op if not capturing; capture_flush writes nothing if no
// frames landed yet.
void Perf_StopCapture(void) {
    if (!s_capture_active) return;
    s_capture_active = 0;
    s_capture_done   = 1;
    capture_flush();
    capture_reset();
}

int  Perf_IsCapturing(void)       { return s_capture_active; }
const char *Perf_CaptureUnit(void) {
    if (!s_capture_active)        return "";
    if (s_capture_level_end)      return "level";
    if (s_capture_target > 0)     return "frames";
    return "seconds";
}
int  Perf_CaptureRemaining(void)  {
    // For frame-bound captures this is the remaining count; for time-bound
    // it's the integer seconds remaining (rounded up so the UI shows a
    // meaningful number even in the final second). For the open-ended "level"
    // mode there's no known end, so report frames captured so far instead.
    if (!s_capture_active) return 0;
    if (s_capture_level_end) return s_capture_frames_seen;
    if (s_capture_target > 0) return s_capture_target;
    double remaining_s = s_capture_deadline_s - now_seconds();
    if (remaining_s < 0) remaining_s = 0;
    return (int)(remaining_s + 0.999);
}
const char *Perf_LastCapturePath(void) {
    return s_last_capture_path[0] ? s_last_capture_path : NULL;
}

// ---------------------------------------------------------------------------
// Console commands
// ---------------------------------------------------------------------------

static void Perf_Profile_f(void) {
    // Argument is one of:
    //   profile           -> 120 frames (default)
    //   profile 120       -> a frame count
    //   profile 10s       -> a wall-clock duration ('s' suffix; "0.5s" ok)
    //   profile level     -> run until the end of the current level
    int   rc;
    int   is_time  = 0;
    int   is_level = 0;
    float val      = 120.0f;
    if (Cmd_Argc() >= 2) {
        const char *a = Cmd_Argv(1);
        int len = (int)strlen(a);
        if (!Q_strcasecmp((char *)a, "level")) {
            is_level = 1;
        } else {
            is_time = (len > 0 && (a[len-1] == 's' || a[len-1] == 'S'));
            val = (float)atof(a);
        }
    }

    if      (is_level) rc = Perf_StartCaptureLevel();
    else if (is_time)  rc = Perf_StartCaptureSeconds(val);
    else               rc = Perf_StartCapture((int)(val + 0.5f));

    if (rc == 1) Con_Printf("perf: already capturing\n");
    else if (rc == 2) Con_Printf("perf: bad duration\n");
    else if (rc == 3) Con_Printf("perf: out of memory\n");
}

static void Perf_Replay_f(void) {
    if (Cmd_Argc() < 2) {
        if (Perf_IsReplaying())
            Con_Printf("perf: replaying %s (frame %d/%d)\n",
                       Perf_ReplayPath(),
                       Perf_ReplayFrameIdx() + 1,
                       Perf_ReplayFrameCount());
        else
            Con_Printf("usage: perf_replay <path>  (or 'off' to stop)\n");
        return;
    }
    const char *arg = Cmd_Argv(1);
    if (strcmp(arg, "off") == 0) {
        Perf_UnloadCapture();
        Con_Printf("perf: replay stopped\n");
        return;
    }
    Perf_LoadCapture(arg);
}

void Perf_RegisterCommands(void) {
    Cmd_AddCommand("profile",      Perf_Profile_f);
    Cmd_AddCommand("perf_replay",  Perf_Replay_f);
    Cvar_RegisterVariable(&scr_showperf);
    Cvar_RegisterVariable(&perf_live);
}

int Perf_ShowOverlay(void) {
    return scr_showperf.value != 0 ? 1 : 0;
}

int Perf_Live(void) {
    return perf_live.value != 0 ? 1 : 0;
}

void Perf_SetLive(int live) {
    Cvar_SetValue("perf_live", live ? 1.0f : 0.0f);
    // Reflect immediately: when the overlay is open the pause state is what
    // gates Perf_BeginFrame, so flip it now rather than waiting for the next
    // ImguiLayer_Toggle.
    Perf_SetPaused(!live);
}

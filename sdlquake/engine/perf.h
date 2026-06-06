// perf.h -- per-frame scoped timing for the dev overlay.
//
// Push/pop scopes around hot regions; the overlay renders a flame graph of the
// most recent frame and the `profile <n>` console command captures N frames to
// a Chrome-trace JSON for offline comparison.
//
// Single-threaded by design — call from the main thread only.

#ifndef SDLQUAKE_PERF_H
#define SDLQUAKE_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

// Lifecycle.
void Perf_Init(void);
void Perf_BeginFrame(void);
void Perf_EndFrame(void);

// While paused, Perf_BeginFrame is a no-op so the most-recent frametime
// history + scope tree stay frozen for inspection. Hooked from
// ImguiLayer_Toggle so the F3 dev overlay freezes the overlay too.
void Perf_SetPaused(int paused);
int  Perf_IsPaused(void);

// Scope push/pop. Nesting is allowed; mismatched pairs are tolerated (the
// surrounding frame is marked invalid and dropped).
void Perf_PushScope(const char *name);
void Perf_PopScope(void);

// `name` must outlive the capture window (use string literals).
#define PERF_CONCAT_(a, b) a##b
#define PERF_CONCAT(a, b)  PERF_CONCAT_(a, b)
#define PERF_SCOPE(name) \
    for (int PERF_CONCAT(_perf_once_, __LINE__) = (Perf_PushScope(name), 1); \
         PERF_CONCAT(_perf_once_, __LINE__); \
         PERF_CONCAT(_perf_once_, __LINE__) = 0, Perf_PopScope())

// Live UI -------------------------------------------------------------

// History of recent frametimes in milliseconds. Newest at index 0.
// `out` is filled with up to `max` entries; returns the number written.
int   Perf_GetFrametimeHistoryMs(float *out, int max);

// Single most-recent frame's overall ms.
float Perf_LastFrameMs(void);

// EWMA over the recent frametimes.
float Perf_SmoothedFps(void);

// Per-frame scope tree (most recent fully-closed frame). Returns NULL until
// at least one frame has completed.
typedef struct perf_scope_view_s perf_scope_view_t;
const perf_scope_view_t *Perf_LastFrameTree(void);
const char  *Perf_ScopeName    (const perf_scope_view_t *s);
float        Perf_ScopeMs      (const perf_scope_view_t *s);
float        Perf_ScopeStartMs (const perf_scope_view_t *s);  // relative to frame start
int          Perf_ScopeDepth   (const perf_scope_view_t *s);
int          Perf_ScopeChildCount(const perf_scope_view_t *s);
const perf_scope_view_t *Perf_ScopeChild(const perf_scope_view_t *s, int i);

// Iterate the scope tree depth-first in start-time order. `i` runs from 0 to
// the count returned by Perf_LastFrameNodeCount. Useful for flame-graph rendering
// without recursion.
int Perf_LastFrameNodeCount(void);
const perf_scope_view_t *Perf_LastFrameNode(int i);

// Capture --------------------------------------------------------------

// Start a capture of `n_frames` (clamped to a sane max). Returns 0 on success,
// nonzero if already capturing or n_frames invalid.
int  Perf_StartCapture(int n_frames);
int  Perf_StartCaptureSeconds(float seconds);
// Capture until the current level ends (map change / disconnect / intermission).
int  Perf_StartCaptureLevel(void);
// Finalize an in-progress capture early (the Profile panel's Stop button).
void Perf_StopCapture(void);
int  Perf_IsCapturing(void);
// Frames remaining (frame-bound capture) or seconds-remaining rounded up
// (time-bound capture).
int  Perf_CaptureRemaining(void);
// "frames" or "seconds" — for the UI's status string.
const char *Perf_CaptureUnit(void);
// The last completed capture writes two files under `profiles/`:
//   perf_<ts>.json          -- Chrome trace events (load in chrome://tracing,
//                              speedscope.app, perfetto.dev)
//   perf_<ts>_summary.json  -- per-scope avg/p50/p95/max/calls — for regression
//                              diffs across runs
// On completion `Perf_LastCapturePath` returns the trace path, else NULL.
const char *Perf_LastCapturePath(void);

// Console-command registration. Called from Host_Init after Cmd_Init.
void Perf_RegisterCommands(void);

// Returns 1 if the live perf overlay should render outside the F3 dev layer
// (driven by the `showperf` cvar — keeps the graph ticking while the game
// runs unpaused).
int Perf_ShowOverlay(void);

// `perf_live` cvar: when set, the F3 dev overlay keeps the profiler ticking
// (and the host runs the game sim — see the SV_Physics gate in host.c) rather
// than freezing on the last frame. Default on. The Profile panel's Play/Pause
// button calls Perf_SetLive, which also flips the pause state immediately.
int  Perf_Live(void);
void Perf_SetLive(int live);

// Aggregate -----------------------------------------------------------
// Per-scope totals over the current source (replay-wide if a capture is
// loaded, otherwise the live frame ring). Sorted by total_ms descending
// after `Perf_AggregateUpdate`.
typedef enum {
    PERF_AGG_SORT_TOTAL = 0,
    PERF_AGG_SORT_AVG,
    PERF_AGG_SORT_MAX,
    PERF_AGG_SORT_CALLS,
    PERF_AGG_SORT_NAME,
} perf_agg_sort_t;

typedef struct {
    const char *name;
    int         calls;
    float       total_ms;
    float       avg_ms;
    float       max_ms;
} perf_agg_row_t;

void Perf_AggregateUpdate  (perf_agg_sort_t sort);
int  Perf_AggregateRowCount(void);
const perf_agg_row_t *Perf_AggregateRow(int i);
// Source-window total used to compute % columns (sum of frame totals).
float Perf_AggregateWindowMs(void);
int   Perf_AggregateFrameCount(void);

// Replay --------------------------------------------------------------
// Load a Chrome-trace JSON written by Perf's own `profile <n>` command and
// expose its frames through the same Perf_LastFrame* accessors so the live
// flame graph can be reused for offline inspection. Returns 0 on success.
//
// While replaying, Perf_LastFrameMs / Perf_LastFrameNode return data from
// the currently-selected replay frame. `Perf_SetReplayFrame` clamps to
// [0, Perf_ReplayFrameCount-1]. `Perf_UnloadCapture` restores live data.
int   Perf_LoadCapture(const char *path);
void  Perf_UnloadCapture(void);
int   Perf_IsReplaying(void);
int   Perf_ReplayFrameCount(void);
int   Perf_ReplayFrameIdx(void);
void  Perf_SetReplayFrame(int idx);
const char *Perf_ReplayPath(void);
// Frame total in milliseconds for the histogram scrubber.
float Perf_ReplayFrameMs(int idx);
float Perf_ReplayMaxMs(void);   // max across all frames — for histogram Y scale

#ifdef __cplusplus
}
#endif

#endif // SDLQUAKE_PERF_H

/*
 * r_threadfill.h -- persistent fork-join worker pool for the software
 * renderer's span fill (Phase 3b of the renderer-fill-threading design,
 * docs/superpowers/specs/2026-06-04-renderer-fill-threading-design.md).
 *
 * The edge sweep resolves occlusion BEFORE the fill, so visible spans of
 * different surfaces are disjoint in screen space. Workers drawing
 * different surfaces therefore write disjoint framebuffer + z-buffer
 * pixels -- no locks on the output. The per-surface fill globals the
 * span drawers consume are already `__thread` (Phase 3a), so each worker
 * loads its surface's recorded `s->fill` into its own thread-local copies
 * and draws.
 *
 * Usage (called from d_edge.c::D_DrawSurfaces):
 *   R_ThreadFill_Run(fn, begin, end);
 * runs fn(i) for every i in [begin, end) across the worker pool AND the
 * calling thread, returning only after every index has been processed
 * (a barrier). The work-claim is atomic, so the index split self-balances.
 */

#ifndef R_THREADFILL_H
#define R_THREADFILL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Create the worker pool once (idempotent). Also registers the
 * `r_threads` cvar. Safe to call before/after Cvar subsystem init. */
void R_ThreadFill_Init (void);

/* Nonzero iff there is at least one worker thread AND `r_threads != 0`.
 * `r_threads 0` forces the serial path (the regression oracle). */
int  R_ThreadFill_Enabled (void);

/* Run fn(i) for i in [begin, end) across the pool + the caller, with a
 * barrier on return. Must be called from the main (render) thread. */
void R_ThreadFill_Run (void (*fn)(int idx), int begin, int end);

#ifdef __cplusplus
}
#endif

#endif /* R_THREADFILL_H */

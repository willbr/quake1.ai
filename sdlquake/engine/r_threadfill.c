/*
 * r_threadfill.c -- persistent fork-join worker pool for the software
 * renderer's span fill (Phase 3b of the renderer-fill-threading design).
 *
 * Model: at init we spawn min(SDL_GetNumLogicalCPUCores(), CAP) - 1 worker
 * threads. Each worker parks on the `s_start` semaphore. Per dispatch the
 * caller (the main render thread) publishes the work range + callback,
 * releases the workers, runs the same atomic grab-loop itself, then waits on
 * the `s_done` semaphore once per worker -- the barrier.
 *
 * Correctness foundation (why this needs no output locks): the edge sweep
 * resolves occlusion BEFORE the fill, so visible spans of different surfaces
 * are disjoint in screen space. Workers drawing different surfaces write
 * disjoint framebuffer + z-buffer pixels. The per-surface span-drawer globals
 * are already `__thread`; the surf_t + s->fill the workers consume are
 * read-only during the draw.
 *
 * Memory ordering: the SDL semaphore signal/wait pairs are release/acquire
 * fences. SignalSemaphore on the caller happens-before WaitSemaphore on the
 * worker, so the worker observes the published s_fn/s_end/s_next writes;
 * SignalSemaphore on the worker (s_done) happens-before the caller's matching
 * WaitSemaphore, so the caller observes every worker's framebuffer writes
 * after the barrier. We rely entirely on these -- no manual fences.
 */

#include "quakedef.h"
#include "r_threadfill.h"

#include <SDL3/SDL.h>

#define R_THREADFILL_CAP 16

static int               s_inited     = 0;
static int               s_n_workers  = 0;       /* worker threads (caller is +1) */
static SDL_Thread       *s_threads[R_THREADFILL_CAP];

/* Published work for the current dispatch. */
static SDL_AtomicInt     s_next;                  /* next index to claim (atomic) */
static int               s_end;                   /* one past the last index */
static void            (*s_fn)(int idx);          /* per-index callback */

static SDL_Semaphore    *s_start;                 /* released once per worker per run */
static SDL_Semaphore    *s_done;                  /* signalled once per worker per run */
static SDL_AtomicInt     s_quit;                  /* set at shutdown to retire workers */

static cvar_t            r_threads = {"r_threads", "1", true};

/*
 * Atomic grab-loop run by both the workers and the calling thread.
 * SDL_AddAtomicInt returns the PRE-add value and is atomic, so each call
 * yields a unique index; the loop ends once every index has been claimed.
 */
static void grab_loop (void)
{
    int i;
    void (*fn)(int) = s_fn;
    int   end       = s_end;

    while ((i = SDL_AddAtomicInt(&s_next, 1)) < end)
        fn(i);
}

static int SDLCALL worker_main (void *ud)
{
    (void)ud;
    for (;;)
    {
        SDL_WaitSemaphore(s_start);
        if (SDL_GetAtomicInt(&s_quit))
            break;
        grab_loop();
        SDL_SignalSemaphore(s_done);
    }
    return 0;
}

void R_ThreadFill_Init (void)
{
    int n, want, i;

    if (s_inited)
        return;
    s_inited = 1;

    Cvar_RegisterVariable (&r_threads);

    SDL_SetAtomicInt(&s_quit, 0);
    SDL_SetAtomicInt(&s_next, 0);

    n = SDL_GetNumLogicalCPUCores();
    if (n < 1)
        n = 1;
    if (n > R_THREADFILL_CAP)
        n = R_THREADFILL_CAP;

    /* The calling thread is the Nth fill lane, so we spawn n-1 workers. With
     * a single core we spawn zero -> R_ThreadFill_Enabled() returns 0 and the
     * serial path is used. */
    want = n - 1;
    if (want <= 0)
    {
        s_n_workers = 0;
        Con_Printf ("R_ThreadFill: %d core%s, fill stays serial\n",
                    n, n == 1 ? "" : "s");
        return;
    }

    s_start = SDL_CreateSemaphore(0);
    s_done  = SDL_CreateSemaphore(0);
    if (!s_start || !s_done)
    {
        if (s_start) { SDL_DestroySemaphore(s_start); s_start = NULL; }
        if (s_done)  { SDL_DestroySemaphore(s_done);  s_done  = NULL; }
        s_n_workers = 0;
        Con_Printf ("R_ThreadFill: semaphore alloc failed, fill stays serial\n");
        return;
    }

    for (i = 0; i < want; i++)
    {
        char name[32];
        SDL_snprintf(name, sizeof(name), "rfill%d", i);
        s_threads[i] = SDL_CreateThread(worker_main, name, NULL);
        if (!s_threads[i])
        {
            /* Could not spawn the full set; keep whatever started. */
            Con_Printf ("R_ThreadFill: only spawned %d/%d workers\n", i, want);
            break;
        }
        s_n_workers++;
    }

    Con_Printf ("R_ThreadFill: %d cores, %d fill worker%s + main\n",
                n, s_n_workers, s_n_workers == 1 ? "" : "s");
}

int R_ThreadFill_Enabled (void)
{
    return (s_n_workers > 0 && r_threads.value != 0.0f);
}

void R_ThreadFill_Run (void (*fn)(int idx), int begin, int end)
{
    int i;

    /* Publish the work range + callback before releasing any worker. The
     * SDL_SignalSemaphore below is a release fence, so these writes are
     * visible to a worker once it returns from SDL_WaitSemaphore. */
    s_fn  = fn;
    s_end = end;
    SDL_SetAtomicInt(&s_next, begin);

    /* If threading is disabled (cvar off / no workers) just run it inline on
     * the calling thread -- still correct, just serial. */
    if (s_n_workers <= 0 || r_threads.value == 0.0f)
    {
        grab_loop();
        return;
    }

    /* Release the workers, then pitch in on the calling thread. */
    for (i = 0; i < s_n_workers; i++)
        SDL_SignalSemaphore(s_start);

    grab_loop();

    /* Barrier: one done-signal per worker. The wait is an acquire fence, so
     * after this loop every worker's framebuffer/z writes are visible here. */
    for (i = 0; i < s_n_workers; i++)
        SDL_WaitSemaphore(s_done);
}

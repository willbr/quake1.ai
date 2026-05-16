/*
 * light_lib.c -- in-process entry point for the ported id-LIGHT compiler.
 *
 * Piggy-backs on qbsp's globals: the editor calls qbsp_compile_to_memory
 * first (which leaves dfaces / dplanes / dnodes / dleafs / dentdata / etc.
 * populated and dlightdata empty), then calls light_compile_to_memory
 * which runs LightWorld() to fill dlightdata, then re-runs WriteBSPFile
 * to emit a fresh BSP byte buffer with lighting baked in. The .lit
 * sidecar emission lands in Stage 2.
 *
 * Sharing qbsp's bspfile.c globals avoids a load-from-bytes round trip:
 * we never serialise the BSP, hand it back, parse it again. The two
 * compilers run as a pair against the same in-memory representation.
 *
 * Threads: vanilla id-LIGHT uses pthreads per face. We single-thread for
 * now (RunThreadsOn collapses to a direct call). Baking e1m1 takes a
 * couple of seconds; a future cut can revisit if it stings.
 */

/* light.h pulls in cmdlib.h, mathlib.h, bspfile.h, entities.h, threads.h.
 * Those headers lack include guards (1996-era id source) so re-including
 * them here would produce typedef-redefinition errors. */
#include "light.h"
#include "light_lib.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* qbsp's tracked allocator is installed via cmdlib.h's malloc/free macros.
 * This translation unit IS a library, not user code, so it needs real
 * libc malloc for the membuf-take dance — undef after the include. */
#undef malloc
#undef free

extern void Con_Printf(char *fmt, ...);

/* qbsp_lib.c owns the longjmp + membuf machinery; reuse them directly
 * rather than duplicating. The shared cmdlib.c Error() routes both
 * compilers' aborts here. */
extern jmp_buf *qbsp_err_jmp;
extern char     qbsp_err_msg[1024];

extern int   qbsp_membuf_active;
extern byte *qbsp_membuf;
extern int   qbsp_membuf_size;
extern void  qbsp_membuf_reset(void);
extern byte *qbsp_membuf_take(int *out_size);

/* Thread stubs -- the namespace header renames these to light_*, so
 * defining them here gives the ported sources a no-op locking layer. */
int numthreads = 1;

void InitThreads(void)
{
}

void RunThreadsOn(threadfunc_t func)
{
    func(NULL);
}

/* qbsp's namespace renames cmdlib's printf to qbsp_con_print. Same
 * cmdlib.h is included for light, so light's printf calls also route
 * to qbsp_con_print, which forwards to Con_Printf. Nothing more to do
 * here. */

/* Per-file reset helpers live alongside the file-scope state they're
 * resetting. The namespace header doesn't mangle these names, so the
 * declarations and the definitions match. */
void light_reset_lightc    (void);
void light_reset_entitiesc (void);
void light_reset_ltfacec   (void);
void light_reset_tracec    (void);

static void light_reset_state(void)
{
    light_reset_lightc();
    light_reset_entitiesc();
    light_reset_ltfacec();
    light_reset_tracec();

    /* dlightdata is shared with qbsp; qbsp leaves it empty (lightdatasize
     * == 0). LightWorld will fill it. Clear in case a previous bake left
     * stale bytes that a partial run wrote past. */
    lightdatasize = 0;
    memset(dlightdata, 0, sizeof(dlightdata));
}

extern byte light_rgb_dlightdata[];

/* Persistent bake options. Both light_compile_to_memory (when called with
 * NULL opts) and light_relight_in_place reapply these after light_reset_state
 * wipes the per-bake globals back to compile-time defaults. The defaults
 * here mirror light.c:13-15,24 so a never-set caller behaves identically
 * to the original argv-driven CLI. */
static light_options_t s_persistent_opts = {
    1.0f,  /* scaledist    */
    0.5f,  /* scalecos     */
    0.5f,  /* rangescale   */
    0,     /* extrasamples */
    0,     /* dirt_enable  */
    1.0f,  /* dirt_gain    */
    384.0f,/* dirt_depth   */
    32,    /* dirt_samples */
    0      /* dirt_debug   */
};

void light_set_persistent_options(const light_options_t *opts)
{
    if (!opts) {
        s_persistent_opts.scaledist    = 1.0f;
        s_persistent_opts.scalecos     = 0.5f;
        s_persistent_opts.rangescale   = 0.5f;
        s_persistent_opts.extrasamples = 0;
        s_persistent_opts.dirt_enable  = 0;
        s_persistent_opts.dirt_gain    = 1.0f;
        s_persistent_opts.dirt_depth   = 384.0f;
        s_persistent_opts.dirt_samples = 32;
        s_persistent_opts.dirt_debug   = 0;
        return;
    }
    s_persistent_opts = *opts;
}

static void apply_persistent_opts(void)
{
    if (s_persistent_opts.scaledist  > 0) scaledist  = s_persistent_opts.scaledist;
    if (s_persistent_opts.scalecos   > 0) scalecos   = s_persistent_opts.scalecos;
    if (s_persistent_opts.rangescale > 0) rangescale = s_persistent_opts.rangescale;
    extrasamples = s_persistent_opts.extrasamples ? true : false;

    dirt_enable = s_persistent_opts.dirt_enable ? true : false;
    /* >0 guards so a struct zeroed-out by memset(0) still gets useful
     * defaults instead of dirt_depth=0 (every ray hits at zero range). */
    if (s_persistent_opts.dirt_gain    > 0) dirt_gain    = s_persistent_opts.dirt_gain;
    if (s_persistent_opts.dirt_depth   > 0) dirt_depth   = s_persistent_opts.dirt_depth;
    if (s_persistent_opts.dirt_samples > 0) dirt_samples = s_persistent_opts.dirt_samples;
    dirt_debug = s_persistent_opts.dirt_debug ? true : false;
}

int light_compile_to_memory(light_options_t *opts,
                            void **out_bsp, int *out_size,
                            void **out_lit, int *out_lit_size)
{
    jmp_buf err_buf;
    int     bsp_size = 0;

    if (out_bsp)  *out_bsp = NULL;
    if (out_size) *out_size = 0;
    if (out_lit)      *out_lit = NULL;
    if (out_lit_size) *out_lit_size = 0;
    if (!out_bsp || !out_size) return 1;

    /* The caller (editor) must have just run qbsp_compile_to_memory.
     * If numfaces is 0, qbsp didn't actually populate anything and
     * we'd run LightFace on garbage. */
    if (numfaces <= 0) {
        Con_Printf("light: no BSP in memory (caller must qbsp first)\n");
        return 1;
    }

    light_reset_state();

    /* Per-call opts override the persistent ones for this bake AND
     * promote into the persistent slot so subsequent live-bakes stay in
     * sync with the editor's last explicit "Compile + Light" settings. */
    if (opts) {
        s_persistent_opts = *opts;
    }
    apply_persistent_opts();

    /* Hand error recovery back to qbsp_lib.c's shared longjmp. Any
     * Error() inside the ported sources unwinds back here. */
    qbsp_err_jmp    = &err_buf;
    qbsp_err_msg[0] = '\0';
    if (setjmp(err_buf) != 0) {
        Con_Printf("light: %s\n", qbsp_err_msg);
        qbsp_err_jmp = NULL;
        return 1;
    }

    {
        double t0, t1, t2, t3;
        extern double I_FloatTime(void);
        t0 = I_FloatTime();
        LoadEntities();
        t1 = I_FloatTime();
        MakeTnodes(&dmodels[0]);
        t2 = I_FloatTime();
        LightWorld();
        t3 = I_FloatTime();
        WriteEntitiesToString();
        Con_Printf("light timing: load=%.3fs tnodes=%.3fs bake=%.3fs (faces=%d, lights=%d)\n",
                   t1 - t0, t2 - t1, t3 - t2,
                   numfaces, num_entities);
    }

    /* Re-emit the BSP with updated lightdata + entdata. WriteBSPFile
     * inspects qbsp_membuf_active and routes its bytes into the membuf
     * instead of opening a file -- same mechanism the qbsp pipeline
     * already uses. */
    qbsp_membuf_active = 1;
    qbsp_membuf_reset();
    WriteBSPFile("light_inproc.bsp");

    qbsp_err_jmp = NULL;
    qbsp_membuf_active = 0;

    *out_bsp  = qbsp_membuf_take(&bsp_size);
    *out_size = bsp_size;
    if (!*out_bsp || bsp_size <= 0) {
        Con_Printf("light: WriteBSPFile produced no output\n");
        return 1;
    }

    /* End of normal compile path -- emit .lit if requested. */
    if (out_lit && out_lit_size && lightdatasize > 0) {
        /* QLIT v1 sidecar: 4 byte magic + int32 version + RGB stream.
         * Total bytes = 8 + 3 * lightdatasize. The RGB stream is sliced
         * straight out of light_rgb_dlightdata, which LightFace filled
         * in parallel with the mono path. */
        int lit_total = 8 + lightdatasize * 3;
        byte *lit = (byte *)malloc((size_t)lit_total);
        if (!lit) {
            Con_Printf("light: out of memory allocating .lit (%d bytes)\n",
                       lit_total);
        } else {
            lit[0] = 'Q'; lit[1] = 'L'; lit[2] = 'I'; lit[3] = 'T';
            /* version 1, little-endian. We're x86-only so a memcpy
             * of the host int suffices; spell it out for clarity. */
            lit[4] = 1; lit[5] = 0; lit[6] = 0; lit[7] = 0;
            memcpy(lit + 8, light_rgb_dlightdata, (size_t)lightdatasize * 3);
            *out_lit      = lit;
            *out_lit_size = lit_total;
        }
    }
    return 0;
}

/* Stash a caller-provided entity string into dentdata. The bake's
 * LoadEntities reads from there. We trim to fit MAX_MAP_ENTSTRING -- if
 * the user's scene is bigger than 64 KiB of entity text we'd already
 * have other problems. */
void light_set_entdata(const char *ents, int ents_len)
{
    int n = ents_len;
    if (n < 0) n = 0;
    if (n >= MAX_MAP_ENTSTRING) n = MAX_MAP_ENTSTRING - 1;
    if (ents && n > 0) memcpy(dentdata, ents, (size_t)n);
    dentdata[n] = '\0';
    entdatasize = n + 1;
}

/* Worker-thread entry: just run the bake against whatever's currently
 * in qbsp's globals + light_set_entdata'd dentdata. No setjmp here --
 * the worker thread does its own try/catch via SDL_Thread + atomics.
 * Any Error() longjmp would land in the main thread's setjmp slot,
 * which would be a disaster. So we set qbsp_err_jmp to NULL for the
 * duration; an Error() will exit the process, which we treat as
 * "should not happen during routine relights" and accept the cost. */
int light_relight_in_place(void)
{
    if (numfaces <= 0) return 1;

    light_reset_state();
    apply_persistent_opts();

    /* Clear destination buffers so an aborted face doesn't leave stale
     * bytes from a prior bake. light_rgb_dlightdata is `extern byte[]`
     * here so we use MAX_MAP_LIGHTING*3 explicitly. */
    memset(dlightdata, 0, sizeof(dlightdata));
    memset(light_rgb_dlightdata, 0, MAX_MAP_LIGHTING * 3);
    lightdatasize = 0;

    qbsp_err_jmp = NULL;   /* Error() will abort; see comment above */

    LoadEntities();
    MakeTnodes(&dmodels[0]);
    LightWorld();
    /* WriteEntitiesToString rebuilds dentdata from the in-memory
     * entities[] (which LoadEntities populated). Skip it: we don't
     * re-serialise the BSP and the caller's edit_scene is the source
     * of truth for the next bake anyway. */
    return 0;
}

int light_snapshot_result(unsigned char *out_mono, int mono_cap,
                          unsigned char *out_rgb,
                          int          *out_lightofs, int max_faces,
                          int *out_mono_size, int *out_face_count)
{
    int i;
    if (!out_mono || !out_rgb || !out_lightofs ||
        !out_mono_size || !out_face_count)
        return -1;
    if (lightdatasize > mono_cap || numfaces > max_faces)
        return -1;

    memcpy(out_mono, dlightdata, (size_t)lightdatasize);
    memcpy(out_rgb,  light_rgb_dlightdata, (size_t)lightdatasize * 3);

    for (i = 0; i < numfaces; i++)
        out_lightofs[i] = dfaces[i].lightofs;

    *out_mono_size  = lightdatasize;
    *out_face_count = numfaces;
    return 0;
}

/* Stand-alone bench: read a real .bsp from disk via the shared
 * cmdlib/bspfile path, then run the bake without touching qbsp's
 * globals. Discards the result. Wired to the `light_bench <name>`
 * console command. */
int light_relight_loaded_bsp(const char *bsp_path)
{
    jmp_buf err_buf;

    if (!bsp_path || !bsp_path[0]) return 1;

    light_reset_state();

    qbsp_err_jmp    = &err_buf;
    qbsp_err_msg[0] = '\0';
    if (setjmp(err_buf) != 0) {
        Con_Printf("light_relight_loaded_bsp: %s\n", qbsp_err_msg);
        qbsp_err_jmp = NULL;
        return 1;
    }

    LoadBSPFile((char *)bsp_path);

    /* Reapply persistent opts AFTER load (which may have changed
     * dirt_samples via reset) and BEFORE LightWorld so the bake uses
     * the editor's current slider values. */
    apply_persistent_opts();

    lightdatasize = 0;
    memset(dlightdata, 0, sizeof(dlightdata));
    memset(light_rgb_dlightdata, 0, MAX_MAP_LIGHTING * 3);

    LoadEntities();
    MakeTnodes(&dmodels[0]);
    LightWorld();

    qbsp_err_jmp = NULL;
    return 0;
}

int light_bench(const char *bsp_path)
{
    extern double I_FloatTime(void);
    jmp_buf err_buf;
    double  tL0, tL1, t0, t1, t2, t3;
    int     lit_size;

    if (!bsp_path || !bsp_path[0]) {
        Con_Printf("light_bench: no bsp_path\n");
        return 1;
    }

    light_reset_state();

    qbsp_err_jmp    = &err_buf;
    qbsp_err_msg[0] = '\0';
    if (setjmp(err_buf) != 0) {
        Con_Printf("light_bench: %s\n", qbsp_err_msg);
        qbsp_err_jmp = NULL;
        return 1;
    }

    tL0 = I_FloatTime();
    LoadBSPFile((char *)bsp_path);
    tL1 = I_FloatTime();
    Con_Printf("light_bench: LoadBSPFile %.3fs (faces=%d, ents=%d bytes)\n",
               tL1 - tL0, numfaces, entdatasize);

    /* Honour whatever options the caller stashed via set_persistent_options
     * so `light_bench` can profile dirt / extrasamples / rangescale tweaks
     * end-to-end without an editor session. */
    apply_persistent_opts();

    /* Clear any stale lighting in dlightdata from a prior bake.
     * light_rgb_dlightdata is `extern byte[]` here (real size lives in
     * light.c); use the matching MAX_MAP_LIGHTING*3 constant for the
     * memset rather than sizeof on an incomplete type. */
    lightdatasize = 0;
    memset(dlightdata, 0, sizeof(dlightdata));
    memset(light_rgb_dlightdata, 0, MAX_MAP_LIGHTING * 3);

    t0 = I_FloatTime();
    LoadEntities();
    t1 = I_FloatTime();
    MakeTnodes(&dmodels[0]);
    t2 = I_FloatTime();
    LightWorld();
    t3 = I_FloatTime();

    lit_size = lightdatasize * 3;
    Con_Printf("light_bench: load=%.3fs tnodes=%.3fs bake=%.3fs total=%.3fs\n",
               t1 - t0, t2 - t1, t3 - t2, t3 - t0);
    Con_Printf("light_bench: %d lights, %d ent strings, lit=%d bytes\n",
               num_entities, num_entities, lit_size);

    qbsp_err_jmp = NULL;
    return 0;
}

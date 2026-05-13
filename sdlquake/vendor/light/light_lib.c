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

    if (opts) {
        if (opts->scaledist  > 0) scaledist  = opts->scaledist;
        if (opts->scalecos   > 0) scalecos   = opts->scalecos;
        if (opts->rangescale > 0) rangescale = opts->rangescale;
        extrasamples = opts->extrasamples ? true : false;
    }

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

/* Stand-alone bench: read a real .bsp from disk via the shared
 * cmdlib/bspfile path, then run the bake without touching qbsp's
 * globals. Discards the result. Wired to the `light_bench <name>`
 * console command. */
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

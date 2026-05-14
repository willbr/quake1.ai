/*
 * vis_lib.c -- in-process entry point for the ported id-VIS compiler.
 *
 * Piggy-backs on qbsp's globals: the editor calls qbsp_compile_to_memory
 * first (which leaves dfaces / dplanes / dnodes / dleafs / etc. populated
 * and writes a .prt next to the destname's stem), then vis_compile_in_place
 * which runs LoadPortals + CalcVis + optional CalcAmbientSounds to fill
 * dvisdata in place. The caller is expected to chain into
 * light_compile_to_memory which re-runs WriteBSPFile to emit a fresh BSP
 * byte buffer with both PVS and lightmaps baked in.
 *
 * Threads: vanilla id-VIS uses pthreads behind #ifdef __alpha which we
 * don't define; the LOCK/UNLOCK macros collapse to no-ops and the
 * CalcPortalVis main loop runs single-threaded on the main thread.
 */

/* vis.h pulls in cmdlib.h, mathlib.h, bspfile.h. Those headers lack
 * include guards in id's 1996 source so re-including them here would
 * produce typedef-redefinition errors. */
#include "vis.h"
#include "vis_lib.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* qbsp's tracked allocator is installed via cmdlib.h's malloc/free macros.
 * vis_lib.c handles its own bookkeeping for the per-call portals/leafs
 * allocations; #undef so the membuf-take dance has real libc symbols. */
#undef malloc
#undef free

extern void Con_Printf(char *fmt, ...);

/* qbsp_lib.c owns the longjmp + membuf machinery; reuse them directly
 * rather than duplicating. The shared cmdlib.c Error() routes all three
 * compilers' aborts here. */
extern jmp_buf *qbsp_err_jmp;
extern char     qbsp_err_msg[1024];

/* Per-file reset helpers. The namespace header doesn't mangle these
 * names. Defined in vis.c / flow.c immediately after the global
 * declarations they reset. */
void vis_reset_visc (void);
void vis_reset_flowc(void);

/* vis.h doesn't declare these two globals even though vis.c defines them.
 * Declare them here so apply_options can write them. The namespace header
 * macros expand `fastvis` -> `vis_fastvis`, `verbose` -> `vis_verbose`. */
extern qboolean fastvis;
extern qboolean verbose;

static void vis_reset_state(void)
{
    vis_reset_visc();
    vis_reset_flowc();

    /* dvisdata is shared with qbsp; qbsp leaves it empty (visdatasize == 0).
     * CalcVis will fill it via LeafFlow. Clear in case a previous run left
     * stale bytes. */
    visdatasize = 0;
    memset(dvisdata, 0, sizeof(dvisdata));
}

/* Default-construct the options struct. Defaults match VIS's main()
 * defaults: testlevel=2, single-threaded, no fast, no verbose. */
static void apply_options(const vis_options_t *opts)
{
    /* These are vis_namespace.h-prefixed symbols at the call site; the
     * compiler will resolve them to the prefixed names. */
    fastvis    = false;
    verbose    = false;
    testlevel  = 2;
    if (!opts) return;
    if (opts->fast)    fastvis = true;
    if (opts->verbose) verbose = true;
    if (opts->level > 0) {
        testlevel = opts->level;
        if (testlevel > 4) testlevel = 4;
    }
}

int vis_compile_in_place(const char *prt_path, const vis_options_t *opts)
{
    jmp_buf err_buf;
    double  t0, t1, t2, t3;
    extern double I_FloatTime(void);

    if (!prt_path || !prt_path[0]) {
        Con_Printf("vis: no .prt path\n");
        return 1;
    }
    if (numfaces <= 0) {
        Con_Printf("vis: no BSP in memory (caller must qbsp first)\n");
        return 1;
    }

    vis_reset_state();
    apply_options(opts);

    qbsp_err_jmp    = &err_buf;
    qbsp_err_msg[0] = '\0';
    if (setjmp(err_buf) != 0) {
        Con_Printf("vis: %s\n", qbsp_err_msg);
        if (uncompressed) { free(uncompressed); uncompressed = NULL; }
        qbsp_err_jmp = NULL;
        return 1;
    }

    t0 = I_FloatTime();
    LoadPortals((char *)prt_path);
    t1 = I_FloatTime();

    /* Workspace allocated by VIS's main() prior to CalcVis. Sized
     * (portalleafs+63)>>3 bytes per leaf; ~125 KB for ~1000-leaf maps. */
    uncompressed = (byte *)malloc((size_t)(bitbytes * portalleafs));
    if (!uncompressed) {
        Con_Printf("vis: out of memory for uncompressed buffer (%d bytes)\n",
                   bitbytes * portalleafs);
        qbsp_err_jmp = NULL;
        return 1;
    }
    memset(uncompressed, 0, (size_t)(bitbytes * portalleafs));

    CalcVis();
    t2 = I_FloatTime();

    visdatasize = vismap_p - dvisdata;
    Con_Printf("vis: %d portalleafs, %d portals, visdata=%d bytes\n",
               portalleafs, numportals, visdatasize);

    if (!opts || !opts->skip_sound_pvs) {
        CalcAmbientSounds();
    }
    t3 = I_FloatTime();

    Con_Printf("vis timing: load=%.3fs calcvis=%.3fs sound=%.3fs total=%.3fs\n",
               t1 - t0, t2 - t1, t3 - t2, t3 - t0);

    free(uncompressed);
    uncompressed = NULL;
    qbsp_err_jmp = NULL;
    return 0;
}

int vis_bench(const char *bsp_path, const char *prt_path)
{
    extern double I_FloatTime(void);
    jmp_buf err_buf;
    double  tL0, tL1, t0, t1, t2;

    if (!bsp_path || !bsp_path[0] || !prt_path || !prt_path[0]) {
        Con_Printf("vis_bench: bsp_path or prt_path missing\n");
        return 1;
    }

    vis_reset_state();
    apply_options(NULL);

    qbsp_err_jmp    = &err_buf;
    qbsp_err_msg[0] = '\0';
    if (setjmp(err_buf) != 0) {
        Con_Printf("vis_bench: %s\n", qbsp_err_msg);
        if (uncompressed) { free(uncompressed); uncompressed = NULL; }
        qbsp_err_jmp = NULL;
        return 1;
    }

    tL0 = I_FloatTime();
    LoadBSPFile((char *)bsp_path);
    tL1 = I_FloatTime();
    Con_Printf("vis_bench: LoadBSPFile %.3fs (faces=%d, leafs=%d)\n",
               tL1 - tL0, numfaces, numleafs);

    /* Clear any stale vis from the loaded BSP and re-bake. */
    visdatasize = 0;
    memset(dvisdata, 0, sizeof(dvisdata));

    t0 = I_FloatTime();
    LoadPortals((char *)prt_path);
    t1 = I_FloatTime();

    uncompressed = (byte *)malloc((size_t)(bitbytes * portalleafs));
    if (!uncompressed) {
        Con_Printf("vis_bench: out of memory\n");
        qbsp_err_jmp = NULL;
        return 1;
    }
    memset(uncompressed, 0, (size_t)(bitbytes * portalleafs));

    CalcVis();
    t2 = I_FloatTime();

    visdatasize = vismap_p - dvisdata;
    Con_Printf("vis_bench: load=%.3fs calcvis=%.3fs total=%.3fs visdata=%d\n",
               t1 - t0, t2 - t1, t2 - t0, visdatasize);

    free(uncompressed);
    uncompressed = NULL;
    qbsp_err_jmp = NULL;
    return 0;
}

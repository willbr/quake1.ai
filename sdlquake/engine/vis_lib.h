/*
 * vis_lib.h -- public API for in-process PVS computation.
 *
 * Pairs with qbsp_lib.h and light_lib.h. Caller flow:
 *
 *   qbsp_compile_to_memory(...)        -- geometry + .prt on disk
 *   vis_compile_in_place(prt_path, ...)-- PVS into dvisdata global
 *   light_compile_to_memory(...)       -- lighting baked, BSP re-serialised
 *                                         (includes PVS just filled)
 *
 * vis_compile_in_place does NOT return a BSP buffer. It mutates qbsp's
 * shared globals (dvisdata, dleafs[].visofs, dleafs[].ambient_level)
 * in place. Light's WriteBSPFile call later re-emits everything.
 *
 * v1 caller invariants (M1):
 *   - One vis_compile_in_place call per process. Globals are not reset
 *     between calls; a second call may produce wrong output or crash.
 *   - The .prt at `prt_path` must exist on disk. qbsp_compile_to_memory
 *     writes it as a side-effect at <gamedir>/maps/<mapname>.prt.
 *   - On failure, returns non-zero and prints via Con_Printf; the BSP
 *     globals are left in a likely-corrupt state. Caller should discard.
 */

#ifndef VIS_LIB_H
#define VIS_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int fast;             /* -fast: flood-fill PVS, ~instant, not real visibility */
    int level;            /* test depth, default 2, max 4 */
    int verbose;
    int skip_sound_pvs;   /* skip CalcAmbientSounds (PHS lump) */
} vis_options_t;

/*
 * Run the PVS pass against the BSP currently sitting in qbsp's globals,
 * sourcing portal connectivity from `prt_path` on disk.
 *
 * Returns 0 on success, non-zero on failure (error printed via Con_Printf).
 *
 * `opts` may be NULL for defaults (level=2, no fast, no verbose, sound PVS
 * enabled).
 */
int vis_compile_in_place(const char *prt_path, const vis_options_t *opts);

/*
 * Stand-alone bench/diagnostic: loads `bsp_path` + `prt_path` from disk
 * into the shared globals via the ported LoadBSPFile/LoadPortals, runs
 * the vis pass, times each phase, and discards the output. Used by the
 * `vis_bench` console command to size the cost of a full re-vis on
 * authoring-scale maps. Returns 0 on success.
 *
 * Same single-call caveat as vis_compile_in_place.
 */
int vis_bench(const char *bsp_path, const char *prt_path);

#ifdef __cplusplus
}
#endif

#endif /* VIS_LIB_H */

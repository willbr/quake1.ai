/*
 * light_lib.h -- public API for in-process .bsp lighting.
 *
 * Pairs with qbsp_lib.h: the caller invokes qbsp_compile_to_memory() to
 * compile a .map into a .bsp byte buffer, then immediately invokes
 * light_compile_to_memory() (without intervening qbsp_compile_to_memory
 * runs, which would reset the shared globals) to bake lighting and
 * obtain a fresh BSP buffer with lightdata populated.
 *
 * Stage 1 (current): emits mono lightmaps embedded in the BSP only.
 * Stage 2 will add a parallel `out_lit` buffer for coloured-light data.
 *
 * Both compilers share qbsp's tracked allocator + longjmp error path;
 * a failure inside light_compile_to_memory unwinds cleanly without
 * leaving the engine in a bad state.
 */

#ifndef LIGHT_LIB_H
#define LIGHT_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float scaledist;     /* default 1.0  -- light range scaling */
    float scalecos;      /* default 0.5  -- ambient term in N.L */
    float rangescale;    /* default 0.5  -- output intensity scale */
    int   extrasamples;  /* 1 = 2x2 supersample per lightmap pixel (slow) */

    /* Ambient occlusion ("dirt"): per-sample hemispherical ray cast,
     * occluded fraction attenuates the lightmap. Off by default; flipping
     * dirt_enable on multiplies bake time by ~dirt_samples / 3 since each
     * sample point gains N extra TestLine probes on top of the per-light
     * shadow rays. dirt_debug overrides the lightmap with the raw AO
     * term in grey so the mask is visible without light entities. */
    int   dirt_enable;   /* 0 = off (default), 1 = on */
    float dirt_gain;     /* default 1.0  -- amount AO attenuates (0..1) */
    float dirt_depth;    /* default 128  -- ray length, world units */
    int   dirt_samples;  /* default 32   -- rays per sample (clamped 1..128) */
    int   dirt_debug;    /* 0 = normal, 1 = write AO mask as grey */
} light_options_t;

/*
 * Set the persistent bake options used by subsequent compiles AND live
 * relights. NULL restores defaults. Without this, the live-bake path
 * (light_relight_in_place) always runs with hard-coded defaults because
 * its reset_state wipes the scale globals back to {1.0, 0.5, 0.5, off}.
 * Call this once when the editor's slider values change; the next bake
 * — whichever entry point fires it — picks them up.
 */
void light_set_persistent_options(const light_options_t *opts);

/*
 * Bake lighting onto the BSP currently sitting in qbsp's globals.
 * Returns 0 on success, non-zero on failure (error printed via Con_Printf).
 *
 * On success: *out_bsp = malloc'd buffer of *out_size bytes containing the
 * full re-serialised .bsp (caller free()s).
 *
 * If out_lit is non-NULL: *out_lit = malloc'd buffer of *out_lit_size bytes
 * containing the QLIT-prefixed coloured-lighting sidecar (8-byte header
 * "QLIT" + int32 version=1 + RGB samples). Caller free()s. NULL means the
 * caller is happy with mono-only output.
 *
 * `opts` may be NULL for defaults.
 */
int light_compile_to_memory(light_options_t *opts,
                            void **out_bsp, int *out_size,
                            void **out_lit, int *out_lit_size);

/*
 * Stand-alone bench/diagnostic: loads `bsp_path` (a regular .bsp on
 * disk) into LIGHT's globals via the ported LoadBSPFile, runs the bake,
 * times each phase, and discards the output. Used by the `light_bench`
 * console command to size the cost of a full re-bake when sketching
 * whether on-edit live baking is feasible. Returns 0 on success.
 */
int light_bench(const char *bsp_path);

/*
 * Set the entity-data lump (dentdata) directly from a caller-provided
 * string. The string is COM_Parse-style: `{ "key" "value" ... }` blocks
 * separated by whitespace. Used by the live-bake path so editor edits
 * can flow into LIGHT's LoadEntities without round-tripping through a
 * .map file on disk.
 */
void light_set_entdata(const char *ents, int ents_len);

/*
 * Run LightWorld against the BSP currently sitting in qbsp's globals.
 * Caller must have populated those globals via a prior
 * qbsp_compile_to_memory + light_compile_to_memory pass; this entry
 * just re-runs the bake (LoadEntities + MakeTnodes + LightWorld). On
 * success the caller can snapshot the result via light_snapshot_result.
 *
 * Designed for the SDL_Thread live-bake worker: no allocation, no
 * membuf, no setjmp. Returns 0 on success, non-zero on failure.
 */
int light_relight_in_place(void);

/*
 * Copy the most recent bake's output into caller buffers. `out_mono`
 * must hold at least `mono_cap` bytes (caller should size to
 * MAX_MAP_LIGHTING = 0x100000); `out_rgb` must hold at least 3x
 * `mono_cap`. `out_lightofs` must hold at least `max_faces` ints.
 *
 * Returns 0 on success and writes:
 *   *out_mono_size = lightdatasize (bytes written into out_mono)
 *   *out_face_count = numfaces (entries written into out_lightofs)
 *
 * Returns -1 on capacity overflow without touching the caller buffers.
 *
 * Lightofs values are the new per-face byte offsets into out_mono; the
 * caller uses these to repoint each engine surface's `samples` and
 * `rgb_samples` after copying out_mono / out_rgb into its own buffer.
 */
int light_snapshot_result(unsigned char *out_mono, int mono_cap,
                          unsigned char *out_rgb,
                          int          *out_lightofs, int max_faces,
                          int *out_mono_size, int *out_face_count);

#ifdef __cplusplus
}
#endif

#endif /* LIGHT_LIB_H */

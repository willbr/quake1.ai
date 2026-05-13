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
} light_options_t;

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

#ifdef __cplusplus
}
#endif

#endif /* LIGHT_LIB_H */

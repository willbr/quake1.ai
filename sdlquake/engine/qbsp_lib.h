/*
 * qbsp_lib.h -- public API for in-process map -> bsp compilation.
 *
 * This wraps id Software's qbsp source (vendored under sdlquake/vendor/qbsp/)
 * with a callable entry point that returns the .bsp as a malloc'd byte
 * buffer instead of writing to disk. The engine's editor calls this on
 * "Compile + Reload" so a wrapped func_door can be played without leaving
 * the running game.
 *
 * Caller invariants for v1 (M1):
 *   - One qbsp_compile_to_memory() call per process. qbsp's globals are
 *     not reset between calls; a second call will likely produce wrong
 *     output or crash.
 *   - The returned buffer is allocated with malloc(); caller free()s.
 *   - On error, *out_bsp is left NULL and the function returns non-zero;
 *     a human-readable error message has already been printed via the
 *     engine's Con_Printf path.
 */

#ifndef QBSP_LIB_H
#define QBSP_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int notjunc;        /* skip t-junction repair */
    int nofill;         /* skip outside flood-fill (debug) */
    int noclip;         /* skip clip hull generation (visual-only) */
    int onlyents;       /* re-emit just the entity lump on top of an existing .bsp */
    int verbose;        /* -verbose */
    int subdivide;      /* surface subdivision threshold; 0 = qbsp default (240) */
} qbsp_options_t;

/*
 * Compile `map_path` (read from disk) into a .bsp byte buffer (in RAM).
 * Returns 0 on success, non-zero on failure.
 *
 * On success: *out_bsp points to a malloc'd buffer of *out_size bytes;
 * caller free()s.
 * On failure: *out_bsp == NULL, *out_size == 0, error already printed
 * to Con_Printf.
 *
 * `opts` may be NULL for defaults.
 */
int qbsp_compile_to_memory(const char *map_path,
                           void **out_bsp, int *out_size,
                           const qbsp_options_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* QBSP_LIB_H */

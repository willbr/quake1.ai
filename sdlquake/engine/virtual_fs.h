/*
 * virtual_fs.h -- in-memory file shim for the engine's loader path.
 *
 * The editor's qbsp library produces a .bsp byte buffer in RAM and
 * registers it here under a virtual path like "maps/test.bsp"; the
 * patched COM_LoadFile (engine_src/common.c) then hands those bytes to
 * Mod_LoadBrushModel without ever touching disk. From the rest of the
 * engine's perspective the virtual file is indistinguishable from a
 * real one — same allocation policy, same null-termination, same load
 * path.
 *
 * Lifetime: registered bytes are owned by the registry. Re-registering
 * a path frees the previous bytes and replaces. Editor_VFS_Shutdown
 * drops everything.
 */

#ifndef VIRTUAL_FS_H
#define VIRTUAL_FS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register `bytes` under `path`. Ownership of `bytes` transfers — the
 * registry will free() it on replace or shutdown. `path` is copied. */
void Editor_VFS_Register   (const char *path, void *bytes, int size);

/* Drop a registered path; frees its bytes. No-op if absent. */
void Editor_VFS_Unregister (const char *path);

/* Lookup. Returns 1 on hit + fills *out_bytes / *out_size; 0 on miss. */
int  Editor_VFS_Find       (const char *path, void **out_bytes, int *out_size);

/* Free everything. Called from Editor_Shutdown. */
void Editor_VFS_Shutdown   (void);

#ifdef __cplusplus
}
#endif

#endif /* VIRTUAL_FS_H */

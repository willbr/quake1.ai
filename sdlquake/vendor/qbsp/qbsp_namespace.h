/*
 * qbsp_namespace.h -- forced-include header (-include flag) prepended to
 * every qbsp .c file by build.zig. Renames qbsp's exported symbols to
 * qbsp_-prefixed versions so they don't collide at link time with the
 * engine's identically-named functions in mathlib.c / common.c.
 *
 * Macro-based renaming applies to declarations, definitions, and call
 * sites uniformly within the qbsp translation units only — engine TUs
 * don't see this header, so they continue to refer to the unprefixed
 * names that resolve to engine implementations.
 *
 * Add new entries here when a future link-time collision shows up.
 */

#ifndef QBSP_NAMESPACE_H
#define QBSP_NAMESPACE_H

/* mathlib */
#define vec3_origin        qbsp_vec3_origin
#define VectorCompare      qbsp_VectorCompare
#define VectorMA           qbsp_VectorMA
#define CrossProduct       qbsp_CrossProduct
#define VectorNormalize    qbsp_VectorNormalize
#define VectorInverse      qbsp_VectorInverse
#define VectorScale        qbsp_VectorScale
#define _DotProduct        qbsp__DotProduct
#define _VectorSubtract    qbsp__VectorSubtract
#define _VectorAdd         qbsp__VectorAdd
#define _VectorCopy        qbsp__VectorCopy

/* cmdlib */
#define Q_strncasecmp      qbsp_Q_strncasecmp
#define Q_strcasecmp       qbsp_Q_strcasecmp
#define COM_Parse          qbsp_COM_Parse
#define com_token          qbsp_com_token
#define com_eof            qbsp_com_eof
#define CRC_Init           qbsp_CRC_Init
#define CRC_ProcessByte    qbsp_CRC_ProcessByte
#define CRC_Value          qbsp_CRC_Value

/* Byte-swap functions. Engine common.c declares these as function
 * POINTERS that get assigned at COM_Init. qbsp's cmdlib.c defines
 * them as concrete functions. With -fcommon, the function definition
 * silently wins, and the engine's `BigShort = ShortSwap` write lands
 * in .text -> ACCESS_VIOLATION on startup. Rename qbsp's so the
 * symbols stay distinct. */
#define BigShort           qbsp_BigShort
#define LittleShort        qbsp_LittleShort
#define BigLong            qbsp_BigLong
#define LittleLong         qbsp_LittleLong
#define BigFloat           qbsp_BigFloat
#define LittleFloat        qbsp_LittleFloat

/* qbsp.c's old main() is renamed and never called — the new entry point
 * is qbsp_compile_to_memory in qbsp_lib.c. The rename also keeps it from
 * colliding with sys_sdl.c's main(). */
#define main               qbsp_main_unused

/* printf / fprintf redirects live in cmdlib.h, after <stdio.h> has
 * declared them — defining them here breaks Windows stdio.h's
 * __format__ attribute on printf. */

/* Note on qbsp's generic globals (numfaces, num_entities, numbrushes,
 * numtexinfo, nummiptex, entities, etc.): these are NOT macro-renamed
 * here and intentionally so.
 *
 * (1) The engine's bspfile.h declares the externs inside `#ifndef
 *     QUAKE_GAME`, and quakedef.h defines QUAKE_GAME. So engine TUs
 *     don't see them, and an engine TU that ever wrote `numfaces++`
 *     would fail to compile (undeclared identifier) before it could
 *     ever link to qbsp's definition.
 * (2) qbsp itself uses several of these names as struct members
 *     (`node->numfaces`, `mtl->nummiptex`, `d->numfaces`). A macro
 *     `#define numfaces qbsp_numfaces` would silently rewrite member
 *     access too, turning member loads into invalid references. Each
 *     candidate has to be vetted against `grep '->NAME\b' vendor/qbsp`
 *     before renaming.
 * (3) Light's identically-named globals are kept distinct via
 *     light_namespace.h's `#define entities light_entities` etc., so
 *     the two tools don't collide at link time today.
 *
 * If you want to rename one of these for defense-in-depth, do it at the
 * definition site (rename `int numfaces` to `int qbsp_numfaces` in
 * qbsp/bspfile.c) and fix the bare-global callers, rather than via a
 * macro here. */

#endif /* QBSP_NAMESPACE_H */

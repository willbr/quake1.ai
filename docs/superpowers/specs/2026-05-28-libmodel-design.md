# libmodel + libqalloc design

**Date:** 2026-05-28
**Status:** Approved (design); implementation plan pending

## Goal

Vendor Quake's MDL (alias model) parsing out of the engine's monolithic
`sdlquake/engine_src/model.c` (1983 lines, mixing brush + sprite + alias
loading) into a standalone library, `libmodel`, so that MDL parsing lives
in exactly one place and can be shared by the engine and by tools.

Alongside it, introduce `libqalloc`, a small shared allocator toolkit, so
`libmodel` (and future shared C libraries and tools) can allocate without
hardcoding `malloc` or depending on the engine's Hunk/Cache.

Parser only for now. The in-memory representation is designed so a writer
(serializer) can be added later without breaking the API. No writer is
built now (YAGNI).

## Background / constraints discovered

- The alias loader is `Mod_LoadAliasModel` + helpers in `model.c`
  (~lines 1307–1756). It depends on engine internals: `Hunk_AllocName` /
  `Hunk_LowMark` / `Hunk_FreeToLowMark` / `Cache_Alloc` (memory),
  `LittleLong` / `LittleFloat` (byteswap), `Sys_Error`, the
  `loadmodel` / `loadname` globals, and the render globals `r_pixbytes`
  and `d_8to16table`.
- Alias models are built in Hunk scratch, then `memcpy`'d wholesale into a
  relocatable `Cache_Alloc` block (`mod->cache.data`, `model.c:1747-1753`),
  and the Hunk scratch is freed. The renderer (`r_alias.c`, `d_polyse.c`,
  `r_aclip.c`) reads everything as `(byte *)paliashdr + offset`.
- **The offset-based `aliashdr_t` layout exists because the cache block is
  relocatable** — `Cache_Check` can evict and move it, and offsets relative
  to `paliashdr` survive that; raw pointers would not. This is an engine
  concern, not a format concern.
- **Consequence:** a shared allocator does *not* remove the need to
  translate libmodel's neutral (pointer-linked) representation into the
  engine's offset layout. The allocator question and the representation
  question are orthogonal — both are addressed, separately.
- The `r_pixbytes == 2` (16-bit skin) branch in the current loader is dead
  code: `r_pixbytes` is hardcoded to `1` everywhere since the SDL_GPU
  palette-LUT shader. libmodel does not carry it.
- The current loader trusts the file blindly (no bounds checks). Fine for
  shipped paks, not fine for tools opening arbitrary files. libmodel adds
  bounds-checked parsing as a deliberate robustness improvement.
- No existing tool reads or writes MDL today. `tools/extract_phase6`
  (Zig) writes `.spr`/`.wav`; `tools/mapcompile/mapcompile.c` (C) does BSP.
  The "shared by tools" goal is forward-looking — the libraries *enable*
  tool use; no tool consumer is built in this work.

## Scope

**In scope:** MDL (alias) parsing only. The two new libraries, and the
engine adapter that replaces inline alias parsing.

**Out of scope (stays in `model.c`, untouched):** sprite loading
(`Mod_LoadSprite*`), BSP/brush loading. **Untouched:** the alias renderer
(`r_alias.c`, `d_polyse.c`, `r_aclip.c`) and the internal render structs
in `model.h` / `modelgen.h`. **Deferred:** the MDL writer (representation
supports it; not built now).

---

## libqalloc — allocator toolkit

Location: `sdlquake/libqalloc/qalloc.h` + `qalloc.c`. Portable C, no engine
deps. Compiled as its own static-lib module.

### Interface

```c
typedef struct qalloc_s {
    void *(*alloc)  (void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)   (void *ctx, void *ptr);
    void  *ctx;
} qalloc_t;
```

- `realloc` carries `old_size` so a bump allocator can copy correctly.
- All allocations guarantee `max_align_t` alignment (like `malloc`).
- Convenience macros: `QALLOC(a, T)`, `QALLOC_ARR(a, T, n)`, `QFREE(a, p)`.

### Implementations

1. **malloc-backed** — `qalloc_t qalloc_malloc(void);` libc `malloc` /
   `realloc` / `free`, `ctx = NULL`. The portable default.
2. **arena / bump** — `qalloc_arena_t` over a caller-provided buffer:
   - `qalloc_t qalloc_arena_init(qalloc_arena_t *a, void *buf, size_t size);`
   - `void qalloc_arena_reset(qalloc_arena_t *a);`
   - `free` is a no-op; `realloc` bumps + memcpy `old_size` bytes. For
     parse-then-discard workloads.
3. **Hunk adapter — ships separately, engine-side**
   (`sdlquake/engine/qalloc_hunk.c` + small header), because it depends on
   `Hunk_AllocName`. Keeping it out of the portable core means tools can
   link `libqalloc` + `libmodel` without dragging in the engine. Provides a
   `qalloc_t` whose `alloc` calls `Hunk_AllocName` and whose `free` is a
   no-op (Hunk is released en masse via `Hunk_FreeToLowMark`).

---

## libmodel — MDL parser

Location: `sdlquake/libmodel/libmodel.h` + `mdl.c`, plus a private
`mdl_format.h` holding the on-disk MDL struct definitions (its own copy,
decoupled from the engine's `modelgen.h`). Depends only on `libqalloc`.
Zero engine dependencies — its own little-endian readers, its own error
reporting, its own memory via the injected `qalloc_t`.

### Neutral representation

Faithful and lossless to the format (so a future writer can round-trip).
Vertices are kept as raw packed bytes + the model's `scale`/`scale_origin`
(decompression is the renderer's job), matching how the engine treats them.

```c
typedef struct { unsigned char v[3]; unsigned char lightnormalindex; } lm_trivertx_t;

typedef struct {
    char          name[16];
    lm_trivertx_t bboxmin, bboxmax;
    lm_trivertx_t *verts;          /* [numverts] */
} lm_pose_t;

typedef struct {
    int        numposes;           /* 1 for a single frame */
    float     *intervals;          /* [numposes] (length 1 for single) */
    lm_pose_t *poses;              /* [numposes] */
} lm_frame_t;

typedef struct {
    int            numpics;        /* 1 for a single skin */
    float         *intervals;      /* [numpics] */
    unsigned char **pics;          /* [numpics], each skinwidth*skinheight indices */
} lm_skin_t;

typedef struct { int onseam, s, t; }            lm_stvert_t;
typedef struct { int facesfront; int vertindex[3]; } lm_triangle_t;

typedef struct {
    /* header scalars */
    float scale[3], scale_origin[3], eyeposition[3];
    float boundingradius, size;
    int   skinwidth, skinheight;
    int   numskins, numverts, numtris, numframes;
    int   synctype, flags;
    /* data */
    lm_skin_t     *skins;          /* [numskins] */
    lm_stvert_t   *stverts;        /* [numverts] */
    lm_triangle_t *triangles;      /* [numtris] */
    lm_frame_t    *frames;         /* [numframes] */
    /* bookkeeping */
    qalloc_t       alloc;          /* remembered, for lm_model_free */
} lm_model_t;
```

Note: `eyeposition` and `boundingradius` are captured even though the
current engine never consumes them — the library is faithful to the format,
and consumers decide what to use.

### API

```c
typedef enum {
    LM_OK = 0,
    LM_ERR_TRUNCATED,    /* a read would run past the buffer */
    LM_ERR_BAD_MAGIC,    /* not "IDPO" */
    LM_ERR_BAD_VERSION,  /* version != 6 */
    LM_ERR_BAD_COUNT,    /* nonsensical numverts/numtris/numframes/numskins/skin dims */
    LM_ERR_OOM           /* allocator returned NULL */
} lm_result_t;

lm_result_t lm_load_mdl(const void *buf, size_t len,
                        const qalloc_t *a, lm_model_t **out);
void        lm_model_free(lm_model_t *m);
const char *lm_strerror(lm_result_t r);
```

- `a == NULL` → default malloc allocator.
- Parsing is **cursor-based and bounds-checked**: every field read and every
  array span is validated against `len` before access; out-of-range counts
  and truncation return an `lm_result_t` error rather than crashing.
- Little-endian readers built in (no dependency on engine `LittleLong`).
- `lm_model_free` releases via the remembered allocator (a no-op for arena
  backings).

---

## Engine integration

`Mod_LoadAliasModel` (`model.c:1517`) becomes a thin adapter. The renderer
and internal structs are untouched; the offset-based `aliashdr_t` is rebuilt
byte-for-byte. Flow:

1. Build a Hunk-backed `qalloc_t` (`qalloc_hunk.c`) over a `Hunk_LowMark`
   scratch region.
2. `lm_load_mdl(buffer, com_filesize, &scratch, &lm)` — `com_filesize` (set
   by `COM_LoadStackFile`) gives a real length for bounds checking. On
   error, `Sys_Error` with `lm_strerror(r)`.
3. Translate `lm` into the Hunk-allocated offset block exactly as the code
   does today — skindesc offsets, single/group frame + skin descriptors,
   stverts, triangles, posedata, and `mod->mins`/`maxs` — reading from `lm`
   instead of parsing inline.
4. `Cache_Alloc` + `memcpy` the finished block (unchanged), then
   `Hunk_FreeToLowMark` releases both the scratch arena and the temp block.

Net: all parsing logic leaves the engine; the engine retains only the
format → relocatable-cache-layout translation, which is the genuinely
engine-specific part.

The `lm_trivertx_t` ↔ engine `trivertx_t` copy is a trivial field-for-field
copy (identical layout). The engine continues to define its own
`trivertx_t` (used pervasively by the renderer); libmodel's is independent
by design.

---

## Build

`build.zig` gains two static-lib modules:

- `libqalloc` — portable core (`qalloc.c`), modern-C flags.
- `libmodel` — `mdl.c`, depends on `libqalloc`, modern-C flags.

The engine compile links both and adds `sdlquake/engine/qalloc_hunk.c` to
the engine source list. No tool is wired to the libraries in this work.

---

## Testing & verification

No test suite exists in the repo today. Because these libraries are pure,
add a minimal one:

1. **libmodel unit/smoke test** — `zig build test-libmodel`: a small C
   harness that parses several real id1 `.mdl` files (e.g.
   `id1/progs/player.mdl`, `id1/progs/armor.mdl`) and asserts header counts,
   frame counts, and skin counts; and that a deliberately truncated buffer
   yields `LM_ERR_TRUNCATED` (not a crash).
2. **Translation regression guard** — dump the final `aliashdr_t` cache
   block bytes for several models from the current (pre-refactor) binary,
   then from the refactored binary, and diff. They must be byte-identical —
   proof the renderer sees exactly the same data.
3. **In-game check** — `zig build run -- +map e1m1`; confirm the player
   viewmodel, monsters, and ammo/pickup models render correctly.

## Open questions

None. Writer, additional tool consumers, and promoting more parsing
(sprites/BSP) into libraries are explicitly deferred.

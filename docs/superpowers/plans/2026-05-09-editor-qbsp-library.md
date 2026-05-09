# Editor — In-Process qbsp Library (Phase 7 follow-up)

## Context

Phase 7's M3 ("Wrap brushes into func_*") authors brush entities correctly into the `.map`, but the engine cannot simulate them without a `.bsp` containing their submodel geometry. `func_door` runs `SV_SetModel(e, e->v.model)` against a string `*N` that's only meaningful when N exists in `cl.worldmodel->submodels[]` — produced by qbsp at compile time. With no compile pipeline, wrapped doors are inert.

The user explicitly does not want a separate compile binary or external tool invocation. Path chosen: vendor id Software's original qbsp source from `ref/Quake-Tools-master/qutils/QBSP/` and refactor it into a library callable in-process from the engine. This unlocks "wrap brushes → press button → walk through the door" without leaving the running game.

Side benefit: gives us a path to an actual `.bsp` output — same library, same call site — if a "publish playable .map → .bsp" workflow is ever needed.

## Goal

A function callable from the engine that compiles a `.map` to a `.bsp` **entirely in memory** — no disk hop on the `.bsp` side:

```c
int qbsp_compile_to_memory(const char *map_path,
                           void **out_bsp, int *out_size,
                           const qbsp_options_t *opts);
```

- Reads the `.map` from disk (`Scene_Save` already writes it; we want it on disk anyway as the saved source of truth + crash-recovery point).
- Returns the `.bsp` as a `malloc`'d byte buffer; caller frees.
- Returns `0` on success, non-zero + console message on failure.
- Does not call `exit()` on errors (longjmp recovery).
- Works on Windows (no `fork()`).
- Logs through `Con_Printf` instead of `printf`.

Plus engine-side machinery so the in-memory `.bsp` can be loaded by the existing `map` command without a disk round-trip:

- A virtual-file registry: `Editor_RegisterVirtualFile(path, bytes, size)` / `Editor_UnregisterVirtualFile(path)`.
- A patch in `COM_FOpenFile` (`engine_src/common.c`) so when the engine asks for `maps/test.bsp` and a virtual file by that name exists, it serves bytes from RAM instead of disk/PAK.

Plus a console command `editor_compile` that ties it together:
```c
Scene_Save(map_path);                                  // disk = source of truth
qbsp_compile_to_memory(map_path, &bytes, &size, NULL); // .bsp in RAM
Editor_RegisterVirtualFile("maps/test.bsp", bytes, size);
Cbuf_AddText("map test\n");                            // engine loads from registry
```

The whole `SV_SpawnServer` → entity-parse → client-reset chain runs unchanged. The only patch the engine pays for the in-memory path is the file-fetch hook.

## Non-goals (v1)

- Multi-call support in a single process. Globals stay un-reset; a second call may crash. Documented; first commit ships single-shot.
- Lighting (`light.exe`) and PVS (`vis.exe`) integration. qbsp alone produces a renderable, walkable `.bsp`; lighting is fullbright and PVS is empty (everything visible everywhere). Acceptable for editor playtest. Light/vis are separable follow-ups.
- "Compile only the brush entities" optimization. Always recompiles the whole map. For the user's authoring-scale `test.map` this is sub-second.
- ImGui "Compile + Reload" toolbar button. Console command first; UI is a one-line wrapper once the command works (M2).

## Constraints

- Source must compile in our existing `build.zig` setup. Plan: a separate `addCSourceFiles` block with its own flag list, separate from `engine_c_flags`.
- The vendored source under `sdlquake/vendor/qbsp/` is not modified except where strictly necessary (function signature changes, `Error` → longjmp, `fork` → sequential, `printf` → `Con_Printf`). Keeps merging upstream fixes feasible.
- No new runtime dependencies beyond what we already link.

## Source survey (already completed)

- `ref/Quake-Tools-master/qutils/QBSP/` — 19 files, ~8.3k LOC. Original id qbsp.
- `ref/Quake-Tools-master/qutils/COMMON/` — 18 files, ~3-4k LOC. Support: `bspfile`, `cmdlib`, `mathlib`, `polylib`, `scriplib`, `wadlib`. Some support files we may not need (`lbmlib`, `trilib`).
- Single `main()` in `QBSP.C:950`. ~80 lines of arg-parsing → calls `ProcessFile()`.
- `Error()` lives in `cmdlib.c:35` — printf + exit(1). 50+ call sites.
- `fork()` only in `QBSP.C:836-848` (`CreateHulls()`), forks 2 child processes for hulls 1 and 2; parent does hull 0. Uses pipe-style file handoff (`-usehulls` mode reads them back).
- All globals declared at top of `QBSP.C` (~28 of them).

## Approach

Vendor the source. Don't rewrite. Refactor the entry point, the abort path, the parallelism, and the I/O surface — leave the core algorithms (CSG, BSP-tree, portal-flood, t-junc, surface merge) untouched.

The five edits:

### 1. Function signature

Replace `int main(int argc, char **argv)` with:
```c
typedef struct {
    int notjunc;       // -notjunc
    int nofill;        // -nofill
    int noclip;        // -noclip
    int onlyents;      // -onlyents
    int verbose;       // -verbose
    int subdivide;     // -subdivide N (0 = default 240)
} qbsp_options_t;

// Returns 0 on success, non-zero on failure.
// On failure, an error message has already been printed via Con_Printf.
int qbsp_compile(const char *map_path, const char *bsp_path,
                 const qbsp_options_t *opts);
```

`opts == NULL` → defaults. The five `-flag` booleans become struct fields. The body inlines what `main()` did after argv-parsing.

### 2. Error handling

Replace `Error(fmt, ...)` body in `cmdlib.c:35` with:
```c
extern jmp_buf *qbsp_err_jmp;     // set by qbsp_compile
extern char     qbsp_err_msg[1024];

void Error(char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(qbsp_err_msg, sizeof(qbsp_err_msg), fmt, ap);
    va_end(ap);
    if (qbsp_err_jmp) longjmp(*qbsp_err_jmp, 1);
    fprintf(stderr, "qbsp: %s\n", qbsp_err_msg); exit(1);
}
```

`qbsp_compile` opens with `setjmp`; non-zero return prints `qbsp_err_msg` via `Con_Printf` and returns. No call site changes — `Error("foo %s", bar)` everywhere keeps working.

### 3. fork() removal

Replace `CreateHulls()` body in `QBSP.C:830-848`. The current parent path computes hull 0; the children compute 1 and 2. Replace with a sequential loop:
```c
for (int h = 0; h < 3; h++) {
    hullnum = h;
    PrintMemory();
    LoadMapFile (sourcebase);
    ProcessFile_Hull();   // factor out from ProcessFile
    /* writes hull[h] data into the bspfile structures */
}
```

Looking at the current code, hull data is written to a per-hull text file by each child process and then re-read by the parent's `ReadClipHull()` call. We bypass the file round-trip — the in-process version computes the hull and stores it directly. Need to read `WriteClipHull()` to understand what it serializes; that becomes our in-memory data.

Realistic: this is the highest-risk single edit. ~half a day to read the existing forked code and structure the sequential equivalent. The `-usehulls` path (already in the code) is a partial template — it skips the fork by reading existing hull files. We adapt that to "compute then store in memory."

### 4. printf redirection

Two macros at the top of `bsp5.h`:
```c
#define printf(...)  Con_Printf(__VA_ARGS__)
#define fprintf(s,...) Con_Printf(__VA_ARGS__)   /* drop stream arg */
```

Crude but works for our purposes — qbsp uses `printf` for progress/info and `fprintf(stderr, ...)` for errors-pre-exit. Both should land in the in-game console. Need to verify no `printf("%c", c)` style calls that the macro mangles.

Alternative: search-and-replace `printf` → `Qbsp_Printf` in vendored source. ~50 sites; mechanical. Safer than the macro.

### 5. I/O surface

`LoadMapFile` reads from disk via `LoadFile()` in `cmdlib.c` — keep as-is. The user's `Scene_Save` flow writes the `.map` first; qbsp then reads what's on disk.

`WriteBSPFile` is the change. Currently it `fopen`s the `.bsp` path and `SafeWrite`s the header + 16 lumps via a `FILE *`. Refactor:

```c
// in bspfile.c — replace SafeWrite(file, ptr, size) with writes to a
// growing byte buffer.
typedef struct {
    byte *bytes;
    int   size;
    int   cap;
} bsp_outbuf_t;

static bsp_outbuf_t g_outbuf;
static void OutbufWrite(const void *ptr, int len) {
    if (g_outbuf.size + len > g_outbuf.cap) {
        int nc = g_outbuf.cap * 2; if (nc < g_outbuf.size + len) nc = g_outbuf.size + len;
        g_outbuf.bytes = realloc(g_outbuf.bytes, nc);
        g_outbuf.cap = nc;
    }
    memcpy(g_outbuf.bytes + g_outbuf.size, ptr, len);
    g_outbuf.size += len;
}

void WriteBSPFile_Memory(byte **out_bytes, int *out_size) {
    g_outbuf.bytes = malloc(1<<16); g_outbuf.cap = 1<<16; g_outbuf.size = 0;
    /* ...all the previous SafeWrite calls now go through OutbufWrite ... */
    *out_bytes = g_outbuf.bytes;
    *out_size  = g_outbuf.size;
    g_outbuf = (bsp_outbuf_t){0};
}
```

The seek-based "go back and patch the header lump offsets" pattern that `WriteBSPFile` uses translates trivially: track current offset = `g_outbuf.size`, patch the header by writing at a stored offset.

### 6. Engine virtual-file hook

New file `sdlquake/engine/virtual_fs.{c,h}`:

```c
// virtual_fs.h
void  Editor_RegisterVirtualFile  (const char *path, void *bytes, int size);
void  Editor_UnregisterVirtualFile(const char *path);
// returns 1 if the path was registered + fills *out_bytes and *out_size;
// 0 if not registered (caller falls through to disk).
int   Editor_FindVirtualFile      (const char *path, void **out_bytes, int *out_size);
```

`bytes` ownership transfers to the registry; `Editor_UnregisterVirtualFile` `free`s. Only one entry per path; re-registering replaces (after freeing the old). Internal storage: tiny linear array of `{path, bytes, size}` structs; we'll have ≤ 1-2 entries at a time so a hash table is overkill.

**Hook point: `COM_LoadFile` in `engine_src/common.c:1533`** — confirmed via grep (`Mod_LoadBrushModel` at `model.c:282` calls `COM_LoadStackFile`, which is just `COM_LoadFile(path, 4)`). All other model load paths (alias models, sprites, sounds) also funnel through this same function. One patch covers everything.

Patch shape:
```c
byte *COM_LoadFile (char *path, int usehunk)
{
    int     h, len;
    byte   *buf = NULL;
    char    base[32];
    void   *vbytes;
    int     vsize;

    /* Editor virtual-file hook — when qbsp produces an in-memory .bsp,
       the engine's `map foo` -> Mod_ForName -> COM_LoadStackFile chain
       finds it here without ever hitting disk. */
    if (Editor_FindVirtualFile(path, &vbytes, &vsize))
    {
        len = vsize;
        COM_FileBase (path, base);
        /* allocate via the same usehunk dispatch as the disk path */
        if      (usehunk == 1) buf = Hunk_AllocName(len+1, base);
        else if (usehunk == 2) buf = Hunk_TempAlloc(len+1);
        else if (usehunk == 0) buf = Z_Malloc(len+1);
        else if (usehunk == 3) buf = Cache_Alloc(loadcache, len+1, base);
        else if (usehunk == 4) {
            if (len+1 > loadsize) buf = Hunk_TempAlloc(len+1);
            else                  buf = loadbuf;
        } else Sys_Error("COM_LoadFile: bad usehunk");
        if (!buf) Sys_Error("COM_LoadFile: not enough space for %s", path);
        memcpy(buf, vbytes, len);
        buf[len] = 0;
        return buf;
    }

    /* original disk/PAK path follows unchanged */
    len = COM_OpenFile (path, &h);
    if (h == -1) return NULL;
    /* ... */
}
```

~30 lines added. No engine semantic change — virtual files behave exactly like real ones from the load chain's perspective.

`COM_FOpenFile` itself doesn't need patching — the only callers of it that matter for our use case are `COM_LoadFile`'s internal `COM_OpenFile`, which is below the hook and only runs when the virtual lookup misses.

## Milestones

### M1 — Vendor + first compile (3 days)

**Files added:**
- `sdlquake/vendor/qbsp/` — copy of `ref/Quake-Tools-master/qutils/QBSP/*.{c,h}` (renamed lowercase) and the subset of `COMMON/*.{c,h}` actually used: `bspfile`, `cmdlib`, `mathlib`, `polylib`, `scriplib`, `wadlib`. Drop `lbmlib`, `trilib`, `threads` (we replace threading).
- `sdlquake/vendor/qbsp/qbsp_lib.h` — public API: the `qbsp_options_t` struct and `qbsp_compile_to_memory` declaration.
- `sdlquake/vendor/qbsp/qbsp_lib.c` — the new entry point (replaces `main`).
- `sdlquake/engine/virtual_fs.{c,h}` — virtual-file registry.

**Files modified (in vendored copies):**
- `qbsp.c` — strip `main`, drop globals into `qbsp_compile_to_memory`'s scope or zero on entry.
- `cmdlib.c` — `Error()` redirects to longjmp.
- `qbsp.c` (CreateHulls) — sequential hull pass, no fork.
- `bspfile.c` — `WriteBSPFile` writes to a `bsp_outbuf_t` instead of a `FILE *`.
- `bsp5.h` (or via search-and-replace) — printf → Con_Printf.

**Files modified (engine side):**
- `build.zig` — new `addCSourceFiles` block for `sdlquake/vendor/qbsp/*.c` with appropriate flags. Set include path so engine can `#include "qbsp_lib.h"`. Add `sdlquake/engine/virtual_fs.c` to the platform sources list.
- `sdlquake/engine_src/common.c` — patch `COM_LoadFile` to check the virtual-file registry before disk/PAK.
- `sdlquake/engine/editor/editor.c` — register `editor_compile` console command:
  ```c
  static void Editor_Cmd_Compile_f(void) {
      char map_path[256], bsp_path[64];
      void *bytes = NULL; int size = 0;
      if (!edit_scene.mapname[0]) { Con_Printf("editor: no map loaded\n"); return; }
      snprintf(map_path, sizeof(map_path), "%s/maps/%s.map",
               com_gamedir, edit_scene.mapname);
      snprintf(bsp_path, sizeof(bsp_path), "maps/%s.bsp", edit_scene.mapname);
      if (!Scene_Save(map_path)) { Con_Printf("editor: save failed\n"); return; }
      if (qbsp_compile_to_memory(map_path, &bytes, &size, NULL) != 0) {
          Con_Printf("editor: qbsp failed\n"); return;
      }
      Editor_RegisterVirtualFile(bsp_path, bytes, size);
      Con_Printf("editor: compiled %s (%d bytes); reloading\n", bsp_path, size);
      char buf[160];
      snprintf(buf, sizeof(buf), "map %s\n", edit_scene.mapname);
      Cbuf_AddText(buf);
  }
  ```

**Verification gate (M1 ships when):**
- `zig build run -- +map test` opens with the existing test.map's .bsp.
- In-game console: `editor` to open editor; `editor_compile` produces a `.bsp` in RAM, registers it as `maps/test.bsp` in the virtual registry, and `Cbuf_AddText("map test\n")` triggers the reload.
- The reloaded engine renders the recompiled geometry — and `id1/maps/test.bsp` on disk is unchanged from before the compile (proof the .bsp never hit disk).
- Wrap a couple of brushes into `func_door` (Phase 7 M3), `editor_compile`, walk into the door — door opens.

**Risks / known gotchas:**
- qbsp's `cmdlib.c:LoadFile` uses `malloc` and never frees → per-compile leak (~few MB for a small map). Documented; M1 does not fix.
- Globals are not reset between calls. M1 supports one `editor_compile` per process. A second call may crash. Workaround: `quit` and relaunch. Fixed in M2.
- The virtual `maps/test.bsp` shadows any disk `id1/maps/test.bsp`. If the user ever wants the disk file again (e.g. to ship), they'll need a `editor_unregister_virtual` command or just relaunch the engine. Document.
- Virtual file lifetime: registered on `editor_compile`, replaced on next `editor_compile` of the same path (old buffer freed), unregistered on `Editor_Shutdown`. Engine's hunk allocations of the bytes are independent — they live as long as any other model load.
- `-onlyents` mode (qbsp re-emits just the entity lump) is interesting for fast brush-entity-only iteration. Not wired up in M1 but the codepath is preserved in the vendored source.
- The vendored qbsp emits a `.bsp` of version 29 (Quake 1 standard). Engine loads this fine — same path `ref/Quake-master` already uses.

### M2 — Multi-call hardening (2 days)

**Goal:** `editor_compile` works repeatedly in one session.

**Approach:**
- Audit qbsp's globals (~28 in qbsp.c, plus structs in csg4/solidbsp/portals/etc). Add a `qbsp_reset_state(void)` called at the top of `qbsp_compile`. memset arrays of structs to zero, reset counters, free lingering allocations from the previous call.
- Wrap qbsp's allocator in a single linear arena: `qbsp_alloc(size)` bumps a pointer; `qbsp_reset_state` resets the pointer to base. ~50 call sites (use ctags/grep on `malloc(`); 1 day to refactor.
- Verify by running `editor_compile` 5+ times in one session with no leak / crash.

**Optional M2.1 (deferred — only if needed):** add an ImGui "Compile + Reload" toolbar button that calls `editor_compile`. ~1 hour.

**Verification gate (M2 ships when):**
- Repeated `editor_compile` calls in one session work without crash.
- VRAM / process RSS stable across 10+ recompiles.

### M3 — Optional polish (deferred)

- "Compile + Reload" toolbar button.
- Light pass (vendor `light.c` from qutils/LIGHT/) so doors aren't fullbright.
- vis pass (vendor `vis.c` from qutils/VIS/) so PVS is right.
- Brush-entity-only recompile (use `-onlyents` plus a manual submodel patch — out of scope here).

Plan no further milestones until M1+M2 are shipped and the user has authored at least one map's worth of doors.

## Open risks

- **Hull computation correctness without fork.** The original code's parent-vs-child split shares state via files. Sequentializing it correctly is the highest-risk single edit. If the resulting hulls produce wrong collision (e.g. doors that visually open but don't let the player through), debugging requires comparing hull output against an external qbsp run on the same .map.

- **Globals reset across calls.** M2's audit may turn up subtle state (e.g. csg4.c's `entity_t entities[MAX_MAP_ENTITIES]` plus a side table that's hard to find). Risk = "second compile silently produces wrong output" rather than "crash" — easier to ship M1 alone and have the user try repeatedly to flush these out.

- **Memory growth.** Even with M2's arena, qbsp may hold references that span calls. If the leak is bounded per call (a few MB) and the user only compiles a few times per session, it's tolerable. Worst case: the engine eventually OOMs. Add a soft warning at compile #20 to remind the user to restart.

- **Build-time bloat.** Adding 11k LOC of C to the engine compile. Probably +2-3 seconds on `zig build`. Acceptable.

- **License.** id Software's qbsp source is GPL. `quake1.ai` is presumably already GPL or compatible (since `sdlquake/engine_src/` is the WinQuake fork). If license is an issue, ericw-tools qbsp (also GPL) is the alternative. Not addressed in M1 — defer to user.

## Verification

End-to-end test (after M1):
1. `zig build run -- +map test`
2. F2 to open editor
3. `editor_brush_add_cube` (or use the toolbar)
4. Add a few more brushes for a small room
5. Select 1 brush, `Wrap...` → `func_door`
6. Inspector: set `angle` to a sensible value (e.g. -1 for "up")
7. `editor_compile` in console
8. Engine reloads on the new .bsp
9. Walk into the door — it opens, closes, you walk through
10. Restart from .map: `+map test` from a fresh launch — door behavior persists (geometry round-tripped through qbsp)

## Critical files

- New: `sdlquake/vendor/qbsp/*.{c,h}` (vendored from `ref/Quake-Tools-master/qutils/QBSP/` + selected `COMMON/`)
- New: `sdlquake/vendor/qbsp/qbsp_lib.{c,h}` (entry point + options)
- New: `sdlquake/engine/virtual_fs.{c,h}` (in-memory file registry)
- Modified: `sdlquake/engine_src/common.c` — `COM_LoadFile` virtual-file hook (~30 lines)
- Modified: `sdlquake/engine/editor/editor.c` — `editor_compile` command
- Modified: `build.zig` — new source group + include path + virtual_fs.c added to platform sources

# Editor — In-Process vis Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the in-editor compile trio (qbsp → vis → light) by vendoring id Software's VIS source into `sdlquake/vendor/vis/` and exposing it as an in-process library that fills the PVS lump on the BSP currently sitting in qbsp's shared globals.

**Architecture:** Mirror the existing qbsp/light vendoring pattern exactly. A new `vis_namespace.h` (forced-include via `build.zig`) prefixes all of VIS's exported names with `vis_` so they don't collide with qbsp's same-named types and globals (`winding_t`, `plane_t`, `portal_t`, `leaf_t`, `numportals`, `portals`, …). A new `vis_lib.{c,h}` provides the entry point and reuses qbsp's longjmp/Error machinery and tracked allocator. The pipeline becomes **qbsp → vis → light**: qbsp leaves geometry in globals and writes a `.prt` alongside the (virtual) `.bsp`; vis reads the `.prt` and fills `dvisdata`; light bakes lighting and re-serialises the BSP (with both PVS and lightmaps present) into the membuf for VFS registration.

**Tech Stack:** C (gnu89 + `-fcommon` + `-fno-sanitize=undefined`, matches qbsp/light flags), Zig 0.16 build system, SDL3 platform, vendored id-Software VIS source (~1,709 LOC).

---

## Context

Phase 7's in-editor compile pipeline currently runs `qbsp_compile_to_memory` and `light_compile_to_memory` in sequence (`editor_compile_full`), producing a renderable, lit BSP without ever touching disk for the `.bsp`. What's missing is the third member of id's compile trio: **vis**, which computes the Potentially Visible Set (PVS) into the BSP's `visdata` lump. Without it the engine treats every leaf as visible from every other leaf — fine for a small editor scene, but unrepresentative of how a published map performs and incorrect for ambient-sound culling (which derives from PHS, the secondary lump VIS also produces via `CalcAmbientSounds`).

VIS is also the canonical slow tool in the trio. A full-quality bake on a real map can take minutes; on the user's typical authoring-scale `test.map` it should still be sub-second. We split the editor surface area so live iteration uses **no vis** (the default `editor_compile_full` is unchanged), while a new **`editor_compile_export`** command runs the full qbsp → vis → light pipeline for a "shippable" BSP. A **`-fast`** option exposes VIS's flood-fill mode for the middle-ground case.

The vendoring template is established. `sdlquake/vendor/qbsp/` and `sdlquake/vendor/light/` are both in tree, both wrapped with `_namespace.h` forced-include headers, both share qbsp's `bspfile.c`/`cmdlib.c`/`mathlib.c` translation units at link time, and both route errors through a single `setjmp`/`Error()` longjmp pair owned by `qbsp_lib.c`. This plan reuses every one of those mechanisms.

## Goal

A pair of public entry points, declared in `sdlquake/engine/vis_lib.h`:

```c
typedef struct {
    int fast;          /* -fast: flood-fill PVS, ~instant, not real visibility */
    int level;         /* test depth, default 2, max 4 */
    int verbose;
    int skip_sound_pvs;  /* skip CalcAmbientSounds */
} vis_options_t;

/* Fill dvisdata on the BSP currently in qbsp's globals from a .prt on disk.
 * Caller must have just run qbsp_compile_to_memory (which leaves dfaces /
 * dplanes / dleafs / ... populated and writes the .prt next to the .bsp's
 * stem).
 * Returns 0 on success, non-zero on failure (error printed via Con_Printf). */
int vis_compile_in_place(const char *prt_path, const vis_options_t *opts);

/* Stand-alone diagnostic: loads bsp_path + prt_path from disk and runs the
 * vis pass once, timing each phase, discarding output. Wired to a console
 * command. Returns 0 on success. */
int vis_bench(const char *bsp_path, const char *prt_path);
```

`vis_compile_in_place` does **not** return a BSP buffer — it only mutates qbsp's `dvisdata` global. The caller's pipeline is:

1. `qbsp_compile_to_memory(map_path, &bsp_unlit, &unlit_size, &opts)` — geometry into globals, `.prt` to disk.
2. `vis_compile_in_place(prt_path, &vopts)` — PVS into globals' `dvisdata`.
3. `light_compile_to_memory(NULL, &bsp_final, &final_size, &lit, &lit_size)` — lighting baked + final BSP re-serialised from globals (now containing geometry + vis + lightmaps + entdata).
4. `Editor_VFS_Register("maps/<name>.bsp", bsp_final, final_size)` and reload via `map <name>`.

The intermediate `bsp_unlit` buffer is freed by the caller after `vis_compile_in_place` returns; only the post-light buffer ends up in the VFS.

## Non-goals (v1)

- **In-memory `.prt` handoff.** M1 keeps the `.prt` as a disk hop next to the `.map`. qbsp already writes it (`portfilename` setup at `qbsp.c:900-902`); vis reads it. M2 refactors `WritePortalfile` and `LoadPortals` to talk to a shared portal-buffer following the BSP `qbsp_membuf` pattern.
- **Threading.** VIS's `#ifdef __alpha` pthread path is dead code (we don't define `__alpha`); `LOCK`/`UNLOCK` already collapse to no-ops, `numthreads = 1` is the default. Single-threaded bake. SDL_Thread parity follows light's `light_bake_thread.c` once correctness is proven.
- **Multi-call hardening past M2's reset audit.** M1 supports one `editor_compile_export` per session; a second call may crash. Same caveat qbsp/light shipped with.
- **ImGui "Compile + Export" button.** Console command first; UI is a one-line wrapper once the command works.
- **Modifying `editor_compile_full`.** That command keeps its qbsp+light shape for fast iteration. `editor_compile_export` is new.

## Constraints

- Vendored sources are not modified except for case normalisation (`VIS.C` → `vis.c`). All exported-name renames happen via `vis_namespace.h` — no source edits.
- New library compiles in `build.zig`'s existing `addCSourceFiles` framework. Third C source group, same flag list as light (qbsp's `_namespace.h` plus vis's, same `-DWIN32 -DDOUBLEVEC_T -std=gnu89 -fcommon -w -fno-sanitize=undefined`).
- No new runtime deps. Links against the qbsp module already in the binary.
- License: id Software VIS is GPLv2; we ship under the same terms as the WinQuake fork in `sdlquake/engine_src/`. No license-related work in this plan.

## Source survey

- `ref/Quake-Tools-master/qutils/VIS/{VIS.C,FLOW.C,SOUNDPVS.C,VIS.H}` — 4 files, 1,709 LOC total.
- `main()` at `VIS.C:885` (78 lines: arg parsing → `LoadBSPFile` → `LoadPortals` → `CalcVis` → `CalcAmbientSounds` → `WriteBSPFile`). We discard `main()` (the namespace header renames it to `vis_main_unused`) and lift the body into `vis_compile_in_place` minus the `LoadBSPFile` and `WriteBSPFile` calls — qbsp loaded the BSP into globals, light will re-serialise it.
- **File-scope globals to reset between calls:**
  - `VIS.C`: `numportals`, `portalleafs`, `portals`, `leafs`, `c_portaltest`, `c_portalpass`, `c_portalcheck`, `leafon`, `vismap`, `vismap_p`, `vismap_end`, `originalvismapsize`, `uncompressed`, `bitbytes`, `bitlongs`, `fastvis`, `verbose`, `testlevel`, `totalvis`, `count_sep`.
  - `FLOW.C`: `c_chains`, `c_portalskip`, `c_leafskip`, `c_vistest`, `c_mighttest`, `active`, `c_leafsee`, `c_portalsee`.
  - `SOUNDPVS.C`: no file-scope globals.
  - (Two read-only constants stay at default: `numthreads = 1`, `showgetleaf = true`.)
- **Type-name clashes with qbsp** (different struct layouts in `vis.h` vs `bsp5.h`/`portals.c`): `winding_t`, `plane_t`, `portal_t`, `leaf_t`. The namespace header must `#define` these to `vis_winding_t` etc.
- **Function-name clashes with qbsp**: `NewWinding`, `FreeWinding`, `ClipWinding`, `CopyWinding` (qbsp's `portals.c` defines its own versions). Plus VIS-only: `LeafFlow`, `BasePortalVis`, `PortalFlow`, `CalcAmbientSounds`, `LoadPortals`, `CalcVis`, `CalcPortalVis`, `CompressRow`, `PlaneFromWinding`, `ClipToSeperators`, `RecursiveLeafFlow`, `SimpleFlood`, `Findpassages`, `pw`, `prl`. All get `vis_` prefix.
- The portal-file format `PRT1` is text: header `PRT1\n<portalleafs>\n<numportals>\n`, then per-portal `<numpts> <leaf0> <leaf1> ` followed by `(x y z) ` per point.
- **What VIS reads from the BSP that qbsp must have left in globals:** `dleafs`, `dvertexes`, `dedges`, `dsurfedges`, `dfaces`, `dtexinfo`, `miptex`/`dtexdata`, `dplanes`, `dnodes`, `dlightdata` (sound-PVS only). All populated by qbsp + accessible via `vendor/qbsp/bspfile.c` globals; same way light reads them today.
- **What VIS writes:** `dvisdata` and `dleafs[].visofs` (per-leaf offsets), plus `dleafs[].ambient_level[]` (`CalcAmbientSounds`).

## File structure

**New files (vendored — verbatim from `ref/Quake-Tools-master/qutils/VIS/` with lowercase names):**
- `sdlquake/vendor/vis/vis.c`
- `sdlquake/vendor/vis/flow.c`
- `sdlquake/vendor/vis/soundpvs.c`
- `sdlquake/vendor/vis/vis.h`

**New files (glue):**
- `sdlquake/vendor/vis/vis_namespace.h` — forced-include, renames VIS's symbols.
- `sdlquake/vendor/vis/vis_lib.c` — implements `vis_compile_in_place` + `vis_bench` + reset helpers.
- `sdlquake/engine/vis_lib.h` — public API for engine TUs.

**Modified files:**
- `build.zig` — third `addCSourceFiles` block for vis (~25 LOC).
- `sdlquake/engine/editor/editor.c` — `editor_compile_export` and `vis_bench` console commands + their registrations in `Editor_Init` (~110 LOC).

Total: 7 new files, 2 modified.

---

## Milestones

### M1 — Vendor + first vis-included BSP loads (3 days)

### M2 — In-memory `.prt` + multi-call reset (2 days)

### M3 — Optional polish (deferred, not in this plan)

---

## M1 Tasks

### Task 1: Vendor the VIS sources

**Files:**
- Create: `sdlquake/vendor/vis/vis.c` (copy of `ref/Quake-Tools-master/qutils/VIS/VIS.C`)
- Create: `sdlquake/vendor/vis/flow.c` (copy of `ref/Quake-Tools-master/qutils/VIS/FLOW.C`)
- Create: `sdlquake/vendor/vis/soundpvs.c` (copy of `ref/Quake-Tools-master/qutils/VIS/SOUNDPVS.C`)
- Create: `sdlquake/vendor/vis/vis.h` (copy of `ref/Quake-Tools-master/qutils/VIS/VIS.H`)

- [ ] **Step 1: Copy the sources verbatim**

PowerShell:
```powershell
New-Item -ItemType Directory -Force -Path sdlquake/vendor/vis
Copy-Item ref/Quake-Tools-master/qutils/VIS/VIS.C       sdlquake/vendor/vis/vis.c
Copy-Item ref/Quake-Tools-master/qutils/VIS/FLOW.C      sdlquake/vendor/vis/flow.c
Copy-Item ref/Quake-Tools-master/qutils/VIS/SOUNDPVS.C  sdlquake/vendor/vis/soundpvs.c
Copy-Item ref/Quake-Tools-master/qutils/VIS/VIS.H       sdlquake/vendor/vis/vis.h
```

Do **not** edit the contents. Renames are entirely macro-driven via the namespace header.

- [ ] **Step 2: Verify the file count + sizes match upstream**

Run:
```powershell
Get-ChildItem sdlquake/vendor/vis/*.c, sdlquake/vendor/vis/*.h | Select-Object Name, Length
```

Expected: four files. `vis.c` ≈ 24 KB, `flow.c` ≈ 11 KB, `soundpvs.c` ≈ 3.6 KB, `vis.h` ≈ 2.0 KB. Exact sizes don't matter — non-zero, ~that order of magnitude.

- [ ] **Step 3: Commit**

```powershell
git add sdlquake/vendor/vis/vis.c sdlquake/vendor/vis/flow.c sdlquake/vendor/vis/soundpvs.c sdlquake/vendor/vis/vis.h
git commit -m @'
vis: vendor id Software VIS sources verbatim

Same layout as vendor/qbsp/ and vendor/light/. No edits — all symbol
renames will happen via a forthcoming vis_namespace.h forced-include.
'@
```

---

### Task 2: Write the namespace header

**Files:**
- Create: `sdlquake/vendor/vis/vis_namespace.h`

- [ ] **Step 1: Create the namespace header**

Write `sdlquake/vendor/vis/vis_namespace.h`:

```c
/*
 * vis_namespace.h -- forced-include header prepended to every vendored
 * vis .c file by build.zig (after qbsp_namespace.h). Renames vis's
 * exported types and functions to vis_-prefixed versions so they don't
 * collide with qbsp's same-named-but-different definitions (qbsp's
 * winding_t / plane_t / portal_t / leaf_t are structurally different
 * from VIS's, and qbsp's portals.c defines its own NewWinding /
 * ClipWinding / CopyWinding / FreeWinding).
 *
 * cmdlib / mathlib / bspfile symbols are already prefixed to qbsp_ by
 * qbsp_namespace.h, which we also force-include — vis's translation
 * units link against qbsp's already-built copies of those modules.
 *
 * Add new entries here when a future link-time collision shows up.
 */

#ifndef VIS_NAMESPACE_H
#define VIS_NAMESPACE_H

/* vis.h type-name clashes with qbsp's bsp5.h (different struct layouts) */
#define winding_t          vis_winding_t
#define plane_t            vis_plane_t
#define portal_t           vis_portal_t
#define leaf_t             vis_leaf_t
#define pstack_s           vis_pstack_s
#define pstack_t           vis_pstack_t
#define threaddata_t       vis_threaddata_t
#define seperating_plane_s vis_seperating_plane_s
#define sep_t              vis_sep_t
#define passage_s          vis_passage_s
#define passage_t          vis_passage_t
#define vstatus_t          vis_vstatus_t

/* vis.c globals */
#define numportals         vis_numportals
#define portalleafs        vis_portalleafs
#define portals            vis_portals
#define leafs              vis_leafs
#define c_portaltest       vis_c_portaltest
#define c_portalpass       vis_c_portalpass
#define c_portalcheck      vis_c_portalcheck
#define showgetleaf        vis_showgetleaf
#define leafon             vis_leafon
#define vismap             vis_vismap
#define vismap_p           vis_vismap_p
#define vismap_end         vis_vismap_end
#define originalvismapsize vis_originalvismapsize
#define uncompressed       vis_uncompressed
#define bitbytes           vis_bitbytes
#define bitlongs           vis_bitlongs
#define numthreads         vis_numthreads
#define fastvis            vis_fastvis
#define verbose            vis_verbose
#define testlevel          vis_testlevel
#define totalvis           vis_totalvis
#define count_sep          vis_count_sep

/* flow.c globals */
#define c_chains           vis_c_chains
#define c_portalskip       vis_c_portalskip
#define c_leafskip         vis_c_leafskip
#define c_vistest          vis_c_vistest
#define c_mighttest        vis_c_mighttest
#define active             vis_active
#define c_leafsee          vis_c_leafsee
#define c_portalsee        vis_c_portalsee

/* Function names that clash with qbsp's portals.c */
#define NewWinding         vis_NewWinding
#define FreeWinding        vis_FreeWinding
#define ClipWinding        vis_ClipWinding
#define CopyWinding        vis_CopyWinding

/* vis-internal functions (no clash today, but prefix for hygiene + so the
 * editor.c side can call them via vis_-prefixed names if needed) */
#define PlaneFromWinding   vis_PlaneFromWinding
#define LeafFlow           vis_LeafFlow
#define BasePortalVis      vis_BasePortalVis
#define PortalFlow         vis_PortalFlow
#define CalcAmbientSounds  vis_CalcAmbientSounds
#define LoadPortals        vis_LoadPortals
#define CalcVis            vis_CalcVis
#define CalcPortalVis      vis_CalcPortalVis
#define CalcPassages       vis_CalcPassages
#define CompressRow        vis_CompressRow
#define ClipToSeperators   vis_ClipToSeperators
#define RecursiveLeafFlow  vis_RecursiveLeafFlow
#define SimpleFlood        vis_SimpleFlood
#define Findpassages       vis_Findpassages
#define CheckStack         vis_CheckStack
#define PlaneCompare       vis_PlaneCompare
#define pw                 vis_pw
#define prl                vis_prl
#define SurfaceBBox        vis_SurfaceBBox

/* vis.c's old main is renamed and never called — vis_compile_in_place is
 * the real entry point. The rename also keeps it from colliding with
 * sys_sdl.c's main(). */
#define main               vis_main_unused

#endif /* VIS_NAMESPACE_H */
```

- [ ] **Step 2: Sanity-check vs the vendored sources**

Run:
```powershell
Select-String -Path sdlquake/vendor/vis/*.c, sdlquake/vendor/vis/*.h -Pattern '^(int|byte|qboolean|portal_t|leaf_t|float|double|char)\s+\*?\w+\s*[;,=]' | ForEach-Object { $_.Line }
```

Every file-scope global named here should already have an entry in `vis_namespace.h`. Cross-check by eye against the `vis.c globals` and `flow.c globals` sections above. If something is missing, add it.

- [ ] **Step 3: Commit**

```powershell
git add sdlquake/vendor/vis/vis_namespace.h
git commit -m @'
vis: add namespace header for symbol prefixing

Renames VIS's exported types (winding_t, plane_t, portal_t, leaf_t) and
globals/functions to vis_-prefixed versions so they don't collide with
qbsp's same-named-but-structurally-different definitions when both
libraries link into the same engine binary.
'@
```

---

### Task 3: Wire VIS into build.zig

**Files:**
- Modify: `build.zig:233` (just after the light `addCSourceFiles` block ending at line 232)

- [ ] **Step 1: Add the vis source group**

Read the existing light block (lines 196-232) once for reference, then insert a new block immediately after it. Use `Edit` to insert after the closing `});` of the light source group:

```zig
    // Vendored id-VIS (id Software, GPLv2). Third member of the in-process
    // compile trio. Caller invokes qbsp_compile_to_memory first (which
    // leaves dfaces / dplanes / dleafs / etc. populated and writes a .prt
    // alongside the destname), then vis_compile_in_place to fill dvisdata
    // from that .prt, then light_compile_to_memory to bake lighting and
    // re-serialise the final BSP. VIS's same-named types and globals
    // (winding_t / plane_t / portal_t / leaf_t / numportals / portals / ...)
    // collide with qbsp's; vis_namespace.h prefixes them.
    const vis_c_flags: []const []const u8 = &.{
        "-DWIN32",
        "-DDOUBLEVEC_T",
        "-include", "sdlquake/vendor/qbsp/qbsp_namespace.h",
        "-include", "sdlquake/vendor/vis/vis_namespace.h",
        "-fno-strict-aliasing",
        "-fwrapv",
        "-std=gnu89",
        "-fcommon",
        "-w",
        "-fno-sanitize=undefined",
    };
    mod.addCSourceFiles(.{
        .files = &.{
            "sdlquake/vendor/vis/vis.c",
            "sdlquake/vendor/vis/flow.c",
            "sdlquake/vendor/vis/soundpvs.c",
            "sdlquake/vendor/vis/vis_lib.c",
        },
        .flags = vis_c_flags,
    });

```

The edit goes between the light block's closing `});` (line 232) and the Dear ImGui block opening comment (line 234).

- [ ] **Step 2: Verify build of vendored sources only**

Run:
```powershell
zig build
```

Expected: build FAILS with linker errors about undefined references to `vis_compile_in_place` / `vis_bench` (because `vis_lib.c` doesn't exist yet). All four `.c` files in `vendor/vis/` should compile cleanly — namespace renames apply, no syntax errors, no type collisions. If a vis `.c` file fails to compile, the namespace header is missing an entry; add it and retry.

If you see "file not found: sdlquake/vendor/vis/vis_lib.c" instead, that's the expected next-step failure. Proceed.

- [ ] **Step 3: Commit**

```powershell
git add build.zig
git commit -m @'
build: wire vendor/vis sources

Mirror the qbsp/light addCSourceFiles pattern: forced-include both
qbsp_namespace.h and vis_namespace.h, same gnu89/fcommon/-w flag set.
vis_lib.c is in the file list ahead of its existence on purpose — the
next commit lands the entry point.
'@
```

---

### Task 4: Implement the entry point

**Files:**
- Create: `sdlquake/vendor/vis/vis_lib.c`
- Create: `sdlquake/engine/vis_lib.h`

- [ ] **Step 1: Write the public header**

Write `sdlquake/engine/vis_lib.h`:

```c
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
```

- [ ] **Step 2: Write the entry-point implementation**

Write `sdlquake/vendor/vis/vis_lib.c`:

```c
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
```

- [ ] **Step 3: Add reset helpers to vendored sources**

The reset helpers (`vis_reset_visc`, `vis_reset_flowc`) are declared in `vis_lib.c` as `extern`s. They live alongside the file-scope globals they reset, in vendored `.c` files. **Two minimal additions** (the only edits to vendored sources in M1):

Edit `sdlquake/vendor/vis/vis.c` — append at end of file:

```c

/* ------------------------------------------------------------------
 * vis_lib.c reset hook. Lives in this TU so it can see the file-scope
 * statics directly (uncompressed isn't extern-declared in vis.h).
 * Free anything still pointing at heap memory and zero counters. */
void vis_reset_visc(void)
{
    if (portals) { free(portals); portals = NULL; }
    if (leafs)   { free(leafs);   leafs   = NULL; }
    if (uncompressed) { free(uncompressed); uncompressed = NULL; }
    numportals = portalleafs = 0;
    c_portaltest = c_portalpass = c_portalcheck = 0;
    leafon = 0;
    vismap = vismap_p = vismap_end = NULL;
    originalvismapsize = 0;
    bitbytes = bitlongs = 0;
    fastvis = verbose = false;
    testlevel = 2;
    totalvis = 0;
    count_sep = 0;
}
```

The namespace `#define`s mean every symbol above is the `vis_`-prefixed version by the time the compiler sees it.

Edit `sdlquake/vendor/vis/flow.c` — append at end of file:

```c

/* vis_lib.c reset hook for this TU's file-scope statics. */
void vis_reset_flowc(void)
{
    c_chains = 0;
    c_portalskip = c_leafskip = 0;
    c_vistest = c_mighttest = 0;
    active = 0;
    c_leafsee = c_portalsee = 0;
}
```

- [ ] **Step 4: Verify build links**

Run:
```powershell
zig build
```

Expected: succeeds. If a linker error mentions an unresolved `vis_<symbol>`, the namespace header is missing that entry (or the reset helper declared an `extern` that no `.c` file actually exposes). Add the missing rename or remove the spurious extern. The most likely culprit is a global I missed in the survey — add it to both `vis_namespace.h` and the appropriate reset helper.

- [ ] **Step 5: Commit**

```powershell
git add sdlquake/vendor/vis/vis_lib.c sdlquake/engine/vis_lib.h sdlquake/vendor/vis/vis.c sdlquake/vendor/vis/flow.c
git commit -m @'
vis: entry point + reset helpers

vis_compile_in_place takes a .prt path on disk (qbsp writes one at
<destname>.prt during qbsp_compile_to_memory) and fills dvisdata on
qbsp's shared globals. Caller chains into light_compile_to_memory which
re-serialises the BSP with PVS now present.

vis_bench loads a .bsp + .prt from disk for stand-alone timing.

Reset helpers live alongside their file-scope statics in vis.c and
flow.c — the only edits to vendored sources in M1.
'@
```

---

### Task 5: Console commands in editor.c

**Files:**
- Modify: `sdlquake/engine/editor/editor.c`

- [ ] **Step 1: Read the current command-registration pattern**

Open `sdlquake/engine/editor/editor.c` and find:
- The `#include "light_lib.h"` line (~line 24).
- The existing `Editor_Cmd_CompileFull_f` function (~line 742-829).
- The `Editor_Cmd_LightBench_f` (~line 839-850) and `Editor_Cmd_Relight_f` (~line 860-869).
- The `Cmd_AddCommand` calls in `Editor_Init` (search for `Cmd_AddCommand("editor_compile_full"`).

The new commands follow the same shape and registration site.

- [ ] **Step 2: Add `#include "vis_lib.h"` next to the light include**

Find:
```c
#include "qbsp_lib.h"           // qbsp_compile_to_memory
#include "light_lib.h"          // light_compile_to_memory (mono Stage 1)
```

Replace with:
```c
#include "qbsp_lib.h"           // qbsp_compile_to_memory
#include "light_lib.h"          // light_compile_to_memory (mono Stage 1)
#include "vis_lib.h"            // vis_compile_in_place
```

- [ ] **Step 3: Add the export-compile and vis_bench commands**

Insert these two functions immediately after `Editor_Cmd_LightBench_f` (around line 850, before `Editor_Cmd_Relight_f`):

```c
/*
 * editor_compile_export: qbsp + vis + light. The "shippable BSP" pipeline.
 * Same flow as editor_compile_full but adds vis_compile_in_place between
 * qbsp and light, sourcing portal data from the .prt qbsp wrote alongside
 * the destname.
 *
 * Args: optional [fast] (uses VIS -fast flood mode) or [level=N] (1-4,
 * default 2). e.g. `editor_compile_export fast`, `editor_compile_export
 * level=4`.
 */
static void Editor_Cmd_CompileExport_f(void)
{
    char  map_path[256];
    char  prt_path[256];
    char  bsp_vpath[64];
    char  lit_vpath[64];
    void *bsp_unlit = NULL;
    int   unlit_size = 0;
    void *bsp_lit   = NULL;
    int   lit_bsp_size = 0;
    void *lit_bytes = NULL;
    int   lit_bytes_size = 0;
    int   rc;
    qbsp_options_t qopts;
    vis_options_t  vopts;
    int            i;

    if (!edit_scene.mapname[0]) {
        Con_Printf("editor_compile_export: no map loaded\n");
        return;
    }

    memset(&vopts, 0, sizeof(vopts));
    vopts.level = 2;
    for (i = 1; i < Cmd_Argc(); i++) {
        const char *a = Cmd_Argv(i);
        if (!strcmp(a, "fast")) {
            vopts.fast = 1;
        } else if (!strncmp(a, "level=", 6)) {
            vopts.level = atoi(a + 6);
            if (vopts.level < 1) vopts.level = 1;
            if (vopts.level > 4) vopts.level = 4;
        } else if (!strcmp(a, "verbose")) {
            vopts.verbose = 1;
        } else if (!strcmp(a, "nosound")) {
            vopts.skip_sound_pvs = 1;
        } else {
            Con_Printf("editor_compile_export: unknown arg \"%s\"\n", a);
            return;
        }
    }

    snprintf(map_path, sizeof(map_path),
             "%s/maps/%s.map", com_gamedir, edit_scene.mapname);
    snprintf(prt_path, sizeof(prt_path),
             "%s/maps/%s.prt", com_gamedir, edit_scene.mapname);
    snprintf(bsp_vpath, sizeof(bsp_vpath),
             "maps/%s.bsp", edit_scene.mapname);
    snprintf(lit_vpath, sizeof(lit_vpath),
             "maps/%s.lit", edit_scene.mapname);

    if (!Scene_Save(map_path)) {
        Con_Printf("editor_compile_export: save to %s failed\n", map_path);
        return;
    }

    {
        char wad_path[1024];
        snprintf(wad_path, sizeof(wad_path), "%s/gfx/base.wad", com_gamedir);
        Sys_mkdir(va("%s/gfx", com_gamedir));
        if (write_wad2_from_worldmodel(wad_path) != 0) {
            Con_Printf("editor_compile_export: WAD synthesis failed\n");
            return;
        }
    }

    Con_Printf("editor_compile_export: saved %s; running qbsp...\n", map_path);

    memset(&qopts, 0, sizeof(qopts));
    qopts.gamedir = com_gamedir;
    rc = qbsp_compile_to_memory(map_path, &bsp_unlit, &unlit_size, &qopts);
    if (rc != 0 || !bsp_unlit) {
        Con_Printf("editor_compile_export: qbsp failed (rc=%d)\n", rc);
        return;
    }
    Con_Printf("editor_compile_export: qbsp produced %d bytes; running vis (level=%d%s%s)...\n",
               unlit_size, vopts.level,
               vopts.fast ? " fast" : "",
               vopts.skip_sound_pvs ? " no-sound-pvs" : "");

    rc = vis_compile_in_place(prt_path, &vopts);
    if (rc != 0) {
        Con_Printf("editor_compile_export: vis failed (rc=%d)\n", rc);
        free(bsp_unlit);
        return;
    }
    Con_Printf("editor_compile_export: vis complete; running light...\n");

    rc = light_compile_to_memory(NULL,
                                 &bsp_lit, &lit_bsp_size,
                                 &lit_bytes, &lit_bytes_size);
    free(bsp_unlit);
    bsp_unlit = NULL;
    if (rc != 0 || !bsp_lit) {
        Con_Printf("editor_compile_export: light failed (rc=%d)\n", rc);
        if (lit_bytes) free(lit_bytes);
        return;
    }

    Editor_VFS_Register(bsp_vpath, bsp_lit, lit_bsp_size);
    Con_Printf("editor_compile_export: %s = %d bytes (lit + vis'd BSP)\n",
               bsp_vpath, lit_bsp_size);

    if (lit_bytes && lit_bytes_size > 0) {
        Editor_VFS_Register(lit_vpath, lit_bytes, lit_bytes_size);
        Con_Printf("editor_compile_export: %s = %d bytes (.lit)\n",
                   lit_vpath, lit_bytes_size);
    }

    Con_Printf("editor_compile_export: reloading map\n");
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "map %s\n", edit_scene.mapname);
        Cbuf_AddText(buf);
    }
}

/*
 * vis_bench <mapname>: load <gamedir>/maps/<mapname>.bsp + .prt from disk
 * and run the vis pass once, printing per-phase timing. Used to size the
 * cost of a full vis bake on a real map. The .bsp + .prt must already
 * exist on disk -- vendored VIS's LoadBSPFile uses raw fopen and doesn't
 * see PAK contents.
 */
static void Editor_Cmd_VisBench_f(void)
{
    char bsp_path[1024], prt_path[1024];
    const char *name;
    if (Cmd_Argc() < 2) {
        Con_Printf("usage: vis_bench <mapname>\n");
        return;
    }
    name = Cmd_Argv(1);
    snprintf(bsp_path, sizeof(bsp_path), "%s/maps/%s.bsp", com_gamedir, name);
    snprintf(prt_path, sizeof(prt_path), "%s/maps/%s.prt", com_gamedir, name);
    vis_bench(bsp_path, prt_path);
}
```

- [ ] **Step 4: Register both commands in Editor_Init**

Find the `Cmd_AddCommand("editor_compile_full", ...)` call in `Editor_Init` and add two lines immediately after it:

```c
    Cmd_AddCommand("editor_compile_export", Editor_Cmd_CompileExport_f);
    Cmd_AddCommand("vis_bench",             Editor_Cmd_VisBench_f);
```

- [ ] **Step 5: Build**

Run:
```powershell
zig build
```

Expected: succeeds.

- [ ] **Step 6: Commit**

```powershell
git add sdlquake/engine/editor/editor.c
git commit -m @'
editor: add editor_compile_export and vis_bench commands

editor_compile_export runs the full qbsp -> vis -> light pipeline,
producing a shippable-quality BSP with PVS baked. Args: `fast` for
flood-fill vis, `level=N` (1-4), `verbose`, `nosound` (skip PHS).

vis_bench <map> reads <map>.bsp + <map>.prt off disk and times the
vis pass standalone — used to size cost on real maps.
'@
```

---

### Task 6: End-to-end verification

- [ ] **Step 1: Smoke test the unchanged pipeline**

Run:
```powershell
zig build run -- +map test
```

Expected: editor's existing `+map test` still loads. No regression from the build wiring.

- [ ] **Step 2: Open editor, run editor_compile_full (baseline)**

In-game, press the key that opens the editor (F2 or similar; check `editor.c` for the toggle binding). At the editor console:
```
editor_compile_full
```

Expected: same behaviour as before this branch — qbsp + light run, reloaded map renders with lighting. Confirms vis sources don't accidentally break the existing path.

- [ ] **Step 3: Run editor_compile_export**

At the editor console:
```
editor_compile_export
```

Expected console output (approximately):
```
editor_compile_export: saved <gamedir>/maps/test.map; running qbsp...
editor_compile_export: qbsp produced <N> bytes; running vis (level=2)...
vis: <N> portalleafs, <M> portals, visdata=<K> bytes
vis timing: load=...s calcvis=...s sound=...s total=...s
editor_compile_export: vis complete; running light...
light timing: load=...s tnodes=...s bake=...s (faces=<F>, lights=<L>)
editor_compile_export: maps/test.bsp = <N> bytes (lit + vis'd BSP)
editor_compile_export: maps/test.lit = <N> bytes (.lit)
editor_compile_export: reloading map
```

Map reloads. The visdata size should be **non-zero** (with `editor_compile_full` alone it would be 0).

- [ ] **Step 4: Verify visdata is actually populated**

Run the `mcp_call.py` helper (or any other introspection mechanism) to dump a few `dleafs[].visofs` values, or just check that the `visdata=<K>` line shows K > 0. For the editor's test.map (small room scene) expect K in the low hundreds of bytes.

Verification command (PowerShell, dumps the first 32 bytes of the visdata lump from the VFS BSP):
```powershell
zig build run -- +map test
# in console:
# editor_compile_export
# (then quit and inspect — easier path: trust the console output)
```

- [ ] **Step 5: Verify the fast-vis path**

```
editor_compile_export fast
```

Expected: `vis timing: calcvis=` is **noticeably faster** than the level=2 run (often by an order of magnitude on real maps; on test.map both will be ~instant). No errors.

- [ ] **Step 6: Verify ambient sounds**

Place an entity with a sky / liquid surface adjacent to it. `editor_compile_export`. In-game, walk toward the surface — ambient sound should ramp up (slime/lava/water/wind hum, depending on texture). This confirms `CalcAmbientSounds` ran and filled `dleafs[].ambient_level`.

If skipped (`editor_compile_export nosound`), ambient sounds should NOT ramp up.

- [ ] **Step 7: Run vis_bench against a real map**

```
vis_bench start
```

Expected: vis runs against `id1/maps/start.bsp` + the .prt that would need to exist alongside. **Note:** the shipped Quake maps don't include `.prt` files on disk — they're qbsp's intermediate output, discarded after vis. For this bench to work on a shipped map you'd need to first run `editor_compile_export` on a freshly-loaded copy (or use a hand-built test map). Document this in the M1 verification: `vis_bench` is most useful on maps the user has compiled themselves with the editor.

If you don't have a .prt around, run `vis_bench test` after a previous `editor_compile_export test` ran (which wrote `<gamedir>/maps/test.prt` to disk as a qbsp side-effect).

Expected output:
```
vis_bench: LoadBSPFile <T>s (faces=<F>, leafs=<L>)
vis_bench: load=<T>s calcvis=<T>s total=<T>s visdata=<K>
```

- [ ] **Step 8: Commit no-op**

If all six verification steps pass, no commit is needed for this task; the previous task's commit already shipped the working code.

---

## M1 verification gate

M1 ships when:
1. `zig build run -- +map test` opens the editor's test map (no regression).
2. `editor_compile_full` (qbsp + light only) works as before.
3. `editor_compile_export` runs qbsp + vis + light; the resulting BSP has non-zero `visdatasize`.
4. The reloaded map plays — geometry, lighting, doors all work.
5. `editor_compile_export fast` completes faster than the default.
6. `editor_compile_export nosound` skips the ambient-sound calculation.
7. `vis_bench <mapname>` times a vis pass against disk-resident BSP+PRT.

## M1 known risks

- **VIS allocates `mightsee` / `visbits` bit-vectors inside `CalcPortalVis` and never frees them.** Per-call leak proportional to `numportals * bitbytes`. For test-scale maps (~hundred portals, ~125 byte bitbytes) this is ~tens of KB and ignorable for a one-shot. M2 audits this.
- **Single-call only.** A second `editor_compile_export` in the same session will most likely crash because qbsp's globals get reset (which clobbers state vis expected to read) and vis's globals don't fully reset (the `mightsee` leak above plus possibly stale `portals[].status`). Quit and relaunch is the workaround. Documented; M2 fixes.
- **`.prt` lives on disk under `id1/maps/<name>.prt`** after `editor_compile_export`. If the user wants `id1/` clean they need to delete it. Document via a console-log line on first compile.
- **No `.prt` for vis_bench on shipped maps.** Bench only works after a prior `editor_compile_export` for the same name. Documented in the help string for the command.
- **VIS does not understand `func_detail` or hint brushes.** Same caveat qbsp has (and will keep, until the qbsp upgrade plan happens). Maps that try to use those entities will get vanilla treatment.
- **Memory: bitvector workspace.** `uncompressed[bitbytes * portalleafs]` plus `portals[2*numportals * sizeof(portal_t)]`. For `portalleafs=1000, numportals=2000`: ~125 KB + ~320 KB. Fine. For a worst-case shipped map (e3m7-ish, ~5000 portals): ~3 MB. Still fine.

---

## M2 Tasks (M2 — in-memory portals + multi-call hardening, 2 days)

### Task 7: In-memory portal handoff

**Files:**
- Modify: `sdlquake/vendor/qbsp/portals.c` — `WritePortalfile` writes to a buffer when `qbsp_portbuf_active`.
- Modify: `sdlquake/vendor/qbsp/qbsp_lib.c` — declare `qbsp_portbuf_*` API mirroring `qbsp_membuf_*`.
- Modify: `sdlquake/vendor/vis/vis.c` — `LoadPortals` reads from the same buffer when active.
- Modify: `sdlquake/vendor/vis/vis_lib.c` — set `qbsp_portbuf_active = 1` instead of taking a `prt_path`.
- Modify: `sdlquake/engine/vis_lib.h` — `vis_compile_in_place(const vis_options_t *opts)` (drop `prt_path`).
- Modify: `sdlquake/engine/editor/editor.c` — `Editor_Cmd_CompileExport_f` drops the `prt_path` snprintf.

- [ ] **Step 1: Add portbuf API to qbsp_lib.c**

In `sdlquake/vendor/qbsp/qbsp_lib.c`, alongside the existing `qbsp_membuf_*` declarations:

```c
/* Portal-buffer: parallel to qbsp_membuf, but for the .prt text file that
 * qbsp emits and vis consumes. Active during qbsp_compile_to_memory (set
 * by qbsp_lib's wrapper), then vis_compile_in_place reads from it. */
int    qbsp_portbuf_active = 0;
char  *qbsp_portbuf         = NULL;
int    qbsp_portbuf_size    = 0;
int    qbsp_portbuf_pos_r   = 0;   /* read cursor */
static int qbsp_portbuf_cap = 0;

void qbsp_portbuf_reset(void)
{
    if (qbsp_portbuf) free(qbsp_portbuf);
    qbsp_portbuf      = NULL;
    qbsp_portbuf_size = 0;
    qbsp_portbuf_pos_r = 0;
    qbsp_portbuf_cap  = 0;
}

void qbsp_portbuf_append(const char *src, int len)
{
    int need = qbsp_portbuf_size + len;
    if (need > qbsp_portbuf_cap) {
        int cap = qbsp_portbuf_cap ? qbsp_portbuf_cap : (1 << 16);
        while (cap < need) cap *= 2;
        qbsp_portbuf = (char *)realloc(qbsp_portbuf, (size_t)cap);
        if (!qbsp_portbuf) Con_Printf("qbsp_portbuf: out of memory\n");
        qbsp_portbuf_cap = cap;
    }
    memcpy(qbsp_portbuf + qbsp_portbuf_size, src, (size_t)len);
    qbsp_portbuf_size += len;
}

/* fgets-equivalent: read up to len-1 chars or up to a newline, NUL-term.
 * Returns NULL at EOF. Matches LoadPortals's reading pattern. */
char *qbsp_portbuf_gets(char *dst, int len)
{
    int n = 0;
    if (qbsp_portbuf_pos_r >= qbsp_portbuf_size) return NULL;
    while (n < len - 1 && qbsp_portbuf_pos_r < qbsp_portbuf_size) {
        char c = qbsp_portbuf[qbsp_portbuf_pos_r++];
        dst[n++] = c;
        if (c == '\n') break;
    }
    dst[n] = '\0';
    return dst;
}
```

- [ ] **Step 2: Redirect WritePortalfile when portbuf is active**

In `sdlquake/vendor/qbsp/portals.c`, modify `WritePortalfile` (around line 554) to check `qbsp_portbuf_active`:

```c
extern int  qbsp_portbuf_active;
extern void qbsp_portbuf_append(const char *src, int len);

/* Existing fprintf calls become two-path helpers. Add at top of portals.c
 * just after the existing #include lines: */
static void portbuf_printf(FILE *f, const char *fmt, ...)
{
    char buf[256];
    int  n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) n = 0; if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    if (qbsp_portbuf_active) qbsp_portbuf_append(buf, n);
    else                     fwrite(buf, 1, (size_t)n, f);
}
```

Then in `WritePortalFile_r` and `WritePortalfile`, replace every `fprintf(pf, ...)` with `portbuf_printf(pf, ...)`. The `fopen`/`fclose` pair stay; when portbuf is active they're no-ops over a `NULL` file pointer (guard the `fopen` with the same flag).

```c
void WritePortalfile (node_t *headnode)
{
    num_visleafs = 0;
    num_visportals = 0;
    NumberLeafs_r (headnode);

    if (qbsp_portbuf_active) {
        /* Reset portbuf to start a fresh capture for this compile. */
        extern void qbsp_portbuf_reset(void);
        qbsp_portbuf_reset();
        extern int qbsp_portbuf_active;
        pf = NULL;
    } else {
        printf ("writing %s\n", portfilename);
        pf = fopen (portfilename, "w");
        if (!pf)
            Error ("Error opening %s", portfilename);
    }

    portbuf_printf (pf, "%s\n", PORTALFILE);
    portbuf_printf (pf, "%i\n", num_visleafs);
    portbuf_printf (pf, "%i\n", num_visportals);

    WritePortalFile_r (headnode);

    if (pf) fclose (pf);
}
```

`WritePortalFile_r`'s six `fprintf(pf, ...)` calls (lines 483, 486, 489-493, 495) all become `portbuf_printf(pf, ...)` with the same arguments.

- [ ] **Step 3: Redirect LoadPortals when portbuf is active**

In `sdlquake/vendor/vis/vis.c`, modify `LoadPortals` (line 769) to read from the portbuf when active. The change: replace every `fscanf(f, fmt, ...)` with a helper that reads from the file OR walks `qbsp_portbuf` via `qbsp_portbuf_gets`.

The simplest path: when `qbsp_portbuf_active`, build a temporary `FILE *` over the in-memory buffer using `fmemopen` (POSIX) — **NOT** portable to Windows MSVC. Instead, hand-parse the buffer using `qbsp_portbuf_gets` + `sscanf`:

```c
extern int  qbsp_portbuf_active;
extern char *qbsp_portbuf_gets(char *dst, int len);

void LoadPortals (char *name)
{
    /* ... existing variable declarations ... */
    char        line[512];

    if (!qbsp_portbuf_active) {
        /* existing disk path */
        if (!strcmp(name,"-")) f = stdin;
        else {
            f = fopen(name, "r");
            if (!f) {
                printf ("LoadPortals: couldn't read %s\n",name);
                Error ("No vising performed.");
            }
        }
        if (fscanf (f,"%79s\n%i\n%i\n",magic, &portalleafs, &numportals) != 3)
            Error ("LoadPortals: failed to read header");
    } else {
        /* memory path: read line-by-line from qbsp_portbuf */
        f = NULL;
        if (!qbsp_portbuf_gets(line, sizeof(line)) ||
            sscanf(line, "%79s", magic) != 1)
            Error ("LoadPortals: failed to read magic");
        if (!qbsp_portbuf_gets(line, sizeof(line)) ||
            sscanf(line, "%i", &portalleafs) != 1)
            Error ("LoadPortals: failed to read portalleafs");
        if (!qbsp_portbuf_gets(line, sizeof(line)) ||
            sscanf(line, "%i", &numportals) != 1)
            Error ("LoadPortals: failed to read numportals");
    }

    if (strcmp(magic,PORTALFILE))
        Error ("LoadPortals: not a portal file");
    /* ... rest unchanged for allocations + reading portals ... */
}
```

The per-portal `fscanf(f, "%i %i %i ", ...)` and `fscanf(f, "(%lf %lf %lf ) ", ...)` calls similarly fork on `qbsp_portbuf_active`:

```c
    for (i=0, p=portals ; i<numportals ; i++) {
        if (qbsp_portbuf_active) {
            if (!qbsp_portbuf_gets(line, sizeof(line)) ||
                sscanf(line, "%i %i %i", &numpoints, &leafnums[0], &leafnums[1]) != 3)
                Error ("LoadPortals: reading portal %i", i);
        } else {
            if (fscanf(f, "%i %i %i ", &numpoints, &leafnums[0], &leafnums[1]) != 3)
                Error ("LoadPortals: reading portal %i", i);
        }
        /* numpoints/leafnums checks unchanged */

        /* points loop */
        for (j=0 ; j<numpoints ; j++) {
            double v[3];
            int    k;
            if (qbsp_portbuf_active) {
                /* Each "( x y z )" is on the same line as the count for
                 * id's writer; LoadPortals's existing format is one
                 * portal per line. Re-parse the same `line` after the
                 * leading 3 ints. Track an offset into the line. */
                /* Easier: scan all points at once via a parsing pointer. */
                /* For M2 first cut, accept multi-line portal blocks: each
                 * point gets its own line. WritePortalFile_r already emits
                 * a newline at the end of each portal. We need WritePortal
                 * to use one line per portal for the memory path too -- it
                 * already does (single fprintf with all points then \n). */
                /* Simplification: lookahead into the next line for points. */
                /* Realistic: re-use qbsp_portbuf via a single-line buffer
                 * and call sscanf with a tracked offset. Implementation
                 * detail; see comments in this step. */
            } else {
                if (fscanf (f, "(%lf %lf %lf ) ", &v[0], &v[1], &v[2]) != 3)
                    Error ("LoadPortals: reading portal %i", i);
            }
            /* assignment unchanged */
        }
        if (!qbsp_portbuf_active) fscanf (f, "\n");
        /* plane calc + create-portals unchanged */
    }
    if (f) fclose (f);
```

The points-on-one-line parsing is the only sticky part. WritePortalFile_r's existing format puts the count + all points on a single line ending in `\n`, so the cleanest implementation is to grab the whole line via `qbsp_portbuf_gets`, then advance a `char *cursor` pointer using `sscanf(cursor, "(%lf %lf %lf ) %n", ..., &consumed); cursor += consumed;`. Spell out for the implementer:

```c
        /* memory path: parse "<n> <a> <b> (x y z) (x y z) ..." from one line */
        char *cursor;
        int   consumed;
        if (qbsp_portbuf_active) {
            if (!qbsp_portbuf_gets(line, sizeof(line)))
                Error ("LoadPortals: reading portal %i", i);
            cursor = line;
            if (sscanf(cursor, "%i %i %i %n", &numpoints, &leafnums[0], &leafnums[1], &consumed) != 3)
                Error ("LoadPortals: reading portal %i header", i);
            cursor += consumed;
        } /* else fscanf as before */
        /* numpoints check + alloc unchanged */
        for (j=0 ; j<numpoints ; j++) {
            double v[3];
            int    k;
            if (qbsp_portbuf_active) {
                if (sscanf(cursor, "(%lf %lf %lf ) %n", &v[0], &v[1], &v[2], &consumed) != 3)
                    Error ("LoadPortals: reading portal %i point %i", i, j);
                cursor += consumed;
            } else {
                if (fscanf (f, "(%lf %lf %lf ) ", &v[0], &v[1], &v[2]) != 3)
                    Error ("LoadPortals: reading portal %i", i);
            }
            for (k=0 ; k<3 ; k++) w->points[j][k] = v[k];
        }
        if (!qbsp_portbuf_active) fscanf (f, "\n");
```

- [ ] **Step 4: Update vis_lib.c entry point signature**

In `sdlquake/engine/vis_lib.h`:
```c
int vis_compile_in_place(const vis_options_t *opts);
int vis_bench(const char *bsp_path);
```

In `sdlquake/vendor/vis/vis_lib.c`, drop the `prt_path` parameter. The body becomes:

```c
int vis_compile_in_place(const vis_options_t *opts)
{
    /* ... setjmp + reset + apply_options unchanged ... */

    /* The caller (qbsp_compile_to_memory in portbuf mode) left
     * qbsp_portbuf populated. LoadPortals checks qbsp_portbuf_active. */
    t0 = I_FloatTime();
    LoadPortals(NULL);    /* name unused when portbuf is active */
    t1 = I_FloatTime();
    /* ... rest unchanged ... */
}

int vis_bench(const char *bsp_path)
{
    /* Disk path -- no portbuf. After LoadBSPFile, build .prt path
     * from bsp_path and feed to LoadPortals via the disk path. */
    extern int qbsp_portbuf_active;
    char prt_path[1024];
    int  i;
    size_t n;

    /* ... existing LoadBSPFile + setjmp + timing setup ... */

    n = strlen(bsp_path);
    if (n + 1 > sizeof(prt_path)) n = sizeof(prt_path) - 1;
    memcpy(prt_path, bsp_path, n); prt_path[n] = '\0';
    for (i = (int)strlen(prt_path) - 1; i >= 0; i--) {
        if (prt_path[i] == '.') { prt_path[i] = '\0'; break; }
        if (prt_path[i] == '/' || prt_path[i] == '\\') break;
    }
    strncat(prt_path, ".prt", sizeof(prt_path) - strlen(prt_path) - 1);

    qbsp_portbuf_active = 0;     /* disk path */
    LoadPortals(prt_path);
    /* ... unchanged ... */
}
```

- [ ] **Step 5: Update qbsp_lib.c to enable portbuf during compile**

In `qbsp_compile_to_memory` body:
```c
    qbsp_membuf_active = 1;
    qbsp_membuf_reset();
    qbsp_portbuf_active = 1;     /* NEW: enable portal capture */
    qbsp_portbuf_reset();        /* NEW */
    /* ... setjmp ... */
    ProcessFile(sourcename, destname);
    qbsp_err_jmp       = NULL;
    qbsp_membuf_active = 0;
    qbsp_portbuf_active = 0;     /* NEW: leave it set for vis to read;
                                    actually set to 0 here -- vis reads
                                    via qbsp_portbuf_gets which doesn't
                                    check active flag, only LoadPortals's
                                    fork does. The flag is per-call. */
```

Wait — vis's `LoadPortals` checks the flag. So we need to either keep the flag set until after vis reads, or refactor `LoadPortals` to take the buffer explicitly. Pick the simpler: leave the flag set after `qbsp_compile_to_memory` returns; `vis_compile_in_place` clears it on exit. Document in `qbsp_lib.h` that the caller MUST run vis (or call `qbsp_portbuf_reset()` + clear the flag manually) before the next qbsp compile.

```c
    /* qbsp_portbuf is left alive for vis_compile_in_place's LoadPortals.
     * vis_compile_in_place clears qbsp_portbuf_active and frees the
     * buffer when it's done. */
    /* qbsp_portbuf_active stays 1 -- do NOT reset here. */
```

And in `vis_compile_in_place` epilogue:
```c
    free(uncompressed); uncompressed = NULL;
    qbsp_err_jmp = NULL;

    /* Release the portbuf -- qbsp's contract is that we consume it. */
    extern int  qbsp_portbuf_active;
    extern void qbsp_portbuf_reset(void);
    qbsp_portbuf_active = 0;
    qbsp_portbuf_reset();

    return 0;
```

- [ ] **Step 6: Update editor.c to drop the prt_path**

In `Editor_Cmd_CompileExport_f`:
- Remove the `char prt_path[256];` declaration.
- Remove the `snprintf(prt_path, ...)` call.
- Change `vis_compile_in_place(prt_path, &vopts);` to `vis_compile_in_place(&vopts);`.

In `Editor_Cmd_VisBench_f`:
- Remove the `prt_path` snprintf.
- Change `vis_bench(bsp_path, prt_path);` to `vis_bench(bsp_path);`.

- [ ] **Step 7: Build + verify**

Run:
```powershell
zig build
```

Expected: builds. Then:
```powershell
zig build run -- +map test
```

In editor:
```
editor_compile_export
```

Expected: same end-to-end output as M1 step 3, but no `<gamedir>/maps/test.prt` left on disk afterward. Verify with:
```powershell
Test-Path id1/maps/test.prt
```
Expected: `False` (or whichever path com_gamedir resolves to).

- [ ] **Step 8: Commit**

```powershell
git add sdlquake/vendor/qbsp/portals.c sdlquake/vendor/qbsp/qbsp_lib.c sdlquake/vendor/vis/vis.c sdlquake/vendor/vis/vis_lib.c sdlquake/engine/vis_lib.h sdlquake/engine/editor/editor.c
git commit -m @'
vis: in-memory .prt handoff via qbsp_portbuf

Mirror the qbsp_membuf pattern for the portal text file. qbsp's
WritePortalfile appends to qbsp_portbuf when qbsp_portbuf_active is
set (qbsp_compile_to_memory enables it); vis_compile_in_place reads
via qbsp_portbuf_gets in LoadPortals's memory branch and frees on
exit. No .prt sidecar lands on disk anymore.

vis_compile_in_place drops the prt_path argument; vis_bench keeps
it via a derived path next to bsp_path for the disk-resident
bench case.
'@
```

---

### Task 8: Multi-call reset audit

**Files:**
- Modify: `sdlquake/vendor/vis/vis_lib.c` — extend `vis_reset_state` to free per-portal allocations.
- Modify: `sdlquake/vendor/vis/flow.c` — declare a tracking head for `mightsee` / `visbits` allocations.

- [ ] **Step 1: Add allocation tracking in flow.c**

VIS's `BasePortalVis` and `CalcPortalVis` allocate `byte *mightsee` and `byte *visbits` per portal. Original VIS never frees these because it's a one-shot process. For M2 we track them in a singly-linked list.

At the top of `sdlquake/vendor/vis/flow.c`, append after the existing globals:

```c

/* M2: per-portal scratch allocations. id's VIS leaks these on purpose
 * (process exit reclaims them); we walk this list in vis_reset_state.
 * Insertion is O(1) at head; no removal needed at the per-allocation
 * level — the list resets wholesale between compiles. */
typedef struct vis_track_s {
    struct vis_track_s *next;
    void               *ptr;
} vis_track_t;
static vis_track_t *vis_track_head = NULL;

void *vis_track_malloc(int size)
{
    vis_track_t *t = (vis_track_t *)malloc(sizeof(vis_track_t));
    void *p = malloc((size_t)size);
    if (!t || !p) return NULL;
    t->next = vis_track_head;
    t->ptr  = p;
    vis_track_head = t;
    return p;
}

void vis_track_free_all(void)
{
    vis_track_t *t = vis_track_head, *next;
    while (t) {
        next = t->next;
        free(t->ptr);
        free(t);
        t = next;
    }
    vis_track_head = NULL;
}
```

- [ ] **Step 2: Replace mightsee/visbits mallocs with tracked variants**

Grep for `malloc(bitbytes)` in `flow.c`:
```powershell
Select-String -Path sdlquake/vendor/vis/flow.c -Pattern 'malloc\s*\(\s*bitbytes' | ForEach-Object { $_.Line }
```

Expected: 2-3 sites where `malloc(bitbytes)` is assigned to `p->mightsee` or `p->visbits` or a `stack.mightsee`. Replace each `malloc(bitbytes)` with `vis_track_malloc(bitbytes)`. Same for any `calloc(bitbytes, 1)` if present (use track_malloc + memset).

Do NOT track the `uncompressed` buffer — `vis_compile_in_place` already frees it explicitly.

- [ ] **Step 3: Call vis_track_free_all from vis_reset_state**

In `sdlquake/vendor/vis/vis_lib.c`, extend `vis_reset_state`:

```c
extern void vis_track_free_all(void);

static void vis_reset_state(void)
{
    vis_reset_visc();
    vis_reset_flowc();
    vis_track_free_all();        /* NEW: free mightsee/visbits arrays */

    visdatasize = 0;
    memset(dvisdata, 0, sizeof(dvisdata));
}
```

- [ ] **Step 4: Build + run 5x**

```powershell
zig build run -- +map test
```

In editor console, run `editor_compile_export` **five** times in succession.

Expected: each completes cleanly, no crash, no error message. The fifth run's timing should be in the same ballpark as the first.

Watch the engine's process RSS in Task Manager between runs. Expected: stable (~no growth across runs after the second; the very first compile allocates qbsp/light hunks the second won't repeat).

- [ ] **Step 5: Commit**

```powershell
git add sdlquake/vendor/vis/flow.c sdlquake/vendor/vis/vis_lib.c
git commit -m @'
vis: track + free mightsee/visbits across calls

VIS's main() never freed the per-portal bit-vector scratch buffers
because the process exited after one bake. In-process means a second
editor_compile_export was leaking ~bitbytes*numportals bytes per call.

Track via a tiny linked-list head in flow.c; vis_reset_state walks it
and frees wholesale between compiles.
'@
```

---

## M2 verification gate

M2 ships when:
1. M1 gate still passes.
2. `editor_compile_export` runs 5+ times in one session without crash.
3. Process RSS is stable (within ~10 MB drift) across 10 repeated compiles.
4. `id1/maps/<name>.prt` is **not** present after an `editor_compile_export` (confirms in-memory portbuf).

## M2 known risks

- **`sscanf` with `%n` is the cross-platform parse trick** — MSVC and clang both support it, but if a future port to a stricter toolchain trips on `%n`'s deprecation warnings, fall back to manual `strtod`/`strtol`.
- **The portbuf grows monotonically per call.** `qbsp_portbuf_reset` is called at the start of each qbsp compile, so growth is per-compile not per-process. For a worst-case ~10,000-portal map the buffer hits ~1 MB. Fine.
- **Cross-thread concerns**: the portbuf API is not thread-safe. Both qbsp and vis run on the main thread today; if vis ever moves to a worker thread (M3), the portbuf hand-off needs a synchronisation rethink.

---

## M3 — Optional polish (deferred — not in this plan)

These items are noted for future work but explicitly out of scope:

- **SDL_Thread parallel vis.** `CalcPortalVis` iterates leaves via the `leafon` counter; mapping this to N SDL_Thread workers with a single atomic counter matches what light's `light_bake_thread.c` does. Defer until single-threaded vis is proven correct.
- **ImGui "Compile + Export" toolbar button.** One-line wrapper around `Cbuf_AddText("editor_compile_export\n")`.
- **In-editor PVS visualisation.** Draw `dvisdata[currentleaf]`'s visible-leaf bitmask as a wireframe overlay so authors can spot misconfigured portals.
- **Auto-compile on save.** Pair with a debounce timer; potentially expensive if vis takes seconds.
- **`func_detail` / hint-brush support.** Would require an upgrade of all three vendored tools to an ericw-tools-style fork. Tracked separately.

---

## Self-review notes

- Every step contains the exact file paths, exact commands, exact code.
- Method signatures align across tasks: `vis_compile_in_place(const char *prt_path, const vis_options_t *opts)` in M1 → `vis_compile_in_place(const vis_options_t *opts)` in M2, called out explicitly in the M2 task that flips it.
- The reset-helper names (`vis_reset_visc`, `vis_reset_flowc`, `vis_track_free_all`) match between declaration sites (`vis_lib.c`) and definition sites (`vis.c`, `flow.c`).
- The portbuf API names (`qbsp_portbuf_active`, `qbsp_portbuf_reset`, `qbsp_portbuf_append`, `qbsp_portbuf_gets`) are consistent across qbsp_lib.c, portals.c, vis.c, vis_lib.c.
- Verification gates are concrete: console output samples + filesystem checks the implementer can run.
- The plan respects the qbsp plan's precedent of "ship M1, harden in M2" rather than trying to land in-memory + multi-call in one go.

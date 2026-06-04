# Threaded Span Fill (`D_DrawSurfaces`) — Design

**Date:** 2026-06-04
**Status:** Phase 0 ✅ (`7bb1864`), Phase 1 ✅ (`076546b`), Phase 2 ✅ (`98f82ca`). Next: Phase 3 (fork the fill).
Phase 1: draw reads a recorded cache (`surf_t.rcache/rmip/rbucket`) validated against the stable
`cachespots` slot; allocator + dlit re-light run only in the serial resolve pass.
Phase 2 (reframed from `__thread`-only, user-approved): all per-surface setup (cacheblock +
bmodel transform + `D_CalcGradients`) moved into the serial resolve pass via `D_SetupSurfaceFill`,
recording gradient OUTPUTS + cacheblock into `surf_t.fill`. The draw loop's turb/solid branches now
just LOAD `s->fill` and draw — **no `R_RotateBmodel`, no `D_CalcGradients` in the fill** (the bmodel
problem is gone). Phase 3 = `__thread` the per-surface load-target globals
(`cacheblock`/`cachewidth`/`miplevel`/the 10 gradients/`d_zi*`/`r_turb_*`) + fork the draw loop by
scanline bands. Thrash frames keep the fill serial (the re-setup path calls the allocator).
**Scope:** Parallelize the software renderer's **span fill** (`D_DrawSurfaces` /
`D_DrawSpans8` / `D_DrawZSpans` / `Turbulent8`) across CPU threads, decomposed by
horizontal scanline bands. The **edge sweep** (`R_ScanEdges` AET maintenance +
span generation) stays 100% serial. No GPU triangle rendering — the 8-bit
software look is preserved (see `keep-software-renderer-look` memory). **All new
code is C** (SDL3 C thread API); no C++.

## Why

Perf targets: **Steam Deck (Zen 2, x86-64) @ 90 fps = 11.1 ms/frame**, **Mac mini
M4 (aarch64) @ 60 fps = 16.7 ms/frame**. The Deck is the binding constraint —
tighter budget *and* weaker single-thread, but 8 hardware threads to fill.

Measured at **1280×800 (Deck native)** on `e1m1` spawn (commit `08a4e61`
instrumentation, `r_sweepprofile` + the `D_DrawSurfaces` child scope):

| Phase | ms/frame | share |
|---|---|---|
| `R_ScanEdges` (total) | 3.738 | 100% |
| └ **`D_DrawSurfaces` — pixel fill** | **3.428** | **92%** |
| └ sweep (insert .010 + gen .201 + remove .003 + step .056) | 0.270 | 7% |
| └ residual | 0.040 | 1% |

The fill is 92% of the rasterizer and is **embarrassingly parallel**: the sweep
resolves occlusion *before* fill, so visible spans are disjoint in screen space —
threads filling different spans write disjoint framebuffer + z-buffer pixels, zero
overdraw. The sweep is only 7% and is serial by nature (vertical coherence), so it
is out of scope. Threading the fill is the single highest-leverage,
architecture-neutral change for both targets, and it is the prerequisite for any
later per-loop SIMD.

## Ground truth (from code survey)

- **Fill path is C-only.** No `.s`/asm in this fork; `id386` is off on both
  x86-64 and arm64 (`#if !id386` guards in `d_scan.c`). We only touch C draw fns.
- **Framebuffer is never the hazard.** Occlusion resolved pre-fill → disjoint
  spans → disjoint pixel + z writes. No locks on output. Output must be
  **bit-identical** serial vs threaded (deterministic, disjoint).
- **Hazard 1 — per-surface global state (~31 vars).** `D_CalcGradients` writes,
  the span drawers read: `d_sdivzstepu/v`, `d_tdivzstepu/v`, `d_sdivzorigin`,
  `d_tdivzorigin`, `d_zistepu/v`, `d_ziorigin`, `sadjust`, `tadjust`, `bbextents`,
  `bbextentt`, `cacheblock`, `cachewidth`, `miplevel`, `transformed_modelorg`,
  `currententity`, and the 13 `r_turb_*` turbulent-span vars. These change per
  surface → must be thread-local.
- **Safe to share — per-frame read-only state (~34 vars).** `d_viewbuffer`,
  `d_pzbuffer`, `d_zwidth`, `d_zrowbytes`, `screenwidth`, `xcenter`/`ycenter`/
  `xscaleinv`/`yscaleinv`, mip tables (`d_minmip`, `d_scalemip`, `scale_for_mip`,
  `d_drawspans`), sky/fog/water/caustics flags. Set once per frame, read-only
  during fill → stay plain globals.
- **Hazard 2 — surface-cache allocator.** `D_SCAlloc` (`d_surf.c`) is a circular
  bump allocator with LRU eviction over `sc_rover`/`sc_base`; mutating the rover
  + chain + `*owner=NULL` from two threads corrupts the pool. A cache **build** is
  ~150k–600k cycles (lightmap×texture combine in `R_DrawSurface`); a **hit** is
  ~20 cycles. Dynamic lightmaps (fire/oil) call `D_FlushCaches` which nukes the
  whole pool, forcing rebuilds.

## Locked decisions

| Decision | Choice | Why |
|---|---|---|
| Target | **Fill only** (`D_DrawSurfaces`); sweep stays serial | Fill is 92%; sweep (7%) is serial-by-nature, not worth de-globalizing |
| Decomposition | **Horizontal scanline bands** (not surface work-queue, not tiles) | Spans are 1 px tall (`espan_t{u,v,count}`) → fall wholly in one band; guaranteed load balance + contiguous-row locality. Surface-queue lets a big floor cap speedup; tiles would split spans at vertical edges for no gain |
| Cache hazard | **Serial pre-build pass, then lock-free parallel reads** | Allocator stays single-threaded; parallel phase only reads built `cache->data`. No lock |
| Per-surface globals | **`__thread` storage class** (C, clang/zig cc) | Least-invasive: `D_CalcGradients`/`D_DrawSpans8` logic unchanged, each worker uses its own copy. ~0 overhead (hoisted to registers in hot loops) |
| Fill timing | **One end-of-frame fill** (remove the 6×/frame overflow flush) | Threading needs the complete span set in one pass; today's mid-sweep flush is a 1996 memory hack |
| Thread pool | **Persistent SDL3 C workers + per-frame fork/join** | No per-frame thread creation; mirrors `light_bake_thread.c` primitives |
| Serial oracle | **`r_threads` cvar; 0/1 = serial path** | Phases 0–2 ship a behavior-preserving serial path that is the regression oracle |
| Bmodel fill | **Leave serial** | `R_DrawBEntitiesOnList` fill is ~0.008 ms; fold in later if ever needed |

## Architecture

```
R_ScanEdges (serial)
  ├─ sweep all scanlines → complete per-surface span lists   (unchanged, serial)
  ├─ PRE-BUILD: walk visible surfaces, D_CacheSurface each    (NEW, serial)
  └─ FILL: fork N bands ─┬─ band 0 rows [y0,y1)               (NEW, parallel)
                         ├─ band 1 …
                         └─ band N-1 …
        barrier ── fill complete before alias models / particles / viewmodel z-test
```

Per worker, per band `[y0,y1)`:

```c
for (s = &surfaces[1]; s < surface_p; s++) {
    if (no span of s has v in [y0,y1)) continue;
    D_CalcGradients(s);                 // writes this worker's __thread globals
    cacheblock = s's PRE-BUILT cache->data;   // read-only
    for (span in s->spans where span->v in [y0,y1)) {
        D_DrawSpans8(span);   // disjoint pixels
        D_DrawZSpans(span);   // disjoint z slots
    }
}
```

A span is 1 px tall, so it belongs to exactly one band — never split, never shared
across threads. Sky/background/turbulent surface-type dispatch (the `s->flags`
branches in `D_DrawSurfaces`) is replicated inside the band loop; all its globals
are covered by the `__thread` conversion.

## Build order (de-risk first — each phase independently verifiable)

**Phase 0 — single-pass fill (serial).** `basespans` is a 72 KB *stack* array
(`MAXSPANS=3000`); it overflows ~6×/frame at 1280 wide, flushing `D_DrawSurfaces`
mid-sweep. Move it to a static/heap buffer sized for a full frame (~40k spans ×
24 B ≈ 1 MB, grow-on-demand) and drop the mid-sweep flush so `D_DrawSurfaces` runs
**once** at end-of-frame. *Verify:* pixel-identical output, unchanged serial
timing. (Also removes 6×/frame `S_ExtraUpdate`.)

**Phase 1 — serial cache pre-build.** Between sweep and fill, walk
`surfaces[1..surface_p]` and `D_CacheSurface` each (miplevel/bucket it will use).
*Verify:* pixel-identical output; allocator now runs only here.

**Phase 2 — `__thread` the per-surface globals.** Flip the ~31 vars' storage
class. Still single-threaded. *Verify:* pixel-identical output + measure TLS
overhead (expected ~0).

**Phase 3 — fork the fill across bands.** Persistent worker pool; per frame:
pre-build (serial) → fork bands → barrier. `r_threads` selects worker count.
*Verify:* bit-identical framebuffer diff vs serial; measure `R_ScanEdges`/`fill`
on Deck + M4.

## Thread pool (pure C, SDL3)

Startup: create `min(SDL_GetNumLogicalCPUCores(), CAP) − 1` workers
(`SDL_CreateThread`); the main thread is the Nth worker. Per frame the main thread
publishes band work, releases workers via `SDL_Semaphore`
(`SDL_SignalSemaphore`), runs one band itself, then waits on an `SDL_AtomicInt`
done-count + completion semaphore. No per-frame thread creation. `r_threads` cvar
sets worker count (0/1 → serial path = the Phases 0–2 oracle). Over-decompose to
~2–4× bands than threads + atomic band claim for balance.

## Verification

Fill is deterministic and spans are disjoint ⇒ threaded output is **bit-identical**
to serial. After each phase: same `e1m1` spawn capture, framebuffer diff
(serial vs threaded) must be zero, and compare `R_ScanEdges` / new `fill` perf
scopes. Live smoke test on the machine with `-nosound -nofocus`. Cross-check on
both Deck (x86-64) and M4 (aarch64).

**Targets:** 3.4 ms fill → ~0.5 ms (Deck, 8 threads) / ~0.4 ms (M4), pulling
`R_ScanEdges` ~3.7 → ~0.8 ms and the Deck under 11.1 ms / 90 fps — before SIMD.

## Out of scope (later slices)

- **SIMD of the per-pixel loops** — separate follow-up. `D_DrawZSpans` + the
  surface-cache lightmap×texture combine are gather-free → 128-bit portable C
  intrinsics (or `simde`, **not** Highway/C++) serve both ISAs. The `D_DrawSpans8`
  texel gather is where x86 (AVX2 gather) and NEON (no gather) diverge → defer /
  per-ISA. Multiplies on top of threading per-thread.
- **Threading the sweep** (per-band independent `R_ScanEdges`) — only if the 7%
  sweep ever dominates; needs de-globalizing `edge_t` links.
- **Bmodel fill threading**, **span re-bucketing into per-band worklists** +
  **per-surface precomputed gradients** (avoids redundant `D_CalcGradients`) —
  refinements if profiling asks.

## References

- Instrumentation + measurement: commit `08a4e61` (`R_ScanEdges` sweep/fill
  split, `r_sweepprofile`).
- Threading pattern: `sdlquake/engine/editor/light_bake_thread.c`.
- Fill path: `d_edge.c` (`D_DrawSurfaces`), `d_scan.c` (`D_DrawSpans8`,
  `D_DrawZSpans`, `Turbulent8`), `d_surf.c` (`D_CacheSurface`, `D_SCAlloc`,
  `D_FlushCaches`), `r_surf.c` (`R_DrawSurface`, `R_BuildLightMap`).
- Constraints: `keep-software-renderer-look`, `perf-target-systems`,
  `feedback-no-new-cpp` memories.

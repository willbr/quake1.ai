# Threaded span fill (software rasterizer)

_Extracted from CLAUDE.md (reference detail; CLAUDE.md keeps a summary + pointer here)._

### Threaded span fill (software rasterizer)

The scene stays **100% software-rendered** (see the `keep-software-renderer-look`
constraint); rendering perf comes from threading + SIMD the CPU rasterizer, not
from GPU triangles. The dominant cost is the **span fill** (`D_DrawSurfaces`,
~92% of `R_ScanEdges`), which is now parallelized across a worker pool.
Design+results: `docs/superpowers/specs/2026-06-04-renderer-fill-threading-design.md`.

`D_DrawSurfaces` (`d_edge.c`) is split into a serial **resolve** pass and a
parallel **fill**:
- **Resolve (serial):** `D_SetupSurfaceFill` runs per-surface setup — mip/dither
  resolution + `D_CacheSurface` (the only allocator + dlit re-light, so the
  non-thread-safe surface-cache pool is only ever touched here), the bmodel
  transform, and `D_CalcGradients` — and **records** the gradient OUTPUTS +
  cacheblock into `surf_t.fill`/`rcache`/`rmip`/`rbucket`. Recording the *outputs*
  decouples the draw from the transform, so the fill is uniform for world and
  bmodel surfaces.
- **Fill (parallel):** `D_DrawSurfaceBody` just LOADS `s->fill` into the
  per-surface span-drawer globals and draws — no transform, no `D_CalcGradients`,
  no allocator. Those globals (`cacheblock`, `d_sdivz*`/`sadjust`/`bbextents`/…,
  the `r_turb_*` scratch, `miplevel`) are `__thread` (`d_vars.c`/`d_scan.c`/
  `d_edge.c`), so workers don't collide; the framebuffer/z-buffer base pointers
  stay shared (read-only base, disjoint writes).

**Correctness foundation:** the edge sweep resolves occlusion BEFORE fill, so
visible spans of different surfaces are **disjoint** in screen space → workers
write disjoint framebuffer + z pixels with no locks. Verified byte-identical
serial-vs-threaded.

**Pre-fork cache-eviction recovery** (`D_DrawSurfaces`, the second hazard the
resolve/fill split introduced): the resolve pass records each solid surface's
cacheblock *pointer* (`s->fill.cacheblock`), but a *later* surface's
`D_CacheSurface` in the same pass can **evict an earlier-resolved surface's
block** — the surfcache bump allocator's rover, advancing on each (re)build (dlit
surfaces rebuild every frame: muzzle flashes, rockets, torches), laps a
prior-frame cache-*hit* block sitting ahead of it and reuses the memory.
`r_cache_thrash` does **not** catch this (it only trips on a full intra-frame
*wrap*, not ordinary forward eviction), so the recorded pointer ends up aimed at
another surface's texels+lightmap → **wrong texture AND lightmap on random
surfaces** (the classic threaded-fill flicker bug, fixed 2026-06-07). The serial
draw self-heals via its per-surface `cachespots` re-check (`allow_resetup=true`);
the lock-free worker can't allocate, so before the fork we re-resolve any
surface whose `cachespots[rmip][rbucket] != s->rcache`, iterating to a fixpoint
(each re-resolve can evict another), and fall back to the serial path if it
won't settle. The validation scan is an O(surfaces) pointer compare — ~free in
the steady state; on e1m1 with the bot it fires on a few surfaces on hundreds of
frames yet always converges (never falls back to serial).

`sdlquake/engine/r_threadfill.c` is the pool (pure C / SDL3, like
`light_bake_thread.c`): persistent `min(cores,16)-1` workers parked on a
semaphore; `R_ThreadFill_Run(fn, begin, end)` publishes the range, releases
workers, runs the same atomic work-stealing grab-loop (`SDL_AddAtomicInt`) on
the caller, then waits one done-signal per worker — the barrier (SDL semaphore
signal/wait give the release/acquire fences). Dispatched from `D_DrawSurfaces`
for indices `[1, surface_p-surfaces)`.

Cvars: **`r_threads`** (default 1; **`0` = serial path**, the regression oracle —
also auto-serial on 1-core hosts and on cache-thrash frames, since the rare
thrash re-setup calls the allocator). **`r_threads_check`** (default 0, debug):
runs a serial coverage pass (`D_CheckSpansDisjoint`) that warns once if any pixel
is covered by two surfaces' spans — self-polices the disjoint-span invariant
against future sweep/transparency changes. Result on a 10-core Mac (e1m1): fill
3.14 ms → 0.75 ms (4.2×), `R_ScanEdges` 3.41 → 1.02 ms, frame 5.65 → 3.21 ms.
Phasing was Phase 0 (single-pass fill) → 1 (cache pre-resolve) → 2 (gradient
recording) → 3a (`__thread`) → 3b (fork), each bit-identical-verified. Next:
SIMD the gather-free inner loops (`D_DrawZSpans`, the lightmap×texture combine)
in 128-bit C intrinsics / simde for both ISAs (Deck x86-64, M4 aarch64).


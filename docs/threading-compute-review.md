# Threading & Compute-Shader Opportunities — Codebase Review

*2026-06-04. A subsystem-by-subsystem review of where CPU threading, SIMD, and GPU
compute shaders could help this SDL3/Zig WinQuake fork. Findings were gathered by 12
parallel subsystem surveys and the highest-value claims were adversarially verified
against the source.*

## Executive summary

This is a 1996 **software renderer** running at 60+ fps, so the honest framing is "shave
worst-frame spikes and unlock new capability," not "10× the frame rate." Three facts shape
everything:

1. **The renderer is single-threaded and global-heavy** (`d_*.c`/`r_*.c` are not
   re-entrant). Only the **palette-LUT graphics pipeline** is on GPU (`vid_sdl.c`); there
   are **zero compute shaders** anywhere — that's greenfield.
2. **The sim/game lives in `game.dll` and is main-thread-only by design** ("all
   game-state access stays on the main thread"). This rules out naive threading of physics
   and AI, and makes GPU-offloading the wind grid a trap (the data lives on the wrong side
   of the engine↔DLL ABI).
3. **The real frame-time pressure isn't where the "parallelize it!" instinct points.** The
   documented worst frames come from M8 fire/oil (an `O(patches×edicts)` scan and
   surfcache over-invalidation), not from the renderer or physics. The biggest *wins* are
   algorithmic prerequisites + a couple of already-written-but-disabled thread pools — not
   heroic SIMD.

The two pieces of **cross-cutting infrastructure** that unlock the most are (A) a tiny
`SDL_Thread` job/worker system generalizing the existing `light_bake_thread.c` pattern,
and (B) the **first `SDL_GPU` compute pipeline** wired into `vid_sdl.c` + `build_shaders.sh`.

## Ranked recommendations

| # | Opportunity | Kind | Win | Effort | Risk | Status |
|---|---|---|---|---|---|---|
| 1 | Re-enable multi-core **light baking** (`vendor/light`) | thread | High (iter) | S–M | Low | ✅ verified |
| 2 | Re-enable multi-core **vis** (`vendor/vis`) | thread | Med (iter) | S | Low–Med | ✅ verified |
| 3 | **O(1) edict-by-number** lookup (fire + AI loops) | algorithmic | Low–Med | Low | V.Low | strong |
| 4 | Spatial-bucket the **fire/oil `O(patches×edicts)`** scan | algorithmic→simd | High | Med | Low | strong |
| 5 | Spatial-bucket the **navmesh-bake `O(N²)`** phases | algorithmic | High (iter) | S–M | Low | strong |
| 6 | Skip the redundant **palette-id texture upload** | gpu-upload | Low | Low | Low | ✅ verified |
| 7 | **Frame-robust targeted lightmap invalidation** | algorithmic | Med–High | Med | Med | ✅ verified (reframed) |
| 8 | SIMD **`blocklights` accumulate** + IQM/alias vertex math | simd | Low–Med | Low | Low | strong |
| 9 | **First SDL_GPU compute pipeline** (enabling infra) | compute | — (unlock) | Med | Med | recipe in hand |
| 10 | **GPU bloom** on fullbright palette indices | compute | Med (capability) | Med | Med | strong |
| 11 | **Scanline-band threading** of the software span fill | thread | Med–High* | Large | Med–High | contingent |
| 12 | **GPU model pipeline** (alias/IQM as real triangles) | gpu-graphics | Med / High capability | High | Med–High | architectural |
| 13 | **Background/time-slice the nav bake** (stop the load hitch) | thread/algorithmic | Med (UX) | Med | Low–Med | strong |

\* #11's win is contingent on the span fill actually dominating worst frames — confirm with
a `profile` capture before committing the large effort.

## The two cross-cutting unlocks

### A. A tiny SDL_Thread job/worker system

`sdlquake/engine/editor/light_bake_thread.c` already proves the pattern in-repo:
snapshot inputs → `SDL_CreateThread` → worker writes thread-private buffers → atomic
`result_ready` flag → main thread consumes/swaps. Generalizing this into a small
`Job_Dispatch(fn, items, n)` + `Job_Wait()` pool (sized to `SDL_GetNumLogicalCPUCores()`)
directly enables #1, #2, #5, #13, the wind worker, and the renderer bands of #11 — all of
which are "read-only fan-out, serial commit" shaped. Low risk; it's already battle-tested
in one place.

### B. The first SDL_GPU compute pipeline

There is no compute shader anywhere today, but SDL3's compute API is fully present in the
vendored header and the device is already up in `vid_sdl.c`. The mechanical recipe
(verified against `SDL_gpu.h` and `build_shaders.sh`):

1. Author `shaders/<name>.comp.glsl` (`layout(local_size_x=8, local_size_y=8) in;`).
   **Compute binding sets differ from fragment:** set 0 = sampled + read-only storage,
   set 1 = read-write storage, set 2 = uniforms.
2. `build_shaders.sh` — add a `comp_stems` list; `glslangValidator -V -S comp`; for MSL
   drop `--flip-vert-y` (meaningless for compute); emit `<stem>_comp_spv`/`_msl` like the
   existing graphics stems. Add the `.comp.glsl` as a `build.zig` `addFileInput`.
3. Create with **`SDL_GPUComputePipelineCreateInfo`** (not `SDL_CreateGPUShader`) +
   `SDL_CreateGPUComputePipeline`; `threadcount_*` must match the shader's `local_size`.
4. Output target needs `SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE`.
5. Dispatch: end the upload copy-pass first, then `SDL_BeginGPUComputePass` →
   `SDL_BindGPUComputePipeline`/samplers → `SDL_PushGPUComputeUniformData` →
   `SDL_DispatchGPUCompute((w+7)/8,(h+7)/8,1)` → end pass → render pass samples the output.
   SDL inserts the read→write barriers automatically at pass boundaries; use `cycle=true`
   on transient RW bindings.

Once this exists, #10 (bloom), #13's GPU-AO variant, color-grading/dither (which can also
just live in the existing fragment shader), and a future GPU particle/smoke pass all become
incremental.

## Tier list

### Quick wins (small effort, real value, low risk)

- **#1 — Re-enable multi-core light baking.** `vendor/light/light_lib.c:53-62` stubs
  `RunThreadsOn` to call the work function *once* with `numthreads=1`, but
  `vendor/light/light.c:61-75` (`LightThread`) **already contains the id work-stealing
  per-face loop** (`i = bspfileface++; LightFace(i)`), and `GetFileSpace` (`light.c:45-54`)
  **already wraps the lightmap-output bump in LOCK/UNLOCK**. So re-enabling near-linear
  multicore baking is: real `SDL_Thread` pool + make `LOCK` a real `SDL_Mutex` (or atomic
  fetch-add on `bspfileface`). The bake already runs off the main render thread
  (`light_bake_thread.c`), so the editor boundary is untouched. Audit the `c_*` debug
  counters / `dirt_*` scratch for shared writes before flipping it on. Biggest payoff on
  AO/dirt bakes (minutes today). **Verified.**

- **#2 — Re-enable multi-core vis.** `vendor/vis/vis.c:490-529` gates the pthread portal-flow
  fan-out behind `#ifdef __alpha` (never defined) → falls back to `LeafThread(0)`. The
  work-dispenser `GetNextPortal` (`vis.c:303-330`) is portable and LOCK-guarded. Swap the
  `__alpha` block for `SDL_Thread` workers and back the LOCK with `SDL_Mutex`. Caveat:
  `PortalFlow` opportunistically reads other portals' `stat_done`/`visbits` to tighten
  bounds — a benign race, but preserve the `none→working→done` discipline and
  least-complex-first ordering. vis is the slowest stage of a full `editor_compile_export`.
  **Verified.**

- **#3 — O(1) edict-by-number lookup.** `sim_fire.c:86` `fire_find_edict` walks the whole
  edict list to find an edict *by its own number* (the engine has `_EDICT_NUM(n)` for O(1));
  `Sim_AI_Frame` (`sim_ai.c:525`) does the same per-brain, making it O(brains×edicts). Add
  an `edict_t *ED_FromNum(int)` to `engine_api_t` (one-line wrapper, needs a
  `GAME_API_VERSION` bump) and replace the scans. Cheapest real win; do it before any
  SIMD/threading since it shrinks the loops everything else builds on.

- **#6 — Skip the redundant palette-id texture upload.** `vid_sdl.c:1286` memsets
  `vid_palette_id` to 0 every frame; the *only* non-zero writer is the Doom/Wolf3D weapon
  blit (`r_sprite.c:349`). Yet `gpu_render_frame` copies+uploads that whole R8 screen every
  frame (`vid_sdl.c:536-562`). Dirty-flag it (set in the sprite blit, force-upload after
  `gpu_create_frame_textures` on resize) and skip the second memcpy+upload on the common
  no-overlay-gun frame. Modest (it's one of two textures, shader still samples it), but free.
  **Verified.**

- **#8 — Safe leaf-math SIMD.** `r_surf.c` `blocklights[]` accumulate (`blocklights[i] +=
  lightmap[i]*scale`) is a flat `int += byte*scale` — trivial SSE/NEON, no gather, zero
  re-entrancy risk. Same for IQM per-joint skin matrices (`R_ConcatTransforms`/`IQM_QuatMat`
  in `r_alias.c`) and the alias vertex transform+project loop (`r_alias.c:498`). These are
  leaf functions on caller-owned buffers, so they're the only "just do it" SIMD targets —
  but the frame-time win is small until IQM actors/monsters proliferate.

### The algorithmic prerequisites that are the *actual* perf wins

- **#4 — Spatial-bucket the fire/oil scan.** `sim_fire.c:492` `oil_frame` runs, for each of
  ≤256 oil patches, a full `ED_Next` walk over ~600 edicts doing squared-distance tests
  (`:590`) — the documented `O(patches×edicts)` follow-up, ~3.4 ms worst Fire scope. Build a
  coarse uniform spatial hash of `takedamage` edicts once per fire tick; each patch queries
  only the ≤9 overlapping cells. This collapses the dominant term and is a behaviour-
  preserving refactor of a *read* phase (all mutations stay serial/main-thread). SIMD the
  residual distance tests only *after* the grid lands; threading isn't worth it at these
  entity counts.

- **#5 — Spatial-bucket the navmesh bake.** `sim_nav.c:1144-1207` (Phase 3.5 jump/drop
  synthesis) is an `O(N²)` pair scan (N≈6k on e1m1 → ~36M iterations), repeated in the
  Phase 4.5 floor-link scans. Reuse the `grid_t` xy-hash **already built during the flood**
  (`sim_nav.c:185-252`) to query only nearby nodes → O(N·k). Largest single bake-time
  reduction available, pure DLL-side, low risk. Land this *before* any bake threading — it
  may shrink the bake enough that the load hitch stops mattering.

- **#7 — Frame-robust targeted lightmap invalidation.** `r_livelight.c:220` `Lightmap_AddDelta`
  calls the **global** `D_FlushCaches()` (nulls *every* surfcache owner) on each additive
  light delta — and M8 fire/oil fires these many times/second, forcing a full-screen
  surfcache rebuild each time (a big contributor to the 10.9 ms worst frame). **You cannot
  simply delete the flush** (adversarially verified): gameplay deltas run in the host frame
  *before* `R_RenderView` bumps `r_framecount`, so the per-surface `dlightframe` bump lands
  one frame behind and `D_CacheSurface` (`d_surf.c:305`) returns the *stale* cache — the
  exact bug documented at `light_bake_thread.c:434-446`. The correct fix is a **per-surface
  generation-counter** invalidation (mirror the existing `stain_gen`/`dither_gen` at
  `d_surf.c:304/350`) that's robust to the frame offset, replacing the global flush for the
  additive-paint path while keeping it for the baseline-restore path. Medium effort, real
  worst-frame win.

### Big swings (large effort, large payoff or capability unlock)

- **#10 — GPU bloom on fullbright palette indices.** Quake palette 224–255 are the
  emissive "fullbrights"; a bright-pass reads the raw index already uploaded in `gpu_fb_tex`,
  then a separable Gaussian blur (the textbook compute use) + composite. This is the best
  *first real* payoff for the #9 compute pipeline — capability and looks, sub-ms at the
  small framebuffer sizes (≈960×600 typical). Color-grading and ordered dithering, by
  contrast, are near-free *fragment* additions to the existing `palette.frag.glsl` UBO and
  need no compute at all.

- **#11 — Scanline-band threading of the software span fill.** The single biggest *frame-time*
  swing if pursued. Spans are **row-disjoint by construction** (each `espan_t` carries its
  scanline `v`), so `d_viewbuffer` and `d_pzbuffer` partition cleanly across N worker bands
  with **no atomics and no z-race**. The cost: phase-split the pipeline (remove the
  mid-sweep flush at `r_edge.c:725`, enlarge `MAXSPANS` so all spans buffer; single-threaded
  surface-cache build; then parallel band-fill that only *reads* `cacheblock`), and hoist
  ~20 file-scope draw-state globals (`d_sdivzstepu/v`, `sadjust/tadjust/bbextents`,
  `cacheblock/cachewidth`, the `r_drawsurf` struct, water/sky state…) into a per-band
  context. Amdahl-limited by the serial span-gen (`R_ScanEdges` exploits vertical edge
  coherence and **does not parallelize**) and cache-build. Mirror `light_bake_thread.c`.
  **Confirm fill dominates worst frames with a `profile` capture first.**

- **#12 — GPU model pipeline (alias/IQM as real triangles).** Render models as real GPU
  triangles into an index/RGBA target, composited over the software scene. Removes the
  d_polyse re-entrancy problem for models, gives perspective-correct texturing and **real
  triangle clipping for free** (the IQM path currently *skips* clipped triangles,
  `r_alias.c:1286`), and scales to many actors — the right long-term architecture for the
  skeletal-actor work. The make-or-break detail is depth-compositing GPU triangles against
  the reconstructed software z-buffer (upload `d_pzbuffer` as a depth texture; get the `zi`
  scaling right or models z-fight the world). Pursue for *better-looking, more numerous*
  models, not raw current-frame speed.

- **#13 — Stop the nav-bake load hitch.** A fresh-map first load synchronously runs
  `bake_floodfill` on the main thread (`sim_nav.c:561`, fired from `Sim_Nav_LevelInit`) — the
  dominant load-time stall. True worker-threading is **XL/high-risk** because, unlike the
  light bake, the nav bake drives the **live edict + server-physics world** (`SV_WalkMove`/
  `SV_DropToFloor` → `SV_LinkEdict` mutates `sv.areanodes`, `pr_global_struct`, and it
  temporarily zeroes every entity's `.solid`) — it is *not* read-only static BSP. The **safe**
  answer is to **time-slice the bake across frames** (bounded BFS/pair budget per
  `Sim_Frame`, persist state in statics, monsters fall back to direct-chase until ready) — no
  data hazard, just amortized. Combine with #5 (which shrinks the total work).

### Traps — looks parallelizable, isn't worth it

- **GPU compute for the wind sim.** Textbook grid-stencil shape, but the grid is ≤64³
  (realistically thousands of cells), ticks at **10 Hz**, and is already inside a small
  `SIM_PERF("Wind")` scope. The data lives in `game.dll` and the answer is consumed
  CPU-side (AI LOS), so a GPU offload needs a per-tick upload→dispatch→**download+fence**
  round-trip across the engine↔DLL ABI — the classic "PCIe costs more than the compute"
  loss for a small grid. If you want it off the server frame, a **double-buffered
  worker thread** (the grid is already double-buffered and DLL-private) is the right tool,
  and the +1-tick latency is invisible. Revisit GPU only with diffusion/pressure-solve or a
  much larger grid.

- **Threading per-edict server physics.** Irreducibly serial: `pr_global_struct->self/other`
  is a single global VM context; `SV_PushMove` moves *other* edicts and flips their `.solid`;
  `SV_Impact`/`SV_TouchLinks` fire game callbacks that spawn/remove arbitrary edicts; the
  `sv.areanodes` lists are relinked mid-loop. And it's sub-1 ms anyway. Reject.

- **GPU compute / threading for collision & AI traces.** Latency-bound (results consumed on
  the next line), low count, pointer-chasing BSP descents. The trace core is also
  non-re-entrant: `SV_HullForBox` **writes** file-static `box_planes`/`box_hull`
  (`world.c:57-116`) and the DLL trace ABI returns through a shared `game_globals.trace_*`
  block (`hotreload.c:439-451`). *De-globalizing the trace core (caller-owned hull,
  return-by-value) is a worthwhile cleanliness/correctness refactor* and the only thing that
  would ever enable packet/threaded traces — but don't thread on spec; the volumes don't
  justify it.

- **Worker-thread particle integration.** The integrate loop is fused with draw (writes the
  software framebuffer + z) and calls the non-re-entrant `R_TraceParticle`, the DLL
  `Wind_SampleVelocity`, and spawns into the shared free-list on liquid splashes. The
  valuable prerequisite is decoupling integrate from draw; threading on top is premature.

- **SIMD audio mixing, A*, BSP byte-swaps.** No measurable target: mixing is <0.1 ms at
  ≤24 channels (and already off the device via the ring buffer — the cleanest SIMD loop is
  the saturating int32→int16 transfer `snd_mix.c:35-58`, for zero net frame win); A*'s cost
  is inside engine trace calls, not DLL vector math; BSP `LittleLong`/`LittleShort` are
  no-op function pointers on the little-endian targets, and brush mips ship precomputed in
  the BSP (no downsample to vectorize).

- **Piecemeal GPU world-fill.** Only makes sense as part of a full GPU-renderer rewrite —
  splitting world-fill onto the GPU while alias/sprites/particles/viewmodel still draw into
  the same CPU framebuffer and depend on the CPU z-buffer creates a mid-frame CPU↔GPU sync
  that costs more than it saves.

## Suggested sequencing

1. **Build the job/worker system (A)** by generalizing `light_bake_thread.c`. Land #1 and
   #2 on it immediately (pure iteration-time wins, already-written parallel loops).
2. **Algorithmic prerequisites:** #3, #5, #4 (in that order — cheapest first; each shrinks a
   documented hot loop). #7 (frame-robust invalidation) for the fire worst-frame.
3. **Stand up the first compute pipeline (B)** and ship #6 + #10; add color-grade/dither to
   the fragment shader while you're there.
4. **Only then** evaluate #11 (renderer band threading) and #12 (GPU models) against a real
   `profile` capture — these are large, and whether they pay off depends on numbers the
   instrumentation can give you before you commit.

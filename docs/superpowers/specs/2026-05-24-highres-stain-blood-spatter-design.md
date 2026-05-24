---
date: 2026-05-24
status: draft
owners: wjbr
---

# High-resolution stain map + blood-particle spatter

## Motivation

Two related problems with the current decal system:

1. **Blood particles don't mark surfaces.** `pt_blood` droplets from `R_BloodSpray` already stick to walls and floors via `PARTFL_STICK_ON_HIT` (`r_part.c:1893-1917`), and slide down walls via the existing slide tick. They look like dots while they live, then disappear when their colour ramp ages out. They leave no permanent trace in the decal map.
2. **The decal map is too coarse for fine detail.** The stain payload is `int16[smax×tmax×3]` at the lightmap luxel grid — **1 cell per 16 game units**. Bullet holes are chunky 16-unit squares; blood splats are 48-unit blobs; sprayed droplet dots collapse onto the same coarse grid as everything else, so the spray reads as overlapping pool dabs rather than discrete spatter.

The goal is to (a) hook stuck blood particles into the decal painter so each one leaves a permanent dot, and (b) raise the stain resolution to 4-unit cells so the new dots are visibly distinct and existing decals (bullet, scorch, splat, drip, pool) gain proper detail too.

## Architectural fit

The stain → render path is already well-factored for this change:

- **Storage** is a per-surface `stain_t` allocated from a fixed pool (`r_stain_slots`, LRU-evicted on overflow). `stain_t::rgb` is the dense delta buffer.
- **Painting** happens via two entry points in `r_decals.c`: `Stain_PaintKernel` (luxel-indexed, used by tests + drips) and `Stain_PaintKernel_World` (world-space center + UV basis, walks coplanar BSP faces).
- **Compositing** happens **after** the lightmap pass, in `R_OverlayStain` (`r_surf.c:425`). That function bilinear-samples the stain rgb across the 4 corner cells per surface-cache pixel, scales by `r_decals_intensity`, and applies an RGB delta to the rendered palette index. The lightmap composite (`R_BuildLightMap`) is untouched by stains.
- **Cache invalidation** uses `surf->stain->generation`: when it doesn't match the surface cache's stored `stain_gen`, the cache rebuilds.

Because compositing is a separable post-pass that already bilinear-samples from the stain grid to the surface-cache pixel grid, raising the stain resolution requires changing the *grid metric* (cells per game unit) and the *sampling shift*, not the rasterizer or the cache invalidation. The kernel definitions need to be re-tuned to the finer grid.

## Design

### 1. Stain grid: 4-unit cells

Replace the existing 16-unit luxel grid with a 4-unit cell grid: **4× linear resolution, 16× cell count per surface**. The existing `STAIN_MAX_LUXELS_DIM = 18` cap (matching `blocklights[18*18]` in `r_surf.c`) maps to a 72×72 cell cap.

In code:

- New constant `STAIN_CELL_SHIFT = 2` (game-units per cell = `1 << STAIN_CELL_SHIFT = 4`).
- `STAIN_MAX_CELLS_DIM = 72` (= `STAIN_MAX_LUXELS_DIM * 4`; covers a max-extent surface at 4-unit cells).
- `STAIN_PAYLOAD_INT16 = 72 * 72 * 3 = 15552 int16 ≈ 31 KB per slot`.
- At the existing `r_decals_max` default of 512 → ~16 MB pool. The existing 4096 hard cap stays — at 4096 slots that's ~127 MB, only relevant if a user explicitly raises `r_decals_max`.

`smax` / `tmax` in `stain_t` now mean **cell counts**, not luxel counts. Per-surface they're computed as:

```c
smax_cells = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
tmax_cells = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;
```

(Currently the shift is hardcoded as `>> 4`. The new constant replaces it.)

### 2. Stain → cell projection

The world-space → cell math in `Stain_AddCell` and `R_DecalsFrame` (bloodpool) is currently:

```c
lu = ((int)floor(u) - surf->texturemins[0]) >> 4;
```

becomes:

```c
lu = ((int)floor(u) - surf->texturemins[0]) >> STAIN_CELL_SHIFT;
```

Identical pattern in `R_DecalsTest_f`, `R_DecalsTestGrid_f`, blood-pool growth, blood-drip growth.

The blood-drip per-frame step in `R_DecalsFrame` currently hardcodes a 16-game-unit luxel step:

```c
int from_luxel = (int)(bd->length_painted / 16.0f);
int to_luxel   = (int)(drip_target          / 16.0f);
```

becomes `/ 4.0f` (or `/ (1 << STAIN_CELL_SHIFT)`). The drip side-offset (currently `dx_off = (float)si * 16.0f`) likewise scales to 4. The drip will now paint 4× as many cells along its length, so the per-cell delta needs to be scaled by `1/4` to preserve current intensity — or kept the same to make drips visually thicker. Default: scale by `1/4` to preserve appearance, with a follow-up cvar `r_decals_blooddrip_intensity` if we want to tune later.

### 3. `R_OverlayStain` sampling

Current code derives the shift from the lightmap luxel size:

```c
mip   = r_drawsurf.surfmip;
shift = 4 - mip;        // surface-pixel-per-luxel exponent
if (shift < 1) shift = 1;
mask  = (1 << shift) - 1;
```

This produces `shift = 4 - mip` because a luxel spans 16 game units = 16 surface pixels at mip 0. With 4-unit cells, the cell spans 4 surface pixels at mip 0:

```c
shift = 2 - mip;        // surface-pixel-per-cell exponent
if (shift < 1) shift = 1;
```

The bilinear interpolation logic, the zero-skip fast path, and the palette quantisation are otherwise unchanged.

**Performance.** The fast path "skip pixels whose 4 corner cells are all zero" is the dominant code path for sparse stains. With 4× resolution, the cells are smaller, so the *region* of a surface where the fast path fails is roughly the same — the painter touches the same physical area, just with finer grain. Net per-frame composite cost is approximately unchanged.

### 4. Kernel redesign

Existing kernels are luxel-sized:

| Decal | Current kernel | Current footprint |
|---|---|---|
| BULLET, SPIKE | K1x1_solid (1×1) | 16×16 game units |
| BLOOD_SPLAT | K3x3 (3×3) | 48×48 game units |
| SCORCH | K5x5 (5×5) | 80×80 game units |
| LIGHTNING | K3x3 (3×3) | 48×48 game units |

At 4-unit cells we get the choice to either:
- **Preserve footprint** (kernel size in *cells* = 4× the luxel size; smoother gradient, same on-screen size).
- **Shrink footprint** (use the higher resolution to draw smaller marks: a 4-unit-wide bullet hole, a 16-unit splat).

We do both, tuned per decal type:

| Decal | New kernel | New footprint | Reasoning |
|---|---|---|---|
| BULLET | 2×2 solid | 8×8 game units | Real bullet-hole scale |
| SPIKE | 1×1 solid | 4×4 game units | Spike is smaller than bullet |
| BLOOD_SPLAT | new K7x7 Gaussian | 28×28 game units | Same physical size as today's 3×3, but smooth falloff |
| SCORCH | new K13x13 Gaussian | 52×52 game units | Slightly larger than today's 80 unit chunky; finer falloff lets it grow without looking bad |
| LIGHTNING | new K7x7 Gaussian | 28×28 game units | Same as splat |
| BLOOD_SPATTER (new) | 1×1 solid | 4×4 game units | One stuck droplet = one fine dot |

The Gaussian kernels are pre-computed integer arrays with corresponding `knorm` divisors, sized so `(centre_delta * weight) / knorm` ≈ centre delta at the centre cell. The existing K3x3 norm = 16 and K5x5 norm = 256 follow the same convention.

Centre-delta colours (`dr`, `dg`, `db` in `decal_kernels[]`) are unchanged — same dark-red blood, same near-black scorch — only the kernel shape and norm change. A new `DECAL_BLOOD_SPATTER` entry is added.

### 5. Blood-particle stick hook

In `r_part.c`, the `PARTFL_STICK_ON_HIT` branch (`r_part.c:1897-1917`) is where a particle transitions to stuck state. Add, immediately after the stuck flag is set and before the wall-slide setup:

```c
if (p->type == pt_blood) {
    R_SpawnBloodSpatter (p->org, n);
}
```

`R_SpawnBloodSpatter(vec3_t pos, vec3_t normal)` is a new public function in `r_decals.c` with the shape of a thin wrapper around the existing infrastructure:

1. Bail if `!r_decals.value` or a new `r_decals_blood_spatter.value` cvar is 0, or `!sv.active`.
2. Locate the target surface via `R_PointOnSurface_World(pos, normal, 4.0f)`.
3. Paint a 1×1 spatter kernel via `Stain_PaintKernel_World(pos, surf, dk->dr, dk->dg, dk->db, dk->k, dk->ksize, dk->knorm)` where `dk = &decal_kernels[DECAL_BLOOD_SPATTER]`.

This reuses the coplanar-face walk in `Stain_PaintKernel_World`, so a droplet that lands near a BSP face boundary still paints correctly on both sides.

`R_SpawnBloodSpatter` is added to `r_local.h` alongside the other `R_Spawn*` prototypes.

#### Wall-slide drag trail (M2, follow-up)

The first pass paints **one** spatter when the particle first sticks. Per the user's "every stuck droplet leaves a dot" answer, this is the baseline. A later iteration may also paint along the wall-slide track (`r_part.c:2101-2114`) so the droplet drags a tiny smear downward as it slides. Out of scope for this spec.

### 6. New cvars

| Cvar | Default | Persistent | Purpose |
|---|---|---|---|
| `r_decals_blood_spatter` | `"1"` | yes | Master toggle for the new particle-stick spatter path |

No other new cvars. `r_decals_max`, `r_decals_intensity`, the existing pool/drip cvars all keep their current meaning; intensity still works at the new resolution because `R_OverlayStain` applies it uniformly.

### 7. Density and persistence (per design questions)

- **Density:** every stuck blood droplet paints. No probabilistic sub-sample. `R_BloodSpray` typically spawns 20-40 droplets per hit; not all stick (some fall to the floor, some die in mid-air to ramp-out before contact). The actual on-wall dot count per spray is typically 5-15.
- **Persistence:** permanent until map end / LRU eviction. Matches every existing decal type. No per-spatter fade — the LRU pool eviction is the only mechanism that removes stains, and that's a global one.

## Memory and performance budget

- **Stain pool memory:** 16 MB at default `r_decals_max=512` (was ~1 MB). Allocated once per map load from the hunk.
- **Per-paint cost:** unchanged for 1×1 kernels (bullet, spike, spatter). For Gaussian kernels (splat, scorch, lightning), the cell count rises (e.g. 7×7 = 49 cells vs. today's 3×3 = 9), but this fires only on hit events — not per frame.
- **Per-frame composite cost (`R_OverlayStain`):** approximately unchanged due to the zero-skip fast path. Worst case is a surface where every cell is stained, in which case cost scales linearly with surface pixel count (independent of stain resolution).
- **Cache invalidation:** unchanged — `stain_gen` already triggers surface-cache rebuild when stain changes; rebuild cost is proportional to surface pixels, not stain cell count.

## Out of scope

- Wall-slide drag trail (see §5).
- Cvar to control spatter density / sub-sample (future tuning if every-droplet is too dense in practice).
- Independent fade for spatter dots.
- Higher cap than 4× (would push into texture-pixel territory; structurally feasible but not asked for).
- Adjusting `r_decals_max` default downward to keep memory at the old ~1 MB target.

## Risks

1. **Pool eviction under combat.** A 16 MB pool at 512 slots — same slot count as today — should hold the same number of distinct stained surfaces. Higher *intensity* of decals per surface doesn't reduce slot count. Risk is low.
2. **Drip step rescaling.** Bumping the per-cell delta scale by `1/4` to preserve drip appearance is a guess. A short side-by-side check on `id1/maps/e1m1` (a few zombie spawns, watch a wall drip) is the verification.
3. **Kernel shape redesign.** The new Gaussian kernels need to be hand-tuned and visually validated — there's no automated test for "looks right." Smoke test rig (`m7_skeleton` + screenshot) plus side-by-side on `e1m1` zombie blood is the verification path.
4. **`STAIN_MAX_CELLS_DIM = 72` cap.** If a surface's lightmap extent exceeds 18 luxels in either dimension (i.e. the engine's existing `blocklights[18*18]` cap was already overflowing), the alloc bail keeps us safe — same defensive code as today.

## Verification plan

1. **Build:** `zig build` succeeds.
2. **Existing decals still render.** `r_decals_test`, `r_decals_test_grid`, `r_decals_test_pool` produce visually sensible output on a wall in `m7_skeleton` (compare against pre-change screenshots).
3. **Bullet holes are visibly smaller.** Fire a few shells at a wall in `e1m1`. Holes should now be ~8-unit squares, not 16-unit chunks.
4. **Blood splats are smoother.** Kill a zombie next to a wall. Splat should show smooth Gaussian falloff instead of the current 3×3 cross pattern.
5. **Blood particle spatter.** Kill a zombie in `e1m1` start area; the wall behind should pick up several small 4-unit blood dots, distinct from the central splat. Disable `r_decals_blood_spatter` to confirm the dots vanish and only the splat remains.
6. **Memory.** `mem_summary` console command confirms the stain pool size is ~16 MB.

## Implementation order (informs the plan)

1. Add `STAIN_CELL_SHIFT` + `STAIN_MAX_CELLS_DIM` constants, bump payload, adjust `>> 4` → `>> STAIN_CELL_SHIFT` in r_decals.c.
2. Adjust `R_OverlayStain` shift derivation.
3. Rescale blood-pool grid math (`gx = dx * 16.0f` → `* 4.0f`).
4. Rescale blood-drip step (`/ 16.0f` → `/ 4.0f`, side offset `* 16.0f` → `* 4.0f`, delta scale `* 0.25f`).
5. Add new Gaussian kernels (K2x2_solid for bullet, K7x7 for splat/lightning, K13x13 for scorch), rewrite `decal_kernels[]`.
6. Add `DECAL_BLOOD_SPATTER` enum, kernel entry, `r_decals_blood_spatter` cvar, `R_SpawnBloodSpatter` public function.
7. Hook the particle-stick branch in `r_part.c` to call `R_SpawnBloodSpatter` for `pt_blood` particles.
8. Smoke-test on `m7_skeleton` and `e1m1`, capture before/after screenshots.

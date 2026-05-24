# Mip-level dither blend (software renderer)

## Problem

Quake's software renderer picks one mip per surface, then snaps between mips as the camera moves. The pop is visible — a wall changes texel density in a single frame. The existing 15% hysteresis at `d_edge.c:333` prevents flutter but doesn't soften the transition itself.

## Goal

Smooth the transition by dithering between mip N and mip N+1 inside a band around each `d_scalemip[]` threshold. Constraint from the user: keep the span rasterizer's hot loop unchanged (no extra texel fetch per pixel, no per-pixel branches). Memory is plentiful.

## Approach

**Pre-bake the dithered blend into the surface cache.** When a surface sits in the transition band between two mips, we generate a cached surface at mip-N resolution where each texel is *either* mip N's value or mip N+1's upsampled value, chosen by a Bayer ordered-dither pattern in texture space. The fraction of mip-N-vs-mip-N+1 texels is set by the surface's position in the band.

The span rasterizer (`D_DrawSpans8` in `d_scan.c`) and `R_DrawSurface` (`r_surf.c`) are unchanged — they keep doing one texel fetch per pixel out of a single cacheblock. All new work lives in `d_edge.c` (mip selection) and `d_surf.c` / `r_surf.c` (cache build).

The dither pattern is sampled in texture coordinates, so it stays locked to the wall as the camera moves. For static world geometry this looks like the wall has subtle noise in the blend zone; for moving bmodels (doors, platforms) the noise moves with the model. The alternative — screen-space dither — would require a fork of `D_DrawSpans8` that fetches from two cacheblocks per pixel, violating the perf constraint.

## Components

### Mip selection (`d_edge.c`)

Replaces the current logic at `d_edge.c:323–346`. There are two boundaries that can put a surface in a dither band: the one below `raw_mip` (between `raw_mip` and `raw_mip+1`, at `d_scalemip[raw_mip]`) and the one above (between `raw_mip-1` and `raw_mip`, at `d_scalemip[raw_mip-1]`). At most one applies — bands are ±30% and thresholds are ~2× apart in mipscale, so they don't overlap.

```
mipscale = nearzi * scale_for_mip * mipadjust
raw_mip  = D_MipLevelForScale(mipscale)

miplevel = raw_mip      // default: no dither
bucket   = 0

if r_mipdither is on:
    // --- Lower boundary: dither between raw_mip (sharper) and raw_mip+1 (coarser).
    // Requires raw_mip+1 to exist as a valid mip.
    if raw_mip + 1 <= MIPLEVELS - 1:
        t_lo = d_scalemip[raw_mip]
        if mipscale < t_lo * (1 + r_mipdither_band)
           AND mipscale > t_lo * (1 - r_mipdither_band):
            // t = 0 at upper edge (pure raw_mip), t = 1 at lower edge (pure raw_mip+1)
            t = (t_lo * (1 + r_mipdither_band) - mipscale)
              / (t_lo * 2 * r_mipdither_band)
            miplevel = raw_mip          // cache at the sharper mip's resolution
            bucket   = quantize(t)

    // --- Upper boundary: dither between raw_mip-1 (sharper) and raw_mip (coarser).
    // Requires raw_mip-1 to exist AND be allowed by d_mipcap floor.
    elif raw_mip - 1 >= d_minmip AND raw_mip >= 1:
        t_hi = d_scalemip[raw_mip - 1]
        if mipscale < t_hi * (1 + r_mipdither_band)
           AND mipscale > t_hi * (1 - r_mipdither_band):
            t = (t_hi * (1 + r_mipdither_band) - mipscale)
              / (t_hi * 2 * r_mipdither_band)
            miplevel = raw_mip - 1      // cache at the sharper mip's resolution
            bucket   = quantize(t)

where quantize(t) = clamp(round(t * (NUM_DITHER_BUCKETS - 1)), 0, NUM_DITHER_BUCKETS - 1)
```

The two boundary checks are written as `if`/`elif` because the bands cannot overlap — if the surface is inside the lower band of `raw_mip`, it's safely far from the upper band, and vice versa. `miplevel` always refers to the *sharper* of the two mips being blended; the cache entry is built at that mip's resolution.

**Bucket hysteresis.** Standing nearly stationary at a bucket seam would otherwise rebuild the cache every frame. Store the previous `(miplevel, bucket)` pair in `msurface_t` (re-using or extending `last_miplevel`). Hold the previous bucket if `mipscale` stays within ±25% of the bucket's slot width. This is the same hysteresis idea already used at the mip-level boundary, scoped down to bucket boundaries.

### Cache layout (`model.h`, `d_surf.c`)

```c
#define NUM_DITHER_BUCKETS 8       // tied to r_mipdither_buckets at compile time
struct cache_user_s *cachespots[MIPLEVELS][NUM_DITHER_BUCKETS];
```

- `cachespots[mip][0]` = pure single-mip cache, identical bit-for-bit to today's `cachespots[mip]`.
- `cachespots[mip][1..NUM_DITHER_BUCKETS-1]` = dithered blends between `mip` and `mip+1`.
- `cachespots[MIPLEVELS-1][b>0]` is illegal and never allocated.

`D_CacheSurface` gets a `bucket` parameter and indexes `surface->cachespots[miplevel][bucket]`. `surfcache_t.owner` points at `&surface->cachespots[miplevel][bucket]` so LRU eviction in `D_SCAlloc` correctly nulls out the right cell.

Pointer-table memory cost: 4 × 8 − 7 = 25 pointers per surface (vs 4 today). At ~8 bytes each on x64 and ~1500 surfaces in a busy map, that's ~250 KB additional pointer overhead. The dithered cache *entries themselves* live in the existing surface-cache arena (`D_SCAlloc`); they consume the same kind of slot a single-mip entry would. Walking through a transition zone fills 3–4 bucket entries before they age out via the existing LRU. No new arena needed; if pressure becomes visible, bump `D_SurfaceCacheForRes`.

### Dithered cache build (`r_surf.c`)

`R_DrawSurface` currently picks a per-mip block writer from `surfmiptable[]` (and `surfmiptable_rgb[]` for stained surfaces) and feeds it texel data via `r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip]`. The block writers read `r_source[s + t * texwidth]` and apply the lightmap to produce the lit cache entry.

**For bucket > 0, substitute `r_source` with a pre-blended scratch buffer**; the block writers (mono *and* RGB) stay unchanged. This avoids forking 6 hot block-writer variants.

Scratch fill, run inside `R_DrawSurface` once per cache-miss build when `bucket > 0`:

```
static byte r_mipdither_scratch[256 * 256];   // file-scope; largest possible mip-0 texture

src_N      = (byte *)mt + mt->offsets[N];        // sharper mip's raw texels
src_Nplus1 = (byte *)mt + mt->offsets[N + 1];    // coarser mip's raw texels
w_N        = mt->width  >> N
h_N        = mt->height >> N
w_Nplus1   = mt->width  >> (N + 1)
threshold  = (bucket * 16) / NUM_DITHER_BUCKETS  // bucket 1→2, 7→14

for t in [0, h_N):
    for s in [0, w_N):
        b = r_bayer4x4[((t & 3) << 2) | (s & 3)]
        if b < threshold:
            r_mipdither_scratch[t * w_N + s] = src_Nplus1[(t >> 1) * w_Nplus1 + (s >> 1)]
        else:
            r_mipdither_scratch[t * w_N + s] = src_N[t * w_N + s]

r_source = r_mipdither_scratch
```

After this substitution, the existing `surfmiptable[N]` or `surfmiptable_rgb[N]` runs unchanged and applies the lightmap to the dithered palette indices. Output looks like mip-N-resolution texels where bucket-1 has a few mip-N+1 texels scattered through and bucket-7 is mostly mip-N+1 texels upsampled to mip-N density.

`r_bayer4x4` is already defined at `r_surf.c:61` (16-byte const array, row-major, identical Bayer 4×4 pattern). Reuse it — no new table needed.

The substitution must come *after* the existing `r_lightmap` and `r_drawflat == 2` overrides (which also stomp `r_source`) so that those debug modes still work correctly when dither is enabled. If `r_lightmap` or `r_drawflat == 2` is active, skip the dither fill — the override texture is what should display.

### Cvars (`d_init.c`)

- `r_mipdither` (default `"1"`) — master enable. `0` skips the dither path entirely; `cachespots[mip][0]` is the only slot used and behavior is bit-identical to today.
- `r_mipdither_band` (default `"0.3"`) — half-width of the transition band around each `d_scalemip[]` threshold, expressed as fraction of the threshold value.
- `r_mipdither_buckets` is fixed at compile time (`NUM_DITHER_BUCKETS = 8`) to keep the cachespots table dimensions static. Live-tunable via cvar is not worth the code complexity.

### Invalidation (`d_surf.c:286–291` and friends)

All existing validity checks (`texture`, `lightadj[0..3]`, `stain_gen`, `dlight`) apply per `(mip, bucket)` cache slot — no logic change, the loop just iterates one more dimension when needed.

Add a single global `r_mipdither_generation` counter, incremented when `r_mipdither` or `r_mipdither_band` changes. Each cache entry stores its generation; mismatched entries are treated stale and rebuild on next access. Avoids walking the surface list on cvar change.

### Edge cases

- **Mip 3 (coarsest) or `d_mipcap`-blocked boundaries:** no mip N+1 available → force bucket 0.
- **Dlights:** any surface with `dlight` set this frame forces bucket 0 (skip dither). Rationale: dlit surfaces already rebuild every frame and the bright/colored light masks any mip-step pop anyway. Saves the per-frame Bayer cost on the cases that would pay it most.
- **Animated textures:** each animation frame caches independently per `(mip, bucket)`. Slightly more churn during animation. No code change required.
- **Stains:** existing `stain_gen` invalidation rebuilds every bucket per surface correctly. No code change.
- **Sky / water / sprites:** bypass the surface-mip cache entirely. Out of scope.
- **Save / load:** `cachespots` is runtime-only and gets cleared at level load via existing `D_FlushCaches`. Adding the bucket dimension changes nothing here.

## Data flow per frame

```
R_RenderView
  └─ R_DrawSurfaceList (d_edge.c)
       └─ for each visible surface:
            mipscale = ...                          [d_edge.c, unchanged math]
            (miplevel, bucket) = pick_mip_and_bucket(mipscale, prev)   [NEW]
            pcurrentcache = D_CacheSurface(face, miplevel, bucket)     [SIGNATURE CHANGE]
                 └─ if cached + valid: return                          [d_surf.c]
                 └─ else: R_DrawSurface() builds it                    [r_surf.c]
                       └─ chooses surfmiptable_dither[N] if bucket>0   [NEW]
                       └─ else surfmiptable[N] as today
            d_drawspans(spans)                      [d_scan.c, UNCHANGED]
```

The rasterizer hot loop touches one cacheblock per pixel, exactly as today. The only added per-frame work is selecting `bucket` (a few comparisons) and, on cache miss, building a dithered cache entry (~1.5–2× the texel work of a normal build). Steady-state perf impact in the rasterizer is zero; cache-build CPU climbs slightly when the camera sweeps across mip thresholds.

## Failure modes

| Failure | Cause | Detection / mitigation |
|---|---|---|
| Visible noise crawls across walls | Bayer sampled in screen space by mistake | Spec mandates `(s & 3, t & 3)` in texture coords; review at PR time |
| Cache thrash, frame stutter when standing still | Missing bucket hysteresis | ±25% hysteresis around bucket boundaries (mirrors existing mip hysteresis) |
| Dlit surfaces too expensive | Building dither + dlight every frame | Force bucket 0 when `dlight` set |
| Mip 0 ↔ mip 1 step still visible | Band too narrow at high mipscale, or `r_mipdither_band` set too low | Default `0.3`, tune via cvar |
| Bit-identical baseline regression with `r_mipdither 0` | New code path leaks into the off branch | Toggle cvar at runtime, screenshot-compare before/after |

## Verification

- Toggle `r_mipdither 0` at runtime — output must be visually identical to current master.
- Walk slowly toward a flat wall (e.g. start of e1m1). With dither off, the mip pop is a single-frame snap; with dither on, the transition becomes a band of visible Bayer noise that smoothly resolves to the new mip.
- Stand still inside a transition band — no frame-rate dip from cache thrash, bucket should be stable.
- `r_mipdither_band 0.5` — wider, more obvious dither zone; `0.1` — barely visible.
- Verify dlit surfaces (e.g. shoot a wall with the lightning gun) don't acquire extra cache cost — bucket 0 path takes over while dlight is active.

## Non-goals

- Per-pixel mip selection (true GL-style trilinear). Would require rewriting the surface cache model; orthogonal to this work.
- Dithering for water (`Turbulent8`), sky, sprites, or alias models. Different code paths, different problems.
- Live `r_mipdither_buckets` tuning. Fixed at 8 at compile time.

# Lightmap dither

**Status:** design approved 2026-05-19.
**Scope:** add 4×4 Bayer ordered dither to both the mono and RGB surface-block writers, gated by a new `r_lightmap_dither` cvar (archived, default 1). Texture-space pattern, applied inside the existing block writers. No new files, no ABI changes.
**Excludes:** screen-space dither (would require deferring brightness quantisation past the surface cache, a much larger refactor), per-channel decorrelated dither (introduces hue noise), dither on alias-model / sprite / sky / water paths.

## Motivation

The software renderer quantises brightness to 6 bits at two places:

- **Mono path** (`R_DrawSurfaceBlock8_mip*` in `r_surf.c`) — bilinear-interpolated `light` (8.8 fixed) selects a colormap row via `(light & 0xFF00) + pix`. The colormap has 64 brightness rows, so the gradient is stepped in 64 levels.
- **RGB path** (`R_DrawSurfaceBlock8_mip*_rgb` in `r_surf_rgb.c`) — each lit channel is quantised to 6 bits and indexed into `rgbtable[]`. Same 64-level cliff per channel.

The result is visible banding wherever the lightmap has a smooth gradient — most obvious on long lit walls, around dynamic lights, and on `.lit`-coloured surfaces where three channels band independently. The 2026-05-13 coloured-lighting spec explicitly listed "dithered LUTs" as v2 work. This spec is that v2.

There is prior art in the codebase: `r_fog_bayer2x2` already dithers distance fog in alias-model spans (`d_polyse.c`). Lightmap dither extends the same idea to surfaces, with a finer 4×4 pattern that suits the smoother gradients of lightmaps better than the 2×2 used for binary fog crossover.

## Approach

A single 4×4 Bayer table indexed by `(by & 3, bx & 3)` within each surface-cache block. The Bayer value is added to the brightness immediately before the 6-bit quantisation step. When `r_lightmap_dither` is 0, the writers skip the dither addition and produce the classic banded output bit-for-bit.

Texture-space (not screen-space) is the natural fit for Quake's surface cache: the dither pattern is baked into the cache when surfaces are built, sticks to walls as the camera moves (no shimmer), and costs nothing extra on cache hits. The same 4×4 pattern aligns to every 16-texel block boundary, which means it tiles cleanly with no visible seam between adjacent surface-cache blocks within a single surface.

Both paths use the **same** Bayer offset across channels — this dithers luminance without introducing colour noise. Per-channel decorrelated offsets would shift hue at every dither boundary and look like chromatic speckle.

## File changes

| File | Change |
|---|---|
| `sdlquake/engine_src/r_surf.c` | Add `r_lightmap_dither` cvar + `r_bayer4x4[16]` table. Modify the four `R_DrawSurfaceBlock8_mip*` writers to add the Bayer offset before the colormap-row mask, with clamp to `63 << 8`. |
| `sdlquake/engine_src/r_surf_rgb.c` | Modify the four `R_DrawSurfaceBlock8_mip*_rgb` writers to add a (scaled) Bayer offset before the `>> RGB_SHIFT` quantisation in each of R/G/B. |
| `sdlquake/engine_src/r_main.c` | Register `r_lightmap_dither` in `R_Init` alongside `r_lightmap` / `r_drawflat` / `r_fullbright`. |

No header changes. No `game_api_t` / `engine_api_t` bump. No new files.

## Bayer table

Standard 4×4 ordered, values 0..15:

```c
static const unsigned char r_bayer4x4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};
```

## Mono path

Current inner-loop core (`r_surf.c` `R_DrawSurfaceBlock8_mip0`):

```c
for (b = 15; b >= 0; b--)
{
    pix = psource[b];
    prowdest[b] = ((unsigned char *)vid.colormap)[(light & 0xFF00) + pix];
    light += lightstep;
}
```

After (illustrative — actual hoisting of cvar/load left to implementation):

```c
unsigned dither_on = r_lightmap_dither.value != 0;
...
for (b = 15; b >= 0; b--)
{
    pix = psource[b];
    unsigned d = dither_on ? ((unsigned)r_bayer4x4[((i & 3) << 2) | (b & 3)] << 4) : 0u;
    unsigned row = (light + d) & 0xFF00;
    if (row > (63u << 8)) row = 63u << 8;
    prowdest[b] = ((unsigned char *)vid.colormap)[row + pix];
    light += lightstep;
}
```

Why scaling `<< 4`: `light` is 8.8 fixed, so the colormap row occupies bits 8..13 (six bits, 64 rows). Bayer 4×4 is 0..15. Shifting by 4 makes the dither span 0..240 — almost a full row's worth — so the threshold-crossing into the next row is well-distributed.

Why the clamp: `R_BuildLightMap` can yield `light` near row 63 already. Adding up to 240 could push past the 64-row colormap (16384 bytes). One branchless `min` per texel guards the read.

The same modification is repeated in the mip1/mip2/mip3 writers. Inner-loop width is 8/4/2 instead of 16 — the `b & 3` index tiles regardless. The outer block loop bound (`i < 8 / 4 / 2`) means the pattern repeats once per block at mip1 and so on. That's fine; mips 1+ are texels far from camera where dither precision matters less.

## RGB path

Current inner-loop core (`r_surf_rgb.c` `R_DrawSurfaceBlock8_mip0_rgb`):

```c
tex = psource[bx];
r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
if (r6 > 63) r6 = 63;
if (g6 > 63) g6 = 63;
if (b6 > 63) b6 = 63;
prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
```

After:

```c
unsigned d = dither_on ? ((unsigned)r_bayer4x4[((by & 3) << 2) | (bx & 3)] << (RGB_SHIFT - 4)) : 0u;
tex = psource[bx];
r6 = ((RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) + d) >> RGB_SHIFT;
g6 = ((RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) + d) >> RGB_SHIFT;
b6 = ((RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) + d) >> RGB_SHIFT;
if (r6 > 63) r6 = 63;
if (g6 > 63) g6 = 63;
if (b6 > 63) b6 = 63;
prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
```

`RGB_SHIFT` is 8 (defined in `r_surf_rgb.c`), so `RGB_SHIFT - 4 = 4` — same shift as the mono path, producing a 0..240 offset in pre-shift units. The existing `if (r6 > 63) r6 = 63;` already clamps the saturation case, so no new clamp is needed. The mip1/mip2/mip3 RGB writers receive the same change; the `bx & 3` index naturally tiles regardless of inner-loop width.

## Cvar

```c
cvar_t r_lightmap_dither = {"r_lightmap_dither", "1", true /* archived */};
```

Registered in `R_Init` next to `r_lightmap`. When 0, the dither offset `d` is 0 and the inner-loop arithmetic is identical to today's code (the compiler should fold the `+ 0` away; if not, the cvar check hoists out of the inner loop). Setting it 0 in the console restores the classic banded appearance for direct comparison.

## Performance notes

The block writers are the hottest inner loop in WinQuake. Cost added per texel:

- **Mono:** one table load (16-byte table, fits in a single cache line), one shift, one add, one mask, one compare-and-clamp.
- **RGB:** one table load, one shift, three adds (R/G/B share the same offset, so the load+shift hoists out).

The table load can be hoisted once per row of the inner loop since the dither offset depends only on `(by & 3, bx & 3)` — actually one offset per row + the `bx` term reads a different table column. The cleanest implementation precomputes a 4-entry "row of dither" array per scanline (4 values) and indexes by `(15 - bx) & 3` in the inner loop.

Expected impact: low single-digit percent on a heavy-surface scene. Implementation plan should include a before/after frame-time check on a representative scene (e.g., `e1m1` looking down a long corridor) before declaring done.

## Limits and non-goals

- **Surface seams:** each surface has its own block-aligned 4×4 pattern. At a seam between two surfaces with aligned lightmap values, the two patterns meet without phase continuity. In practice this is invisible because surface seams rarely sit mid-gradient.
- **Stains/decals:** applied in `R_OverlayStain` *after* the block writer runs; the dithered output is the input to the stain pass, which is fine.
- **Sky / water / alias / sprite paths:** untouched. Sky and water use their own warp shaders; alias and sprite lighting use a different code path (`d_polyse.c`) and have their own existing fog dither.
- **Mip 1/2/3 pattern frequency:** the 4×4 Bayer pattern repeats per-block at coarser mips. Acceptable because mip-N surfaces are by definition far from camera.
- **Bit-for-bit identical when off:** with `r_lightmap_dither 0`, output matches today's renderer exactly.

## Testing

- Build clean, run `e1m1`, walk a long lit wall — banding should be visibly broken up.
- Set `r_lightmap_dither 0` in the console — banding should return to classic appearance.
- Load a map with `.lit` coloured lighting (e.g., one of the stock id1 maps with a community `.lit` file alongside) — RGB path dither should reduce per-channel banding without introducing hue speckle.
- F12 debug overlay should still show correct surface counts; no new frame timing regression beyond a few percent.
- Existing `r_fog_bayer2x2` fog dither in alias models is unaffected (different code path).

# Depth-graded liquid sheen (software renderer)

**Date:** 2026-06-01
**Status:** Design approved, implementing

## Problem

Liquid surfaces (water/slime/lava) look **flat**. Two causes in this renderer:
no see-through (no parallax to the bottom) and uniform shading (the whole
surface is one brightness — no gradient, no sheen, no sense of a 3D plane).

An earlier idea — a uniform tint — was rejected: it lightens every pixel
equally and reads *flatter*, not deeper.

## Goal

Make liquids read as a **3D surface with depth**, cheaply and at low risk,
*without* true see-through (that's a separate, larger change — see Non-goals).
Two mechanisms, both pure LUT math in the existing turbulent span filler:

1. **Depth-graded sheen.** Tint the surface toward a cool highlight color, with
   strength rising by distance. Key insight: for a horizontal water plane,
   **distance ≈ grazing angle**, so a distance gradient doubles as a fresnel
   sheen — clear/textured up close, brightening to sky-sheen at the glancing
   far edge. That near→far gradient is what reads as a receding 3D surface.
2. **Ripple relief.** Nudge the sheen per-pixel by the warp displacement so
   ripple crests catch more sheen and troughs less — fake specular glints that
   give the near field surface relief. Cvar-gated; off = gradient only.

## Non-goals

- **True see-through / parallax to the bottom.** Impossible to bolt on cheaply:
  the world uses Quake's analytic zero-overdraw VSD, so geometry behind a liquid
  is never drawn — nothing to blend against. Real see-through needs a
  non-occluding liquid pass (Makaqu approach). Deferred; revisit only if the
  sheen+ripple still feels flat.
- **Per-liquid colors / true normal-based fresnel / caustics.** Later
  refinements. Distance-as-fresnel is a proxy: great for horizontal water,
  approximate for vertical (waterfalls get a distance gradient, no true
  fresnel). One sheen color for all liquids for now.

## Mechanism — parallels the fog system (`r_fog.c`)

Fog already proves a precomputed palette-index→palette-index LUT applied
per-pixel in the turbulent span filler is cheap and correct. Sheen is "a
water-only second fog with a sheen color, its own distance curve, and a
per-pixel ripple offset."

### The sheen LUT (`r_water.c`)

- `r_water_sheenmap[64*256]` — 64 rows. Row `N` maps each palette index `c` to
  the nearest palette index of `lerp(host_basepal[c], sheen_rgb, (N/63)·strength)`.
  **Row 0 is identity** (no tint), row 63 is max sheen. Built by `BuildSheenmap`,
  mirroring `BuildFogColormap` (`r_fog.c:44`) but baking `strength` into the row.
- Rebuilt only when strength **or** sheen color changes (cached-value compare,
  like `R_Fog_Update`). One 256×256 nearest-color search per row — sub-ms, only
  on cvar change.

### Per-frame / per-sub-span state

`R_Water_Update()` (called each frame next to `R_Fog_Update`, `r_main.c:1125`):
- `r_water_sheen_active = (r_water_sheen.value > 0) && (r_drawflat.value == 0)`
  — auto-off in the `r_drawflat` debug views (they swap `cacheblock`).
- `r_water_ripple_scale` — ripple strength as an int row-offset scale.

`R_Water_GetRows(dist, &base, &thresh4)` returns the **floor base row 0..62**
plus a 0..3 Bayer threshold from the **Euclidean eye→liquid distance**:
`f = 1 - exp(-r_water_sheen_dist·dist); base = floor(f·63)` (near→0, far→63),
`thresh4` = fractional part for 2×2 Bayer dithering toward `base+1` (smooths the
gradient — no hard row-step lines, exactly like the fog path). Set per sub-span
in `Turbulent8`, beside the fog row pick.

**Why Euclidean distance, not camera-space `1/z`:** keying to view-forward depth
made the gradient *swim* as you rotate (a fixed water point's `z` changes with
view direction). Euclidean distance is rotation-invariant — turning in place
doesn't change it — and for a flat liquid plane it *is* the true fresnel cue
(grazing angle is a function of distance at fixed eye height). Computed per
sub-span from `1/z` × the screen-ray secant (`sqrt(1 + tanθx² + tanθy²)` via the
projection constants `xscale`/`yscale`/`xcenter`/`ycenter`); one `sqrt` per
≤16-px span. The underwater-warp buffer (`r_dowarp`) falls back to plain `1/z`
(its pixel coords don't share the `xcenter` frame; full-screen warp hides it).

### Integration in the span filler (`d_scan.c`)

`D_DrawTurbulent8Span` gets a one-line guard at the top:
`if (r_water_sheen_active) { D_DrawTurbulent8Span_Sheen(); return; }` — so the
existing fog/non-fog loops stay **byte-identical** when sheen is off (zero added
cost; same hoist-the-branch discipline as fog).

The new `D_DrawTurbulent8Span_Sheen` reuses the same warp, then per pixel:
```
dbase = (bayer2x2[y,x] < thresh4) ? base + 1 : base;          // dither the gradient
row   = dbase + (((turbs - AMP) >> 8) * ripple_scale >> 16);  // ripple glint; AMP=8<<16 centers it
clamp row to 0..63
idx   = r_water_sheenmap[(row<<8) + texel];                   // depth-graded sheen
*dest = fog_active ? fog[idx] : idx;                          // tint then fog
```
`turbs` (the s-axis turbulence) is already computed for the warp, so ripple is
near-free. `ripple_scale == 0` ⇒ ripple off (gradient only). The Bayer value is
shared with the fog dither; screen x/y for it are hoisted to the function top so
both fog/non-fog sub-loops use them. Fog branch hoisted (2 sub-loops). C path
only (`#if !id386`; asm path not built).

## Cvars (`r_water.c`, registered in `R_Water_Init`)

| Cvar | Default | Meaning |
|---|---|---|
| `r_water_sheen` | `0.4` | Master sheen strength 0..1. `0` = off / classic opaque. |
| `r_water_sheen_gain` | `2.0` | Highlight = the texel's own colour × gain (≥1, hue-preserving). |
| `r_water_sheen_dist` | `0.004` | Distance falloff — how fast water reaches full sheen. |
| `r_water_ripple` | `0.3` | Ripple-relief strength 0..1. `0` = gradient only. |

> **Revision (2026-06-01):** the original fixed RGB highlight (`r_water_sheen_red/green/blue`
> = 0.80/0.90/1.0) pushed every liquid toward a cool blue-white, which read as "too blue"
> and ignored the water's actual colour. Replaced by `r_water_sheen_gain`: the highlight is
> now each texel's **own** colour brightened (hue-preserving), so the sheen reflects the water
> texture — and self-corrects per liquid (slime brightens green, lava orange, etc.).

Default **on** so the effect shows immediately; defaults to be tuned in-game.
Applies to all liquids (incl. teleporters, which bucket as water — harmless).

## Files

- **New** `sdlquake/engine_src/r_water.{c,h}` — cvars, sheen LUT, init/update,
  `R_Water_RowFromZi`, globals. Modeled on `r_fog.{c,h}`.
- `sdlquake/engine_src/d_scan.c` — include `r_water.h`; `r_turb_sheen_base`
  global; row pick in `Turbulent8`; guard + `D_DrawTurbulent8Span_Sheen`.
- `sdlquake/engine_src/r_main.c` — `R_Water_Init` after `R_Fog_Init` (`:337`);
  `R_Water_Update` after `R_Fog_Update` (`:1125`); include `r_water.h`.
- `build.zig` — add `"r_water.c"` next to `"r_fog.c"` (`:60`).

## Verification (visual, in-game — no test suite)

1. `zig build` succeeds.
2. `+map e1m1`: water shows a near→far gradient/sheen, not a flat sheet.
3. Slime and lava also sheened (all-liquids choice).
4. `r_water_sheen 0` ⇒ visually identical to pre-change (off path unchanged).
5. `r_water_ripple` 0↔0.3 toggles ripple glints.
6. Composes with fog (`r_fog_density 0.02`) without artifacts.
7. `r_drawflat 2` still shows flat liquids (sheen auto-disabled).
8. Tune `r_water_sheen` / `_dist` / color / `_ripple` defaults live with user.

# Murky underwater fog (software renderer)

**Date:** 2026-06-01
**Status:** Design approved, implementing

## Goal

Make submerged views look **murky** by giving the view its own, denser fog while
underwater, with the murk color **tied to the water's content tint** (so it
matches the water automatically). Independent of any above-water fog.

## Approach

A contained extension of the **existing** fog module (`r_fog.c`). The fog system
already builds a 64-row `r_fog_colormap` from a color and selects a depth→row per
pixel from `1/z` using `r_fog_density`; `R_Fog_Update()` runs each frame at
`r_main.c:1129`, right after `R_SetupFrame` sets `r_viewleaf`. We make
`R_Fog_Update` choose the fog parameters based on whether the view leaf is liquid.

No new file. Every existing fog consumer (world surfaces, alias models, the
caustic surfaces) picks up the underwater fog for free, and it's carried along
by the underwater warp.

## Mechanism

### One new cvar

`r_water_fog_density` (default `0.015`; `0` = no underwater fog). The only knob —
the murk color is derived, not configured.

### `R_Fog_Update` chooses effective parameters

```
if (r_viewleaf && r_viewleaf->contents <= CONTENTS_WATER)   // in liquid (water/slime/lava)
{
    cs = (contents==CONTENTS_LAVA) ? cshift_lava :          // tint for this liquid
         (contents==CONTENTS_SLIME)? cshift_slime : cshift_water;
    eff_density = r_water_fog_density.value;
    eff_color   = cs.destcolor / 255;        // water {130,80,50}, slime {0,25,5}, lava {255,80,0}
}
else
{
    eff_density = r_fog_density.value;        // above water: unchanged
    eff_color   = (r_fog_red, r_fog_green, r_fog_blue);
}

if (eff_color changed) BuildFogColormap(eff_color);   // existing cached-rebuild
r_fog_density_eff = eff_density;                       // new file-static
r_fog_active      = eff_density > 0;
```

This matches the engine's own "in liquid" test (`r_dowarp` uses the same
`contents <= CONTENTS_WATER`, `r_misc.c:443`). The condition covers all three
liquids; the view leaf is never sky/solid, so the bucket is exact.

### Depth→row functions use the effective density

`R_Fog_GetRows` and `R_Fog_RowFromZi` currently read `r_fog_density.value`; they
switch to the new `r_fog_density_eff` (set above). Nothing else in those
functions changes — they still index `r_fog_colormap`, which now holds whichever
color is in effect.

### Externs needed in `r_fog.c`

- `extern mleaf_t *r_viewleaf;` (set in `R_SetupFrame`).
- `extern cshift_t cshift_water, cshift_slime, cshift_lava;` (defined `view.c:255-257`;
  only `cshift_water` is currently extern'd, in `r_local.h:338` — add the other two).
- `CONTENTS_WATER`/`_SLIME`/`_LAVA` and `cshift_t`/`mleaf_t` come via `quakedef.h`.

`R_Fog_Init` registers `r_water_fog_density` and initializes `r_fog_density_eff`.

## Properties

- **Independent of above-water fog.** Underwater density can be > 0 while
  `r_fog_density` is 0 (the common case: no fog up top, murk below). Diving swaps
  fog on; surfacing swaps it off.
- **All liquids, tied to each tint** — water = brownish murk, slime = green, lava
  = orange. (Water is the main case; trivially restrictable to water-only by
  tightening the condition to `== CONTENTS_WATER`.)
- Distance fades toward the liquid tint, *layered* under the existing palette
  content-shift + caustics → reads as real depth-murk.

## Non-goals / caveats

- **No separate underwater fog color cvars** — tied to the tint per the design
  choice. Easy to add an override later.
- **Bobbing at the waterline re-bakes the colormap** each crossing (~couple ms,
  64×256 nearest-color). Fine in practice (transitions are infrequent, not
  per-frame). If it ever hitches, the fix is a two-colormap swap (prebuild
  above-water + underwater, switch a pointer); deferred (YAGNI).

## Files

- `sdlquake/engine_src/r_fog.c` — new cvar, `r_fog_density_eff`, submerged branch
  in `R_Fog_Update`, density swap in `R_Fog_GetRows`/`R_Fog_RowFromZi`, externs,
  register in `R_Fog_Init`.
- `sdlquake/engine_src/r_fog.h` — `extern` the new cvar.

(No new module, no `build.zig`/`r_main.c` change — `R_Fog_Update` is already wired.)

## Verification (visual, in-game — no test suite)

1. `zig build` succeeds.
2. With `r_fog_density 0` (no fog above water): walk around dry — no fog. Dive into
   the `e1m1` pool — distance now fogs out to brownish murk.
3. `r_water_fog_density 0` ⇒ underwater fog off (back to today's look).
4. Set `r_fog_density 0.01` (light gray fog above): confirm above-water fog still
   works and is visibly *different* (lighter, gray) from the underwater murk.
5. Murk color tracks the liquid (brown in water; green in slime if reachable).
6. Tune `r_water_fog_density` live for the right murk depth.

# Underwater caustics (software renderer)

**Date:** 2026-06-01
**Status:** Design approved, ready for planning

## Goal

Animated caustic light ripples projected onto solid surfaces while the player's
view is submerged in water — the dancing light network you see underwater — to
sell the submerged feel. **Surface-projected** (not screenspace) so they stay
anchored to the world and don't swim as you turn; they ride along with the
existing underwater warp so the whole scene moves together.

## Scope / non-goals

- **Submerged-only.** Caustics show only while the view is inside a water leaf.
  *From-above* (caustics on a pool floor seen from dry land) is **deferred** — it
  needs per-surface waterline marking at map-load time (there is no per-surface
  contents field today; contents live only on leaves). Separate spec later.
- **Water only.** Slime and lava skip caustics — caustics are light refracting
  through *clear* water; their murky/molten tint already reads right.
- **Not a screenspace overlay** (`D_WarpScreen`) — it would swim. Rejected.
- **Not lightmap modulation** (`Lightmap_AddDelta`) — projects correctly but is
  per-luxel (coarse) and forces lightmap-cache rebuilds every frame. Rejected.

## Trigger / gating

Per-frame flag set in `R_Caustics_Update()` (called after `R_SetupFrame` has set
`r_viewleaf`, next to `R_Water_Update` in `r_main.c`):

```
r_caustics_active = r_caustics.value && r_viewleaf
                    && (r_viewleaf->contents == CONTENTS_WATER)
```

Contents-based, **independent of `r_waterwarp`** — caustics work even with the
warp disabled; `r_caustics` is the master toggle. When the warp *is* on (the
default), `D_WarpScreen` (`d_scan.c`) carries the caustics along with the scene.
`r_viewleaf`/`CONTENTS_WATER` are set in `r_misc.c:440-443`.

## Mechanism

A direct relative of the sheen/fog palette-LUT approach, but it modulates
*solid* surfaces (walls/floors) and keys the pattern to surface `(s,t)` + time.

### Caustic ridge texture (baked once)

`r_caustic_tex[CSIZE*CSIZE]` (CSIZE = 128, power of two) — a tileable intensity
texture, values `0..63`, baked at init from ridged sines (sum of a few sine
gratings sharpened toward thin bright ridges). Tileable so `(s,t)` sampling wraps
with a mask.

### Per-pixel intensity — two scrolling layers, multiplied

In the solid-surface span inner loop (`D_DrawSpans8`), each pixel has
perspective-correct `s,t` (fixed16, integer texel = `>>16`):

```
a = r_caustic_tex[(((s>>SH1)+ox1)&MASK) + (((t>>SH1)+oy1)&MASK)*CSIZE];  // layer 1
b = r_caustic_tex[(((s>>SH2)+ox2)&MASK) + (((t>>SH2)+oy2)&MASK)*CSIZE];  // layer 2
level = (a*b) >> 6;                       // 0..63: multiply -> sparse bright cells
*pdest = r_caustic_map[(level<<8) + texel];
```

`SH1`/`SH2` are scale shifts (two different scales, from `r_caustics_scale`);
`ox*/oy*` are per-frame scroll offsets from `cl.time * r_caustics_speed`
(computed once per frame in `R_Caustics_Update`, the two layers scrolling in
different directions). **Multiplying** the layers makes caustics appear only
where both are bright — the constructive-interference look of real caustic
networks — and the dance comes from the two offsets sliding past each other.
This pattern formula is the part most likely to need visual tuning.

### Caustic LUT

`r_caustic_map[64*256]` — row `N` = nearest palette index to
`lerp(palette[c], caustic_color, (N/63)*r_caustics_intensity)`. **Row 0 =
identity** (surface unchanged); higher rows brighten the texel toward a light
blue-white highlight. Built on cvar change (intensity), mirroring `BuildSheenmap`
/ `BuildFogColormap`. Caustic color is a fixed light blue-white for v1.

### Hook in `D_DrawSpans8` (`d_scan.c`)

A guarded variant mirroring the sheen's `D_DrawTurbulent8Span_Sheen`: a
top-of-function `if (r_caustics_active) { ...caustic path...; return; }`, so the
common (not-submerged) path stays **byte-identical**. The caustic path composes
with fog (caustic then fog: `fog[r_caustic_map[(level<<8)+texel]]`), same as
sheen. C path only (`#if !id386`; asm path not built).

*Implementation note:* exact factoring — a separate `D_DrawSpans8_Caustic`
function (duplicates the ~130-line perspective sub-span setup but leaves the hot
path untouched) vs. per-sub-span gated inner loops inside `D_DrawSpans8`
(duplicates only the small inner loop but edits the hot function) — is decided
during planning. Bias toward whichever keeps the off-path identical with least
duplication.

## Cvars (`r_caustics.c`, registered in `R_Caustics_Init`)

| Cvar | Default | Meaning |
|---|---|---|
| `r_caustics` | `1` | Master toggle (`0` = off). |
| `r_caustics_intensity` | `0.5` | Highlight strength 0..1 (bakes into the LUT). |
| `r_caustics_scale` | tuned | Cell size on surfaces (texel→texture shift). |
| `r_caustics_speed` | tuned | Scroll/animation rate. |

Light blue-white highlight color fixed for v1 (easy to expose as cvars later).

## Files

- **New** `sdlquake/engine_src/r_caustics.{c,h}` — cvars, baked ridge texture,
  caustic LUT, `R_Caustics_Init`/`R_Caustics_Update`, gating + per-frame scroll
  offsets. Modeled on `r_water.{c,h}`. Add `"r_caustics.c"` to `build.zig`.
- `sdlquake/engine_src/d_scan.c` — include `r_caustics.h`; `D_DrawSpans8` guard +
  caustic variant.
- `sdlquake/engine_src/r_main.c` — `R_Caustics_Init` after `R_Water_Init`;
  `R_Caustics_Update` after `R_Water_Update`.

## Perf

A couple of texture lookups + one LUT per solid-surface pixel, but only while
submerged (rare/transient). Negligible in normal play; off-path unchanged.

## Verification (visual, in-game — no test suite)

1. `zig build` succeeds.
2. Dive into the `e1m1` water pool: caustic ripples play over the walls/floor,
   stay anchored to surfaces as you turn (no swim), and wobble with the warp.
3. Stand above water: no caustics. Climb out: caustics stop.
4. `r_caustics 0` ⇒ off, byte-identical to pre-change.
5. Submerge in slime/lava (other maps): no caustics (water-only).
6. Tune `r_caustics_intensity` / `_scale` / `_speed` live with the user.

## Deferred extension: caustics from above

Mark liquid-adjacent solid surfaces at map-load time (or track each surface's
leaf contents) so caustics can also render on a pool floor viewed from dry land.
Its own spec when we get there.

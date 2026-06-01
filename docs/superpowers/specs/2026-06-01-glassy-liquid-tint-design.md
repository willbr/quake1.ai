# Glassy liquid tint (software renderer)

**Date:** 2026-06-01
**Status:** Design approved, ready for planning

## Goal

Make liquid (turbulent) surfaces — water, slime, lava — read as **lighter and
less solid** ("glassy"), by blending each liquid pixel toward a configurable
tint color. Live-tunable via cvars, default on.

## Non-goals (explicitly out of scope)

- **True see-through transparency.** The user chose a surface tint, *not*
  seeing the submerged floor through the water. Real see-through is impossible
  to bolt on cheaply here: the world uses Quake's analytic visible-surface
  determination (zero overdraw), so the geometry *behind* a liquid surface is
  never written to the framebuffer — there is nothing to blend against. That
  would require making liquid surfaces non-occluding plus a second blended
  pass (the Makaqu approach), a much larger renderer change. Not doing it.
- **Per-liquid tint colors.** One global tint applies to all liquids. Splitting
  water/slime/lava colors is easy to add later if wanted; YAGNI for now.
- **GPU-stage tinting.** Would need liquid pixels tagged into a coverage buffer
  first; more plumbing than the CPU LUT, which reuses proven fog machinery.

## Mechanism

A direct parallel to the existing fog system (`r_fog.c`). Fog already proves
that a precomputed palette-index → palette-index LUT, applied per-pixel in the
turbulent span filler, is cheap and correct.

### The tint LUT

- `r_water_tintmap[256]` — for each palette index `c`, store the nearest
  palette index to `lerp(host_basepal[c], tint_rgb, strength)`.
- Built by a `BuildWaterTintmap()` that mirrors `BuildFogColormap()`
  (`r_fog.c:44`) but produces a single 256-entry row instead of 64 rows
  (there is no depth dimension — the tint is uniform).
- Rebuilt only when `strength` **or** the tint color changes (cached-value
  comparison, exactly like `R_Fog_Update` at `r_fog.c:75`). Build cost is one
  256×256 nearest-color search — sub-millisecond, only on cvar change.

### Per-frame state

`R_Water_Update()` (called each frame next to `R_Fog_Update`) sets two globals
consumed by the span filler:

- `r_water_tint_active` = `(r_water_tint.value > 0) && (r_drawflat.value == 0)`
  — so the tint auto-disables in the `r_drawflat` debug views (which swap
  `cacheblock` for a flat-color buffer at `d_edge.c:279`).
- `r_water_tintmap` pointer (the built LUT).

The tint is uniform per frame (no z dependence), so unlike fog's per-sub-span
row selection, this state is set once per frame, not per span.

### Integration in the span filler

`D_DrawTurbulent8Span()` (`d_scan.c:105`) currently has two inner loops, gated
by a hoisted `r_fog_active` branch:

- non-fog: `*dest++ = texel;`               (`d_scan.c:134`)
- fog:     `*dest++ = fog[texel];`          (`d_scan.c:122`)

We wrap these in an outer `r_water_tint_active` branch, giving a 2×2 of inner
loops. The **off** side keeps today's two loops verbatim, so when tint is off
the output is byte-identical to today with zero added inner-loop cost (same
hoist-the-branch discipline the fog path already uses). The **on** side adds:

- non-fog + tint: `*dest++ = tintmap[texel];`
- fog + tint:     `*dest++ = fog[tintmap[texel]];`  (tint first, then fog)

Composition order — tint then fog — is correct: tint is the liquid's own
surface color, fog is atmospheric depth layered on top.

Only the `#if !id386` C path is touched; the id386 assembly path is not built
on this target.

## Cvars

Registered in a new `R_Water_Init()` (called next to `R_Fog_Init`), naming
mirrors the fog cvars:

| Cvar | Default | Meaning |
|---|---|---|
| `r_water_tint` | `0.35` | Blend strength 0..1. `0` = off / classic opaque. |
| `r_water_tint_red` | `0.80` | Tint color R, 0..1. |
| `r_water_tint_green` | `0.88` | Tint color G, 0..1. |
| `r_water_tint_blue` | `1.0` | Tint color B, 0..1. |

Default color is a light cool white: it lightens all three liquids while
leaning faintly blue. Default **on** at 0.35 so the effect is visible
immediately; final default numbers to be eyeballed and tuned in-game.

## Files touched

- **New** `sdlquake/engine_src/r_water.c` + `r_water.h` — cvars, tint LUT,
  `R_Water_Init` / `R_Water_Update`, the globals. Modeled on `r_fog.c`/`r_fog.h`.
  (Plan must wire it into the build the same way `r_fog.c` is.)
- `sdlquake/engine_src/d_scan.c` — include `r_water.h`; add the tint inner
  loops to `D_DrawTurbulent8Span`.
- Wherever `R_Fog_Init` / `R_Fog_Update` are called — add the matching
  `R_Water_Init` / `R_Water_Update` calls (plan to locate exact sites).

## Wrinkles / accepted behavior

- **Teleporters** (`*teleport`) are turbulent and bucket as "water," so they
  get tinted too. Harmless; trivial to exclude by texture name later if it
  looks off.
- **Underwater full-screen warp** (`D_WarpScreen`, `r_waterwarp`) is a separate
  effect and is untouched.
- **Lava** is tinted lighter per the "all liquids" choice — intentional.

## Verification

No automated tests exist; verification is visual in-game.

1. Build succeeds (`zig build`).
2. Launch a map with visible water (e.g. `+map e1m1`), confirm water reads
   lighter/glassier vs. `r_water_tint 0` (toggle live).
3. Confirm slime and lava are also tinted (e.g. a lava map / `e1m1` slime).
4. Confirm `r_water_tint 0` is visually identical to pre-change (off path
   unchanged).
5. Confirm tint composes with fog (`r_fog_density 0.02`) without artifacts.
6. Confirm `r_drawflat 2` still shows flat liquids (tint auto-disabled).
7. Tune the default strength/color live with the user.

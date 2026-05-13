# Coloured lighting in the software renderer

**Status:** design approved 2026-05-13.
**Scope:** add per-channel coloured static lighting via `.lit` sidecar files plus coloured dynamic lights, while keeping the renderer's 8-bit screen buffer and all downstream paths (alias models, sprites, particles, UI) untouched. Excludes: an in-engine `.lit` baker from `_color` light entities, dithered LUTs, coloured liquid/sky surfaces, coloured alias/sprite lighting. Those are explicit v2+ items.

## Motivation

The project is preparing a decals/bullet-holes feature (Phase 8 — immersive sim). A surface-cache-patched decal pipeline composes naturally on top of a multi-pass lit surface cache. Rather than build the decal system on the current monochrome cache and rebuild it later, the user opted to add coloured lighting first. The decals feature is set aside pending this work landing; its design will be re-opened in a separate spec.

Coloured lighting is also a desirable visual upgrade on its own — community `.lit` files exist for most stock and well-known custom maps, and Quake's combat verbs (muzzle flash, rockets, explosions, lightning) all gain readability when their dynamic-light contributions are tinted.

## Approach

Stay 8-bit through the entire pipeline. The surface-cache writer is the *only* component that ever sees RGB; everywhere else continues to consume palette indices.

Three additions:

1. **`.lit` loader** — when a BSP is loaded, attempt to load `maps/<name>.lit` alongside the BSP's existing mono lightmap. If present and consistent, each `msurface_t` gains an `rgb_samples` pointer parallel to its existing `samples` pointer. If absent or malformed, `rgb_samples` is `NULL` and the renderer takes the unchanged mono path.
2. **RGB surface-block writer** — a second variant of `R_DrawSurfaceBlock8_mip*` that consumes three corner-light channels, computes per-texel `basepal[texel] * light_RGB`, quantises to 6 bits per channel, and looks up the nearest palette index in a precomputed 256-KB LUT. Chosen at the top of `R_DrawSurface` based on `surf->rgb_samples != NULL`.
3. **Coloured dynamic lights** — `dlight_t` gains a `color` field, defaulting to `{1,1,1}`. Engine call sites in `cl_main.c` / `cl_tent.c` set sensible per-event colours.

The mono path is the unmodified default. Coloured rendering is gated by `r_coloredlight` (cvar, archived, default `1`) and by the presence of `.lit` data — turning either off restores the existing classic look identically.

## File changes

| File | Change |
|---|---|
| `model.h` (and `gl_model.h` for parity) | `msurface_t` gains `byte *rgb_samples`. `model_t` gains `byte *rgblightdata` (parallel to `lightdata`). |
| `model.c` | `Mod_LoadLighting` calls a new `Mod_LoadLITFile` after the mono load. On success, `rgblightdata` is filled and surface pointers are wired in the existing per-surface fixup. |
| `r_local.h` | Declarations for `blocklights_rgb`, `R_BuildLightMap_RGB`, `R_AddDynamicLights_RGB`, the four `R_DrawSurfaceBlock8_mip*_rgb` variants. |
| `r_surf.c` | `R_DrawSurface` chooses the mono or RGB writer based on `r_drawsurf.surf->rgb_samples` and the `r_coloredlight` cvar. `R_BuildLightMap_RGB` and `R_AddDynamicLights_RGB` live alongside the existing mono versions. |
| `r_surf_rgb.c` (new) | The four `R_DrawSurfaceBlock8_mip*_rgb` writers. Kept in a separate file so the existing `r_surf.c` mono code is not diff-noisy. |
| `r_lut.c` (new) | 64³ RGB→palette LUT generator + disk cache (`id1/rgbtable.lmp`). Called from `R_Init`. |
| `client.h` | `dlight_t` gains `vec3_t color`. |
| `cl_main.c` | `CL_AllocDlight` initialises `color` to `{1,1,1}`. |
| `cl_main.c` / `cl_tent.c` | Each dlight call site sets `color` to the value from `cl_dlight_colors.h`. |
| `engine_src/cl_dlight_colors.h` (new) | Named constants for muzzle / rocket / explosion / lightning / etc. |
| `render.h` (or wherever cvars are declared) | `r_coloredlight`, `r_colored_dlights`. |
| `engine/hotreload.c` ↔ `game_api.h` | No ABI change. `Sample_Lightmap` (Phase 8) keeps its signature and returns `max(R,G,B)` when RGB data is present. No `GAME_API_VERSION` bump. |

## `.lit` format

The FitzQuake/QuakeSpasm de-facto standard, little-endian:

```
char  magic[4]   = "QLIT"
int   version    = 1
byte  rgb[3 * lightmap_sample_count]   // matches BSP's mono lightmap byte count
```

`lightmap_sample_count` is the byte size of the BSP's mono `lightdata` lump. If the loaded `.lit` declares a different count, the loader emits `Con_Printf("ignoring maps/%s.lit: sample count mismatch (%d vs %d)\n", ...)` and falls back to mono on that map. Bad sidecars never `Sys_Error`.

`Mod_LoadLITFile` allocates `rgblightdata` on the hunk in the same lifetime as the BSP's `lightdata`, immediately following it. Each surface's `rgb_samples` is then derived by the same offset arithmetic as `samples`:

```c
surf->rgb_samples = (mod->rgblightdata && surf->samples)
    ? mod->rgblightdata + (surf->samples - mod->lightdata) * 3
    : NULL;
```

Surfaces with `samples == NULL` (fullbright, sky, certain liquid surfaces) keep `rgb_samples == NULL`. The dispatch in `R_DrawSurface` already special-cases `samples == NULL` by routing through other paths; the RGB writer is only ever invoked when both pointers are non-NULL.

## RGB→palette LUT

`rgbtable[64*64*64]` — 256 KB, fits comfortably in L2 on any target. Generated once from `host_basepal` by exhaustive nearest-Euclidean-RGB match:

```c
for (r = 0; r < 64; r++)
  for (g = 0; g < 64; g++)
    for (b = 0; b < 64; b++) {
      int best = 0, best_d = INT_MAX;
      for (int p = 0; p < 256; p++) {
        int dr = host_basepal[p*3+0] - r*4;
        int dg = host_basepal[p*3+1] - g*4;
        int db = host_basepal[p*3+2] - b*4;
        int d = dr*dr + dg*dg + db*db;
        if (d < best_d) { best_d = d; best = p; }
      }
      rgbtable[(r<<12)|(g<<6)|b] = (byte)best;
    }
```

~16M comparisons; 200–400 ms cold on a modern CPU. The result is cached to `id1/rgbtable.lmp`:

```
char  magic[4]   = "RGBT"
int   version    = 1
byte  pal_hash[8]   // first 8 bytes of MD5(host_basepal[768])
byte  table[64*64*64]
```

On startup, `R_Init` reads the cache; if magic/version/hash match `host_basepal`, the table is `memcpy`'d in directly. If anything mismatches or the file is missing, the table is rebuilt silently and rewritten. Cache invalidation is therefore automatic if the palette ever changes.

`basepal_r[256]`, `basepal_g[256]`, `basepal_b[256]` are extracted from `host_basepal` at the same time and held as module-static arrays. The block writer reads from those rather than re-striding `host_basepal` per texel.

## Build-lightmap RGB variant

```c
unsigned blocklights_rgb[18*18*3];  // R0,G0,B0,R1,G1,B1,...

void R_BuildLightMap_RGB(void)
{
    msurface_t *surf = r_drawsurf.surf;
    int smax = (surf->extents[0]>>4)+1;
    int tmax = (surf->extents[1]>>4)+1;
    int size = smax * tmax;

    if (r_fullbright.value || !cl.worldmodel->lightdata) {
        for (int i = 0; i < size*3; i++) blocklights_rgb[i] = 0;
        return;
    }

    unsigned amb = r_refdef.ambientlight << 8;
    for (int i = 0; i < size; i++) {
        blocklights_rgb[i*3+0] = amb;
        blocklights_rgb[i*3+1] = amb;
        blocklights_rgb[i*3+2] = amb;
    }

    byte *src = surf->rgb_samples;
    for (int m = 0; m < MAXLIGHTMAPS && surf->styles[m] != 255; m++) {
        unsigned scale = r_drawsurf.lightadj[m];   // existing 8.8 from R_DrawSurface
        for (int i = 0; i < size; i++) {
            blocklights_rgb[i*3+0] += src[i*3+0] * scale;
            blocklights_rgb[i*3+1] += src[i*3+1] * scale;
            blocklights_rgb[i*3+2] += src[i*3+2] * scale;
        }
        src += size * 3;
    }

    if (surf->dlightframe == r_framecount)
        R_AddDynamicLights_RGB();

    for (int i = 0; i < size * 3; i++) {
        int t = (255*256 - (int)blocklights_rgb[i]) >> (8 - VID_CBITS);
        if (t < (1 << 6)) t = (1 << 6);
        blocklights_rgb[i] = t;
    }
}
```

Same shape as `R_BuildLightMap`, three channels. `R_AddDynamicLights_RGB` is the mono function with `temp` multiplied per channel by `cl_dlights[lnum].color[0..2]` — the rest of its distance/falloff math is unchanged.

Lightstyles already modulate brightness via `lightadj[]`; with `.lit` the colour stays fixed and brightness flickers as it always has. No lightstyle logic changes.

## Block writer

Per-texel inner loop in `R_DrawSurfaceBlock8_mip0_rgb`:

```c
unsigned tex = pbasesource[s_offset];     // palette index 0..255
unsigned r = ((lightR >> 16) * basepal_r[tex]) >> RGB_SHIFT;
unsigned g = ((lightG >> 16) * basepal_g[tex]) >> RGB_SHIFT;
unsigned b = ((lightB >> 16) * basepal_b[tex]) >> RGB_SHIFT;
if (r > 63) r = 63;
if (g > 63) g = 63;
if (b > 63) b = 63;
*dest++ = rgbtable[(r << 12) | (g << 6) | b];
```

`lightR`, `lightG`, `lightB` are interpolated 16.16 fixed-point values from the four corner channels of the current block, mirroring how the mono writer interpolates a single `lightleft`/`lightright` pair. `>> 16` takes the integer part (0..63 after the `R_BuildLightMap_RGB` shift). `basepal_r[tex]` is 0..255.

`RGB_SHIFT` starts at **8** (so `(63 * 255) >> 8 ≈ 62`, mapping the full light range into a 6-bit channel before LUT lookup). Final value is calibrated by comparing mid-tone output of the mono and RGB paths on a flat wall via the `r_coloredlight 0/1` toggle. The clamping `if`s handle additive dlight contributions overshooting 63; on hot paths the compiler should turn them into `min` intrinsics.

The four mip variants (`mip0..mip3`) differ only in block size and step counts, matching the existing mono variants. They share an inline body via a macro or `static inline` helper, kept consistent with the upstream's `#include` pattern for `surf8.s` if a clean asm-free C path needs duplication.

## Dynamic light colours

`dlight_t`:

```c
typedef struct {
    vec3_t origin;
    float  radius;
    float  die, decay, minlight;
    int    key;
    vec3_t color;     // NEW — {1,1,1} default
#ifdef QUAKE2
    qboolean dark;
#endif
} dlight_t;
```

`CL_AllocDlight` clears `color` to `{1,1,1}`. The mono renderer ignores `color`; behaviour on the mono path is bit-for-bit unchanged.

`cl_dlight_colors.h`:

```c
static const vec3_t DLIGHT_COLOR_MUZZLE     = {1.00f, 0.85f, 0.45f}; // warm yellow
static const vec3_t DLIGHT_COLOR_ROCKET     = {1.00f, 0.60f, 0.20f}; // orange
static const vec3_t DLIGHT_COLOR_EXPLOSION  = {1.00f, 0.50f, 0.20f}; // orange-red
static const vec3_t DLIGHT_COLOR_LIGHTNING  = {0.60f, 0.70f, 1.00f}; // pale blue
static const vec3_t DLIGHT_COLOR_TORCH      = {1.00f, 0.80f, 0.40f}; // warm
static const vec3_t DLIGHT_COLOR_WHITE      = {1.00f, 1.00f, 1.00f};
```

Each existing dlight allocation in `cl_main.c` / `cl_tent.c` is followed by a `VectorCopy(DLIGHT_COLOR_*, dl->color)`. The constants live in a single header so tuning is easy.

## Phase 8 compatibility

The Phase 8 immersive-sim design (`docs/superpowers/specs/2026-05-04-immersive-sim-systems-design.md`) defines `Sample_Lightmap(vec3_t origin) → int 0..255` for the AI light tier. Gameplay must not change.

Implementation: when `rgblightdata` is present, the helper computes `max(R, G, B)` of the sampled RGB lightmap value (closer to perceived brightness than the channel mean and matches the intent of "is this spot lit?"); when absent, returns the existing mono value verbatim. Signature and value range stay identical, no `engine_api_t` change, no `GAME_API_VERSION` bump.

## Cvars

```c
cvar_t r_coloredlight    = {"r_coloredlight",    "1", true};   // archived
cvar_t r_colored_dlights = {"r_colored_dlights", "1", true};   // archived
```

- `r_coloredlight 0` forces the mono path regardless of `.lit` data — useful for A/B comparison and as a kill-switch.
- `r_colored_dlights 0` strips dlight colour (forces white) without disabling static `.lit`. Useful for isolating dlight-only contribution while debugging.
- Toggling either cvar mid-game does not require a map reload; the surface cache flushes naturally as views change. If a stale-cache visual is observed, `vid_restart` (or equivalent) is the documented fallback.

## Error handling

- Missing `maps/<name>.lit` → no message; mono path is used. This is the common case for most maps.
- Present but malformed `.lit` (bad magic, wrong version, sample count mismatch) → `Con_Printf` warning, mono path is used. Never fatal.
- Missing `id1/rgbtable.lmp` → silent rebuild and write. ~250 ms first-run cost only.
- Corrupt `id1/rgbtable.lmp` (bad magic/version/hash) → silent rebuild and overwrite.
- `r_fullbright 1` → RGB path returns zeros from `R_BuildLightMap_RGB`, matching the mono path's early-out; the block writer's clamp then produces palette index 0 (typically dark) which is intentional, mirroring mono behaviour.

## Performance

Per-texel block writer cost:

- Mono: 1 shift + 1 LUT read.
- RGB:  3 multiplies + 3 shifts + 3 clamps + 1 LUT read.

The block writer fires per surface-cache build, not per pixel of the framebuffer; a surface is built once and re-sampled across many spans. At 320×200 with typical map visibility the additional muls amount to single-digit megaops per second. Expected frametime hit: <10% at native res, scaling linearly with resolution. `host_speeds 1` baselines (mono vs RGB, native and 2× scaled) on `e1m1` start-area view will be recorded in the implementation plan and re-checked at the end.

The 256 KB LUT fits in L2 on every realistic target. The three channel slices (`basepal_r/g/b`, 256 bytes each) easily fit in L1.

## Smoke test plan

No test suite exists. Verification is in-engine:

1. `zig build run -- +map e1m1` with **no** `.lit` present → pixel-identical to current master. Confirms the mono fallback is untouched.
2. Drop a real `e1m1.lit` (community pack; exact source noted in commit) into `id1/maps/`. Walk through the start area, slipgate room, and first lava pool. Expect warm-orange wall tint near lava, blue near the slipgate, cooler tones in metal corridors.
3. Fire the shotgun in a dim room → muzzle flash visibly yellow on nearby walls, single-frame. Fire a grenade in open space → explosion lights walls orange. Lightning gun (give yourself one via `impulse 9`) → blue dlight on walls.
4. `r_coloredlight 0` mid-game → classic Quake lighting restored as the surface cache turns over (≤1 s of camera movement).
5. `r_coloredlight 1; r_colored_dlights 0` → static colour intact, dlights white. Useful for confirming dlight colour wiring in isolation.
6. `r_fullbright 1` → still fullbright; no crashes; no banding artefacts.
7. `host_speeds 1` baselines recorded mono vs RGB on a fixed view at native and 2× scaled.

## Out of scope (v2+)

- **In-engine `.lit` baker** from light-entity `_color` fields — separate, larger spec.
- **Dithered LUT** to mask banding — polish pass once v1 ships.
- **Coloured liquid/sky surfaces** — lava/water/sky use `D_DrawTurbulent8Span` / sky paths, neither of which goes through the surface-cache writer; needs its own design.
- **Coloured model lighting** — alias models / sprites / particles still use `R_LightPoint` (mono). Visible mismatch but acceptable for v1.
- **Saving the coloured-light state across saves** — irrelevant; `.lit` is map-keyed and re-loaded automatically.

## Implementation order (sketch — full plan in a separate writing-plans pass)

1. `r_lut.c` + LUT bake + disk cache; standalone, no rendering impact. Smoke-test via a console command that prints `rgbtable[(r<<12)|(g<<6)|b]` for a few RGBs.
2. `Mod_LoadLITFile` plus `model_t.rgblightdata` / `msurface_t.rgb_samples` plumbing. Still no rendering change. Smoke-test: console command reporting the first few RGB samples of the loaded map.
3. `R_BuildLightMap_RGB` + the four `R_DrawSurfaceBlock8_mip*_rgb` writers + the dispatch in `R_DrawSurface` + `r_coloredlight` cvar. First visible result.
4. `dlight_t.color` + `R_AddDynamicLights_RGB` + per-site colour assignments + `r_colored_dlights` cvar.
5. `Sample_Lightmap` returns `max(R,G,B)` when RGB data is present.
6. Smoke-test pass, `host_speeds` baselines, commit.

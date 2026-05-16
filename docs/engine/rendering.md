# Rendering

The renderer is the original 1996 software rasteriser, unmodified at its
core. Everything is drawn into an 8-bit indexed-colour framebuffer; SDL3 is
only used at the very end of the pipeline to expand-and-present.

## Fixed pipeline

```
              vid.buffer (320×200, 8-bit palette indices)
                       │
       SCR_UpdateScreen draws into this every frame:
       ├── R_RenderView (BSP traversal, surface cache, edge raster)
       ├── R_DrawEntitiesOnList (alias models, sprites)
       ├── R_DrawParticles
       ├── Draw_*  (HUD, console, status bar)
       └── ImGui   (when active; rendered last over the framebuffer)
                       │
                       ▼
       VID_Update (sdlquake/platform/vid_sdl.c)
                       │
       For each pixel:  argb = d_8to24table[vid.buffer[i]]
                       │
                       ▼
       SDL_Texture (SDL_PIXELFORMAT_ARGB8888, TEXTUREACCESS_STREAMING)
                       │
                       ▼
       SDL_SetRenderLogicalPresentation(..., INTEGER_SCALE)
       SDL_RenderTexture → SDL_RenderPresent
```

The logical-presentation call is what keeps pixels crisp at any window size —
SDL upscales 320×200 by the largest integer that fits and letterboxes the
rest.

## Palette and the 8-to-24 table

The palette is 256 × 24-bit RGB, loaded from `gfx/palette.lmp` (768 bytes).
At video-init time the engine builds `d_8to24table[256]` — each entry is a
32-bit ARGB value that `VID_Update` looks up per pixel.

Per palette colour the engine also builds:

- `host_basepal[768]` — raw RGB triples.
- `host_colormap[64 × 256]` — for every (light-level, palette-index) pair,
  the palette index that best matches `basepal[idx] * (light/64)`. This is
  the lookup table the software lightmap path uses.

Coloured lighting (see below) needs a second table, `rgbtable[]`, built at
startup in `r_lut.c`. Given a 6-bits-per-channel RGB value, `rgbtable[]`
returns the nearest palette index. This is what the RGB surface writers
quantise into.

## Lightmaps

BSP lump 8 (`LUMP_LIGHTING`) holds the per-surface lightmap: one byte per
luxel, possibly modulated by up to four light-styles (animated lights like
`light_flicker`). At surface-cache build time the engine sums the four styles
into a single 8.8-fixed-point light value per luxel (`blocklights[]`) and
runs the bilinear-interpolating writer to fill the cached surface tile.

Dynamic lights add their contribution to `blocklights[]` before the writer
runs, then dirty the cache entry so it gets rebuilt next time it's visible.

### Coloured lighting (`.lit` sidecars)

Loaded by `Mod_LoadLITFile` in `engine_src/model.c`. Format (FitzQuake /
QuakeSpasm convention):

```
char magic[4] = "QLIT"
int  version  = 1                 // little-endian
byte rgb[3 * mono_lightmap_size]  // RGB per luxel, matched 1:1 to BSP lump 8
```

If the file is absent, malformed, or size-mismatched, we silently stay mono.
When present, `loadmodel->rgblightdata` is non-NULL and the surface writer
switches to `R_DrawSurfaceBlock8_mip*_rgb` (`r_surf_rgb.c`). That path:

1. Reads three corner light values per channel into `blocklights_rgb`.
2. Bilinearly interpolates per texel.
3. Computes `basepal[texel] * (R, G, B)`.
4. Quantises to 6 bits per channel.
5. Looks up the nearest palette index in `rgbtable[]`.

Five non-obvious gotchas baked into the implementation (see
`docs/superpowers/specs/2026-05-13-coloured-lighting-design.md` for context):

- **Gamma**: lit files store linear RGB; the mono path is in palette-gamma
  space. The RGB writer applies `host_colormap`'s gamma curve when present.
- **Channel inversion**: some lit-tooling writes BGR; the loader does not
  swap, the level designer must ship the right byte order.
- **Underflow**: light-style subtraction can push luxels negative; the
  writer clamps to 0 before quantising.
- **Cache invalidation**: any dlight touching a coloured surface forces the
  RGB writer; the mono path doesn't get re-run mid-frame.
- **Dlight RGB scale**: dynamic lights write into both `blocklights` and
  `blocklights_rgb` — the RGB scale matches `cl_dlights[].color` so coloured
  dlights tint correctly.

## Dynamic lights

`MAX_DLIGHTS` is **512** (originally 32). The mask of dlights touching a
surface (`dlightbits_t`) is an 8-element `uint64_t` array — adding 64 more
dlights is a one-line array-size bump.

`cl_static_dlights` is a count of "persistent map-light" dlights packed at
the **high end** of `cl_dlights[]`. Set the cvar `r_maplights_as_dlights 1`
to spawn every map `light*` entity as a persistent dlight at server-info
parse — useful for debug and for ImGui's light-tier visualisation. They use
the surface-cache RGB path, so coloured lights tint correctly.

The runtime dlight slots (0 .. MAX_DLIGHTS − cl_static_dlights) are recycled
LRU-style for temporary effects (rocket exhaust, muzzle flash, …).

## Decals

`engine_src/r_decals.c` adds blood splats and similar projected decals onto
BSP surfaces. The implementation builds a clipped polygon over the affected
faces and writes a darkened palette-tinted version of the underlying texture
back into the surface cache. Decals fade with `r_decals_fade` and can be
disabled via `r_decals 0`. The blood-pool decal beneath dying monsters is
spawned from `game.dll` via `engine_api->spawn_blood_pool`.

## Model interpolation

Alias models (`.mdl`) animate by interpolating vertex positions between
keyframes. Driven by `r_lerpmodels` (default 1). Two failure modes that
required workarounds:

- `U_NOLERP` / `forcelink`: `MOVETYPE_STEP` monsters set `forcelink` every
  packet, which would defeat lerping if used as a generic "snap" signal.
  The renderer instead snaps only when the alias-model pointer changes.
  (See `memory/project_u_nolerp_gotcha.md`.)
- The lerp time is taken from the entity's `msg_time` history rather than
  `cl.time`, so server framerate jitter doesn't show up as visible stutter.

## Particles

`r_part.c`. Up to `MAX_PARTICLES` (default 2048) live in a free list.
`R_RunParticleEffect` masks `color & ~7` — colours are taken from the
Quake palette in 8-wide blocks. Common gotcha: QuakeC names like
`COLOR_LIGHTNING` look like they should be electric blue but actually
resolve to a blood-red 8-wide block. The convention is to think in
**block index = color >> 3** when emitting effects.

## Screenshots & introspection

`VID_SaveScreenshotPNG` and `VID_SamplePixel` in `platform/vid_sdl.c` are
the MCP server's `screenshot` and `sample_pixel` tools. Both walk
`vid.buffer`, look up `d_8to24table`, and write PNG via `stb_image_write`.
The 24→8 table reads were swapped (R/B inverted) for ~3 years until fixed
on 2026-05-14 — on-screen rendering was always correct, only the
introspection lied. Don't reintroduce the byte order: ARGB8888 stores
`B`, `G`, `R`, `A` in memory order (little-endian).

## ImGui dev overlay

Dear ImGui is layered on top of the software framebuffer using
`imgui_impl_sdlrenderer3` — it renders into the same `SDL_Renderer` after
the Quake framebuffer texture has been blitted. Toggle the global panel
with backtick + a mod, or feature panels with F1–F12. The F12 "Debug
Render" panel exposes `r_drawpaths`, AI bboxes, the navmesh, sim debug
toggles, decals, and renderer flags as one-row checkboxes
(`engine/imgui_debug_panel.c`).

## Debug draw

`engine/debug_lines.c` plus `engine/r_debugdraw.c` accept submission from
the game DLL via `engine_api->SV_DebugLine(start, end, color, ztest)`. The
list clears every frame, so a line must be re-submitted each frame to
stay visible — that's why the AI / navmesh / wind / patrol overlays all
look like immediate-mode UIs from the game side.

`ztest != 0` clips lines against the world depth buffer (only the visible
part draws); `ztest == 0` is xray. Sim debug cvars use `Sim_DebugZTest()`
to encode `0 = off`, `1 = ztest`, `2 = xray` as a tri-state.

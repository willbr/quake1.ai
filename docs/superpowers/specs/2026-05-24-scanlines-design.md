# Scanlines rendering option — design

**Date:** 2026-05-24
**Status:** Draft
**Touches:** `sdlquake/platform/vid_sdl.c`

## Goal

Add an optional CRT-style scanline overlay to the SDL3 software-renderer
output. The scanlines are applied at the window's *physical* pixel grid
(not the engine's internal framebuffer), so they look like real CRT
scanlines at any window size and remain independent of the existing
`vid_supersample` / render-scale plumbing.

## User-visible surface

Three cvars (all persisted via the existing config-write path in
`vid_sdl.c`):

| cvar | values | default | meaning |
|---|---|---|---|
| `vid_scanlines` | 0 / 1 | 0 | master enable |
| `vid_scanline_intensity` | 0.0 – 1.0 | 0.5 | how dark the dark bands are; 0 = invisible, 1 = pure black |
| `vid_scanline_size` | 1 / 2 / 3 | 1 | thickness of each band in physical pixels; pattern is `size` dark rows, then `size` bright rows, repeating (always 50/50 duty cycle) |

Three new rows are added to the Video Options menu (`M_Video_Draw` /
`M_Video_Key` in `vid_sdl.c`), placed immediately below the existing
Supersample group and above the "Save" cursor slot:

```
Scanlines      [On] / [Off]
Intensity      [25%] / [50%] / [75%] / [100%]
Size           [1px] / [2px] / [3px]
```

The Intensity stepper snaps to four buckets (0.25 / 0.50 / 0.75 / 1.00)
so the menu UI stays a discrete left/right stepper like the existing
supersample/render-scale/window-scale rows. The underlying cvar remains
a float; setting it from the console accepts any value in `[0,1]`.

Left/right cycles the value, Enter on Save persists via `Cvar_SetValue`
and writes to config just like the existing video rows do.

The cursor slot index of the "Save" row shifts up by 3 to accommodate
the new rows. The existing supersample / render scale / window scale
slot constants are reorganised so the additions are contiguous.

## Rendering

A new static `SDL_Texture *sdl_scanline_tex` holds the scanline pattern.

- **Layout:** `1 × output_h` pixels, `SDL_PIXELFORMAT_RGBA8888`,
  `SDL_TEXTUREACCESS_STREAMING`. A single column is enough; SDL stretches
  it horizontally when drawn full-screen, so the memory cost is trivial
  regardless of window width.
- **Contents:** for each physical row `y` in `[0, output_h)`,
  let `band = (y / size) % 2`. If `band == 0` (dark row), write
  `(d, d, d, 255)` where `d = (uint8_t)((1.0f - intensity) * 255.0f + 0.5f)`.
  If `band == 1` (bright row), write `(255, 255, 255, 255)`.
- **Blend:** `SDL_SetTextureBlendMode(sdl_scanline_tex, SDL_BLENDMODE_MOD)`.
  Multiplicative blend means bright rows are a no-op (×1) and dark rows
  multiply the underlying framebuffer toward black by the chosen
  intensity.

### Frame integration

Inside `VID_Update`, after the existing
`SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL)` call and
before `SDL_RenderPresent`:

```
if (vid_scanlines.value != 0.0f) {
    Scanline_Ensure(); // rebuild texture if size/intensity/window changed
    SDL_SetRenderLogicalPresentation(sdl_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_RenderTexture(sdl_renderer, sdl_scanline_tex, NULL, NULL);
    SDL_SetRenderLogicalPresentation(sdl_renderer,
                                     vid_render_w, vid_render_h,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
}
```

We must drop logical presentation while drawing the overlay so the
1-column texture maps to physical pixels rather than logical ones, then
restore it so subsequent frames render the framebuffer the same way.

### Lazy regeneration

`Scanline_Ensure()` is called only when scanlines are enabled. It
compares cached statics (`last_output_w`, `last_output_h`,
`last_size`, `last_intensity`) against the current values from
`SDL_GetRenderOutputSize` and the cvars. If anything changed (or the
texture doesn't exist yet), it:

1. Destroys any existing `sdl_scanline_tex`.
2. Creates a new one at `1 × output_h`.
3. Locks it, fills the column per the rule above, unlocks.
4. Sets blend mode to `SDL_BLENDMODE_MOD`.
5. Updates the cached statics.

Per-frame cost when nothing changed: four float/int comparisons.

### Persistence

Three additional `sscanf` lines in the config-load block (matching the
existing pattern around `vid_sdl.c:387`):

```
else if (sscanf(line, "vid_scanlines \"%f", &v) == 1)          Cvar_SetValue("vid_scanlines", v);
else if (sscanf(line, "vid_scanline_intensity \"%f", &v) == 1) Cvar_SetValue("vid_scanline_intensity", v);
else if (sscanf(line, "vid_scanline_size \"%f", &v) == 1)      Cvar_SetValue("vid_scanline_size", v);
```

The three cvars are declared with the `archive` flag set so they are
auto-written to `config.cfg` on shutdown like other vid cvars.

## Out of scope

Explicit non-goals for this change:

- Phosphor curves, gamma-correct blending, scanline aspect-ratio
  correction, shadow masks, NTSC bleed — those are CRT-shader projects.
- Per-axis scanlines (vertical bands as well as horizontal). Quake's
  CRT analogue is horizontal lines only.
- Tying scanline size to render scale or supersample. The three knobs
  are intentionally independent.

## Verification

No automated tests exist for the video path. Verification is visual:

1. `zig build run` — title screen shows no scanlines (default off).
2. Open Video Options, toggle Scanlines on — alternating 1px dark/bright
   bands appear over everything (HUD, menu, world).
3. Sweep Size 1 → 2 → 3 — bands thicken proportionally on the physical
   pixel grid; integer scaling of the framebuffer is unaffected.
4. Sweep Intensity 25% → 100% — dark bands deepen monotonically;
   100% gives pure black bands.
5. Resize the window — scanlines re-fit to the new physical height on
   the next frame, still 1 physical pixel per band at size=1.
6. Combine with `vid_supersample 2` and a window scale — overlay still
   matches physical pixels and is unaffected by the supersample factor.
7. Quit and relaunch — settings persist via `config.cfg`.

# Supersample (SSAA)

## Goal

Add a `vid_supersample` option to the renderer that renders the frame at a higher resolution than the chosen render-res, then box-filter downsamples it back to render-res before the existing nearest-upscale-to-window step. The chunky render-res aesthetic is preserved; sub-pixel detail in the world (texture shimmer, sloped-edge aliasing, particle thinness) gets smoothed out.

User-described flow: **supersample → render res → resize for window**.

## User-facing surface

One new archived cvar:

| Cvar | Default | Range | Meaning |
|---|---|---|---|
| `vid_supersample` | `1` | `1..4` | Multiplier on render-res. `1` = today's behavior. `N` = render at `(render_w·N) × (render_h·N)`, box-average down to `render_w × render_h`. |

Combined cap: `vid_scale · vid_supersample ≤ 8`. Larger combinations clamp to 8 at apply time (e.g. picking SS=4 while `vid_scale=4` silently uses SS=2). The auto-fit init logic for `vid_scale` does not consider SS.

### Video Options menu

The existing menu (`VID_MenuDraw` in `sdlquake/platform/vid_sdl.c`) adds a third group above the existing two:

```
        VIDEO OPTIONS

  Supersample
    1x  (off)
    2x
    3x
    4x

  Render Resolution
    1x  320x200
    ...

  Window Size
    ...

  Save Window Pos & Size
```

Cursor layout extends from 9 items to 13: 4 SS rows, 4 render-res rows, 4 window rows, save row. Selecting a SS row sets `vid_supersample` and re-runs the resolution-apply path that today lives in `VID_ApplyScale`.

The label for the active SS row uses the existing `M_PrintWhite` highlight (same as the other groups). At SS=1 the label reads `1x  (off)` — explicit visual signal that supersampling is disabled.

## Render pipeline change

### Today

```
software renderer ─► vid.buffer (render-res, 8bpp)
                  ─► palette-expand into SDL_Texture (render-res, ARGB)
                  ─► INTEGER_SCALE nearest-upscale ─► window
```

### With SS

```
software renderer ─► vid.buffer (SUPER-res, 8bpp; vid.width = render_w·SS, vid.height = render_h·SS)
                  ─► palette-expand + box-average SS² block ─► SDL_Texture (render-res, ARGB)
                  ─► INTEGER_SCALE nearest-upscale ─► window
```

The texture stays at render-res. The `SDL_SetRenderLogicalPresentation(..., INTEGER_SCALE)` call stays untouched. The only change to the present step is in the `for (y...) for (x...)` palette-expand loop inside `VID_Update` — instead of one lookup per output pixel, it averages SS² lookups.

`vid_palette_id[]` (the per-pixel palette-slot tag, used to mix Quake/Doom palettes when a Doom weapon sprite overlaps text) sits next to the framebuffer at super-res. The averaging loop reads both `vid.buffer[y·super_w + x]` and `vid_palette_id[y·super_w + x]` for each of the SS² source pixels. Two source pixels with the same palette slot can be averaged together; two with different slots fall back to picking the top-left source pixel (mixing across palettes would produce a meaningless intermediate color).

### Engine-side effects of vid.width/vid.height = super

The engine reads `vid.width`/`vid.height` to compute the world projection (`R_SetupFrame` → `vid.recalc_refdef`) and to position HUD/sbar elements. Both naturally scale by SS:

- **World renderer:** wider viewport, more pixels rendered. This is the entire point — gives us sub-pixel sample density.
- **HUD/sbar/menu:** they position themselves at `vid.width/2 - …` etc., so they end up at the proportionally-correct screen location. The element bitmaps themselves stay at their native pixel size, so they take up `1/SS` of the screen relative to SS=1. This is the same behavior you already see today when bumping `vid_scale` — the HUD gets smaller at 4x than at 1x. SS reuses that path.
- **Console text:** uses `vid.conwidth/conheight`. These also become super-res, so console text also shrinks. Matches current `vid_scale` behavior.

The box-downsample preserves crisp HUD pixels when each HUD pixel happens to be SS-aligned in the super buffer (full SS×SS solid block → identical render-res pixel). When the HUD layout produces non-aligned pixels (likely, since HUD draws are pixel-coordinate, not SS-aligned), the downsample softens HUD edges by a fractional pixel. We accept this — the alternative (separate HUD pass at render-res) is a much larger surgery for marginal gain on already-shrunken HUD elements.

## Memory budget

The current code reserves hunk space for `VID_RENDER_MAX_W × VID_RENDER_MAX_H = 1280 × 800` (z-buffer + surface cache) and a 1 MB `vid_buffer[]` static. SS=2 at vid_scale=4 demands 2560×1600.

New caps:

```c
#define VID_RENDER_MAX_W 2560
#define VID_RENDER_MAX_H 1600
```

| Buffer | Today (1280·800) | After (2560·1600) | Delta |
|---|---|---|---|
| `vid_buffer[]` (8bpp) | 1.0 MB | 4.0 MB | +3.0 MB |
| `vid_palette_id[]` (8bpp) | 1.0 MB | 4.0 MB | +3.0 MB |
| z-buffer (hunk) | 2.0 MB | 8.0 MB | +6.0 MB |
| surface cache (hunk) | ~3 MB | ~12 MB | +9.0 MB |
| **total** | ~7 MB | ~28 MB | **+21 MB** |

The `vid_buffer[]` and `vid_palette_id[]` arrays are `static byte` in `vid_sdl.c` — bumping the size bumps the BSS. The hunk side already pre-allocates at max-res, so increasing the constants just makes that reservation bigger.

If the default hunk size becomes too small, `host_initialized` paths will OOM during `D_InitCaches`. Need to verify the default `-mem` value covers ~28 MB plus the rest of the engine's hunk usage. (Empirically the engine boots fine with much more than this today on 64-bit builds, but the spec should call out the check.)

## Implementation order

1. Add `vid_supersample` cvar registration in `VID_Init` and the config-preload helper.
2. Bump `VID_RENDER_MAX_W/H` to 2560/1600. Verify build runs at max combined res without OOM.
3. Factor the "compute and apply render-res-derived state" out of `VID_ApplyScale` so both SS changes and render-res changes call the same code (single source of truth for `vid.width = render_w · ss`, texture (re)create at render-res, hunk re-init, `vid.recalc_refdef = 1`).
4. Modify `VID_Update`'s expand loop to box-average SS² source pixels per output pixel. Same-palette-slot pixels average; mixed slots fall back to top-left source.
5. Extend `VID_MenuDraw` / `VID_MenuKey` with the third group. Update `VID_MENU_ITEMS`, `VID_MENU_SAVE_POS`, and the cursor → action mapping in `VID_MenuKey`.
6. Smoke-test: launch with `+map e1m1`, walk to a sloped wall, verify the world edge anti-aliases at SS=2 and the HUD stays positioned correctly; verify SS=1 is byte-identical to today (no regression).

## Out of scope

- Separate render-res for world vs. HUD (would let HUD stay crisp under SS but is a large engine restructure — defer until someone actually wants it).
- Non-integer SS (e.g. 1.5x). The whole pipeline is integer-snap from end to end; non-integer SS would require a different downsample kernel.
- Linear-light or gamma-correct averaging. Quake's palette isn't linear, but averaging in palette-RGB is what every Quake software-renderer SSAA implementation does and matches user expectation; the spec doesn't add a gamma step.
- A "supersample only the world, not the HUD/sbar" mode. The picked design intentionally rides on the existing "everything is at vid.width" assumption.

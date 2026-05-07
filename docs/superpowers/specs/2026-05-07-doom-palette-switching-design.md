# Doom palette switching — design

Date: 2026-05-07
Status: design (awaiting plan)
Phase: 6 (port Wolf3D & Doom1 guns)

## Problem

The Phase 6 asset extractor (`tools/extract_phase6/`) currently nearest-color remaps every Doom pixel into Quake's `gfx/palette.lmp` at build time. Quake's palette is heavy on browns and lacks the cleaner reds, greys, and skin tones Doom uses, so the Doom pistol viewmodel ships with muddy, off-key colours that look noticeably worse than the original. Doom assets need to render with Doom's own palette while everything else continues using Quake's.

Scope: all Doom-derived assets (the Phase 6 viewmodels today plus future Doom monsters/items/HUD). The architecture must extend cleanly to world-space sprites later, but only the screen-space viewmodel path is wired up in this spec.

Non-goals: Wolf3D palette switching (slot reserved, not wired); fixing Quake's own palette; any GL renderer concerns; runtime palette editing.

## Approach

A parallel 1-byte-per-pixel `vid_palette_id` buffer rides alongside `vid_buffer`. Each pixel carries an index into a small table of palette LUTs (slot 0 = Quake, slot 1 = Doom, slot 2 reserved for Wolf3D). At present time `VID_Update` expands each pixel through `lut[palette_id[i]][buffer[i]]` instead of through a single global LUT.

The buffer is zero-cleared each frame, so every existing renderer path defaults to slot 0 (Quake) with no change. Only the new Doom blit code opts in by writing slot 1 alongside its pixel writes. The extractor stops nearest-color remapping Doom sprites and instead emits a `gfx/palette_doom.lmp` companion to Quake's `palette.lmp`; the engine loads both at startup and builds two LUTs.

## Architecture

### Buffers

`sdlquake/platform/vid_sdl.c` owns:

- `vid_buffer[VID_WIDTH * VID_HEIGHT]` — paletted indices (existing).
- `vid_palette_id[VID_WIDTH * VID_HEIGHT]` — palette slot per pixel (new).
- `d_8to24table[NUM_PALETTES][256]` — promoted from `[256]` to `[NUM_PALETTES][256]`. The engine's `extern unsigned d_8to24table[]` declarations remain valid: `d_8to24table` decays to `unsigned*` pointing at slot 0, and engine code that indexes `d_8to24table[i]` continues to read slot 0 (Quake), which is what it should do — engine code never tags pixels with a non-Quake palette.

`NUM_PALETTES = 3` (Quake, Doom, reserved Wolf3D).

### Per-frame lifecycle

1. Start of `VID_Update`: `memset(vid_palette_id, 0, sizeof(vid_palette_id))`.
2. Engine renders the world, status bar, HUD, etc. into `vid_buffer` as today; `vid_palette_id` stays 0 across these writes.
3. The viewmodel blit path detects a Doom-palette sprite (see Identification), and for each opaque pixel writes the index to `vid_buffer[i]` and slot `1` to `vid_palette_id[i]`.
4. The expand loop reads both buffers and indexes `d_8to24table[pal][idx]`.

### Expand loop

```c
unsigned (*lut)[256] = d_8to24table;
for (int y = 0; y < h; y++) {
    byte *src     = vid.buffer       + y * vid.rowbytes;
    byte *pal_src = vid_palette_id   + y * VID_WIDTH;     // (rowbytes == VID_WIDTH at fixed res)
    Uint32 *dst   = (Uint32*)((Uint8*)pixels + y * pitch);
    for (int x = 0; x < w; x++)
        dst[x] = lut[pal_src[x]][src[x]];
}
```

Cost: one extra byte fetch per pixel. The expand step is already memory-bound; this stays comfortably so.

### Palette LUT construction

`VID_SetPalette` (Quake's existing entry point) continues building slot 0. A new private helper `vid_load_aux_palette(slot, "gfx/palette_doom.lmp")` is called once during `VID_Init` after the main palette is built. It uses `COM_LoadFile` to read the 768-byte LMP and fills `d_8to24table[slot][0..255]`. Slot `[255]` is masked to alpha 0 the same way slot 0 already does (`d_8to24table[slot][255] &= 0x00ffffff`).

If `gfx/palette_doom.lmp` is missing (player has not run `zig build extract`), slot 1 is zero-filled. This is reachable only if the player also lacks the Doom sprites — the engine will refuse to load `progs/v_doom*.spr` in that case, so a black Doom palette is unreachable in practice. We'll log a `Con_Printf` warning if the LMP is missing but a Doom sprite later loads.

## Identification — which sprites use which palette

Tag at load time, not draw time. The Doom blit reads the tag from the model.

### Where the tag lives

A new field on `model_t`:

```c
typedef struct model_s {
    ...
    byte palette_id;   /* 0 = Quake (default), 1 = Doom, 2 = Wolf3D */
    ...
} model_t;
```

This requires editing `Quake-master/WinQuake/model.h`. The CLAUDE.md "never modify Quake-master" rule has already been relaxed for Phase 6 (see `r_sprite.c` `R_DrawViewModelSprite`), so this is consistent with existing precedent. Adding a single byte at the end of the struct does not change any existing offset usage.

### Filename-prefix detection

In `Mod_LoadSpriteModel` (`Quake-master/WinQuake/model.c`), after the sprite loads:

```c
if (!strncmp(mod->name, "progs/v_doom", 12))
    mod->palette_id = 1;
else if (!strncmp(mod->name, "progs/v_wolf", 12))
    mod->palette_id = 2;
else
    mod->palette_id = 0;   /* implicit, but explicit for clarity */
```

The naming convention is already established and stable: `manifest.zig` writes `id1/progs/v_doom*.spr` and `id1/progs/v_wolf*.spr`.

This is also a small edit to `Quake-master/`. Same justification as the field add.

### Wiring through the blit

`R_BlitSpriteScreen` in `r_sprite.c` already takes a `mspriteframe_t *frame`. We extend its signature to take a `byte palette_id` argument, and `R_DrawViewModelSprite` passes `e->model->palette_id`. For each opaque pixel the function writes:

```c
dest[u] = tbyte;
pal_dest[u] = palette_id;
```

`pal_dest` is `vid_palette_id + sy * VID_WIDTH + sx` advanced one row at a time, mirroring how `dest` advances along `vid.rowbytes`.

The transparent-pixel skip stays the same: when a pixel is transparent we touch neither `vid_buffer` nor `vid_palette_id` at that location, so both keep whatever the previous writer left there. That is automatically correct — the underlying pixel's index and palette tag were written together by whoever drew it, so the pair stays consistent regardless of whether the prior writer was Quake (tag 0) or another Doom blit (tag 1).

`vid_palette_id` is exported from `vid_sdl.c` via a new declaration in `vid.h`-equivalent. Since we don't modify Quake's `vid.h`, the export goes in `sdlquake/platform/winquake.h` (which already shadows the original platform header) or a new `sdlquake/platform/vid_palette.h` included from `r_sprite.c` and `vid_sdl.c`. Implementation choice during the plan; the spec doesn't pin it.

## Render order — the gotcha and how we handle it

When a Quake-palette write lands on a pixel that was previously tagged with `palette_id = 1`, the tag is now stale: the pixel's index belongs to Quake's palette but the LUT lookup will go through Doom's palette and produce wrong colours.

For Phase 6 today this does not happen, because:

- The only Doom-tagged content is the viewmodel sprite blit.
- The viewmodel is anchored inside `r_refdef.vrect` (commit 044a7ea), which sits above the status bar.
- Nothing else draws on top of the viewmodel later in the frame.

We rely on this invariant and document it with a comment in `R_DrawViewModelSprite` and at the `vid_palette_id` declaration in `vid_sdl.c`:

> Invariant: every renderer path that writes a non-zero palette_id must do so AFTER any Quake-palette writes that could overlap. Currently only the screen-space viewmodel blit writes a non-zero palette_id, and it sits inside r_refdef.vrect above the status bar, so the invariant holds trivially.

When world-space Doom sprites are added (Phase D/E), the span blitter (`d_sprite.c` / `D_DrawSprite`) will need palette-aware tagging — the same pattern (write `vid_buffer` and `vid_palette_id` together for each opaque pixel), but plumbed through the span descriptor. Out of scope here; this spec only commits to making the architecture support it.

## Extractor changes

`tools/extract_phase6/manifest.zig` and `quake_spr.zig` currently nearest-color remap every Doom pixel via `palette_mod.buildLut(quake_pal)`. New behaviour:

- **Doom sprites** (the `doom_*_lumps` manifest entries): skip the Quake remap entirely. Write the source 8-bit indices straight into the SP1 sprite. Transparent pixels stay byte 255 (post-extraction convention; Doom WAD column-post format already encodes transparency as gaps, which the extractor converts to 255 today).
- **Wolf3D sprites**: unchanged for this spec. They remain Quake-remapped until/unless we wire palette slot 2.
- **New extractor step**: read `PLAYPAL` from `DOOM1.WAD` and write the first 768 bytes (palette 0 of the 14 PLAYPAL banks) to `id1/gfx/palette_doom.lmp`. Format: 256 RGB triples, byte-for-byte the same layout as Quake's `gfx/palette.lmp`.

`palette_doom.lmp` is `.gitignore`d alongside the sprite outputs (it's reproducible from `DOOM1.WAD`).

## Components and ownership

| Component | File(s) | Status |
| --- | --- | --- |
| Two-LUT framebuffer expand | `sdlquake/platform/vid_sdl.c` | new code, edits existing |
| Aux palette loader | `sdlquake/platform/vid_sdl.c` | new helper |
| `vid_palette_id` declaration | `sdlquake/platform/winquake.h` (or new `vid_palette.h`) | new |
| `model_t.palette_id` field | `Quake-master/WinQuake/model.h` | small edit |
| Sprite-load palette tagging | `Quake-master/WinQuake/model.c` `Mod_LoadSpriteModel` | small edit |
| Doom-aware viewmodel blit | `Quake-master/WinQuake/r_sprite.c` `R_BlitSpriteScreen`, `R_DrawViewModelSprite` | edit existing |
| Doom palette emission | `tools/extract_phase6/manifest.zig`, new `tools/extract_phase6/doom_palette.zig` | new step |
| Skip-remap for Doom sprites | `tools/extract_phase6/manifest.zig`, `quake_spr.zig` | edit existing |

## Data flow (one frame, with a Doom viewmodel visible)

```
[engine renderer]                         [sdl present]
 R_RenderView                              VID_Update:
   draws world  ->  vid_buffer              memset(vid_palette_id, 0)
   R_DrawViewModel                          for each pixel:
     R_DrawViewModelSprite                    pal = vid_palette_id[i]
       palette_id = e->model->palette_id      idx = vid_buffer[i]
       R_BlitSpriteScreen(.., palette_id):    argb = d_8to24table[pal][idx]
         vid_buffer[i] = idx                lock + memcpy -> SDL_Texture
         vid_palette_id[i] = palette_id     SDL_RenderTexture + SDL_RenderPresent
 Sbar_Draw -> vid_buffer (above viewmodel,
              never overlapping)
```

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Stale palette_id when Quake content overdraws Doom-tagged pixel | Render-order invariant; documented; today the viewmodel sits above status bar so non-issue. |
| `model.h` edit conflicts with future Quake-master rebase | Same risk already exists (other small Quake-master edits). One byte at end of struct, low conflict surface. |
| Missing `gfx/palette_doom.lmp` at runtime | Slot 1 zero-filled, `Con_Printf` warning emitted on first Doom sprite load. Unreachable if extractor was run. |
| Doom palette swap changes pistol's apparent dominant colour, may need viewmodel-position retune | Visual verification step; no code follow-up expected. |
| Performance regression in expand loop | One extra L1 load per pixel at 320×200 (= 64000 extra reads/frame). Negligible; expand was already memory-bound. |

## Verification

No automated tests (consistent with project convention). Manual verification:

1. `zig build extract` produces `id1/gfx/palette_doom.lmp` (768 bytes).
2. `zig build run -- +map e1m1`, `impulse 100`, `impulse 13`. Doom pistol viewmodel renders with Doom's grey/blue gun-metal tones, not Quake's brown remap.
3. Quake shotgun (`impulse 2`) and other Quake content (status bar, HUD, world) render unchanged.
4. Switching back and forth between Doom pistol (impulse 13) and Quake weapons (impulse 1..8) produces no colour artefacts.
5. Wolf3D viewmodels (impulses 14..16) render unchanged from current behaviour (still using Quake palette via slot 0; their files are still Quake-remapped at extract time).

## Out of scope

- Wolf3D palette switching (slot 2 reserved; assets still Quake-remapped at extract time).
- World-space Doom sprite rendering through the span blitter (Phase D/E).
- 2D Doom HUD elements (Phase D/E if any).
- Doom's PLAYPAL damage/pickup tint banks (uses palette 0 only).
- GL renderer (project is software-only).

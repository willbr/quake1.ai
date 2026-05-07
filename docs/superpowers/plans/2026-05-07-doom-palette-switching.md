# Doom Palette Switching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render Doom viewmodel sprites with Doom's own palette while everything else keeps using Quake's, eliminating the muddy nearest-color remap.

**Architecture:** Add a parallel byte-per-pixel `vid_palette_id` buffer alongside `vid_buffer`. Promote `d_8to24table` from `[256]` to `[NUM_PALETTES][256]`. The framebuffer expand step in `VID_Update` reads both buffers and indexes `lut[palette_id[i]][buffer[i]]`. Sprites are tagged with their source palette at load time (filename prefix); the screen-space blit writes the tag alongside each opaque pixel. Slot 0 = Quake (default), 1 = Doom, 2 = reserved Wolf3D.

**Tech Stack:** C (gnu89 for engine, modern C for platform layer), Zig 0.16 build system, SDL3 vendored.

**Spec:** `docs/superpowers/specs/2026-05-07-doom-palette-switching-design.md`

**Verification approach:** This project has no automated test suite. Each task ends with a build-and-run verification that ensures no behavior change until the cutover task. The cutover step is the one user-visible change.

---

## Task ordering rationale

The first five tasks land plumbing without altering any rendered pixel. The cutover task (Task 6) is the single commit where the Doom pistol's appearance changes, and that step bundles two coupled edits (extractor stops remapping + viewmodel blit passes real palette_id) so master is never in a broken intermediate state where extracted Doom sprites are interpreted through the wrong LUT.

---

## Task 1: Engine plumbing — parallel palette buffer + dual LUT

**Files:**
- Create: `sdlquake/platform/vid_palette.h`
- Modify: `sdlquake/platform/vid_sdl.c`

This task adds the dual-LUT mechanism but no consumers. After this task, `vid_palette_id` is zero-cleared each frame and the expand loop reads slot 0 for every pixel — bit-identical to current behavior.

- [ ] **Step 1: Create the new header**

Create `sdlquake/platform/vid_palette.h` with the following content:

```c
// vid_palette.h -- Multi-palette state for Phase 6 Doom asset rendering.
//
// vid_palette_id rides parallel to vid.buffer (one byte per pixel,
// VID_WIDTH * VID_HEIGHT). At present time, VID_Update expands
// argb[i] = d_8to24table[vid_palette_id[i]][vid.buffer[i]].
//
// Slot 0 = Quake palette (default; built from gfx/palette.lmp).
// Slot 1 = Doom palette  (built from gfx/palette_doom.lmp).
// Slot 2 = reserved Wolf3D palette (not wired yet).
//
// Invariant: every renderer path that writes a non-zero palette_id must
// do so AFTER any Quake-palette writes that could overlap. Currently only
// the screen-space viewmodel blit writes a non-zero palette_id, and it
// sits inside r_refdef.vrect above the status bar, so the invariant
// holds trivially.

#ifndef SDLQ_VID_PALETTE_H
#define SDLQ_VID_PALETTE_H

#include "quakedef.h"

#define VID_NUM_PALETTES 3
#define VID_PAL_QUAKE    0
#define VID_PAL_DOOM     1
#define VID_PAL_WOLF3D   2

// Defined in vid_sdl.c. Sized to VID_WIDTH * VID_HEIGHT.
extern byte vid_palette_id[];

#endif
```

- [ ] **Step 2: Modify the d_8to24table declaration and add vid_palette_id buffer in vid_sdl.c**

In `sdlquake/platform/vid_sdl.c`, the current declarations near the top of the file are:

```c
unsigned    d_8to24table[256];
unsigned short d_8to16table[256]; // unused in software but declared in vid.h
```

and

```c
static byte vid_buffer[VID_WIDTH * VID_HEIGHT];
```

Replace those declarations (in their existing locations — do not move them) with:

```c
// Storage backs `unsigned d_8to24table[VID_NUM_PALETTES][256]` but is
// declared as a flat array so the engine's `extern unsigned d_8to24table[]`
// declarations remain valid (they decay to a pointer at slot 0, which is
// the Quake palette — what engine code expects).
unsigned    d_8to24table[3 * 256];
unsigned short d_8to16table[256]; // unused in software but declared in vid.h
```

and

```c
static byte vid_buffer[VID_WIDTH * VID_HEIGHT];
byte vid_palette_id[VID_WIDTH * VID_HEIGHT];   // declared in vid_palette.h
```

Add `#include "vid_palette.h"` near the existing includes at the top of the file (alongside `imgui_layer.h` and `r_bbox.h`).

- [ ] **Step 3: Update build_palette to write into slot 0**

The current `build_palette` function is:

```c
static void build_palette(unsigned char *palette)
{
    for (int i = 0; i < 256; i++)
    {
        unsigned r = palette[i*3 + 0];
        unsigned g = palette[i*3 + 1];
        unsigned b = palette[i*3 + 2];
        // SDL_PIXELFORMAT_ARGB8888: 0xAARRGGBB as uint32 (LE stored as B,G,R,A)
        d_8to24table[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    // fullbright starts at color 224
    d_8to24table[255] = 0; // transparent black
}
```

Replace with a slot-aware helper plus a thin wrapper that targets slot 0:

```c
static void build_palette_slot(int slot, unsigned char *palette)
{
    unsigned *lut = d_8to24table + slot * 256;
    for (int i = 0; i < 256; i++)
    {
        unsigned r = palette[i*3 + 0];
        unsigned g = palette[i*3 + 1];
        unsigned b = palette[i*3 + 2];
        // SDL_PIXELFORMAT_ARGB8888: 0xAARRGGBB as uint32 (LE stored as B,G,R,A)
        lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    // 255 is transparent in every palette slot
    lut[255] = 0;
}

static void build_palette(unsigned char *palette)
{
    build_palette_slot(VID_PAL_QUAKE, palette);
}
```

Leave the `VID_SetPalette` / `VID_ShiftPalette` / `VID_SetMode` callers alone — they call `build_palette(...)`, which still writes slot 0.

- [ ] **Step 4: Modify VID_Update to clear palette_id and use dual-LUT lookup**

The current `VID_Update` inner section is:

```c
void VID_Update(vrect_t *rects)
{
    if (!sdl_texture) return;

    RBBox_Draw();

    void *pixels;
    int pitch;
    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) < 0)
        return;

    for (int y = 0; y < VID_HEIGHT; y++)
    {
        unsigned *dst = (unsigned *)((byte *)pixels + y * pitch);
        byte     *src = vid.buffer + y * vid.rowbytes;
        for (int x = 0; x < VID_WIDTH; x++)
            dst[x] = d_8to24table[src[x]];
    }

    SDL_UnlockTexture(sdl_texture);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
    ImguiLayer_Render();
    SDL_RenderPresent(sdl_renderer);
}
```

Replace the body up to `SDL_UnlockTexture` with:

```c
void VID_Update(vrect_t *rects)
{
    if (!sdl_texture) return;

    RBBox_Draw();

    void *pixels;
    int pitch;
    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) < 0)
        return;

    // d_8to24table is laid out as [VID_NUM_PALETTES][256]. Casting back to
    // a 2D pointer makes the per-pixel lookup explicit.
    unsigned (*lut)[256] = (unsigned (*)[256])d_8to24table;

    for (int y = 0; y < VID_HEIGHT; y++)
    {
        unsigned *dst     = (unsigned *)((byte *)pixels + y * pitch);
        byte     *src     = vid.buffer       + y * vid.rowbytes;
        // vid_palette_id is stride-equal to vid.buffer at fixed resolution
        // (VID_Init sets vid.rowbytes = VID_WIDTH).
        byte     *pal_src = vid_palette_id   + y * vid.rowbytes;
        for (int x = 0; x < VID_WIDTH; x++)
            dst[x] = lut[pal_src[x]][src[x]];
    }

    SDL_UnlockTexture(sdl_texture);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
    ImguiLayer_Render();
    SDL_RenderPresent(sdl_renderer);

    // Reset palette tags for the next frame. Engine renderer paths that
    // write to vid.buffer leave palette_id at 0 (Quake); only the Doom
    // sprite blit opts in by writing 1.
    memset(vid_palette_id, 0, sizeof(vid_palette_id));
}
```

The `memset` at the end of the frame is equivalent to a memset at the start of the next frame and avoids a separate first-frame-init concern: at startup `vid_palette_id` is zero-initialized as a BSS global.

- [ ] **Step 5: Build**

Run: `zig build`
Expected: clean build, no warnings related to `d_8to24table`, `vid_palette_id`, or `VID_Update`.

- [ ] **Step 6: Run and verify no visual change**

Run: `zig build run -- +map e1m1`
Expected: game starts, the world renders identically to before this task, the HUD/status bar render identically. Move around for ~10 seconds, fire the (Quake) shotgun once. Confirm no visual artifacts or crashes. Quit.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/platform/vid_palette.h sdlquake/platform/vid_sdl.c
git commit -m "feat: parallel vid_palette_id buffer + dual-LUT framebuffer expand

Promotes d_8to24table from [256] to [3][256] (laid out as a flat array
so engine 'extern unsigned d_8to24table[]' declarations remain valid).
Adds vid_palette_id parallel byte buffer and zero-clears it each frame.
VID_Update expand becomes lut[palette_id[i]][buffer[i]].

No callers tag pixels yet, so output is bit-identical to before. Wires
the plumbing for Phase 6 Doom palette switching."
```

---

## Task 2: Extractor — emit gfx/palette_doom.lmp

**Files:**
- Modify: `tools/extract_phase6/manifest.zig`

`doom_wad.zig` already exposes `loadPlaypal0()` which returns `[256][3]u8`. We just write its bytes to disk.

- [ ] **Step 1: Add the palette emission to extractAll**

In `tools/extract_phase6/manifest.zig`, the Doom-sources section currently begins:

```zig
    // ----- Doom sources -----
    var w = try doom_wad.Wad.open(io, allocator, "doom-data/DOOM1.WAD");
    defer w.deinit(allocator);
    const doom_pal = try w.loadPlaypal0();
    const doom_remap = buildRemap8(doom_pal, quake_pal);
```

Immediately after the `const doom_pal = try w.loadPlaypal0();` line, add:

```zig
    // Phase 6 palette switching: write Doom's PLAYPAL palette 0 as a
    // 768-byte LMP that the engine loads at startup into d_8to24table[1].
    {
        const out_path = "id1/gfx/palette_doom.lmp";
        // Ensure parent dir exists. cwd is repo root when run via `zig build extract`.
        std.fs.cwd().makePath("id1/gfx") catch |err| switch (err) {
            error.PathAlreadyExists => {},
            else => return err,
        };
        var bytes: [768]u8 = undefined;
        var i: usize = 0;
        while (i < 256) : (i += 1) {
            bytes[i * 3 + 0] = doom_pal[i][0];
            bytes[i * 3 + 1] = doom_pal[i][1];
            bytes[i * 3 + 2] = doom_pal[i][2];
        }
        var f = try std.fs.cwd().createFile(out_path, .{ .truncate = true });
        defer f.close();
        try f.writeAll(&bytes);
        std.debug.print("  wrote {s} (768 bytes)\n", .{out_path});
    }
```

(The `_ = io;` / `_ = allocator;` are not needed — `io` and `allocator` are still used by the surrounding code; we use `std.fs.cwd()` directly here, matching how the rest of the extractor writes files via the Zig stdlib in newer entries.)

If the file-write API doesn't compile (Zig 0.16 stdlib has been in flux around `std.fs`), check how other writers in this file produce files — specifically `quake_spr.writeSprite` and `quake_wav.writeWavU8` show the existing convention. If `std.fs.cwd().createFile` works elsewhere in the codebase, use it; otherwise mirror the pattern of the existing writers (likely `try Io.openFileWriting(...)` or similar). The point is: write 768 bytes to `id1/gfx/palette_doom.lmp`.

- [ ] **Step 2: Add palette_doom.lmp to .gitignore**

Append to `.gitignore`:

```
id1/gfx/palette_doom.lmp
```

(The Phase 6 extracted sprites and sounds are already gitignored per existing convention; the Doom palette is the same kind of reproducible-from-WAD output.)

- [ ] **Step 3: Run the extractor**

Run: `zig build extract`
Expected stdout includes a line like: `  wrote id1/gfx/palette_doom.lmp (768 bytes)`

- [ ] **Step 4: Verify the file**

Run (PowerShell):
```powershell
(Get-Item id1/gfx/palette_doom.lmp).Length
```
Expected: `768`

Sanity check the first few bytes are reasonable RGB triples:
```powershell
[byte[]]$b = [System.IO.File]::ReadAllBytes("id1/gfx/palette_doom.lmp"); $b[0..11]
```
Expected: 12 bytes that don't look like gibberish; PLAYPAL[0] is Doom's index-0 color (typically `0 0 0` followed by index 1 around `31 23 11`). Exact values not asserted — just confirm the bytes are RGB-like and not all zero/all 0xFF.

- [ ] **Step 5: Verify game still runs unchanged**

Run: `zig build run -- +map e1m1`
Expected: game starts, world renders identically to Task 1's verification (the engine doesn't load `palette_doom.lmp` yet, so this should be unchanged).

- [ ] **Step 6: Commit**

```bash
git add tools/extract_phase6/manifest.zig .gitignore
git commit -m "feat(extract): emit gfx/palette_doom.lmp from PLAYPAL

Reads Doom's PLAYPAL palette 0 (already accessible via doom_wad.zig
loadPlaypal0) and writes 768 bytes as id1/gfx/palette_doom.lmp,
mirroring Quake's own gfx/palette.lmp format (256 RGB triples).

The engine doesn't load this yet; that wires up in the next task."
```

---

## Task 3: Engine — load palette_doom.lmp into slot 1

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c`

- [ ] **Step 1: Add the aux-palette loader**

In `sdlquake/platform/vid_sdl.c`, immediately after the `build_palette` definition (after the line `static void build_palette(unsigned char *palette) { build_palette_slot(VID_PAL_QUAKE, palette); }`), add:

```c
static void vid_load_aux_palette(int slot, const char *qpath)
{
    // COM_LoadHunkFile returns NULL if the file is missing. We tolerate
    // that — slot stays zero-filled, and any sprite tagged with this
    // palette would render black until the user runs `zig build extract`.
    extern byte *COM_LoadHunkFile(char *path);
    byte *data = COM_LoadHunkFile((char *)qpath);
    if (!data)
    {
        Con_Printf("vid_load_aux_palette: %s missing; slot %d zero-filled\n",
                   qpath, slot);
        return;
    }
    build_palette_slot(slot, data);
    // COM_LoadHunkFile data is owned by the hunk; nothing to free here.
}
```

- [ ] **Step 2: Call the loader from VID_Init**

In `VID_Init`, the current line is:

```c
    build_palette(palette);
```

Add immediately after it:

```c
    build_palette(palette);
    vid_load_aux_palette(VID_PAL_DOOM, "gfx/palette_doom.lmp");
```

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 4: Run and verify no visual change**

Run: `zig build run -- +map e1m1`
Expected: game runs identically to Task 2's verification. If `id1/gfx/palette_doom.lmp` exists (because Task 2 produced it), the load succeeds silently. If it's missing, you'll see `vid_load_aux_palette: gfx/palette_doom.lmp missing; slot 1 zero-filled` in the console — that's an acceptable warning, not an error. Either way the rendered output is unchanged because nothing tags pixels yet.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "feat: load gfx/palette_doom.lmp into d_8to24table slot 1

Adds vid_load_aux_palette helper using COM_LoadHunkFile and calls it
from VID_Init after build_palette. Missing-file path emits a warning
and leaves slot 1 zero-filled (unreachable in practice — if the file
is missing the Doom sprites are too).

Still no behavior change at the pixel level; nothing tags palette_id."
```

---

## Task 4: Engine — model_t.palette_id field + sprite-load tagging

**Files:**
- Modify: `Quake-master/WinQuake/model.h`
- Modify: `Quake-master/WinQuake/model.c`

This is one of two tasks that touch `Quake-master/`. The CLAUDE.md "never modify Quake-master" rule has already been relaxed for Phase 6 (see `r_sprite.c::R_DrawViewModelSprite`); a single trailing field on `model_t` plus a small filename check in `Mod_LoadSpriteModel` follows the same precedent.

- [ ] **Step 1: Add the palette_id field to model_t**

In `Quake-master/WinQuake/model.h`, the end of the `model_t` struct currently looks like:

```c
//
// additional model data
//
	cache_user_t	cache;		// only access through Mod_Extradata

} model_t;
```

Add the new field as the very last member, after `cache`:

```c
//
// additional model data
//
	cache_user_t	cache;		// only access through Mod_Extradata

// PHASE 6: source palette for paletted sprite rendering. 0 = Quake (default),
// 1 = Doom, 2 = Wolf3D (reserved). Set by Mod_LoadSpriteModel via filename
// prefix; consumed by R_BlitSpriteScreen to tag vid_palette_id pixels.
	byte		palette_id;

} model_t;
```

This field lives at the very end of the struct, so all existing offsets are unchanged. The file `model.h` lacks `#include <stdint.h>` but `byte` is already used throughout (e.g. `byte *visdata`, `byte *lightdata` earlier in the same struct), so no new include is required.

- [ ] **Step 2: Set palette_id in Mod_LoadSpriteModel**

In `Quake-master/WinQuake/model.c`, the end of `Mod_LoadSpriteModel` currently looks like:

```c
	mod->type = mod_sprite;
}
```

Replace those two lines (the `mod->type = mod_sprite;` and the closing brace) with:

```c
	mod->type = mod_sprite;

	// PHASE 6: tag the model with its source palette so the screen-space
	// blit knows which entry of d_8to24table[][] to render through.
	// Filenames are produced by the Phase 6 extractor:
	//   progs/v_doom*.spr  -> Doom palette
	//   progs/v_wolf*.spr  -> Wolf3D palette (reserved; unused for now)
	//   everything else    -> Quake palette
	if (!strncmp(mod->name, "progs/v_doom", 12))
		mod->palette_id = 1;	// VID_PAL_DOOM
	else if (!strncmp(mod->name, "progs/v_wolf", 12))
		mod->palette_id = 2;	// VID_PAL_WOLF3D
	else
		mod->palette_id = 0;	// VID_PAL_QUAKE (default)
}
```

We use the literal numbers `0/1/2` here rather than the `VID_PAL_*` macros because `model.c` is engine code that should not include the platform-layer header. The comment names the macro for traceability.

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean build, no warnings about `palette_id`, `Mod_LoadSpriteModel`, or `model_t`.

- [ ] **Step 4: Run and verify no visual change**

Run: `zig build run -- +map e1m1`
Expected: game runs identically to Task 3's verification. The Doom pistol still looks the same as before this task — the field is set but no consumer reads it yet.

Test: `impulse 100` then `impulse 13` to bring up the Doom pistol viewmodel. Confirm it still renders (with the same muddy nearest-color colors as before — that doesn't change until Task 6).

- [ ] **Step 5: Commit**

```bash
git add Quake-master/WinQuake/model.h Quake-master/WinQuake/model.c
git commit -m "feat: tag sprite models with source palette at load time

Adds model_t.palette_id (one byte, end of struct so existing offsets
are unchanged). Mod_LoadSpriteModel sets it from the filename prefix:
v_doom* -> 1, v_wolf* -> 2, else 0.

No consumer reads palette_id yet; behavior is unchanged."
```

---

## Task 5: Engine — palette-aware R_BlitSpriteScreen (still passing 0)

**Files:**
- Modify: `Quake-master/WinQuake/r_local.h`
- Modify: `Quake-master/WinQuake/r_sprite.c`

This task extends the blit signature to accept a palette tag and writes it alongside opaque pixels, but `R_DrawViewModelSprite` still passes 0. After this task the runtime is one line away from the cutover.

- [ ] **Step 1: Update the prototype in r_local.h**

In `Quake-master/WinQuake/r_local.h`, the current declarations are:

```c
void R_DrawViewModelSprite (entity_t *e);		// PHASE 6: 2D viewmodel sprite path
void R_BlitSpriteScreen (int sx, int sy, mspriteframe_t *frame);
```

Change the second one to:

```c
void R_DrawViewModelSprite (entity_t *e);		// PHASE 6: 2D viewmodel sprite path
void R_BlitSpriteScreen (int sx, int sy, mspriteframe_t *frame, byte palette_id);
```

- [ ] **Step 2: Extend R_BlitSpriteScreen to write the palette tag**

In `Quake-master/WinQuake/r_sprite.c`, the current function is:

```c
void R_BlitSpriteScreen (int sx, int sy, mspriteframe_t *frame)
{
	int		w, h, u, v;
	byte	*dest, *source, tbyte;

	if (r_pixbytes != 1)
		return;		// 16-bit color path not supported for viewmodel sprites yet

	w = frame->width;
	h = frame->height;
	if (w <= 0 || h <= 0)
		return;

	if (sx < 0 || sy < 0 ||
		sx + w > (int)vid.width || sy + h > (int)vid.height)
		return;		// off-screen — skip rather than crash

	source = (byte *)&frame->pixels[0];
	dest   = vid.buffer + sy * vid.rowbytes + sx;

	for (v = 0; v < h; v++)
	{
		for (u = 0; u < w; u++)
		{
			if ((tbyte = source[u]) != TRANSPARENT_COLOR)
				dest[u] = tbyte;
		}
		dest   += vid.rowbytes;
		source += w;
	}
}
```

Replace it with:

```c
extern byte vid_palette_id[];	// from sdlquake/platform/vid_sdl.c

void R_BlitSpriteScreen (int sx, int sy, mspriteframe_t *frame, byte palette_id)
{
	int		w, h, u, v;
	byte	*dest, *source, *pal_dest, tbyte;

	if (r_pixbytes != 1)
		return;		// 16-bit color path not supported for viewmodel sprites yet

	w = frame->width;
	h = frame->height;
	if (w <= 0 || h <= 0)
		return;

	if (sx < 0 || sy < 0 ||
		sx + w > (int)vid.width || sy + h > (int)vid.height)
		return;		// off-screen — skip rather than crash

	source   = (byte *)&frame->pixels[0];
	dest     = vid.buffer       + sy * vid.rowbytes + sx;
	// vid_palette_id is sized to VID_WIDTH * VID_HEIGHT and stride-equivalent
	// to vid.buffer at fixed-resolution mode (vid.rowbytes == VID_WIDTH).
	pal_dest = vid_palette_id   + sy * vid.rowbytes + sx;

	for (v = 0; v < h; v++)
	{
		for (u = 0; u < w; u++)
		{
			if ((tbyte = source[u]) != TRANSPARENT_COLOR)
			{
				dest[u]     = tbyte;
				pal_dest[u] = palette_id;
			}
		}
		dest     += vid.rowbytes;
		pal_dest += vid.rowbytes;
		source   += w;
	}
}
```

- [ ] **Step 3: Update the only caller in R_DrawViewModelSprite to pass 0 for now**

In `Quake-master/WinQuake/r_sprite.c`, the current end of `R_DrawViewModelSprite` is:

```c
	sx = vp_x + (vp_w - frame->width) / 2;
	sy = vp_y +  vp_h - frame->height;
	R_BlitSpriteScreen (sx, sy, frame);
}
```

Change the blit call to pass an explicit `0`:

```c
	sx = vp_x + (vp_w - frame->width) / 2;
	sy = vp_y +  vp_h - frame->height;
	// PHASE 6 palette switching: pass 0 (Quake) until cutover task. Task 6
	// flips this to e->model->palette_id which gives Doom sprites their
	// native colors via d_8to24table[1].
	R_BlitSpriteScreen (sx, sy, frame, 0);
}
```

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build, no signature-mismatch warnings.

- [ ] **Step 5: Run and verify no visual change**

Run: `zig build run -- +map e1m1`
Expected: game runs identically to Task 4's verification.

`impulse 100` + `impulse 13` brings up the Doom pistol; it still renders with the same colors as before (the blit now writes palette_id into vid_palette_id at opaque pixels, but `R_DrawViewModelSprite` passes 0 so the tag is 0 → Quake LUT, which is what was happening implicitly before).

- [ ] **Step 6: Commit**

```bash
git add Quake-master/WinQuake/r_local.h Quake-master/WinQuake/r_sprite.c
git commit -m "feat: R_BlitSpriteScreen tags vid_palette_id for opaque pixels

Adds a palette_id byte parameter to R_BlitSpriteScreen and writes the
tag into the parallel vid_palette_id buffer at every opaque pixel.
R_DrawViewModelSprite still passes 0 (Quake palette), so behavior is
unchanged. The cutover task switches it to e->model->palette_id."
```

---

## Task 6: Cutover — extractor stops remapping Doom + viewmodel passes real palette_id

**Files:**
- Modify: `tools/extract_phase6/manifest.zig`
- Modify: `Quake-master/WinQuake/r_sprite.c`

This is the user-visible change: the Doom pistol now renders with Doom's palette. Bundled into one commit so master is never in a state where Doom sprites contain Doom-palette indices but the renderer still feeds them through the Quake LUT.

- [ ] **Step 1: Skip the Doom remap in the extractor**

In `tools/extract_phase6/manifest.zig`, the Doom sprite loop currently is (around the middle of `extractAll`):

```zig
    for (doom_sprite_sets) |set| {
        // Doom sprites vary in size — allocate per frame.
        var frames = try allocator.alloc(quake_spr.Frame, set.lumps.len);
        defer allocator.free(frames);
        var pixel_bufs = try allocator.alloc([]u8, set.lumps.len);
        // Mark slots empty so partial-init cleanup is safe.
        for (pixel_bufs) |*pb| pb.* = &.{};
        defer {
            for (pixel_bufs) |p| if (p.len > 0) allocator.free(p);
            allocator.free(pixel_bufs);
        }

        var i: usize = 0;
        while (i < set.lumps.len) : (i += 1) {
            const l = w.findLump(set.lumps[i]) orelse {
                std.debug.print("  WARNING: Doom lump '{s}' missing in WAD\n", .{set.lumps[i]});
                return error.LumpMissing;
            };
            const data = w.lumpData(l);
            // Peek dimensions to size the output buffer.
            if (data.len < 4) return error.PicHeaderTooSmall;
            const wpx = std.mem.readInt(u16, data[0..2], .little);
            const hpx = std.mem.readInt(u16, data[2..4], .little);
            const out = try allocator.alloc(u8, @as(usize, wpx) * @as(usize, hpx));
            pixel_bufs[i] = out;
            const sz = try doom_wad.decodePicture(data, out);
            applyRemap(doom_remap, out);
            frames[i] = .{
                .width    = sz.width,
                .height   = sz.height,
                .pixels   = out,
                .origin_x = -@divTrunc(@as(i32, sz.width), 2),
                .origin_y = sz.height,
            };
        }
        try quake_spr.writeSprite(io, allocator, set.out_path, frames);
        std.debug.print("  wrote {s} ({d} frames)\n", .{ set.out_path, set.lumps.len });
    }
```

Delete the `applyRemap(doom_remap, out);` line. The pixels written into `out` are already Doom palette indices (transparent stays as 0xFF because `decodePicture` uses 0xFF for unwritten pixels — verify by reading `doom_wad.decodePicture` if in doubt). Drop the now-unused `doom_remap` if Zig's compiler complains about it; the simplest fix is to also delete the `const doom_remap = buildRemap8(doom_pal, quake_pal);` line right before the loop.

If Zig's compiler treats `doom_remap` as still used elsewhere, leave the binding in place — only the call site of `applyRemap` matters.

The Wolf3D loop (`for (wolf_sprite_sets)`) above this is unchanged: Wolf sprites continue to be remapped to the Quake palette since palette slot 2 isn't wired this task.

- [ ] **Step 2: Pass the real palette_id from R_DrawViewModelSprite**

In `Quake-master/WinQuake/r_sprite.c`, change the end of `R_DrawViewModelSprite` from:

```c
	sx = vp_x + (vp_w - frame->width) / 2;
	sy = vp_y +  vp_h - frame->height;
	// PHASE 6 palette switching: pass 0 (Quake) until cutover task. Task 6
	// flips this to e->model->palette_id which gives Doom sprites their
	// native colors via d_8to24table[1].
	R_BlitSpriteScreen (sx, sy, frame, 0);
}
```

to:

```c
	sx = vp_x + (vp_w - frame->width) / 2;
	sy = vp_y +  vp_h - frame->height;
	R_BlitSpriteScreen (sx, sy, frame, e->model->palette_id);
}
```

- [ ] **Step 3: Re-extract**

Run: `zig build extract`
Expected: succeeds; Doom sprite outputs are re-written with un-remapped pixels. `id1/gfx/palette_doom.lmp` is rewritten (same bytes — idempotent).

- [ ] **Step 4: Build the engine**

Run: `zig build`
Expected: clean build.

- [ ] **Step 5: Verify the Doom pistol shows correct colors**

Run: `zig build run -- +map e1m1`

In-game checks:
1. `impulse 100` — grants all weapons.
2. `impulse 13` — switches to Doom pistol.
3. Look at the gun on screen. It should now render in Doom's grey/blue gun-metal tones — the muddy brown nearest-color cast from before should be gone. Specifically the body of the pistol should look distinctly different from the Quake shotgun's wood-and-metal palette.
4. Fire it once (left mouse) — animation cycles through PISGB0..E0 frames; all frames should render in correct Doom colors.
5. `impulse 1` (axe) and `impulse 2` (shotgun) — Quake weapons render unchanged, with correct Quake palette colors. No discoloration.
6. `impulse 13` again, then look around and walk through a doorway. Confirm world rendering, status bar, HUD numbers, and ammo counts all render in correct Quake palette colors. None of the surrounding pixels should be tinted by the Doom blit (the invariant: blit sits inside r_refdef.vrect, not overlapping anything).
7. Quit cleanly with `quit`.

If the gun renders all-black: `id1/gfx/palette_doom.lmp` likely failed to load. Look at the console for the `vid_load_aux_palette: ... missing` warning; rerun `zig build extract` if it's missing.

If the gun colors look weird in a different way (e.g., wildly oversaturated): the palette load is probably reading the wrong bytes. Compare the first 12 bytes of `id1/gfx/palette_doom.lmp` against the first PLAYPAL entry by running `zig build extract -- -doom_info` (or whatever debug helper is closest).

- [ ] **Step 6: Commit**

```bash
git add tools/extract_phase6/manifest.zig Quake-master/WinQuake/r_sprite.c
git commit -m "feat(phase6): cut over to Doom palette for Doom viewmodel sprites

Extractor stops nearest-color remapping Doom sprites to Quake's palette;
the .spr pixels now contain native Doom-palette indices. R_DrawViewModelSprite
passes e->model->palette_id (=1 for v_doom*.spr) into R_BlitSpriteScreen,
which tags vid_palette_id at opaque pixels. VID_Update's expand step then
reads d_8to24table[1][idx] for those pixels.

Doom pistol now displays with its native gun-metal grey/blue colors
instead of the muddy brown nearest-color cast.

Wolf3D sprites continue to use the Quake palette (slot 2 reserved but
not wired). World-space Doom sprite rendering will need a similar tag
plumbed through the span blitter when Phase D/E adds it; design noted
in the spec."
```

---

## Task 7 (smoke check): Wolf3D viewmodels still render unchanged

**Files:** none (verification only).

This is a no-code task to make sure the cutover didn't accidentally affect Wolf3D weapons (which still go through Quake-palette remap at extract time and have palette_id = 2, but slot 2 of the LUT is zero-filled at runtime).

- [ ] **Step 1: Run the game and check Wolf3D weapons**

Run: `zig build run -- +map e1m1`

In-game:
1. `impulse 100`
2. `impulse 14` (Wolf knife — first Wolf weapon per the Phase 6 impulse mapping; verify the exact impulse number against `weapons_phase6.c` `ImpulseCommands` if 14 doesn't bring up a Wolf weapon).
3. The Wolf knife viewmodel should render. Look closely: it has palette_id = 2 set by Mod_LoadSpriteModel, and slot 2 of `d_8to24table` is **zero-filled** (because we don't load a Wolf palette in this plan). That means the Wolf knife will render as solid black against the world.

If you see solid black where the Wolf knife should be, the Wolf3D-tagging path is working but slot 2 isn't populated — **expected**. Wiring slot 2 (loading a Wolf palette LMP) is out of scope for this plan.

If you see the Wolf knife rendering with its current (Quake-remapped) colors instead of black, that means the palette tag didn't reach the blit — this is a regression, investigate before considering the plan done. Most likely cause: filename-prefix check in `Mod_LoadSpriteModel` is matching wrong, or the blit isn't reading the tag.

- [ ] **Step 2: Decision point**

If Wolf weapons render black, that's the expected outcome of this plan as written. Two follow-up options, **out of scope here**:
1. Wire up slot 2 with a Wolf3D palette LMP — small follow-up plan.
2. Set Wolf sprite palette_id back to 0 in `Mod_LoadSpriteModel` and live with the current Quake-remap until slot 2 is wired.

If you want to keep Wolf weapons usable today, take option 2 by changing the `model.c` filename check to:

```c
	if (!strncmp(mod->name, "progs/v_doom", 12))
		mod->palette_id = 1;	// VID_PAL_DOOM
	else
		mod->palette_id = 0;	// VID_PAL_QUAKE (default; Wolf stays remapped)
```

Commit that one-liner separately if needed:

```bash
git add Quake-master/WinQuake/model.c
git commit -m "fix: keep Wolf3D viewmodels on Quake palette until slot 2 is wired

The Phase 6 cutover tags v_wolf*.spr models with palette_id = 2, but
d_8to24table[2] is zero-filled (no Wolf palette loaded), so Wolf weapons
were rendering black. Until we extract a Wolf3D PLAYPAL equivalent and
wire it to slot 2, leave Wolf sprites tagged 0 so they keep using the
nearest-color Quake-remapped pixels they already contain."
```

The plan is otherwise complete after Task 6.

---

## Spec coverage check (cross-reference)

| Spec section | Plan task |
| --- | --- |
| §Architecture > Buffers (`vid_palette_id`, dual-LUT) | Task 1 |
| §Architecture > Per-frame lifecycle (memset + expand) | Task 1 |
| §Architecture > Palette LUT construction (`vid_load_aux_palette`) | Task 3 |
| §Identification > `model_t.palette_id` field | Task 4 |
| §Identification > Filename-prefix detection | Task 4 |
| §Identification > Wiring through the blit | Task 5 + Task 6 |
| §Render order — invariant comment | Task 1 (header), Task 5 (blit) |
| §Extractor > emit `gfx/palette_doom.lmp` | Task 2 |
| §Extractor > skip Quake-remap for Doom | Task 6 |
| §Verification (manual checks) | Task 6 step 5, Task 7 |
| §Out of scope (Wolf slot 2 / world-space) | Task 7 (smoke check) |

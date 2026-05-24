# Supersample (SSAA) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `vid_supersample` cvar (1..4) that renders the frame at `render-res · SS`, box-averages back to render-res, then nearest-upscales to window — preserving Quake's chunky-pixel aesthetic while smoothing sub-pixel aliasing in the world.

**Architecture:** Single buffer at super-resolution (no separate world/HUD split). `vid.buffer`, `vid.width`, `vid.height`, `vid.rowbytes`, `vid.conwidth`, `vid.conheight`, `vid.conrowbytes` all reflect super-res so the engine renders everything (world + HUD) there. The SDL texture and the `SDL_SetRenderLogicalPresentation` size stay at render-res; the only "extra work" is in `VID_Update`'s palette-expand loop, which averages SS² source pixels per output pixel. HUD shrinks under SS the same way it already shrinks when raising `vid_scale` — accepted side-effect of the chosen design.

**Tech Stack:** C99 (platform layer), Zig build, SDL3 renderer/texture. Verification = `zig build run -- +map e1m1` plus visual inspection (no test suite exists).

**Spec:** [`docs/superpowers/specs/2026-05-24-supersample-design.md`](../specs/2026-05-24-supersample-design.md)

## File Structure

Everything is in **one file**: `sdlquake/platform/vid_sdl.c`. The implementation touches:

- Constants block (lines ~97–105): bump `VID_RENDER_MAX_W/H`, introduce `vid_super_w/h`.
- Cvar block (lines ~70–82): add `vid_supersample` cvar + active mirror.
- `VID_ApplyScale` → renamed to `VID_ApplyResolution(scale, ss)`: single source of truth for "compute and apply (scale, ss)-derived state".
- `vid_preload_cvars_from_config` (lines ~316–338): add one line for `vid_supersample`.
- `VID_Init` (lines ~340–471): use `VID_ApplyResolution` for initial sizing.
- `VID_Update` (lines ~485–557): box-average SS² source pixels per output pixel.
- `VID_MenuDraw` and `VID_MenuKey` (lines ~225–304): add Supersample group above Render Resolution.

No other source file is modified.

---

### Task 1: Bump VID_RENDER_MAX caps and verify build still runs

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c:97-100`

- [ ] **Step 1: Edit the size caps**

Replace lines 97–100 with:

```c
#define VID_WIDTH  320
#define VID_HEIGHT 200
#define VID_RENDER_MAX_W (VID_WIDTH  * 8)  /* 2560 — render_scale·SS up to 8 */
#define VID_RENDER_MAX_H (VID_HEIGHT * 8)  /* 1600 */
```

The arrays that depend on these (`static byte vid_buffer[VID_RENDER_MAX_W * VID_RENDER_MAX_H]` at line ~105 and `byte vid_palette_id[VID_RENDER_MAX_W * VID_RENDER_MAX_H]` at line ~106) grow automatically from the constants — no separate edit needed.

- [ ] **Step 2: Build and run**

Run: `zig build run -- +map e1m1`
Expected: build succeeds, game boots, e1m1 renders normally. Quit with `quit` at the console.

Memory budget sanity-check: the hunk pre-reservation in `VID_Init` now reserves `2560·1600·2 + D_SurfaceCacheForRes(2560, 1600) ≈ 20 MB` instead of `~5 MB`. If the boot crashes with a hunk-OOM message, bump the default heap (search for `-mem`, default ~16 MB in `host.c`); record the new default in the commit message.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "$(cat <<'EOF'
feat(renderer): grow VID_RENDER_MAX caps to 2560x1600 for SSAA headroom

Bumps max combined render-scale * supersample from 4 to 8. Required so
the supersample multiplier (coming next) can layer on top of vid_scale=4
without further reallocation. Adds ~21 MB to hunk + BSS reservations.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Add vid_supersample cvar registration + config preload

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c:70-82` (cvar declarations)
- Modify: `sdlquake/platform/vid_sdl.c:316-338` (`vid_preload_cvars_from_config`)
- Modify: `sdlquake/platform/vid_sdl.c:342-347` (`VID_Init` cvar registration)

- [ ] **Step 1: Declare the cvar and its active mirror**

After the `vid_scale_active` / `vid_scale` declarations at lines 70–71, add a parallel block:

```c
static int vid_supersample_active = 1;
static cvar_t vid_supersample = {"vid_supersample", "1", true};
```

- [ ] **Step 2: Add config-preload line**

Inside `vid_preload_cvars_from_config`, alongside the existing `else if` chain (line ~335), append one more clause:

```c
else if (sscanf(line, "vid_supersample \"%f", &v) == 1)  Cvar_SetValue("vid_supersample", v);
```

- [ ] **Step 3: Register the cvar at VID_Init entry**

In `VID_Init`, alongside the existing `Cvar_RegisterVariable(&vid_scale)` at line 342, add:

```c
Cvar_RegisterVariable(&vid_supersample);
```

- [ ] **Step 4: Build and verify cvar is live**

Run: `zig build run -- +map e1m1`
At the in-game console (`~`), type `vid_supersample`.
Expected output: `"vid_supersample" is "1"`.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "$(cat <<'EOF'
feat(renderer): register vid_supersample cvar (no behavior change yet)

Adds the archived cvar and its active mirror; preloads from config.cfg
alongside vid_scale. Behavior is unchanged because nothing reads the
value yet.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Plumb supersample into the resolution-apply path

This task changes the semantics of `vid_render_w/h`: today it means "framebuffer width" (= display width). After this task it means "display width", and a new pair `vid_super_w/h` means "framebuffer width = display·SS". The engine's `vid.width / vid.height / vid.rowbytes / vid.conwidth / vid.conheight / vid.conrowbytes` are all set to the super-res values. The SDL texture and the logical-presentation size stay at display-res.

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c:97-104` (add `vid_super_w/h` globals)
- Modify: `sdlquake/platform/vid_sdl.c:158-199` (rewrite `VID_ApplyScale` → `VID_ApplyResolution`)
- Modify: `sdlquake/platform/vid_sdl.c:340-471` (`VID_Init`: use new apply path)
- Modify: `sdlquake/platform/vid_sdl.c:280-286` (`VID_MenuKey`: pass current SS to apply)

- [ ] **Step 1: Add `vid_super_w/h` globals next to `vid_render_w/h`**

Replace lines 102–106 (the `vid_render_w/h` definitions and the static buffers) with:

```c
static int vid_render_w = VID_WIDTH;
static int vid_render_h = VID_HEIGHT;
static int vid_super_w  = VID_WIDTH;   /* = vid_render_w * vid_supersample_active */
static int vid_super_h  = VID_HEIGHT;

static byte vid_buffer[VID_RENDER_MAX_W * VID_RENDER_MAX_H];
byte vid_palette_id[VID_RENDER_MAX_W * VID_RENDER_MAX_H];  /* declared in vid_palette.h */
```

- [ ] **Step 2: Rewrite the apply function**

Replace the entire `VID_ApplyScale` function (lines 158–199) with:

```c
// Apply a (render_scale, supersample) pair. The framebuffer/engine state
// (vid.width etc.) reflects render_scale * ss; the SDL texture and the
// logical-presentation size stay at render_scale (chunky display grid).
// `ss` is clamped so render_scale * ss <= 8.
static void VID_ApplyResolution(int render_scale, int ss)
{
    extern short *d_pzbuffer;
    extern int    D_SurfaceCacheForRes(int w, int h);
    extern void   D_InitCaches(void *buffer, int size);

    if (render_scale < 1) render_scale = 1;
    if (render_scale > 4) render_scale = 4;
    if (ss < 1) ss = 1;
    if (ss > 4) ss = 4;
    while (render_scale * ss > 8 && ss > 1) ss--;

    int render_w = VID_WIDTH  * render_scale;
    int render_h = VID_HEIGHT * render_scale;
    int super_w  = render_w * ss;
    int super_h  = render_h * ss;

    if (sdl_renderer) {
        if (sdl_texture) { SDL_DestroyTexture(sdl_texture); sdl_texture = NULL; }
        sdl_texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            render_w, render_h);
        if (!sdl_texture)
            Sys_Error("SDL_CreateTexture failed: %s", SDL_GetError());
        SDL_SetTextureScaleMode(sdl_texture, SDL_SCALEMODE_NEAREST);

        SDL_SetRenderLogicalPresentation(sdl_renderer, render_w, render_h,
            SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    }

    vid_render_w = render_w;
    vid_render_h = render_h;
    vid_super_w  = super_w;
    vid_super_h  = super_h;
    vid_scale_active       = render_scale;
    vid_supersample_active = ss;

    vid.width         = super_w;
    vid.height        = super_h;
    vid.rowbytes      = super_w;
    vid.conwidth      = super_w;
    vid.conheight     = super_h;
    vid.conrowbytes   = super_w;
    vid.maxwarpwidth  = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.recalc_refdef = 1;

    {
        extern void D_FlushCaches(void);
        D_FlushCaches();
    }
    int zbuf_bytes  = super_w * super_h * sizeof(short);
    int cache_bytes = D_SurfaceCacheForRes(super_w, super_h);
    D_InitCaches((byte *)d_pzbuffer + zbuf_bytes, cache_bytes);
}
```

- [ ] **Step 3: Update VID_Init to use the new apply path**

In `VID_Init`, find the block at lines 369–378 that currently reads:

```c
int render_req = (int)vid_scale.value;
int render_scale = (render_req >= 1 && render_req <= 4) ? render_req : auto_scale;

int window_req = (int)vid_window_scale.value;
int window_scale = (window_req >= 1 && window_req <= 4) ? window_req : auto_scale;

vid_scale_active        = render_scale;
vid_window_scale_active = window_scale;
vid_render_w            = VID_WIDTH  * render_scale;
vid_render_h            = VID_HEIGHT * render_scale;
```

Replace with:

```c
int render_req = (int)vid_scale.value;
int render_scale = (render_req >= 1 && render_req <= 4) ? render_req : auto_scale;

int window_req = (int)vid_window_scale.value;
int window_scale = (window_req >= 1 && window_req <= 4) ? window_req : auto_scale;

int ss_req = (int)vid_supersample.value;
int ss = (ss_req >= 1 && ss_req <= 4) ? ss_req : 1;
while (render_scale * ss > 8 && ss > 1) ss--;

vid_scale_active        = render_scale;
vid_supersample_active  = ss;
vid_window_scale_active = window_scale;
vid_render_w            = VID_WIDTH  * render_scale;
vid_render_h            = VID_HEIGHT * render_scale;
vid_super_w             = vid_render_w * ss;
vid_super_h             = vid_render_h * ss;
```

Then, further down, the block at lines 407–418 that does the initial `SDL_SetRenderLogicalPresentation` + `SDL_CreateTexture` already uses `vid_render_w / vid_render_h` — that becomes the **display** size which is now correct (display, not framebuffer). Leave those two lines alone.

The viddef-fill block at lines 423–438 currently sets `vid.width = vid_render_w` (framebuffer) and `vid.conwidth = vid_render_w`. Change all six of these (vid.width, vid.height, vid.rowbytes, vid.conwidth, vid.conheight, vid.conrowbytes) to use `vid_super_w / vid_super_h`:

```c
vid.width      = vid_super_w;
vid.height     = vid_super_h;
vid.rowbytes   = vid_super_w;
/* aspect line unchanged */
vid.buffer     = vid_buffer;
vid.conbuffer  = vid_buffer;
vid.conwidth   = vid_super_w;
vid.conheight  = vid_super_h;
vid.conrowbytes = vid_super_w;
```

Finally, the hunk init block at lines 451–466 currently sizes itself by `vid_render_w/h`. Change it to size by `vid_super_w/h`:

```c
int zbuf_bytes  = vid_super_w * vid_super_h * sizeof(short);
int cache_bytes = D_SurfaceCacheForRes(vid_super_w, vid_super_h);
D_InitCaches((byte *)d_pzbuffer + zbuf_bytes, cache_bytes);
```

- [ ] **Step 4: Update VID_MenuKey to pass the current SS when changing render scale**

In `VID_MenuKey`, find lines 280–286:

```c
if (vid_menu_cursor < VID_NUM_SCALES)
{
    int new_scale = vid_scale_factors[vid_menu_cursor];
    vid_scale_active = new_scale;
    Cvar_SetValue("vid_scale", (float)new_scale);
    VID_ApplyScale(new_scale);
}
```

Replace with:

```c
if (vid_menu_cursor < VID_NUM_SCALES)
{
    int new_scale = vid_scale_factors[vid_menu_cursor];
    Cvar_SetValue("vid_scale", (float)new_scale);
    VID_ApplyResolution(new_scale, vid_supersample_active);
}
```

(The `vid_scale_active = new_scale` assignment moves into `VID_ApplyResolution` itself, which is the new single source of truth.)

- [ ] **Step 5: Build and verify SS=1 is byte-equivalent to today**

Run: `zig build run -- +map e1m1`
Expected: game renders identically to before — `vid_supersample` cvar is still `1` by default, so `vid_super_w == vid_render_w` and the framebuffer geometry hasn't changed. Open the Video Options menu (Options → Video) and toggle render-resolution to 2x / 4x. Each should still apply correctly.

- [ ] **Step 6: Sanity-check that a manual SS bump is wired**

At the console, type `vid_supersample 2; vid_restart` — wait, there's no `vid_restart` here. Instead, the cvar is only consumed by the apply path which currently only fires from the menu. To force a reapply at runtime, type at the console:

```
vid_supersample 2
```

then toggle render-res in the menu (e.g. 2x → 3x → 2x) — that will call `VID_ApplyResolution(2, 2)` and the world will now render at 1280×800 into a 640×400 display texture. The actual SSAA loop hasn't been written yet (next task), so the image will look wrong (only the top-left quadrant of the super-buffer will be palette-expanded). That's expected at this checkpoint. Confirm the engine *doesn't crash* — that's the assertion here.

After confirming, set `vid_supersample 1` and reapply.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "$(cat <<'EOF'
feat(renderer): plumb supersample factor into resolution-apply path

Splits vid_render_w/h (display resolution) from vid_super_w/h (engine
framebuffer = display * SS). Engine now renders at super-res; the SDL
texture and INTEGER_SCALE logical presentation stay at render-res. The
present-time downsample isn't here yet — at SS>1 the image will look
wrong until the next commit. SS=1 is identical to before.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Box-average SS² source pixels in VID_Update

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c:485-557` (`VID_Update`)

- [ ] **Step 1: Replace the palette-expand inner block**

Find the inner block of `VID_Update` from the `for (int y = 0; y < vid_render_h; y++)` loop down through the `SDL_UnlockTexture` call (currently lines 517–531). Replace **just the y/x loop** (not the surrounding `SDL_LockTexture` / unlock / render-present scaffolding) with the SSAA-aware version below.

Current loop (lines 517–526):

```c
for (int y = 0; y < vid_render_h; y++)
{
    unsigned   *dst = (unsigned *)((byte *)pixels + y * pitch);
    const byte *src = use_crop ? (crop_src + y * vid_render_w)
                               : (vid.buffer + y * vid.rowbytes);
    const byte *pal = use_crop ? (crop_pal + y * vid_render_w)
                               : (vid_palette_id + y * vid_render_w);
    for (int x = 0; x < vid_render_w; x++)
        dst[x] = lut[pal[x]][src[x]];
}
```

After replacement (note: `vid_render_w/h` is now the *display* size; the source buffer is at *super* size):

```c
int ss = vid_supersample_active;
/* Source stride for crop is the snapshot's own dimensions (matches super
   when crop was taken under the current resolution); for the live frame
   the stride is vid.rowbytes which already equals vid_super_w. */
int src_stride = use_crop ? crop_w : (int)vid.rowbytes;
const byte *src_base = use_crop ? crop_src     : vid.buffer;
const byte *pal_base = use_crop ? crop_pal     : vid_palette_id;

if (ss == 1)
{
    /* Fast path: 1:1 palette expand, identical to pre-SSAA code. */
    for (int y = 0; y < vid_render_h; y++)
    {
        unsigned   *dst = (unsigned *)((byte *)pixels + y * pitch);
        const byte *src = src_base + y * src_stride;
        const byte *pal = pal_base + y * src_stride;
        for (int x = 0; x < vid_render_w; x++)
            dst[x] = lut[pal[x]][src[x]];
    }
}
else
{
    /* SSAA path: average SS*SS source samples per output pixel.
       Mixed palette slots within a block fall back to the top-left
       sample (mixing across palettes would produce nonsense colors). */
    int ss2 = ss * ss;
    for (int ry = 0; ry < vid_render_h; ry++)
    {
        unsigned *dst = (unsigned *)((byte *)pixels + ry * pitch);
        for (int rx = 0; rx < vid_render_w; rx++)
        {
            int sx0 = rx * ss;
            int sy0 = ry * ss;
            int tl_off = sy0 * src_stride + sx0;
            byte tl_slot = pal_base[tl_off];

            int mixed = 0;
            for (int sy = 0; sy < ss && !mixed; sy++)
                for (int sx = 0; sx < ss; sx++)
                    if (pal_base[(sy0 + sy) * src_stride + (sx0 + sx)] != tl_slot)
                    { mixed = 1; break; }

            if (mixed)
            {
                dst[rx] = lut[tl_slot][src_base[tl_off]];
            }
            else
            {
                unsigned ar = 0, ag = 0, ab = 0;
                for (int sy = 0; sy < ss; sy++)
                    for (int sx = 0; sx < ss; sx++)
                    {
                        unsigned c = lut[tl_slot]
                            [src_base[(sy0 + sy) * src_stride + (sx0 + sx)]];
                        ar += (c >> 16) & 0xff;
                        ag += (c >>  8) & 0xff;
                        ab += (c >>  0) & 0xff;
                    }
                dst[rx] = 0xFF000000u
                        | ((ar / ss2) << 16)
                        | ((ag / ss2) <<  8)
                        |  (ab / ss2);
            }
        }
    }
}
```

- [ ] **Step 2: Update the palette-id reset to clear the super buffer**

At line ~544 there's:

```c
memset(vid_palette_id, 0, vid_render_w * vid_render_h);
```

Change to:

```c
memset(vid_palette_id, 0, vid_super_w * vid_super_h);
```

- [ ] **Step 3: Update Crop_PresentOverlay call (if it uses render dims)**

The current code calls `Crop_PresentOverlay((unsigned *)pixels, pitch, vid_render_w, vid_render_h);` at line ~529. Those are dimensions of the SDL texture (display res), which is still correct after this change. **No edit needed** — confirm by reading that line and moving on.

- [ ] **Step 4: Build and verify SS=1 looks identical**

Run: `zig build run -- +map e1m1`
Expected: game renders normally. SS is still `1` by default; the fast path is taken in `VID_Update` and the result must be visually byte-identical to before this task.

- [ ] **Step 5: Smoke-test SS=2**

At the console, type:

```
vid_supersample 2
```

Then open Video Options menu (Options → Video) and toggle render-res from its current value to one other value and back (this forces `VID_ApplyResolution` to re-run with `ss=2`). Walk near a sloped wall (e.g. the lava channel walls in e1m1's start area). Expected: world edges visibly anti-alias compared to SS=1 — sloped texture seams stop crawling/sparkling as you move. HUD numbers will look smaller (this is the known design trade-off).

- [ ] **Step 6: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "$(cat <<'EOF'
feat(renderer): box-average SS² source pixels in VID_Update

The actual SSAA step. At ss=1 the existing fast path is taken; at ss>1
each render-res output pixel averages its SS*SS source pixels in palette-
expanded ARGB. Same-palette-slot pixels average; mixed-slot blocks fall
back to the top-left sample to avoid Quake/Doom palette interpolation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Add Supersample group to the Video Options menu

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c:83-95` (menu constants and labels)
- Modify: `sdlquake/platform/vid_sdl.c:225-258` (`VID_MenuDraw`)
- Modify: `sdlquake/platform/vid_sdl.c:260-304` (`VID_MenuKey`)
- Modify: `sdlquake/platform/vid_sdl.c:468` (`VID_Init`: initial cursor position)

- [ ] **Step 1: Add SS labels and update menu constants**

Find the constants block at lines 83–95:

```c
#define VID_NUM_SCALES 4
// Cursor positions: 0-3 render scales, 4-7 window scales, 8 = save-position.
#define VID_MENU_ITEMS (VID_NUM_SCALES * 2 + 1)
#define VID_MENU_SAVE_POS (VID_NUM_SCALES * 2)
static const int    vid_scale_factors[VID_NUM_SCALES] = {1, 2, 3, 4};
static const char  *vid_scale_labels[VID_NUM_SCALES]  = {
    "1x  320x200",
    "2x  640x400",
    "3x  960x600",
    "4x  1280x800"
};
// 0-3: render-resolution rows, 4-7: window-size rows.
static int vid_menu_cursor = 0;
```

Replace with:

```c
#define VID_NUM_SCALES 4
// Cursor positions: 0-3 supersample, 4-7 render scale, 8-11 window scale, 12 = save.
#define VID_MENU_ITEMS    (VID_NUM_SCALES * 3 + 1)
#define VID_MENU_SS_BASE      0
#define VID_MENU_RENDER_BASE  (VID_NUM_SCALES)
#define VID_MENU_WINDOW_BASE  (VID_NUM_SCALES * 2)
#define VID_MENU_SAVE_POS     (VID_NUM_SCALES * 3)
static const int    vid_scale_factors[VID_NUM_SCALES] = {1, 2, 3, 4};
static const char  *vid_scale_labels[VID_NUM_SCALES]  = {
    "1x  320x200",
    "2x  640x400",
    "3x  960x600",
    "4x  1280x800"
};
static const char  *vid_ss_labels[VID_NUM_SCALES] = {
    "1x  (off)",
    "2x",
    "3x",
    "4x"
};
static int vid_menu_cursor = 0;
```

- [ ] **Step 2: Rewrite VID_MenuDraw with three groups**

Replace the entire body of `VID_MenuDraw` (lines 225–258, between the `{` and `}` of the function) with:

```c
qpic_t *p = Draw_CachePic("gfx/vidmodes.lmp");
M_DrawPic((320 - p->width) / 2, 4, p);

M_Print(64, 30, "Supersample");
int ss_y = 42;
for (int i = 0; i < VID_NUM_SCALES; i++)
{
    if (vid_scale_factors[i] == vid_supersample_active)
        M_PrintWhite(80, ss_y + i * 8, (char *)vid_ss_labels[i]);
    else
        M_Print(80, ss_y + i * 8, (char *)vid_ss_labels[i]);
    if (vid_menu_cursor == VID_MENU_SS_BASE + i)
        M_DrawCharacter(72, ss_y + i * 8, 12 + ((int)(realtime * 4) & 1));
}

M_Print(64, 78, "Render Resolution");
int render_y = 90;
for (int i = 0; i < VID_NUM_SCALES; i++)
{
    if (vid_scale_factors[i] == vid_scale_active)
        M_PrintWhite(80, render_y + i * 8, (char *)vid_scale_labels[i]);
    else
        M_Print(80, render_y + i * 8, (char *)vid_scale_labels[i]);
    if (vid_menu_cursor == VID_MENU_RENDER_BASE + i)
        M_DrawCharacter(72, render_y + i * 8, 12 + ((int)(realtime * 4) & 1));
}

M_Print(64, 126, "Window Size");
int window_y = 138;
for (int i = 0; i < VID_NUM_SCALES; i++)
{
    if (vid_scale_factors[i] == vid_window_scale_active)
        M_PrintWhite(80, window_y + i * 8, (char *)vid_scale_labels[i]);
    else
        M_Print(80, window_y + i * 8, (char *)vid_scale_labels[i]);
    if (vid_menu_cursor == VID_MENU_WINDOW_BASE + i)
        M_DrawCharacter(72, window_y + i * 8, 12 + ((int)(realtime * 4) & 1));
}

int save_y = 176;
M_Print(80, save_y, "Save Window Pos & Size");
if (vid_menu_cursor == VID_MENU_SAVE_POS)
    M_DrawCharacter(72, save_y, 12 + ((int)(realtime * 4) & 1));
```

- [ ] **Step 3: Update VID_MenuKey to dispatch by group**

Replace the `K_ENTER` / `K_SPACE` case body (lines 278–299) with:

```c
case K_ENTER:
case K_SPACE:
    if (vid_menu_cursor < VID_MENU_RENDER_BASE)
    {
        // Supersample row.
        int new_ss = vid_scale_factors[vid_menu_cursor - VID_MENU_SS_BASE];
        Cvar_SetValue("vid_supersample", (float)new_ss);
        VID_ApplyResolution(vid_scale_active, new_ss);
    }
    else if (vid_menu_cursor < VID_MENU_WINDOW_BASE)
    {
        // Render-resolution row.
        int new_scale = vid_scale_factors[vid_menu_cursor - VID_MENU_RENDER_BASE];
        Cvar_SetValue("vid_scale", (float)new_scale);
        VID_ApplyResolution(new_scale, vid_supersample_active);
    }
    else if (vid_menu_cursor < VID_MENU_SAVE_POS)
    {
        // Window-size row.
        int new_scale = vid_scale_factors[vid_menu_cursor - VID_MENU_WINDOW_BASE];
        vid_window_scale_active = new_scale;
        Cvar_SetValue("vid_window_scale", (float)new_scale);
        VID_ApplyWindowScale(new_scale);
    }
    else
    {
        VID_SaveWindow();
    }
    S_LocalSound("misc/menu2.wav");
    break;
```

- [ ] **Step 4: Set a sensible initial cursor**

In `VID_Init`, find line 468 (`vid_menu_cursor = vid_scale_active - 1;`) and replace with:

```c
vid_menu_cursor = VID_MENU_RENDER_BASE + (vid_scale_active - 1);
```

(Lands the cursor on the active render-res row when the menu opens, matching today's behavior.)

- [ ] **Step 5: Build and visually verify the menu**

Run: `zig build run -- +map e1m1`
Open the Video Options menu (Esc → Options → Video). Expected layout:

```
        VIDEO OPTIONS

  Supersample
    1x  (off)        ← highlighted (active)
    2x
    3x
    4x

  Render Resolution
    1x  320x200      ← cursor here on open
    2x  640x400
    3x  960x600
    4x  1280x800

  Window Size
    1x  320x200
    ...

  Save Window Pos & Size
```

Arrow-key navigation should cycle through all 13 positions. Selecting a Supersample row should immediately re-apply with the new SS. Selecting a Render-Resolution row should keep the current SS. The cap (render·SS ≤ 8) means picking 4x render then 4x SS will silently clamp SS to 2 — verify by selecting render=4x then SS=4x and observing the highlighted SS row becomes `2x` after the click.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "$(cat <<'EOF'
feat(menu): add Supersample group to Video Options

Three-group layout: Supersample, Render Resolution, Window Size, plus
Save row. Each menu pick dispatches to VID_ApplyResolution with the
current (scale, ss) pair, so the combined cap clamp lives in one place.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: End-to-end smoke test

**Files:** none modified (verification only).

- [ ] **Step 1: Visual diff at SS=1 vs SS=2**

Run: `zig build run -- +map e1m1`

At the start of e1m1, walk a few steps so you're looking at the lava and the diagonal wall behind the slipgate. Take a screenshot (use the engine's screenshot path or the MCP `screenshot` tool — see the smoke-test-rig memory).

Quit. Edit `id1/config.cfg` to set `vid_supersample "2"` (or set it via the menu before screenshotting). Relaunch and take a screenshot from the same vantage.

Compare: SS=2 should show visibly smoother diagonal/lava edges with no shimmer when you turn slowly. HUD elements should be smaller than SS=1.

- [ ] **Step 2: Verify cvar archived correctly**

After picking SS=2 in the menu, quit normally. Open `id1/config.cfg` in an editor and confirm a line `vid_supersample "2"` is present.

Relaunch the engine. The opening view should be at SS=2 immediately (no menu interaction needed).

- [ ] **Step 3: Verify the cap clamp works**

In the menu, set Render Resolution = 4x, then set Supersample = 4x. The Supersample highlight should land on 2x (not 4x) because 4·4 > 8. Confirm via the console: `vid_supersample` should report `2`.

- [ ] **Step 4: No commit unless tweaks**

If any visual issue surfaces (mis-positioned menu text, palette mismatch, crash), record it and fix inline before declaring complete. Otherwise this task is verification-only — no commit.

---

## Self-Review

**Spec coverage:**

| Spec requirement | Plan task |
|---|---|
| `vid_supersample` cvar (1..4, archived) | Task 2 |
| Combined cap `vid_scale · SS ≤ 8` | Task 3 step 2 (clamp inside `VID_ApplyResolution`), Task 5 step 3 (menu hits the same clamp) |
| Menu group "Supersample" with active highlight, `1x (off)` label | Task 5 steps 1-2 |
| Render pipeline: super buffer + downsample to render-res texture | Task 3 (state) + Task 4 (downsample loop) |
| Same-palette-slot averaging; mixed → top-left fallback | Task 4 step 1 |
| `VID_RENDER_MAX_W/H` → 2560×1600 | Task 1 |
| Single source of truth for resolution-apply (`VID_ApplyResolution`) | Task 3 step 2 |
| Cvar preload from config | Task 2 step 2 |
| SS=1 byte-identical fast path | Task 4 step 1 (`if (ss == 1)` branch) |
| Smoke-test on e1m1 | Task 6 |

**Placeholder scan:** none of the listed patterns appear; all code is given in full.

**Type consistency:** `VID_ApplyResolution(int render_scale, int ss)` signature appears in Task 3 step 2 (definition), Task 3 step 4 (menu call), Task 5 step 3 (menu calls). All three pass `(scale, ss)` in that order. Globals `vid_super_w / vid_super_h / vid_render_w / vid_render_h / vid_supersample_active / vid_scale_active` are introduced in Task 3 step 1 and referenced consistently afterwards.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-24-supersample.md`. Two execution options:

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?

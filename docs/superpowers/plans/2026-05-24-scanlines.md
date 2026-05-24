# Scanlines Rendering Option Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a CRT-style scanline overlay applied at the window's physical pixel grid, controlled by three cvars (`vid_scanlines`, `vid_scanline_intensity`, `vid_scanline_size`) and three new Video Options menu rows.

**Architecture:** All changes live in `sdlquake/platform/vid_sdl.c`. A 1×output_h `SDL_Texture` holds the scanline pattern with `SDL_BLENDMODE_MOD`; it is drawn full-screen after the framebuffer texture but before the ImGui dev overlay. Logical presentation is temporarily disabled around the overlay draw so the 1-column texture maps to physical pixels. Lazy regeneration: the texture is rebuilt only when window output size or scanline cvars change.

**Tech Stack:** C (gnu89), SDL3.

**Spec:** `docs/superpowers/specs/2026-05-24-scanlines-design.md`

**Deviations from spec (locked in here):**
- Spec sketched the menu as three 4-radio groups; that overflows the 320×200 menu canvas. This plan uses compact single-line stepper rows (one cursor slot per row, left/right arrows cycle values). Cvar values and persistence are identical.
- Scanline overlay is drawn *between* the framebuffer texture and `ImguiLayer_Render()` so the dev overlay stays crisp.

**Verification:** No automated tests for the video path (per `CLAUDE.md`); verification is by build + visual inspection. Each task ends with a build step.

---

## File Structure

Only one file changes:

- **Modify:** `sdlquake/platform/vid_sdl.c`
  - Add cvar declarations and statics near the existing `vid_supersample` declaration (~L83-92).
  - Add config-load `sscanf` lines in `vid_preload_cvars_from_config` (~L387).
  - Register the three cvars in `VID_Init` (~L400).
  - Add a `Scanline_Ensure()` + `Scanline_Draw()` helper section.
  - Hook `Scanline_Draw()` into `VID_Update` between `SDL_RenderTexture` and `ImguiLayer_Render` (~L650).
  - Add menu cursor slot constants, rendering rows in `VID_MenuDraw`, and key handling in `VID_MenuKey`.

No new files. No tests.

---

## Task 1: Register cvars + persist them (no behavior change)

**Goal:** Add the three cvars, register them, and round-trip them through `config.cfg`. Nothing visible changes yet.

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c`

- [ ] **Step 1: Add the cvar declarations**

In `sdlquake/platform/vid_sdl.c`, immediately after the existing `vid_supersample` declaration (currently L83-84):

```c
static int vid_supersample_active = 1;
static cvar_t vid_supersample = {"vid_supersample", "1", true};

// Scanlines (CRT overlay). All three are persisted to config.cfg.
static cvar_t vid_scanlines          = {"vid_scanlines",          "0",   true};
static cvar_t vid_scanline_intensity = {"vid_scanline_intensity", "0.5", true};
static cvar_t vid_scanline_size      = {"vid_scanline_size",      "1",   true};
```

- [ ] **Step 2: Register the cvars in `VID_Init`**

Find the `Cvar_RegisterVariable(&vid_supersample);` line (~L400) and add three lines after it:

```c
    Cvar_RegisterVariable(&vid_supersample);
    Cvar_RegisterVariable(&vid_scanlines);
    Cvar_RegisterVariable(&vid_scanline_intensity);
    Cvar_RegisterVariable(&vid_scanline_size);
```

- [ ] **Step 3: Add config-preload `sscanf` lines**

Find the `vid_supersample` sscanf line in `vid_preload_cvars_from_config` (~L387) and add three lines after it. The new block looks like:

```c
        else if (sscanf(line, "vid_supersample \"%f", &v) == 1)         Cvar_SetValue("vid_supersample", v);
        else if (sscanf(line, "vid_scanlines \"%f", &v) == 1)           Cvar_SetValue("vid_scanlines", v);
        else if (sscanf(line, "vid_scanline_intensity \"%f", &v) == 1)  Cvar_SetValue("vid_scanline_intensity", v);
        else if (sscanf(line, "vid_scanline_size \"%f", &v) == 1)       Cvar_SetValue("vid_scanline_size", v);
```

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build, no warnings about unused statics (the cvars are referenced via `Cvar_RegisterVariable`).

- [ ] **Step 5: Sanity-check the cvars work**

Run: `zig build run`
At the game's console (`~`), type:

```
vid_scanlines 1
vid_scanlines
```

Expected: the second command echoes `"vid_scanlines" is "1"`. Nothing visual yet — that comes in Task 2. Quit normally so config.cfg is written.

Inspect `id1/config.cfg` for the three new keys:

```
grep '^vid_scanline' id1/config.cfg
```

Expected: three lines, one per cvar.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "feat(renderer): register vid_scanline* cvars (no behavior yet)

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Render the scanline overlay

**Goal:** Hook the overlay into `VID_Update`. After this task scanlines are visible when `vid_scanlines 1` is set from the console.

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c`

- [ ] **Step 1: Add the scanline texture statics and helpers**

Add a new section just above `VID_Update` (~L540). Find:

```c
// ---------------------------------------------------------------------------
// Frame update: expand 8-bit -> 32-bit, upload, present
// ---------------------------------------------------------------------------

void VID_Update(vrect_t *rects)
```

Insert immediately above it:

```c
// ---------------------------------------------------------------------------
// Scanlines: CRT-style horizontal-line overlay
// ---------------------------------------------------------------------------

static SDL_Texture *sdl_scanline_tex = NULL;
static int   scanline_cached_h        = -1;
static int   scanline_cached_size     = -1;
static float scanline_cached_intensity = -1.0f;

// Rebuild the 1xH scanline column if window output height, size, or intensity
// changed. Width is fixed at 1; SDL stretches it horizontally on draw.
static void Scanline_Ensure(void)
{
    int out_w = 0, out_h = 0;
    SDL_GetRenderOutputSize(sdl_renderer, &out_w, &out_h);
    if (out_h <= 0) return;

    int   size      = (int)vid_scanline_size.value;
    if (size < 1) size = 1;
    if (size > 3) size = 3;
    float intensity = vid_scanline_intensity.value;
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;

    if (sdl_scanline_tex &&
        scanline_cached_h        == out_h &&
        scanline_cached_size     == size &&
        scanline_cached_intensity == intensity)
        return;

    if (sdl_scanline_tex) {
        SDL_DestroyTexture(sdl_scanline_tex);
        sdl_scanline_tex = NULL;
    }

    sdl_scanline_tex = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_STREAMING,
        1, out_h);
    if (!sdl_scanline_tex) {
        Con_Printf("Scanline_Ensure: SDL_CreateTexture failed: %s\n",
                   SDL_GetError());
        return;
    }
    SDL_SetTextureBlendMode(sdl_scanline_tex, SDL_BLENDMODE_MOD);
    SDL_SetTextureScaleMode(sdl_scanline_tex, SDL_SCALEMODE_NEAREST);

    void *pixels;
    int   pitch;
    if (SDL_LockTexture(sdl_scanline_tex, NULL, &pixels, &pitch) >= 0) {
        unsigned char dark = (unsigned char)((1.0f - intensity) * 255.0f + 0.5f);
        for (int y = 0; y < out_h; y++) {
            int band = (y / size) % 2;  // 0 = dark, 1 = bright
            unsigned char *row = (unsigned char *)pixels + y * pitch;
            unsigned char v = (band == 0) ? dark : 255;
            // SDL_PIXELFORMAT_RGBA8888: byte order R,G,B,A on big-endian
            // semantics; SDL handles host endianness when locking.
            row[0] = v;
            row[1] = v;
            row[2] = v;
            row[3] = 255;
        }
        SDL_UnlockTexture(sdl_scanline_tex);
    }

    scanline_cached_h         = out_h;
    scanline_cached_size      = size;
    scanline_cached_intensity = intensity;
}

// Draw the scanline overlay full-screen at physical-pixel scale.
// Must be called between the framebuffer SDL_RenderTexture and ImguiLayer_Render
// so the dev overlay stays crisp.
static void Scanline_Draw(void)
{
    if (vid_scanlines.value == 0.0f) return;
    Scanline_Ensure();
    if (!sdl_scanline_tex) return;

    // Drop logical presentation so the 1-column texture stretches across the
    // entire physical render output (not the 320x200-equivalent logical area).
    SDL_SetRenderLogicalPresentation(sdl_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_RenderTexture(sdl_renderer, sdl_scanline_tex, NULL, NULL);
    SDL_SetRenderLogicalPresentation(sdl_renderer,
                                     vid_render_w, vid_render_h,
                                     SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
}
```

- [ ] **Step 2: Hook `Scanline_Draw()` into `VID_Update`**

Find this block inside `VID_Update` (~L648-652):

```c
        SDL_UnlockTexture(sdl_texture);
        SDL_RenderClear(sdl_renderer);
        SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
        ImguiLayer_Render();
        SDL_RenderPresent(sdl_renderer);
```

Insert one line between the framebuffer draw and the ImGui draw:

```c
        SDL_UnlockTexture(sdl_texture);
        SDL_RenderClear(sdl_renderer);
        SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
        Scanline_Draw();
        ImguiLayer_Render();
        SDL_RenderPresent(sdl_renderer);
```

- [ ] **Step 3: Free the texture on shutdown**

Find the existing `VID_Shutdown` function (search for `void VID_Shutdown`). If it currently destroys `sdl_texture`, add a matching destroy for `sdl_scanline_tex` immediately before that, e.g.:

```c
    if (sdl_scanline_tex) { SDL_DestroyTexture(sdl_scanline_tex); sdl_scanline_tex = NULL; }
    scanline_cached_h = -1;
    scanline_cached_size = -1;
    scanline_cached_intensity = -1.0f;
```

If `VID_Shutdown` doesn't already destroy `sdl_texture`, skip this step — the OS reclaims the texture on process exit and there's no leak.

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 5: Visual verification via console**

Run: `zig build run`
At the console:

```
vid_scanlines 1
```

Expected: alternating 1-physical-pixel horizontal dark/bright bands appear across the entire window (title screen, then anywhere in-game).

Then test the other knobs:

```
vid_scanline_intensity 1
vid_scanline_intensity 0.25
vid_scanline_size 2
vid_scanline_size 3
vid_scanline_size 1
vid_scanlines 0
```

Expected:
- Intensity 1.0 → dark bands are pure black (very harsh).
- Intensity 0.25 → dark bands barely visible.
- Size 2 → bands are 2 physical pixels thick (2 dark / 2 bright period).
- Size 3 → bands are 3 physical pixels thick.
- Size 1 → back to single-pixel bands.
- `vid_scanlines 0` → effect disappears entirely.

Resize the window. Scanlines should re-fit on the next frame at the new height (still 1 physical pixel per band when size=1).

- [ ] **Step 6: Verify ImGui dev overlay stays crisp**

With scanlines on, press `F1` (or whatever toggles the ImGui dev overlay — see `sdlquake/engine/imgui_layer.c` if unsure). The dev overlay UI should NOT have scanlines drawn over it; it should look normal.

If the dev overlay also has scanlines, the `Scanline_Draw()` call was placed in the wrong order — verify it sits *before* `ImguiLayer_Render()` in `VID_Update`.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "feat(renderer): CRT scanline overlay at physical-pixel grid

vid_scanlines / vid_scanline_intensity / vid_scanline_size control a
SDL_BLENDMODE_MOD overlay drawn between the framebuffer texture and the
ImGui dev overlay. Texture is a 1xH column lazily rebuilt on window
resize or cvar change.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Add Video Options menu rows

**Goal:** Three compact stepper rows below the existing groups: Scanlines (On/Off), Intensity (25/50/75/100%), Size (1/2/3 px). Left/right arrows cycle values. After this task the feature is reachable without using the console.

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c`

- [ ] **Step 1: Reshuffle menu cursor slot constants**

Find this block (~L86-93):

```c
#define VID_NUM_SCALES 4
// Cursor positions: 0-3 supersample, 4-7 render scale, 8-11 window scale, 12 = save.
#define VID_MENU_ITEMS    (VID_NUM_SCALES * 3 + 1)
#define VID_MENU_SS_BASE      0
#define VID_MENU_RENDER_BASE  (VID_NUM_SCALES)
#define VID_MENU_WINDOW_BASE  (VID_NUM_SCALES * 2)
#define VID_MENU_SAVE_POS     (VID_NUM_SCALES * 3)
```

Replace with:

```c
#define VID_NUM_SCALES 4
// Cursor positions:
//   0..3   supersample
//   4..7   render scale
//   8..11  window scale
//   12     scanlines toggle
//   13     scanline intensity
//   14     scanline size
//   15     save
#define VID_MENU_SS_BASE          0
#define VID_MENU_RENDER_BASE      (VID_NUM_SCALES)
#define VID_MENU_WINDOW_BASE      (VID_NUM_SCALES * 2)
#define VID_MENU_SCANLINES_POS    (VID_NUM_SCALES * 3)
#define VID_MENU_SL_INTENSITY_POS (VID_NUM_SCALES * 3 + 1)
#define VID_MENU_SL_SIZE_POS      (VID_NUM_SCALES * 3 + 2)
#define VID_MENU_SAVE_POS         (VID_NUM_SCALES * 3 + 3)
#define VID_MENU_ITEMS            (VID_MENU_SAVE_POS + 1)
```

- [ ] **Step 2: Add helper to snap intensity to four buckets**

Add helpers immediately above `VID_MenuDraw` (~L256). These map between the float cvar and the four discrete menu buckets (0.25, 0.50, 0.75, 1.00):

```c
static const float vid_sl_intensity_buckets[4] = { 0.25f, 0.50f, 0.75f, 1.00f };
static const char *vid_sl_intensity_labels[4]  = { "25%", "50%", "75%", "100%" };

static int vid_sl_intensity_bucket(void)
{
    float v = vid_scanline_intensity.value;
    int   best_i = 1;       // default bucket for 0.5
    float best_d = 1e9f;
    for (int i = 0; i < 4; i++) {
        float d = v - vid_sl_intensity_buckets[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best_i = i; }
    }
    return best_i;
}

static int vid_sl_size_index(void)
{
    int s = (int)vid_scanline_size.value;
    if (s < 1) s = 1;
    if (s > 3) s = 3;
    return s - 1;  // 0-based index for cycling
}
```

- [ ] **Step 3: Render the new menu rows**

Find the end of `VID_MenuDraw`, currently:

```c
    int save_y = 176;
    M_Print(80, save_y, "Save Window Pos & Size");
    if (vid_menu_cursor == VID_MENU_SAVE_POS)
        M_DrawCharacter(72, save_y, 12 + ((int)(realtime * 4) & 1));
}
```

Replace with:

```c
    // Compact scanline section — three single-line stepper rows.
    int sl_y = 170;

    M_Print(64, sl_y, "Scanlines");
    M_Print(160, sl_y, vid_scanlines.value != 0.0f ? "On" : "Off");
    if (vid_menu_cursor == VID_MENU_SCANLINES_POS)
        M_DrawCharacter(56, sl_y, 12 + ((int)(realtime * 4) & 1));

    M_Print(64, sl_y + 8, "Intensity");
    M_Print(160, sl_y + 8, (char *)vid_sl_intensity_labels[vid_sl_intensity_bucket()]);
    if (vid_menu_cursor == VID_MENU_SL_INTENSITY_POS)
        M_DrawCharacter(56, sl_y + 8, 12 + ((int)(realtime * 4) & 1));

    M_Print(64, sl_y + 16, "Size");
    {
        char sz[8];
        snprintf(sz, sizeof sz, "%dpx", vid_sl_size_index() + 1);
        M_Print(160, sl_y + 16, sz);
    }
    if (vid_menu_cursor == VID_MENU_SL_SIZE_POS)
        M_DrawCharacter(56, sl_y + 16, 12 + ((int)(realtime * 4) & 1));

    int save_y = sl_y + 24;
    M_Print(80, save_y, "Save Window Pos & Size");
    if (vid_menu_cursor == VID_MENU_SAVE_POS)
        M_DrawCharacter(72, save_y, 12 + ((int)(realtime * 4) & 1));
}
```

- [ ] **Step 4: Handle left/right keys for stepper cycling**

In `VID_MenuKey`, the existing code only handles `K_UPARROW`, `K_DOWNARROW`, `K_ENTER`, `K_SPACE`, and `K_ESCAPE`. Add left/right handling. Find:

```c
    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor + 1) % VID_MENU_ITEMS;
        break;

    case K_ENTER:
```

Insert new cases between `K_DOWNARROW` and `K_ENTER`:

```c
    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor + 1) % VID_MENU_ITEMS;
        break;

    case K_LEFTARROW:
    case K_RIGHTARROW:
    {
        int dir = (key == K_RIGHTARROW) ? +1 : -1;
        if (vid_menu_cursor == VID_MENU_SCANLINES_POS) {
            float nv = (vid_scanlines.value != 0.0f) ? 0.0f : 1.0f;
            Cvar_SetValue("vid_scanlines", nv);
            S_LocalSound("misc/menu3.wav");
        } else if (vid_menu_cursor == VID_MENU_SL_INTENSITY_POS) {
            int b = vid_sl_intensity_bucket();
            b = (b + dir + 4) % 4;
            Cvar_SetValue("vid_scanline_intensity", vid_sl_intensity_buckets[b]);
            S_LocalSound("misc/menu3.wav");
        } else if (vid_menu_cursor == VID_MENU_SL_SIZE_POS) {
            int s = vid_sl_size_index();
            s = (s + dir + 3) % 3;
            Cvar_SetValue("vid_scanline_size", (float)(s + 1));
            S_LocalSound("misc/menu3.wav");
        }
        break;
    }

    case K_ENTER:
```

- [ ] **Step 5: Handle Enter on scanline rows as a no-op (so it doesn't fall into the supersample branch)**

In `VID_MenuKey`'s `K_ENTER`/`K_SPACE` block, the existing chain uses `if (vid_menu_cursor < VID_MENU_RENDER_BASE) ... else if ... < VID_MENU_WINDOW_BASE ... else if ... < VID_MENU_SAVE_POS ... else { VID_SaveWindow(); }`. The `< VID_MENU_SAVE_POS` branch currently treats anything from 8..11 as a "window-size row," but with the new constants the scanline slots 12, 13, 14 also satisfy `< VID_MENU_SAVE_POS`. Fix the comparison so window-size only matches its actual range.

Find:

```c
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
```

Replace with:

```c
        else if (vid_menu_cursor < VID_MENU_WINDOW_BASE + VID_NUM_SCALES)
        {
            // Window-size row.
            int new_scale = vid_scale_factors[vid_menu_cursor - VID_MENU_WINDOW_BASE];
            vid_window_scale_active = new_scale;
            Cvar_SetValue("vid_window_scale", (float)new_scale);
            VID_ApplyWindowScale(new_scale);
        }
        else if (vid_menu_cursor == VID_MENU_SAVE_POS)
        {
            VID_SaveWindow();
        }
        else
        {
            // Scanline rows: Enter mirrors right-arrow (cycle forward).
            int dir = +1;
            if (vid_menu_cursor == VID_MENU_SCANLINES_POS) {
                float nv = (vid_scanlines.value != 0.0f) ? 0.0f : 1.0f;
                Cvar_SetValue("vid_scanlines", nv);
            } else if (vid_menu_cursor == VID_MENU_SL_INTENSITY_POS) {
                int b = (vid_sl_intensity_bucket() + dir + 4) % 4;
                Cvar_SetValue("vid_scanline_intensity", vid_sl_intensity_buckets[b]);
            } else if (vid_menu_cursor == VID_MENU_SL_SIZE_POS) {
                int s = (vid_sl_size_index() + dir + 3) % 3;
                Cvar_SetValue("vid_scanline_size", (float)(s + 1));
            }
        }
```

- [ ] **Step 6: Build**

Run: `zig build`
Expected: clean build.

- [ ] **Step 7: Visual verification of the menu**

Run: `zig build run`. From the main menu navigate to Options → Video Options. Press Down arrow repeatedly until the cursor reaches the new "Scanlines" row.

Verify:
- Scanlines row: Left/Right (or Enter) toggles between "On" and "Off". With "On" you see scanlines on screen behind the menu.
- Intensity row: Left/Right cycles 25% → 50% → 75% → 100% → 25% (and back). The visible darkness of the bands changes accordingly.
- Size row: Left/Right cycles 1px → 2px → 3px → 1px. Band thickness changes accordingly.
- Save Window Pos & Size still works (Enter triggers `VID_SaveWindow`, no scanline cycling).
- Supersample, Render Resolution, Window Size rows still behave correctly (regression check — the cursor slot reshuffle could break them).

- [ ] **Step 8: Verify persistence across launches**

While in-game, set:
- Scanlines = On
- Intensity = 75%
- Size = 2px

Quit normally. Re-launch:

```
zig build run
```

Title screen should already show 75%-intensity 2px scanlines. The menu should reflect the same values.

- [ ] **Step 9: Commit**

```bash
git add sdlquake/platform/vid_sdl.c
git commit -m "feat(menu): add Scanlines / Intensity / Size to Video Options

Three compact stepper rows below the existing Supersample / Render /
Window groups. Left/right arrows cycle values; Enter on Save still
saves the window position.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

---

## Final verification

After all three tasks are committed:

- [ ] Run `git log --oneline -3` and confirm three commits in order: cvars, render, menu.
- [ ] Run `zig build` once more from a clean tree — clean build, no warnings.
- [ ] `zig build run +map e1m1`, open the Video Options menu, exercise all three new rows in-game (not just at the title screen) to confirm scanlines render on top of the world, HUD, and Quake menu equally.
- [ ] Toggle the ImGui dev overlay once more in-game with scanlines on — confirm the dev overlay still draws crisp on top of the scanlines.

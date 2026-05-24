# Rect-selection screenshot mode

Status: approved 2026-05-24

## Motivation

The `screenshot` console command already writes PNG (see `screen.c:722` and `vid_sdl.c:570`; the PCX path is gone despite some lingering variable names in the uncompiled `gl_screen.c`). What's missing is a way to grab a *subrect* of the framebuffer so a cropped image can be passed straight to Claude Code without an external editor step.

Two modes:

1. **Fullscreen** — unchanged. `screenshot` → next free `id1/quakeNN.png`.
2. **Rect** — new. `screenshot rect` enters a modal selection state: the current frame freezes visually, the OS cursor appears, mouse drag defines the crop, release saves, Esc cancels.

Both modes share the same `quakeNN.png` filename counter and write into `com_gamedir` (`id1/`).

## Architecture

A new platform-side module owns the modal state and overlay rendering. The engine-side `SCR_ScreenShot_f` only learns about the new subcommand; it doesn't carry rect state itself.

### New module: `sdlquake/platform/crop_screenshot.{c,h}`

State (file-static):

- `byte *frozen` — palette-indexed snapshot of `vid.buffer` taken on `Crop_Enter`.
- `byte *frozen_pal` — matching `vid_palette_id` slots so colour expansion is identical to the live path.
- `int frozen_w, frozen_h` — captured framebuffer dimensions.
- `int rect_x0, rect_y0, rect_x1, rect_y1` — current drag endpoints in framebuffer (logical) coords.
- `qboolean dragging, active`
- `char out_path[MAX_OSPATH]`

Public API:

```c
void Crop_Enter(const char *out_path);
int  Crop_Active(void);
int  Crop_HandleEvent(const SDL_Event *ev);     // returns 1 if consumed
void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h);
void Crop_Exit(void);                            // frees frozen buffers, clears active
```

`Crop_Enter` allocates both frozen buffers, `memcpy`s from `vid.buffer` and `vid_palette_id`, then sets `active = true`. `Crop_Exit` frees and resets.

### Touch points

| File | Change |
|---|---|
| `sdlquake/engine_src/screen.c` (`SCR_ScreenShot_f`) | If `Cmd_Argv(1)` equals `"rect"`, find next free `quakeNN.png` using the existing 00..99 loop, then call `Crop_Enter(checkname)` (declared `extern`). Otherwise existing fullscreen path. |
| `sdlquake/platform/vid_sdl.c` (`VID_Update`) | If `Crop_Active()`, expand the **frozen** buffer into the SDL texture instead of `vid.buffer`, then call `Crop_PresentOverlay()` to dim outside the rect and stamp the border. Imgui/present remain unchanged. |
| `sdlquake/platform/in_sdl.c` (`IN_ProcessEvents`, `IN_WantRelativeMouse`) | Before existing dispatch: `if (Crop_Active() && Crop_HandleEvent(&ev)) continue;`. Also extend `IN_WantRelativeMouse()` to return `false` when crop is active, so the OS cursor reappears. |

### Coordinate mapping

Mouse events arrive in window pixels. The framebuffer is logical 320×200 (or whatever `vid.width/height` is) presented via `SDL_SetRenderLogicalPresentation(INTEGER_SCALE)`. Use `SDL_RenderCoordinatesFromWindow(renderer, wx, wy, &rx, &ry)` to convert, then clamp to `[0, vid.width-1] × [0, vid.height-1]`.

### Event handling inside `Crop_HandleEvent`

| Event | Action |
|---|---|
| `SDL_EVENT_MOUSE_BUTTON_DOWN` (left) | `dragging = true`, `rect_x0 = rect_x1 = clamped_x`, same for y. |
| `SDL_EVENT_MOUSE_MOTION` (while dragging) | Update `rect_x1, rect_y1`. |
| `SDL_EVENT_MOUSE_BUTTON_UP` (left) | Commit: normalise (min/max), enforce min size 1×1, encode + write PNG, `Con_Printf("Wrote %s\n", out_path)`, `Crop_Exit()`. |
| `SDL_EVENT_KEY_DOWN` with `SDL_SCANCODE_ESCAPE` | `Con_Printf("screenshot rect: cancelled\n")`, `Crop_Exit()`. |
| Anything else | Return 0 — pass through. |

Returning 1 swallows the event so neither the editor, ImGui, nor the game sees it.

### Overlay (`Crop_PresentOverlay`)

Walks the ARGB destination after the frozen-frame expand. For each pixel `(x, y)`:

- Inside the normalised rect: leave untouched.
- On the rect border (1-pixel edge): `0xFFFFFFFF`.
- Outside: `c = (c >> 1) & 0x7F7F7F` (50% dim, alpha bits dropped — they're unused in the SDL texture format).

### Save path

Reuses the palette → RGB conversion already in `VID_SaveScreenshotPNG` but operates on a subrect of the **frozen** 8-bit buffer:

```c
int rw = rect_x1 - rect_x0 + 1;
int rh = rect_y1 - rect_y0 + 1;
unsigned char *rgb = malloc(rw * rh * 3);
for (int y = 0; y < rh; y++) {
    const byte *src = frozen + (rect_y0 + y) * frozen_w + rect_x0;
    unsigned char *dst = rgb + y * rw * 3;
    for (int x = 0; x < rw; x++) {
        unsigned c = d_8to24table[src[x]];
        dst[x*3 + 0] = (unsigned char)(c >> 16);
        dst[x*3 + 1] = (unsigned char)(c >>  8);
        dst[x*3 + 2] = (unsigned char)(c >>  0);
    }
}
stbi_write_png(out_path, rw, rh, 3, rgb, rw * 3);
free(rgb);
```

(If a saved frame uses non-default palettes via `vid_lut`, swap `d_8to24table` for the per-pixel `vid_lut[frozen_pal[i]][src[i]]` lookup to match what the user saw.)

## Pause behaviour

Visual freeze only — simulation keeps ticking. `cl.paused` is **not** touched. The frozen appearance comes entirely from `VID_Update` substituting the frozen buffer for `vid.buffer` while active. This matches what the user sees pixel-for-pixel in the saved crop.

## File naming

Identical 00..99 scan to the existing fullscreen path, both modes share the counter. The decision to also share the directory (`com_gamedir`, i.e. `id1/`) was explicit — no `screenshots/` subdir, no `crop_NN.png` prefix.

## Build

`crop_screenshot.c` joins `platform_files` in `build.zig` (Platform list, around line 80). No new dependencies; `stb_image_write.h` is already vendored and `<<>>` SDL3 headers are on the include path.

## YAGNI

- No shift-to-square or aspect-ratio constraints.
- No configurable border colour or dim amount.
- No built-in keybind — user can `bind X "screenshot rect"` themselves.
- No auto-cleanup of the unused `gl_screen.c` PCX block (it's in the GL build path, not compiled).
- No saving of the dimmed overlay or the border. Saved image is the clean cropped frozen frame.
- No undo / re-draw within a single session — release commits; to recompose, run the command again.

## Verification

Manual:

1. `zig build run -- +map e1m1`
2. Console: `screenshot` → confirm `id1/quakeNN.png` written and visually correct.
3. Console: `screenshot rect` → confirm: cursor appears, world freezes, drag draws border + dims outside, release writes a smaller `quakeNN.png` matching the selected region exactly, Esc cancels without writing.
4. Repeat with a non-default palette active (e.g. inside a Doom-themed map area) to confirm colour fidelity.

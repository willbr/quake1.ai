# Rect-selection screenshot mode

Status: approved 2026-05-24

## Motivation

The `screenshot` console command already writes PNG (see `screen.c:722` and `vid_sdl.c:570`; the PCX path is gone despite some lingering variable names in the uncompiled `gl_screen.c`). What's missing is a way to grab a *subrect* of the framebuffer so a cropped image can be passed straight to Claude Code without an external editor step.

Two modes:

1. **Fullscreen** — `screenshot` → next free `screenshots/shot_NNNN.png`. Now also copies the PNG to the system clipboard.
2. **Rect** — new. `screenshot rect` enters a modal selection state: the current frame freezes visually, the OS cursor appears, mouse drag defines the crop, release saves the cropped PNG and copies it to the clipboard, Esc cancels.

Both modes share the same `screenshots/shot_NNNN.png` filename pattern (4-digit, zero-padded, 0000..9999), matching what the MCP `screenshot` tool already writes. The folder is relative to cwd — the repo root when running via `zig build run`, the same location MCP uses. The fullscreen path's previous home (`com_gamedir/quakeNN.png`) is dropped; both console and MCP screenshots now coexist in one place and share the counter scan, so there is one continuous numbered sequence regardless of which tool produced each file.

Both modes also push the encoded PNG bytes onto the system clipboard with MIME type `image/png` so Cmd/Ctrl-V into Slack, Discord, Claude Code, etc. pastes the image directly. Clipboard copy is gated on a new cvar `scr_screenshot_clipboard` (default 1).

## Architecture

A new platform-side module owns the modal state and overlay rendering. The engine-side `SCR_ScreenShot_f` only learns about the new subcommand; it doesn't carry rect state itself.

### New module: `sdlquake/platform/crop_screenshot.{c,h}`

State (file-static):

- `byte *frozen` — palette-indexed snapshot of `vid.buffer` taken on `Crop_Enter`.
- `byte *frozen_pal` — matching `vid_palette_id` slots so colour expansion is identical to the live path.
- `int frozen_w, frozen_h` — captured framebuffer dimensions.
- `int rect_x0, rect_y0, rect_x1, rect_y1` — current drag endpoints in framebuffer (logical) coords.
- `qboolean dragging, active`
- `qboolean prev_paused` — `cl.paused` value at entry, restored on exit
- `char out_path[MAX_OSPATH]`

Public API:

```c
void Crop_Enter(const char *out_path);
int  Crop_Active(void);
int  Crop_HandleEvent(const SDL_Event *ev);     // returns 1 if consumed
void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h);
void Crop_Exit(void);                            // frees frozen buffers, clears active
```

`Crop_Enter` allocates both frozen buffers, `memcpy`s from `vid.buffer` and `vid_palette_id`, saves `cl.paused` into a state field and sets `cl.paused = true`, then sets `active = true`. `Crop_Exit` restores `cl.paused`, frees the buffers, and clears `active`.

### Touch points

| File | Change |
|---|---|
| `sdlquake/engine_src/screen.c` (`SCR_ScreenShot_f`) | Ensure `screenshots/` exists, find next free `screenshots/shot_NNNN.png` (0000..9999 scan). If `Cmd_Argv(1)` equals `"rect"`, call `Crop_Enter(checkname)` (declared `extern`); otherwise call the fullscreen save. The directory-create + index-scan helpers live in a shared spot — see "Filename helper" below. |
| `sdlquake/platform/vid_sdl.c` (`VID_Update`) | If `Crop_Active()`, expand the **frozen** buffer into the SDL texture instead of `vid.buffer`, then call `Crop_PresentOverlay()` to dim outside the rect and stamp the border. Imgui/present remain unchanged. |
| `sdlquake/platform/in_sdl.c` (`IN_ProcessEvents`, `IN_WantRelativeMouse`) | Before existing dispatch: `if (Crop_Active() && Crop_HandleEvent(&ev)) continue;`. Also extend `IN_WantRelativeMouse()` to return `false` when crop is active, so the OS cursor reappears. |

### Coordinate mapping

Mouse events arrive in window pixels. The framebuffer is logical 320×200 (or whatever `vid.width/height` is) presented via `SDL_SetRenderLogicalPresentation(INTEGER_SCALE)`. Use `SDL_RenderCoordinatesFromWindow(renderer, wx, wy, &rx, &ry)` to convert, then clamp to `[0, vid.width-1] × [0, vid.height-1]`.

### Event handling inside `Crop_HandleEvent`

| Event | Action |
|---|---|
| `SDL_EVENT_MOUSE_BUTTON_DOWN` (left) | `dragging = true`, `rect_x0 = rect_x1 = clamped_x`, same for y. |
| `SDL_EVENT_MOUSE_MOTION` (while dragging) | Update `rect_x1, rect_y1`. |
| `SDL_EVENT_MOUSE_BUTTON_UP` (left) | Commit: normalise (min/max), enforce min size 1×1, encode + write PNG, push to clipboard (gated on cvar), print result (see clipboard section), `Crop_Exit()`. |
| `SDL_EVENT_KEY_DOWN` with `SDL_SCANCODE_ESCAPE` | `Con_Printf("screenshot rect: cancelled\n")`, `Crop_Exit()`. |
| Anything else | Return 0 — pass through. |

Returning 1 swallows the event so neither the editor, ImGui, nor the game sees it.

### Overlay (`Crop_PresentOverlay`)

Walks the ARGB destination after the frozen-frame expand. For each pixel `(x, y)`:

- Inside the normalised rect: leave untouched.
- On the rect border (1-pixel edge): `0xFFFFFFFF`.
- Outside: `c = (c >> 1) & 0x7F7F7F` (50% dim, alpha bits dropped — they're unused in the SDL texture format).

### Save path

Reuses the palette → RGB conversion already in `VID_SaveScreenshotPNG` but operates on a subrect of the **frozen** 8-bit buffer. The encoder runs once into a memory buffer so the same PNG bytes feed both the file and the clipboard:

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
/* Encode once to memory via stbi_write_png_to_func; the callback appends to
   a growable buffer. Then write the buffer to disk and hand the same bytes
   to the clipboard. */
png_buffer_t buf = {0};
stbi_write_png_to_func(png_buffer_append, &buf, rw, rh, 3, rgb, rw * 3);
free(rgb);

write_file(out_path, buf.data, buf.size);
if (scr_screenshot_clipboard.value)
    Clipboard_SetPNG(buf.data, buf.size);   /* takes ownership or copies */
```

(If a saved frame uses non-default palettes via `vid_lut`, swap `d_8to24table` for the per-pixel `vid_lut[frozen_pal[i]][src[i]]` lookup to match what the user saw.)

The fullscreen path in `VID_SaveScreenshotPNG` mirrors the same shape: encode to memory once, write file, optionally copy to clipboard.

### Filename helper

A single helper, exposed from the platform layer, generates the next free path so `screen.c` and `mcp_server.c` produce the same continuous sequence:

```c
/* Ensures screenshots/ exists, fills `out` (size `outsz`) with the next free
   screenshots/shot_NNNN.png. Returns 1 on success, 0 if all 10000 slots are
   taken or the path won't fit. */
int Screenshot_NextPath(char *out, size_t outsz);
```

`mcp_server.c` switches its `mcp_next_screenshot_index` + `mkdir` call to use this helper, so the existing MCP behaviour stays identical while console screenshots fall into the same numbered sequence. No more divergence between the two.

### Clipboard (`Clipboard_SetPNG`)

New thin wrapper in `sdlquake/platform/clipboard.{c,h}`:

```c
void Clipboard_SetPNG(const void *png_bytes, size_t size);
```

Implementation calls `SDL_SetClipboardData(callback, cleanup, userdata, mime_types, 1)` with `mime_types = {"image/png"}`. The callback returns the stored buffer pointer + size; cleanup `free`s it. Userdata is a heap-allocated struct holding a `memcpy`'d copy of the PNG bytes — we don't trust the caller's buffer to outlive the clipboard's lifetime, and SDL3 may re-invoke the callback later when another app actually requests the data.

The `scr_screenshot_clipboard` cvar (registered in `screen.c` next to existing screenshot bits, default `1`) gates whether `Clipboard_SetPNG` gets called at all — `Con_Printf` should still report the file write either way.

On success the console message becomes `"Wrote %s (also copied to clipboard)\n"`; on cvar-off it stays `"Wrote %s\n"`; on `SDL_SetClipboardData` failure we print the file message plus a `"clipboard copy failed: %s\n"` warning but don't fail the screenshot.

## Pause behaviour

Full pause for rect mode: both display and simulation freeze while composing.

- **Display**: `VID_Update` substitutes the frozen buffer for `vid.buffer` while `Crop_Active()`, so the user sees the snapshot frame and the saved crop matches it pixel-for-pixel.
- **Simulation**: `Crop_Enter` records the prior value of `cl.paused`, sets it to `true`, and `Crop_Exit` restores it. Monsters stop moving, projectiles freeze, lava can't kill you mid-selection. (Caveat: `cl.paused` only stops time when the local client is also the server — single-player or listen-server. In a remote-server multiplayer game the world keeps ticking, but this is acceptable — single-player + dev workflows are the target.)

Fullscreen `screenshot` does not touch `cl.paused`; it's instantaneous so there's nothing to compose.

## File naming

Both console modes and the MCP tool share one helper (`Screenshot_NextPath`) that writes `screenshots/shot_NNNN.png` (0000..9999) relative to cwd. One continuous numbered sequence per working directory; no separate counters, no per-tool prefix.

## Build

`crop_screenshot.c` and `clipboard.c` join `platform_files` in `build.zig` (Platform list, around line 80). No new dependencies; `stb_image_write.h` is already vendored and SDL3 headers are on the include path.

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
2. Console: `screenshot` → confirm `screenshots/shot_NNNN.png` written and visually correct; Cmd/Ctrl-V into a chat client pastes the same image.
3. Console: `screenshot rect` → confirm: cursor appears, world freezes (both display and simulation — provoke a nailgrunt nearby and verify its nail doesn't land while you're dragging), drag draws border + dims outside, release writes a smaller `shot_NNNN.png` matching the selected region exactly, and that image is on the clipboard. Esc cancels without writing or touching the clipboard. After exit (commit or cancel), simulation resumes — verify by hearing the nail finally hit.
4. Take a console screenshot, then an MCP screenshot, then another console screenshot → confirm the sequence numbers continue without collision (`shot_0007.png`, `shot_0008.png`, `shot_0009.png`).
5. Repeat with a non-default palette active (e.g. inside a Doom-themed map area) to confirm colour fidelity.
6. `scr_screenshot_clipboard 0` → both modes still write the file but no longer touch the clipboard.

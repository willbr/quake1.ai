# Rect-selection Screenshot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `screenshot rect` console command that lets the user freeze the frame, mouse-drag a crop, and save the cropped PNG to `screenshots/shot_NNNN.png` while also copying it to the system clipboard. Existing `screenshot` keeps fullscreen behaviour but moves to the same folder and also copies to clipboard.

**Architecture:** A small platform-side module (`crop_screenshot.{c,h}`) owns the modal state, the frozen 8-bit framebuffer copy, and the overlay rendering. `screen.c` only learns the new subcommand. `vid_sdl.c` substitutes the frozen buffer + dim/border overlay while crop is active. `in_sdl.c` routes mouse/Esc into the crop module and shows the OS cursor. A `Screenshot_NextPath` helper unifies the filename scan with the MCP `screenshot` tool. A `Clipboard_SetPNG` wrapper uses `SDL_SetClipboardData` with MIME type `image/png` so Cmd/Ctrl-V pastes the image into other apps.

**Tech Stack:** C (gnu89 for engine_src, modern C for platform), SDL3, stb_image_write (vendored), Zig build system.

**Spec:** `docs/superpowers/specs/2026-05-24-rect-screenshot-design.md`

**Verification model:** This codebase has no test suite. Each task ends with a `zig build` (compile check) and the final task ends with a manual play-through. Commit after each task.

---

## File map

| Path | Role | New / Modified |
|---|---|---|
| `sdlquake/platform/screenshot_path.h` | `Screenshot_NextPath` prototype | New |
| `sdlquake/platform/screenshot_path.c` | `mkdir` + 0000..9999 scan | New |
| `sdlquake/platform/clipboard.h` | `Clipboard_SetPNG` prototype | New |
| `sdlquake/platform/clipboard.c` | SDL3 clipboard callback wrapper | New |
| `sdlquake/platform/crop_screenshot.h` | `Crop_*` API | New |
| `sdlquake/platform/crop_screenshot.c` | Modal state, overlay, save | New |
| `sdlquake/platform/vid_sdl.c` | `VID_SaveScreenshotPNG` refactor; `VID_Update` overlay hook | Modify |
| `sdlquake/platform/in_sdl.c` | Event routing; relative-mouse gating | Modify |
| `sdlquake/engine_src/screen.c` | New cvar; `SCR_ScreenShot_f` uses `Screenshot_NextPath`, dispatches `rect` | Modify |
| `sdlquake/mcp/mcp_server.c` | Adopt `Screenshot_NextPath` | Modify |
| `build.zig` | Add three new `.c` files to `platform_files` | Modify |

---

## Task 1: `Screenshot_NextPath` helper + MCP adoption

Smallest isolated change. Confirms the new file builds before we layer anything on top.

**Files:**
- Create: `sdlquake/platform/screenshot_path.h`
- Create: `sdlquake/platform/screenshot_path.c`
- Modify: `sdlquake/mcp/mcp_server.c` (drop `mcp_next_screenshot_index`, drop `mcp_mkdir("screenshots")` in `tool_screenshot`; call helper instead)
- Modify: `build.zig` (add `sdlquake/platform/screenshot_path.c` to `platform_files` around line 80)

- [ ] **Step 1: Create the header**

Write `sdlquake/platform/screenshot_path.h`:

```c
#ifndef SDLQUAKE_SCREENSHOT_PATH_H
#define SDLQUAKE_SCREENSHOT_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ensures screenshots/ exists, writes the next free
   screenshots/shot_NNNN.png into `out` (size `outsz`).
   Returns 1 on success, 0 if all 10000 slots are taken or
   the path won't fit. */
int Screenshot_NextPath(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Create the implementation**

Write `sdlquake/platform/screenshot_path.c`:

```c
#include "screenshot_path.h"

#include <stdio.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <direct.h>
#  define ss_mkdir(p) _mkdir(p)
#else
#  include <sys/types.h>
#  define ss_mkdir(p) mkdir((p), 0755)
#endif

int Screenshot_NextPath(char *out, size_t outsz)
{
    if (!out || outsz < sizeof("screenshots/shot_9999.png"))
        return 0;

    /* mkdir best-effort; ignore EEXIST and any other error so a
       read-only cwd surfaces later as a stbi_write_png failure. */
    (void)ss_mkdir("screenshots");

    for (int i = 1; i <= 9999; i++) {
        snprintf(out, outsz, "screenshots/shot_%04d.png", i);
        struct stat st;
        if (stat(out, &st) != 0)
            return 1;
    }
    return 0;
}
```

- [ ] **Step 3: Wire it into the build**

Edit `build.zig`. Find the `platform_files` array (around line 80, starts with `"sdlquake/platform/sys_sdl.c"`) and add `"sdlquake/platform/screenshot_path.c",` after `"sdlquake/platform/net_sdl.c",`.

- [ ] **Step 4: Have MCP use it**

In `sdlquake/mcp/mcp_server.c`:

1. Add `#include "../platform/screenshot_path.h"` near the other includes at the top of the file.
2. Delete the entire `mcp_next_screenshot_index` function (lines 829–840, roughly).
3. Inside `tool_screenshot` (around line 842), replace the block:

```c
    char path[512] = {0};
    mcp_mkdir("screenshots");   /* ignore errors -- may already exist */

    if (!input[0])
    {
        int n = mcp_next_screenshot_index();
        if (n == 0) { mcp_error(id_json, -32603, "screenshot dir full"); return; }
        snprintf(path, sizeof(path), "screenshots/shot_%04d.png", n);
    }
```

with:

```c
    char path[512] = {0};

    if (!input[0])
    {
        if (!Screenshot_NextPath(path, sizeof(path))) {
            mcp_error(id_json, -32603, "screenshot dir full");
            return;
        }
    }
```

(Keep the `else` branch — the caller-supplied path path is untouched. The `mcp_mkdir` call in the helper handles directory creation for both branches now, but the existing `else` branch never relied on it being called outside, so this is safe.)

Wait — the original `else` branch writes to `screenshots/<base>`, and it relied on the now-deleted `mcp_mkdir` call at the top of the function to ensure the directory existed. Add `(void)mcp_mkdir("screenshots");` at the top of the `else` branch to preserve that behaviour:

```c
    else
    {
        (void)mcp_mkdir("screenshots");
        /* Constrain every caller-supplied path to live inside screenshots/... */
        ... existing validation + snprintf ...
    }
```

- [ ] **Step 5: Build and verify**

Run: `zig build`
Expected: clean build, no warnings about unused statics.

- [ ] **Step 6: Smoke-test the MCP path still works**

Run: `zig build run -- +map e1m1` in one terminal. In another:

```sh
python3 scripts/mcp_call.py screenshot
```

Expected: prints absolute path to `screenshots/shot_NNNN.png`; file exists; opens as a valid PNG. Quit the game.

- [ ] **Step 7: Commit**

```sh
git add sdlquake/platform/screenshot_path.h sdlquake/platform/screenshot_path.c \
        sdlquake/mcp/mcp_server.c build.zig
git commit -m "refactor(screenshot): extract shared next-path helper for screenshots/shot_NNNN.png"
```

---

## Task 2: `Clipboard_SetPNG` wrapper

**Files:**
- Create: `sdlquake/platform/clipboard.h`
- Create: `sdlquake/platform/clipboard.c`
- Modify: `build.zig`

- [ ] **Step 1: Create the header**

Write `sdlquake/platform/clipboard.h`:

```c
#ifndef SDLQUAKE_CLIPBOARD_H
#define SDLQUAKE_CLIPBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `size` bytes of PNG-encoded image data onto the system
   clipboard with MIME type "image/png". The bytes are duplicated
   internally — the caller may free `png_bytes` immediately.
   Returns 1 on success, 0 if SDL rejected the request. */
int Clipboard_SetPNG(const void *png_bytes, size_t size);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Create the implementation**

Write `sdlquake/platform/clipboard.c`:

```c
#include "clipboard.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void   *bytes;
    size_t  size;
} clip_payload_t;

static const void *clip_provide(void *userdata, const char *mime_type, size_t *size)
{
    clip_payload_t *p = (clip_payload_t *)userdata;
    if (!p || !mime_type) { if (size) *size = 0; return NULL; }
    if (strcmp(mime_type, "image/png") != 0) { if (size) *size = 0; return NULL; }
    if (size) *size = p->size;
    return p->bytes;
}

static void clip_cleanup(void *userdata)
{
    clip_payload_t *p = (clip_payload_t *)userdata;
    if (!p) return;
    free(p->bytes);
    free(p);
}

int Clipboard_SetPNG(const void *png_bytes, size_t size)
{
    if (!png_bytes || size == 0) return 0;

    clip_payload_t *p = (clip_payload_t *)malloc(sizeof(*p));
    if (!p) return 0;
    p->bytes = malloc(size);
    if (!p->bytes) { free(p); return 0; }
    memcpy(p->bytes, png_bytes, size);
    p->size = size;

    const char *mimes[1] = { "image/png" };
    if (!SDL_SetClipboardData(clip_provide, clip_cleanup, p, mimes, 1)) {
        /* On failure SDL does not call the cleanup callback. */
        free(p->bytes);
        free(p);
        return 0;
    }
    return 1;
}
```

- [ ] **Step 3: Wire it into the build**

In `build.zig`, add `"sdlquake/platform/clipboard.c",` to `platform_files` next to `screenshot_path.c`.

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean build. (No callers yet — `Clipboard_SetPNG` is dead code until Task 4.)

- [ ] **Step 5: Commit**

```sh
git add sdlquake/platform/clipboard.h sdlquake/platform/clipboard.c build.zig
git commit -m "feat(clipboard): add Clipboard_SetPNG wrapper for SDL3 image/png clipboard data"
```

---

## Task 3: Refactor `VID_SaveScreenshotPNG` to encode-once

Pull the file-write out of stb's `stbi_write_png` so we can also push the same bytes to the clipboard. Behaviour remains identical for now (no clipboard call yet — that comes in Task 4 once the cvar exists).

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c:570–601`

- [ ] **Step 1: Add the includes**

Near the top of `sdlquake/platform/vid_sdl.c`, after the existing `#include "stb_image_write.h"` line (around line 13), add a forward declaration to avoid pulling in the clipboard header from a file that's already very dense:

```c
/* Defined in clipboard.c — copy a PNG to the system clipboard.
   We forward-declare rather than #include to keep vid_sdl.c's
   header surface small. */
extern int  Clipboard_SetPNG(const void *bytes, size_t size);
```

(`size_t` is already in scope via the headers vid_sdl.c includes.)

- [ ] **Step 2: Rewrite `VID_SaveScreenshotPNG` to encode-into-memory**

Replace the body of `VID_SaveScreenshotPNG` (around lines 570–601) with:

```c
typedef struct {
    unsigned char *data;
    size_t         size;
    size_t         cap;
} png_buf_t;

static void png_buf_append(void *ctx, void *data, int len)
{
    png_buf_t *b = (png_buf_t *)ctx;
    if (len <= 0) return;
    size_t need = b->size + (size_t)len;
    if (need > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        unsigned char *p = (unsigned char *)realloc(b->data, new_cap);
        if (!p) return;   /* writing stops; final size != need will be caught */
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->size, data, (size_t)len);
    b->size = need;
}

int VID_SaveScreenshotPNG(const char *path)
{
    if (!path || !path[0]) return 0;
    if (!vid.buffer) return 0;

    int w = (int)vid.width;
    int h = (int)vid.height;
    int rowbytes = (int)vid.rowbytes;
    if (w <= 0 || h <= 0 || rowbytes < w) return 0;

    unsigned char *rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return 0;

    /* See historical comment: d_8to24table is 0xAARRGGBB. */
    for (int y = 0; y < h; y++) {
        const byte    *src = vid.buffer + y * rowbytes;
        unsigned char *dst = rgb        + y * w * 3;
        for (int x = 0; x < w; x++) {
            unsigned c = d_8to24table[src[x]];
            dst[x*3 + 0] = (unsigned char)(c >> 16);  /* R */
            dst[x*3 + 1] = (unsigned char)(c >>  8);  /* G */
            dst[x*3 + 2] = (unsigned char)(c >>  0);  /* B */
        }
    }

    png_buf_t buf = {0};
    int ok = stbi_write_png_to_func(png_buf_append, &buf, w, h, 3, rgb, w * 3);
    free(rgb);
    if (!ok || !buf.data) { free(buf.data); return 0; }

    FILE *fp = fopen(path, "wb");
    if (!fp) { free(buf.data); return 0; }
    size_t wrote = fwrite(buf.data, 1, buf.size, fp);
    fclose(fp);
    if (wrote != buf.size) { free(buf.data); return 0; }

    /* Clipboard copy is gated by the caller via scr_screenshot_clipboard;
       VID_SaveScreenshotPNG itself stays single-purpose (write-to-disk). */
    free(buf.data);
    return 1;
}
```

(Note: this function no longer does the clipboard push itself. The cvar gate lives in the caller (`SCR_ScreenShot_f` / `Crop_HandleEvent`), which keeps `VID_SaveScreenshotPNG`'s contract narrow — write file, return ok. We'll need a sibling function that *both* writes and clips; defer that to Task 4 once we have a place for it.)

Wait — re-reading, doing this in two layers would duplicate the palette-expand + encode work. Simpler: have `VID_SaveScreenshotPNG` *also* hand the buffer to the clipboard, and gate the clipboard call here via the cvar lookup. The cvar will exist after Task 4. For Task 3, just refactor to encode-once and write file. Task 4 adds the cvar and the clipboard call inside `VID_SaveScreenshotPNG`.

- [ ] **Step 3: Build and smoke-test**

Run: `zig build run -- +map e1m1`. In console: `screenshot`. Expected: `screenshots/shot_0001.png` (or next free) exists, opens correctly, looks like the live frame.

(Note: the old `id1/quakeXX.png` path is *not* yet retired — `SCR_ScreenShot_f` still writes there. That gets fixed in Task 4.)

- [ ] **Step 4: Commit**

```sh
git add sdlquake/platform/vid_sdl.c
git commit -m "refactor(screenshot): encode PNG into memory buffer once, then write file"
```

---

## Task 4: Update `SCR_ScreenShot_f` to use shared path + cvar + clipboard

Adds the `scr_screenshot_clipboard` cvar, retires the `id1/quakeNN.png` filename loop in favour of `Screenshot_NextPath`, and routes through a new `VID_SaveScreenshotPNG_AndClip` helper so fullscreen screenshots also land on the clipboard. Rect support is *not* added in this task — it slots in at Task 7.

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c` (new helper)
- Modify: `sdlquake/engine_src/screen.c` (`SCR_ScreenShot_f`, cvar registration)

- [ ] **Step 1: Add `VID_SaveScreenshotPNG_AndClip` to `vid_sdl.c`**

Inside `VID_SaveScreenshotPNG` (Task 3 version), pull the palette-expand + encode out into a static helper so both the disk-only and disk-plus-clipboard paths can share it. Replace the Task 3 body with:

```c
/* Encode the current vid.buffer (palette-indexed) as PNG bytes into
   *out_bytes / *out_size. Caller frees with free(). Returns 1 on ok. */
static int vid_encode_screenshot_png(unsigned char **out_bytes, size_t *out_size)
{
    *out_bytes = NULL;
    *out_size  = 0;
    if (!vid.buffer) return 0;

    int w = (int)vid.width;
    int h = (int)vid.height;
    int rowbytes = (int)vid.rowbytes;
    if (w <= 0 || h <= 0 || rowbytes < w) return 0;

    unsigned char *rgb = (unsigned char *)malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return 0;
    for (int y = 0; y < h; y++) {
        const byte    *src = vid.buffer + y * rowbytes;
        unsigned char *dst = rgb        + y * w * 3;
        for (int x = 0; x < w; x++) {
            unsigned c = d_8to24table[src[x]];
            dst[x*3 + 0] = (unsigned char)(c >> 16);
            dst[x*3 + 1] = (unsigned char)(c >>  8);
            dst[x*3 + 2] = (unsigned char)(c >>  0);
        }
    }

    png_buf_t buf = {0};
    int ok = stbi_write_png_to_func(png_buf_append, &buf, w, h, 3, rgb, w * 3);
    free(rgb);
    if (!ok || !buf.data) { free(buf.data); return 0; }
    *out_bytes = buf.data;
    *out_size  = buf.size;
    return 1;
}

static int vid_write_file(const char *path, const unsigned char *bytes, size_t size)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    size_t wrote = fwrite(bytes, 1, size, fp);
    fclose(fp);
    return wrote == size;
}

int VID_SaveScreenshotPNG(const char *path)
{
    if (!path || !path[0]) return 0;
    unsigned char *bytes = NULL;
    size_t size = 0;
    if (!vid_encode_screenshot_png(&bytes, &size)) return 0;
    int ok = vid_write_file(path, bytes, size);
    free(bytes);
    return ok;
}

/* Write the current framebuffer to `path` and, if `also_clipboard`,
   also push the same PNG bytes onto the system clipboard. Returns
   bitfield: bit 0 = file write ok, bit 1 = clipboard ok (only set
   when also_clipboard was nonzero). */
int VID_SaveScreenshotPNG_AndClip(const char *path, int also_clipboard)
{
    if (!path || !path[0]) return 0;
    unsigned char *bytes = NULL;
    size_t size = 0;
    if (!vid_encode_screenshot_png(&bytes, &size)) return 0;

    int result = 0;
    if (vid_write_file(path, bytes, size)) result |= 1;
    if (also_clipboard && Clipboard_SetPNG(bytes, size)) result |= 2;
    free(bytes);
    return result;
}
```

- [ ] **Step 2: Register the cvar and rewrite `SCR_ScreenShot_f`**

Edit `sdlquake/engine_src/screen.c`. After line 42 (the existing `scr_printspeed` cvar declaration), add:

```c
cvar_t      scr_screenshot_clipboard = {"scr_screenshot_clipboard", "1", true};
```

In `SCR_Init` (around line 449), after `Cvar_RegisterVariable(&scr_printspeed);` (line 459), add:

```c
    Cvar_RegisterVariable (&scr_screenshot_clipboard);
```

Replace `SCR_ScreenShot_f` in its entirety (currently lines 722–746) with:

```c
void SCR_ScreenShot_f (void)
{
    extern int  VID_SaveScreenshotPNG_AndClip(const char *path, int also_clipboard);
    extern void Crop_Enter(const char *out_path);
    extern int  Screenshot_NextPath(char *out, size_t outsz);

    char path[256];
    if (!Screenshot_NextPath(path, sizeof(path))) {
        Con_Printf("SCR_ScreenShot_f: screenshots/ is full (10000 slots)\n");
        return;
    }

    /* `screenshot rect` enters modal selection; commit/cancel happens
       later from the platform layer. The path we computed is reserved
       for that crop. */
    if (Cmd_Argc() > 1 && !strcmp(Cmd_Argv(1), "rect")) {
        Crop_Enter(path);
        return;
    }

    D_EnableBackBufferAccess();
    int clip = (int)scr_screenshot_clipboard.value;
    int rc = VID_SaveScreenshotPNG_AndClip(path, clip);
    D_DisableBackBufferAccess();

    if (!(rc & 1)) {
        Con_Printf("SCR_ScreenShot_f: write failed\n");
        return;
    }
    if (clip && (rc & 2))
        Con_Printf("Wrote %s (also copied to clipboard)\n", path);
    else if (clip)
        Con_Printf("Wrote %s (clipboard copy failed)\n", path);
    else
        Con_Printf("Wrote %s\n", path);
}
```

Add the include for `Screenshot_NextPath`'s prototype source — actually the `extern` declaration in the function body is sufficient, and matches the existing style in this file (see the existing `extern int VID_SaveScreenshotPNG` in the original code).

`<string.h>` for `strcmp` is already transitively included via `quakedef.h`.

`Crop_Enter` won't link yet — that's added in Task 5. Don't try to call `screenshot rect` until then.

- [ ] **Step 3: Build**

Build will fail at link with `undefined symbol: Crop_Enter`. To unblock Task 4 alone, add a temporary stub at the bottom of `screen.c` (above any extant trailing brace):

```c
/* Temporary stub — real implementation lands in Task 5. Delete the
   `extern` declaration of Crop_Enter in SCR_ScreenShot_f and this stub
   together when Task 5 ships the real symbol. */
void Crop_Enter(const char *out_path) { (void)out_path; }
```

- [ ] **Step 4: Build and smoke-test fullscreen**

```sh
zig build run -- +map e1m1
```

In console:
- `screenshot` → expected: `Wrote screenshots/shot_NNNN.png (also copied to clipboard)`; file exists, opens, paste into a chat app pastes the image.
- `scr_screenshot_clipboard 0; screenshot` → expected: `Wrote screenshots/shot_NNNN.png` (no clipboard line); paste yields whatever was last on the clipboard.
- `screenshot rect` → expected: nothing visible happens (stub). That's fine.

- [ ] **Step 5: Commit**

```sh
git add sdlquake/platform/vid_sdl.c sdlquake/engine_src/screen.c
git commit -m "feat(screenshot): share screenshots/ path with MCP; copy PNG to clipboard

Adds scr_screenshot_clipboard cvar (default 1). Fullscreen screenshot
now lands in screenshots/shot_NNNN.png alongside MCP screenshots and
pushes the PNG to the system clipboard via SDL3."
```

---

## Task 5: `crop_screenshot.{c,h}` skeleton — Enter / Active / Exit

Just the state-management API and `cl.paused` toggle. No overlay, no input, no save. The skeleton lets us delete the stub from Task 4 and lets later tasks layer in the real behaviour.

**Files:**
- Create: `sdlquake/platform/crop_screenshot.h`
- Create: `sdlquake/platform/crop_screenshot.c`
- Modify: `sdlquake/engine_src/screen.c` (delete the `Crop_Enter` stub)
- Modify: `build.zig`

- [ ] **Step 1: Create the header**

Write `sdlquake/platform/crop_screenshot.h`:

```c
#ifndef SDLQUAKE_CROP_SCREENSHOT_H
#define SDLQUAKE_CROP_SCREENSHOT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enter modal rect-selection. Snapshots the current framebuffer,
   pauses simulation (cl.paused = true), and shows the OS cursor.
   `out_path` is the destination filename the eventual commit will
   write to. Pass an empty string to suppress entry (defensive). */
void Crop_Enter(const char *out_path);

/* Restore cl.paused, free internal buffers, hide-cursor handled
   by the input layer via Crop_Active() going false. Safe to call
   when not active (no-op). */
void Crop_Exit(void);

/* 1 while modal selection is active, 0 otherwise. */
int  Crop_Active(void);

/* Returns 1 if the event was consumed (don't dispatch further). */
int  Crop_HandleEvent(const SDL_Event *ev);

/* Composite dim + border overlay onto the ARGB present buffer.
   Caller has just expanded the frozen 8-bit buffer into `argb`. */
void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h);

/* Returns the cached frozen 8-bit framebuffer + matching palette-id
   plane so VID_Update can expand them instead of vid.buffer. Returns
   NULL if not active. */
const unsigned char *Crop_FrozenBuffer (int *w, int *h);
const unsigned char *Crop_FrozenPalette(void);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 2: Create the skeleton implementation**

Write `sdlquake/platform/crop_screenshot.c`:

```c
#include "crop_screenshot.h"

#include <stdlib.h>
#include <string.h>

#include "../engine_src/quakedef.h"   /* cl.paused, vid, byte, d_8to24table */
#include "vid_palette.h"              /* extern byte vid_palette_id[] */

typedef struct {
    unsigned char *frozen;       /* w*h bytes */
    unsigned char *frozen_pal;   /* w*h bytes */
    int            w, h;
    int            x0, y0, x1, y1;
    int            dragging;
    int            active;
    int            prev_paused;
    char           out_path[256];
} crop_state_t;

static crop_state_t g;

void Crop_Enter(const char *out_path)
{
    if (g.active) return;       /* already in a session — ignore */
    if (!out_path || !out_path[0]) return;
    if (!vid.buffer) return;

    int w = (int)vid.width;
    int h = (int)vid.height;
    int rowbytes = (int)vid.rowbytes;
    if (w <= 0 || h <= 0 || rowbytes < w) return;

    g.frozen     = (unsigned char *)malloc((size_t)w * (size_t)h);
    g.frozen_pal = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!g.frozen || !g.frozen_pal) {
        free(g.frozen);     g.frozen     = NULL;
        free(g.frozen_pal); g.frozen_pal = NULL;
        return;
    }

    /* Match the convention of fullscreen SCR_ScreenShot_f: wrap the
       read in D_Enable/DisableBackBufferAccess so the framebuffer is
       guaranteed readable. */
    D_EnableBackBufferAccess();
    for (int y = 0; y < h; y++) {
        memcpy(g.frozen + y * w, vid.buffer + y * rowbytes, (size_t)w);
    }
    /* vid_palette_id is tight (w bytes per row) already. */
    memcpy(g.frozen_pal, vid_palette_id, (size_t)w * (size_t)h);
    D_DisableBackBufferAccess();

    g.w = w; g.h = h;
    g.x0 = g.y0 = 0;
    g.x1 = w - 1; g.y1 = h - 1;
    g.dragging = 0;
    g.active   = 1;
    g.prev_paused = cl.paused;
    cl.paused = true;
    strncpy(g.out_path, out_path, sizeof(g.out_path) - 1);
    g.out_path[sizeof(g.out_path) - 1] = '\0';
}

void Crop_Exit(void)
{
    if (!g.active) return;
    cl.paused = g.prev_paused;
    free(g.frozen);     g.frozen     = NULL;
    free(g.frozen_pal); g.frozen_pal = NULL;
    g.active = 0;
    g.dragging = 0;
}

int Crop_Active(void) { return g.active; }

int Crop_HandleEvent(const SDL_Event *ev)
{
    (void)ev;
    /* Filled in by Task 7. */
    return 0;
}

void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h)
{
    (void)argb; (void)pitch_bytes; (void)w; (void)h;
    /* Filled in by Task 6. */
}

const unsigned char *Crop_FrozenBuffer(int *w, int *h)
{
    if (!g.active) return NULL;
    if (w) *w = g.w;
    if (h) *h = g.h;
    return g.frozen;
}

const unsigned char *Crop_FrozenPalette(void)
{
    return g.active ? g.frozen_pal : NULL;
}
```

`mcp_server.c` already includes `quakedef.h` from a platform-adjacent location, so this pattern is known-good.

- [ ] **Step 3: Remove the stub from `screen.c`**

Delete the `Crop_Enter` stub added in Task 4 Step 3.

- [ ] **Step 4: Wire the new file into the build**

In `build.zig`, add `"sdlquake/platform/crop_screenshot.c",` to `platform_files`.

- [ ] **Step 5: Build and smoke-test**

```sh
zig build run -- +map e1m1
```

In console: `screenshot rect`. Expected: nothing visible (no overlay, no input). However `cl.paused` should now be true — try `+forward` or shoot: the player should be frozen. To exit, type `cl.paused 0` in the console — but `cl.paused` is a struct field, not a cvar, so that won't work. Instead, `disconnect` and reconnect, or just quit; we'll get proper Esc handling in Task 7.

(If pausing-with-no-way-out feels too rough for incremental testing, you can temporarily wire a `Crop_Exit` console command:

```c
static void Crop_Cancel_f(void) { Crop_Exit(); }
/* ... and Cmd_AddCommand("crop_cancel", Crop_Cancel_f) somewhere. */
```

— but delete it once Task 7 lands.)

- [ ] **Step 6: Commit**

```sh
git add sdlquake/platform/crop_screenshot.h sdlquake/platform/crop_screenshot.c \
        sdlquake/engine_src/screen.c build.zig
git commit -m "feat(screenshot): crop_screenshot skeleton — Enter/Exit, frozen buffer snapshot, cl.paused toggle"
```

---

## Task 6: Frozen-frame display + dim/border overlay

Wires `Crop_FrozenBuffer` / `Crop_PresentOverlay` into `VID_Update` so the player sees the frozen frame plus a rect outline. Without input yet (Task 7) the rect is fixed at full-screen; that's fine for visual verification of the dim/border code.

**Files:**
- Modify: `sdlquake/platform/vid_sdl.c` (`VID_Update`)
- Modify: `sdlquake/platform/crop_screenshot.c` (`Crop_PresentOverlay`)

- [ ] **Step 1: Implement `Crop_PresentOverlay`**

Add a forward-declared accessor for the rect endpoints (we keep them file-static so the present code can read them without exposing the whole state struct). At the bottom of `crop_screenshot.c`, add:

```c
void Crop_GetRect(int *x0, int *y0, int *x1, int *y1)
{
    int lo_x = g.x0, hi_x = g.x1;
    if (lo_x > hi_x) { int t = lo_x; lo_x = hi_x; hi_x = t; }
    int lo_y = g.y0, hi_y = g.y1;
    if (lo_y > hi_y) { int t = lo_y; lo_y = hi_y; hi_y = t; }
    if (x0) *x0 = lo_x;
    if (y0) *y0 = lo_y;
    if (x1) *x1 = hi_x;
    if (y1) *y1 = hi_y;
}
```

Add its prototype to `crop_screenshot.h`:

```c
void Crop_GetRect(int *x0, int *y0, int *x1, int *y1);
```

Replace the stub `Crop_PresentOverlay` with:

```c
void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h)
{
    if (!g.active || !argb) return;
    int x0, y0, x1, y1;
    Crop_GetRect(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;

    int pitch_px = pitch_bytes / 4;
    for (int y = 0; y < h; y++) {
        unsigned *row = argb + y * pitch_px;
        for (int x = 0; x < w; x++) {
            int inside = (x >= x0 && x <= x1 && y >= y0 && y <= y1);
            int on_border = inside && (x == x0 || x == x1 || y == y0 || y == y1);
            if (on_border)      row[x] = 0xFFFFFFFFu;
            else if (!inside)   row[x] = (row[x] >> 1) & 0x7F7F7Fu;
            /* else: inside, leave untouched */
        }
    }
}
```

- [ ] **Step 2: Hook into `VID_Update`**

In `sdlquake/platform/vid_sdl.c`, at the top of `VID_Update` (around line 479), add the include:

```c
#include "crop_screenshot.h"
```

(Place at the top with the other includes.)

Inside `VID_Update`, replace the expand loop body (around lines 491–510) so that when crop is active the frozen buffer + palette plane are used in place of `vid.buffer` + `vid_palette_id`:

```c
    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) >= 0)
    {
        unsigned (*lut)[256] = vid_lut;

        int crop_w = 0, crop_h = 0;
        const unsigned char *crop_src = Crop_FrozenBuffer(&crop_w, &crop_h);
        const unsigned char *crop_pal = Crop_FrozenPalette();
        /* Only swap if dimensions match the live framebuffer — a safety net
           against the (very unlikely) case the resolution changed mid-session. */
        int use_crop = (crop_src && crop_pal &&
                        crop_w == vid_render_w && crop_h == vid_render_h);

        for (int y = 0; y < vid_render_h; y++)
        {
            unsigned       *dst = (unsigned *)((byte *)pixels + y * pitch);
            const byte     *src = use_crop ? (crop_src + y * vid_render_w)
                                           : (vid.buffer + y * vid.rowbytes);
            const byte     *pal = use_crop ? (crop_pal + y * vid_render_w)
                                           : (vid_palette_id + y * vid_render_w);
            for (int x = 0; x < vid_render_w; x++)
                dst[x] = lut[pal[x]][src[x]];
        }

        if (use_crop)
            Crop_PresentOverlay((unsigned *)pixels, pitch, vid_render_w, vid_render_h);

        SDL_UnlockTexture(sdl_texture);
        SDL_RenderClear(sdl_renderer);
        SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
        ImguiLayer_Render();
        SDL_RenderPresent(sdl_renderer);
    }
```

- [ ] **Step 3: Build and visually verify**

```sh
zig build run -- +map e1m1
```

In console: `screenshot rect`. Expected: the visible frame freezes; the **entire screen** has a 1-pixel white border around it (because rect defaults to full framebuffer); pixels inside the rect (i.e. the whole frame) look normal; there are no dimmed regions because the rect covers everything. Walk forward — player doesn't move (cl.paused).

To exit, use whatever temporary unpause you wired up in Task 5; otherwise quit.

- [ ] **Step 4: Commit**

```sh
git add sdlquake/platform/vid_sdl.c sdlquake/platform/crop_screenshot.{c,h}
git commit -m "feat(screenshot): freeze the frame and draw dim+border overlay during rect mode"
```

---

## Task 7: Input routing, mouse-up commit, Esc cancel

Final wiring: mouse drag updates the rect, mouse-up writes the cropped PNG + clipboard + console message + exits crop, Esc cancels. Also gates `IN_WantRelativeMouse` so the OS cursor reappears.

**Files:**
- Modify: `sdlquake/platform/crop_screenshot.c` (`Crop_HandleEvent`, save-on-commit)
- Modify: `sdlquake/platform/in_sdl.c` (event routing, relative-mouse gating)
- Modify: `sdlquake/platform/vid_sdl.c` (need `Crop_Active()` for `IN_WantRelativeMouse`)
- Modify: `sdlquake/engine_src/screen.c` (delete temporary `crop_cancel` command if added in Task 5)

- [ ] **Step 1: Add the save helper inside `crop_screenshot.c`**

Forward-declare the prototypes we need at the top of `crop_screenshot.c` (after the existing externs):

```c
extern int  Clipboard_SetPNG(const void *bytes, size_t size);

/* Hand-coded extern for the cvar (struct layout is stable across the engine). */
extern cvar_t scr_screenshot_clipboard;

/* SDL renderer needed for window→logical mouse coordinate conversion. */
extern SDL_Renderer *VID_GetRenderer(void);
```

Pull in stb's prototypes by relying on the same vendored header `vid_sdl.c` uses, but *without* the implementation define (the implementation lives in `vid_sdl.c`):

```c
#include "../vendor/stb/stb_image_write.h"
```

The Con_Printf and d_8to24table externs come from `quakedef.h` already included.

Add a static encode-and-save helper:

```c
typedef struct {
    unsigned char *data;
    size_t size, cap;
} crop_png_buf_t;

static void crop_png_append(void *ctx, void *data, int len)
{
    crop_png_buf_t *b = (crop_png_buf_t *)ctx;
    if (len <= 0) return;
    size_t need = b->size + (size_t)len;
    if (need > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        unsigned char *p = (unsigned char *)realloc(b->data, new_cap);
        if (!p) return;
        b->data = p; b->cap = new_cap;
    }
    memcpy(b->data + b->size, data, (size_t)len);
    b->size = need;
}

static void crop_commit(void)
{
    int x0, y0, x1, y1;
    Crop_GetRect(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g.w) x1 = g.w - 1;
    if (y1 >= g.h) y1 = g.h - 1;
    int rw = x1 - x0 + 1;
    int rh = y1 - y0 + 1;
    if (rw <= 0 || rh <= 0) { Con_Printf("screenshot rect: empty selection\n"); return; }

    unsigned char *rgb = (unsigned char *)malloc((size_t)rw * (size_t)rh * 3);
    if (!rgb) { Con_Printf("screenshot rect: out of memory\n"); return; }
    for (int y = 0; y < rh; y++) {
        const unsigned char *src = g.frozen + (size_t)(y0 + y) * g.w + x0;
        unsigned char       *dst = rgb       + (size_t)y * rw * 3;
        for (int x = 0; x < rw; x++) {
            unsigned c = d_8to24table[src[x]];
            dst[x*3 + 0] = (unsigned char)(c >> 16);
            dst[x*3 + 1] = (unsigned char)(c >>  8);
            dst[x*3 + 2] = (unsigned char)(c >>  0);
        }
    }

    crop_png_buf_t buf = {0};
    int ok = stbi_write_png_to_func(crop_png_append, &buf, rw, rh, 3, rgb, rw * 3);
    free(rgb);
    if (!ok || !buf.data) {
        free(buf.data);
        Con_Printf("screenshot rect: PNG encode failed\n");
        return;
    }

    FILE *fp = fopen(g.out_path, "wb");
    int wrote = 0;
    if (fp) {
        wrote = (fwrite(buf.data, 1, buf.size, fp) == buf.size);
        fclose(fp);
    }

    int do_clip = (int)scr_screenshot_clipboard.value;
    int clipped = (do_clip && Clipboard_SetPNG(buf.data, buf.size));
    free(buf.data);

    if (!wrote)
        Con_Printf("screenshot rect: write failed (%s)\n", g.out_path);
    else if (do_clip && clipped)
        Con_Printf("Wrote %s (also copied to clipboard)\n", g.out_path);
    else if (do_clip)
        Con_Printf("Wrote %s (clipboard copy failed)\n", g.out_path);
    else
        Con_Printf("Wrote %s\n", g.out_path);
}
```

- [ ] **Step 2: Implement `Crop_HandleEvent`**

Replace the stub:

```c
static void crop_set_endpoint(int idx, float wx, float wy)
{
    SDL_Renderer *r = VID_GetRenderer();
    float lx = wx, ly = wy;
    if (r) SDL_RenderCoordinatesFromWindow(r, wx, wy, &lx, &ly);
    int ix = (int)lx, iy = (int)ly;
    if (ix < 0) ix = 0; else if (ix >= g.w) ix = g.w - 1;
    if (iy < 0) iy = 0; else if (iy >= g.h) iy = g.h - 1;
    if (idx == 0) { g.x0 = g.x1 = ix; g.y0 = g.y1 = iy; }
    else          { g.x1 = ix;        g.y1 = iy; }
}

int Crop_HandleEvent(const SDL_Event *ev)
{
    if (!g.active || !ev) return 0;

    switch (ev->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (ev->button.button == SDL_BUTTON_LEFT) {
            g.dragging = 1;
            crop_set_endpoint(0, ev->button.x, ev->button.y);
            return 1;
        }
        return 1;  /* swallow other buttons too — no accidental fire */
    case SDL_EVENT_MOUSE_MOTION:
        if (g.dragging)
            crop_set_endpoint(1, ev->motion.x, ev->motion.y);
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button == SDL_BUTTON_LEFT && g.dragging) {
            crop_set_endpoint(1, ev->button.x, ev->button.y);
            g.dragging = 0;
            crop_commit();
            Crop_Exit();
        }
        return 1;
    case SDL_EVENT_KEY_DOWN:
        if (ev->key.scancode == SDL_SCANCODE_ESCAPE) {
            Con_Printf("screenshot rect: cancelled\n");
            Crop_Exit();
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}
```

- [ ] **Step 3: Route events in `in_sdl.c`**

Add to the top of `sdlquake/platform/in_sdl.c`:

```c
#include "crop_screenshot.h"
```

In `IN_ProcessEvents`, **before** the existing `ImguiLayer_ProcessEvent(&ev);` line (around line 131), insert:

```c
        if (Crop_HandleEvent(&ev))
            continue;
```

This routes all events to crop first when it's active, swallowing the ones it consumes so the editor/ImGui/game don't see them. (Place it before ImGui so a screenshot-rect drag inside the dev overlay still works as expected.)

In `IN_WantRelativeMouse` (around line 70), add at the top:

```c
    if (Crop_Active())
        return false;
```

so the OS cursor appears while composing.

- [ ] **Step 4: Remove any temporary unpause hack**

If you added a `crop_cancel` console command in Task 5 Step 5 for testing, delete the `Cmd_AddCommand` line and the `Crop_Cancel_f` function now.

- [ ] **Step 5: Build and full manual verification**

```sh
zig build run -- +map e1m1
```

Verify in console:

1. `screenshot` → `Wrote screenshots/shot_NNNN.png (also copied to clipboard)`; file opens, paste into a chat app pastes the image.
2. `screenshot rect`:
   - The frame freezes (display).
   - The OS cursor appears.
   - Walking forward / firing has no effect (simulation paused).
   - Click-drag from one corner to another: the dragged-out rectangle stays bright with a 1-px white border; everything outside dims to ~50%.
   - Release: console prints `Wrote screenshots/shot_NNNN.png (also copied to clipboard)`; the saved PNG is the exact bright region; pasting into a chat app pastes the same crop; game resumes.
3. `screenshot rect`, then press Esc → console prints `screenshot rect: cancelled`; no file written; game resumes; clipboard unchanged.
4. Trigger a hostile monster (e.g. `noclip` past one, then `noclip` back); fire to provoke it; right as a projectile is in the air, type `screenshot rect`. Confirm: the projectile freezes mid-air; you cannot be hit while composing; after commit/cancel the projectile resumes its trajectory.
5. `scr_screenshot_clipboard 0`, then `screenshot` and `screenshot rect` → both still write files but clipboard message says "(clipboard copy failed)" — wait, actually with cvar 0 it should say neither "(also copied)" nor "(clipboard copy failed)". Re-read the message branches in `crop_commit` / `SCR_ScreenShot_f`: when `do_clip` is 0 we fall to the bare `"Wrote %s\n"` branch. Verify that's what shows.
6. Take 1 console screenshot, then 1 MCP screenshot (`python3 scripts/mcp_call.py screenshot` from a second terminal), then another console screenshot → confirm the three files have contiguous indices.

- [ ] **Step 6: Commit**

```sh
git add sdlquake/platform/crop_screenshot.{c,h} sdlquake/platform/in_sdl.c sdlquake/engine_src/screen.c
git commit -m "feat(screenshot): rect-selection commit/cancel + input routing

Mouse-drag inside the frozen frame defines the crop; release saves the
PNG, copies to clipboard (when scr_screenshot_clipboard is set), and
prints a console summary. Esc cancels. The OS cursor is shown while
the crop session is active; relative-mouse mode resumes on exit."
```

---

## Done

All spec requirements covered:

- **Fullscreen mode** → Task 4 (uses shared path, encode-once, clipboard).
- **Rect mode** → Tasks 5–7 (skeleton + overlay + input).
- **Shared `screenshots/shot_NNNN.png` with MCP** → Tasks 1 + 4.
- **Clipboard via SDL3 `image/png`** → Tasks 2 + 4 + 7.
- **Full pause (display + sim)** → Task 5 (`cl.paused`) + Task 6 (frozen-frame substitution).
- **`scr_screenshot_clipboard` cvar (default 1)** → Task 4.
- **No PCX cleanup, no shift-square, no keybind, no aspect-lock** → not implemented (YAGNI from spec).

## Notes for future use

- The frozen frame snapshot includes whatever is on screen at the moment the command runs — including the console if it's pulled down. For clean rect screenshots, `bind p "screenshot rect"` (or any other key) and trigger from the game view rather than typing it into the console. Same behaviour as fullscreen `screenshot`.

#include "crop_screenshot.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../engine_src/quakedef.h"   /* cl.paused, vid, byte, d_8to24table */
#include "vid_palette.h"              /* extern byte vid_palette_id[] */
#include "../vendor/stb/stb_image_write.h"   /* prototypes only; impl in vid_sdl.c */

extern int           Clipboard_SetPNG(const void *bytes, size_t size);
extern cvar_t        scr_screenshot_clipboard;
extern SDL_Renderer *VID_GetRenderer(void);
extern float         scr_con_current;   /* defined in screen.c, no header */

typedef struct {
    unsigned char *frozen;       /* w*h bytes */
    unsigned char *frozen_pal;   /* w*h bytes */
    int            w, h;
    int            x0, y0, x1, y1;
    int            dragging;
    int            active;
    int            pending;      /* request received; snapshot on next VID_Update */
    qboolean       prev_paused;
    char           out_path[256];
} crop_state_t;

static crop_state_t g;

/* The command-time entry point only requests a session: it hides the
   console/menu and pauses simulation, but defers the framebuffer
   snapshot until the next VID_Update via Crop_FrameStart. Snapshotting
   here would capture the previous frame, which still has the console
   pulled down — by deferring one frame the engine gets a chance to
   redraw the world without the console first. */
void Crop_Enter(const char *out_path)
{
    if (g.active || g.pending) return;   /* already in a session — ignore */
    if (!out_path || !out_path[0]) return;

    /* Force the console (or menu) off the screen for the next render
       so the snapshot is clean. Switching key_dest is enough on its
       own — SCR_UpdateScreen sets scr_conlines = 0 when key_dest is
       key_game — but scr_con_current animates down over several
       frames, so zero it directly. */
    key_dest        = key_game;
    scr_con_current = 0;

    g.prev_paused = cl.paused;
    cl.paused     = true;

    strncpy(g.out_path, out_path, sizeof(g.out_path) - 1);
    g.out_path[sizeof(g.out_path) - 1] = '\0';
    g.pending = 1;
}

/* Called at the top of VID_Update each frame. Performs the deferred
   framebuffer snapshot the frame after Crop_Enter, so vid.buffer
   already reflects a render that excluded the console. */
static int crop_do_snapshot(void)
{
    int w, h, rowbytes, y;

    if (!vid.buffer) return 0;
    w        = (int)vid.width;
    h        = (int)vid.height;
    rowbytes = (int)vid.rowbytes;
    if (w <= 0 || h <= 0 || rowbytes < w) return 0;

    g.frozen     = (unsigned char *)malloc((size_t)w * (size_t)h);
    g.frozen_pal = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!g.frozen || !g.frozen_pal) {
        free(g.frozen);     g.frozen     = NULL;
        free(g.frozen_pal); g.frozen_pal = NULL;
        return 0;
    }

    D_EnableBackBufferAccess();
    for (y = 0; y < h; y++) {
        memcpy(g.frozen + (size_t)y * w,
               vid.buffer + (size_t)y * rowbytes,
               (size_t)w);
    }
    memcpy(g.frozen_pal, vid_palette_id, (size_t)w * (size_t)h);
    D_DisableBackBufferAccess();

    g.w  = w;  g.h  = h;
    g.x0 = 0;  g.y0 = 0;
    g.x1 = w - 1; g.y1 = h - 1;
    g.dragging = 0;
    g.active   = 1;
    return 1;
}

void Crop_FrameStart(void)
{
    if (!g.pending) return;
    g.pending = 0;
    if (!crop_do_snapshot()) {
        /* Snapshot failed — restore pause state and bail. */
        cl.paused = g.prev_paused;
    }
}

void Crop_Exit(void)
{
    if (!g.active && !g.pending) return;
    cl.paused = g.prev_paused;
    free(g.frozen);     g.frozen     = NULL;
    free(g.frozen_pal); g.frozen_pal = NULL;
    g.active   = 0;
    g.dragging = 0;
    g.pending  = 0;
}

int Crop_Active(void) { return g.active; }

/* Growable buffer for stbi_write_png_to_func — duplicates the same shape
   as png_buf_t in vid_sdl.c, kept file-local here to avoid a header. */
typedef struct {
    unsigned char *data;
    size_t         size;
    size_t         cap;
} crop_png_buf_t;

static void crop_png_append(void *ctx, void *data, int len)
{
    crop_png_buf_t *b = (crop_png_buf_t *)ctx;
    size_t need;
    unsigned char *p;
    size_t new_cap;
    if (len <= 0) return;
    need = b->size + (size_t)len;
    if (need > b->cap) {
        new_cap = b->cap ? b->cap * 2 : 4096;
        while (new_cap < need) new_cap *= 2;
        p = (unsigned char *)realloc(b->data, new_cap);
        if (!p) return;
        b->data = p;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->size, data, (size_t)len);
    b->size = need;
}

static void crop_commit(void)
{
    int x0, y0, x1, y1;
    int rw, rh, x, y;
    unsigned char *rgb;
    crop_png_buf_t buf;
    int ok;
    FILE *fp;
    int wrote;
    int do_clip;
    int clipped;

    Crop_GetRect(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= g.w) x1 = g.w - 1;
    if (y1 >= g.h) y1 = g.h - 1;
    rw = x1 - x0 + 1;
    rh = y1 - y0 + 1;
    if (rw <= 0 || rh <= 0) { Con_Printf("screenshot rect: empty selection\n"); return; }

    rgb = (unsigned char *)malloc((size_t)rw * (size_t)rh * 3);
    if (!rgb) { Con_Printf("screenshot rect: out of memory\n"); return; }
    for (y = 0; y < rh; y++) {
        const unsigned char *src = g.frozen + (size_t)(y0 + y) * g.w + x0;
        unsigned char       *dst = rgb       + (size_t)y * rw * 3;
        for (x = 0; x < rw; x++) {
            unsigned c = d_8to24table[src[x]];
            dst[x*3 + 0] = (unsigned char)(c >> 16);
            dst[x*3 + 1] = (unsigned char)(c >>  8);
            dst[x*3 + 2] = (unsigned char)(c >>  0);
        }
    }

    memset(&buf, 0, sizeof(buf));
    ok = stbi_write_png_to_func(crop_png_append, &buf, rw, rh, 3, rgb, rw * 3);
    free(rgb);
    if (!ok || !buf.data) {
        free(buf.data);
        Con_Printf("screenshot rect: PNG encode failed\n");
        return;
    }

    wrote = 0;
    fp = fopen(g.out_path, "wb");
    if (fp) {
        wrote = (fwrite(buf.data, 1, buf.size, fp) == buf.size);
        fclose(fp);
    }

    do_clip = (int)scr_screenshot_clipboard.value;
    clipped = (do_clip && Clipboard_SetPNG(buf.data, buf.size));
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

/* Map a window-pixel mouse coordinate into framebuffer-local coords,
   clamp to [0, g.w/g.h - 1], and store as either the start endpoint
   (idx == 0) or the drag endpoint (idx != 0). */
static void crop_set_endpoint(int idx, float wx, float wy)
{
    SDL_Renderer *r = VID_GetRenderer();
    float lx = wx, ly = wy;
    int ix, iy;
    if (r) SDL_RenderCoordinatesFromWindow(r, wx, wy, &lx, &ly);
    ix = (int)lx;
    iy = (int)ly;
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
        }
        return 1;  /* swallow all button events while active */
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

void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h)
{
    int x0, y0, x1, y1;
    int pitch_px;
    int x, y;

    if (!g.active || !argb) return;

    Crop_GetRect(&x0, &y0, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= w) x1 = w - 1;
    if (y1 >= h) y1 = h - 1;

    pitch_px = pitch_bytes / 4;
    for (y = 0; y < h; y++) {
        unsigned *row = argb + (size_t)y * pitch_px;
        for (x = 0; x < w; x++) {
            int inside    = (x >= x0 && x <= x1 && y >= y0 && y <= y1);
            int on_border = inside && (x == x0 || x == x1 || y == y0 || y == y1);
            if (on_border)      row[x] = 0xFFFFFFFFu;
            else if (!inside)   row[x] = (row[x] >> 1) & 0x7F7F7Fu;
            /* else: inside, leave untouched */
        }
    }
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

void Crop_GetRect(int *x0, int *y0, int *x1, int *y1)
{
    int lo_x = g.x0, hi_x = g.x1;
    int lo_y = g.y0, hi_y = g.y1;
    if (lo_x > hi_x) { int t = lo_x; lo_x = hi_x; hi_x = t; }
    if (lo_y > hi_y) { int t = lo_y; lo_y = hi_y; hi_y = t; }
    if (x0) *x0 = lo_x;
    if (y0) *y0 = lo_y;
    if (x1) *x1 = hi_x;
    if (y1) *y1 = hi_y;
}

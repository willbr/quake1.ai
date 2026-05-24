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
    qboolean       prev_paused;
    char           out_path[256];
} crop_state_t;

static crop_state_t g;

void Crop_Enter(const char *out_path)
{
    int w, h, rowbytes, y;

    if (g.active) return;                /* already in a session — ignore */
    if (!out_path || !out_path[0]) return;
    if (!vid.buffer) return;

    w        = (int)vid.width;
    h        = (int)vid.height;
    rowbytes = (int)vid.rowbytes;
    if (w <= 0 || h <= 0 || rowbytes < w) return;

    g.frozen     = (unsigned char *)malloc((size_t)w * (size_t)h);
    g.frozen_pal = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!g.frozen || !g.frozen_pal) {
        free(g.frozen);     g.frozen     = NULL;
        free(g.frozen_pal); g.frozen_pal = NULL;
        return;
    }

    /* Match the convention of fullscreen SCR_ScreenShot_f: wrap the
       framebuffer read in D_Enable/DisableBackBufferAccess so vid.buffer
       is guaranteed readable. */
    D_EnableBackBufferAccess();
    for (y = 0; y < h; y++) {
        memcpy(g.frozen + (size_t)y * w,
               vid.buffer + (size_t)y * rowbytes,
               (size_t)w);
    }
    /* vid_palette_id is tight (w bytes per row) already. */
    memcpy(g.frozen_pal, vid_palette_id, (size_t)w * (size_t)h);
    D_DisableBackBufferAccess();

    g.w  = w;  g.h  = h;
    g.x0 = 0;  g.y0 = 0;
    g.x1 = w - 1; g.y1 = h - 1;
    g.dragging   = 0;
    g.active     = 1;
    g.prev_paused = cl.paused;
    cl.paused     = true;

    strncpy(g.out_path, out_path, sizeof(g.out_path) - 1);
    g.out_path[sizeof(g.out_path) - 1] = '\0';
}

void Crop_Exit(void)
{
    if (!g.active) return;
    cl.paused = g.prev_paused;
    free(g.frozen);     g.frozen     = NULL;
    free(g.frozen_pal); g.frozen_pal = NULL;
    g.active   = 0;
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

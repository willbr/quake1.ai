// vid_sdl.c -- SDL3 video driver replacing vid_win.c
// Software framebuffer (8-bit palettized) -> SDL_Texture -> SDL_Renderer

#include <SDL3/SDL.h>
#include "quakedef.h"
#include "winquake.h"
#include "imgui_layer.h"
#include "r_bbox.h"
#include "r_paths.h"
#include "vid_palette.h"
#include "debug_lines.h"

// Menu helpers from menu.c (no shared header)
extern void M_Print(int cx, int cy, char *str);
extern void M_PrintWhite(int cx, int cy, char *str);
extern void M_DrawPic(int x, int y, qpic_t *pic);
extern void M_DrawCharacter(int cx, int line, int num);
extern void M_Menu_Options_f(void);

// ---------------------------------------------------------------------------
// Globals that Win32 platform files normally define
// ---------------------------------------------------------------------------
HINSTANCE global_hInstance = NULL;
int       global_nCmdShow  = 0;
HWND      mainwindow       = NULL;

qboolean DDActive    = false;
qboolean ActiveApp   = true;
qboolean Minimized   = false;
qboolean WinNT       = false;
qboolean block_drawing = false;

LPDIRECTDRAW        lpDD        = NULL;
LPDIRECTDRAWSURFACE lpPrimary   = NULL;
LPDIRECTDRAWSURFACE lpFrontBuffer = NULL;
LPDIRECTDRAWSURFACE lpBackBuffer  = NULL;
LPDIRECTDRAWPALETTE lpDDPal     = NULL;
LPDIRECTSOUND       pDS         = NULL;

modestate_t modestate = MS_WINDOWED;

cvar_t _windowed_mouse = {"_windowed_mouse", "1", true};

viddef_t vid;

// d_8to24table[256]: slot 0 Quake palette. Engine code reads this directly
// via the `extern unsigned d_8to24table[256]` declaration in vid.h.
unsigned    d_8to24table[256];
unsigned short d_8to16table[256]; // unused in software but declared in vid.h

// Multi-palette LUT used only by VID_Update.
// vid_lut[VID_PAL_QUAKE] mirrors d_8to24table[]; slots 1+ are filled on demand.
static unsigned vid_lut[VID_NUM_PALETTES][256];

void (*vid_menudrawfn)(void) = NULL;
void (*vid_menukeyfn)(int key) = NULL;

static SDL_Window   *sdl_window   = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture  *sdl_texture  = NULL;

static int vid_scale_active = 3;               // render scale actually applied
static cvar_t vid_scale = {"vid_scale", "0", true}; // 0=auto, 1-4=explicit; archived

static int vid_window_scale_active = 3;        // window-size scale actually applied
static cvar_t vid_window_scale = {"vid_window_scale", "0", true};

// Last-saved window position; -1 = unset, let the OS pick.
static cvar_t vid_window_x = {"vid_window_x", "-1", true};
static cvar_t vid_window_y = {"vid_window_y", "-1", true};

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

#define VID_WIDTH  320
#define VID_HEIGHT 200
#define VID_RENDER_MAX_W (VID_WIDTH  * 4)  /* 1280 */
#define VID_RENDER_MAX_H (VID_HEIGHT * 4)  /* 800  */

static int vid_render_w = VID_WIDTH;
static int vid_render_h = VID_HEIGHT;

static byte vid_buffer[VID_RENDER_MAX_W * VID_RENDER_MAX_H];
byte vid_palette_id[VID_RENDER_MAX_W * VID_RENDER_MAX_H];  /* declared in vid_palette.h */

// ---------------------------------------------------------------------------
// Palette helpers
// ---------------------------------------------------------------------------

static void build_palette_slot(int slot, unsigned char *palette)
{
    unsigned *lut = vid_lut[slot];
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
    // Slot 0 also backs the engine-visible d_8to24table[256].
    if (slot == VID_PAL_QUAKE)
        memcpy(d_8to24table, lut, 256 * sizeof(unsigned));
}

static void build_palette(unsigned char *palette)
{
    build_palette_slot(VID_PAL_QUAKE, palette);
}

// Load an auxiliary palette from a .lmp file (768 bytes: 256 * RGB) into the
// given vid_lut slot. Tolerates a missing file — slot stays zero-filled and a
// warning is printed. COM_LoadHunkFile is declared in common.h (via quakedef.h).
static void vid_load_aux_palette(int slot, const char *qpath)
{
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

void VID_SetPalette(unsigned char *palette)   { build_palette(palette); }
void VID_ShiftPalette(unsigned char *palette) { build_palette(palette); }

// ---------------------------------------------------------------------------
// Live resolution switch
// ---------------------------------------------------------------------------

static void VID_ApplyScale(int scale)
{
    extern short *d_pzbuffer;
    extern int    D_SurfaceCacheForRes(int w, int h);
    extern void   D_InitCaches(void *buffer, int size);

    int new_w = VID_WIDTH  * scale;
    int new_h = VID_HEIGHT * scale;

    if (sdl_texture) { SDL_DestroyTexture(sdl_texture); sdl_texture = NULL; }
    sdl_texture = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        new_w, new_h);
    if (!sdl_texture)
        Sys_Error("SDL_CreateTexture failed: %s", SDL_GetError());

    SDL_SetRenderLogicalPresentation(sdl_renderer, new_w, new_h,
        SDL_LOGICAL_PRESENTATION_LETTERBOX);

    vid_render_w = new_w;
    vid_render_h = new_h;

    vid.width         = new_w;
    vid.height        = new_h;
    vid.rowbytes      = new_w;
    vid.conwidth      = new_w;
    vid.conheight     = new_h;
    vid.conrowbytes   = new_w;
    vid.maxwarpwidth  = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.recalc_refdef = 1;

    {
        extern void D_FlushCaches(void);
        D_FlushCaches();
    }
    int zbuf_bytes  = new_w * new_h * sizeof(short);
    int cache_bytes = D_SurfaceCacheForRes(new_w, new_h);
    D_InitCaches((byte *)d_pzbuffer + zbuf_bytes, cache_bytes);
}

static void VID_ApplyWindowScale(int scale)
{
    SDL_SetWindowSize(sdl_window, VID_WIDTH * scale, VID_HEIGHT * scale);
}

// Capture the current SDL window position into the archived cvars so the
// next launch can restore it. Invoked from the Video Options menu — no
// autosave, so casual window dragging won't quietly overwrite a saved spot.
static void VID_SaveWindowPos(void)
{
    int x = 0, y = 0;
    if (!sdl_window) return;
    SDL_GetWindowPosition(sdl_window, &x, &y);
    Cvar_SetValue("vid_window_x", (float)x);
    Cvar_SetValue("vid_window_y", (float)y);
}

// ---------------------------------------------------------------------------
// Video Options menu
// ---------------------------------------------------------------------------

static void VID_MenuDraw(void)
{
    qpic_t *p = Draw_CachePic("gfx/vidmodes.lmp");
    M_DrawPic((320 - p->width) / 2, 4, p);

    M_Print(64, 40, "Render Resolution");
    int render_y = 56;
    for (int i = 0; i < VID_NUM_SCALES; i++)
    {
        if (vid_scale_factors[i] == vid_scale_active)
            M_PrintWhite(80, render_y + i * 8, (char *)vid_scale_labels[i]);
        else
            M_Print(80, render_y + i * 8, (char *)vid_scale_labels[i]);
        if (vid_menu_cursor == i)
            M_DrawCharacter(72, render_y + i * 8, 12 + ((int)(realtime * 4) & 1));
    }

    M_Print(64, 100, "Window Size");
    int window_y = 116;
    for (int i = 0; i < VID_NUM_SCALES; i++)
    {
        if (vid_scale_factors[i] == vid_window_scale_active)
            M_PrintWhite(80, window_y + i * 8, (char *)vid_scale_labels[i]);
        else
            M_Print(80, window_y + i * 8, (char *)vid_scale_labels[i]);
        if (vid_menu_cursor == VID_NUM_SCALES + i)
            M_DrawCharacter(72, window_y + i * 8, 12 + ((int)(realtime * 4) & 1));
    }

    int save_y = 160;
    M_Print(80, save_y, "Save Window Position");
    if (vid_menu_cursor == VID_MENU_SAVE_POS)
        M_DrawCharacter(72, save_y, 12 + ((int)(realtime * 4) & 1));
}

static void VID_MenuKey(int key)
{
    switch (key)
    {
    case K_ESCAPE:
        M_Menu_Options_f();
        break;

    case K_UPARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor - 1 + VID_MENU_ITEMS) % VID_MENU_ITEMS;
        break;

    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor + 1) % VID_MENU_ITEMS;
        break;

    case K_ENTER:
    case K_SPACE:
        if (vid_menu_cursor < VID_NUM_SCALES)
        {
            int new_scale = vid_scale_factors[vid_menu_cursor];
            vid_scale_active = new_scale;
            Cvar_SetValue("vid_scale", (float)new_scale);
            VID_ApplyScale(new_scale);
        }
        else if (vid_menu_cursor < VID_MENU_SAVE_POS)
        {
            int new_scale = vid_scale_factors[vid_menu_cursor - VID_NUM_SCALES];
            vid_window_scale_active = new_scale;
            Cvar_SetValue("vid_window_scale", (float)new_scale);
            VID_ApplyWindowScale(new_scale);
        }
        else
        {
            VID_SaveWindowPos();
        }
        S_LocalSound("misc/menu2.wav");
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

extern qboolean sys_headless;

// Pre-load the video cvars we need at window-creation time. The engine
// doesn't exec config.cfg until after VID_Init returns (via deferred
// Cbuf_Execute in the first _Host_Frame), so without this the cvars still
// hold their registration defaults when we call SDL_CreateWindow.
static void vid_preload_cvars_from_config(void)
{
    extern char com_gamedir[MAX_OSPATH];
    char path[MAX_OSPATH + 16];
    FILE *f;
    char line[256];

    snprintf(path, sizeof path, "%s/config.cfg", com_gamedir);
    f = fopen(path, "r");
    if (!f) return;

    while (fgets(line, sizeof line, f))
    {
        float v;
        if      (sscanf(line, "vid_window_scale \"%f", &v) == 1) Cvar_SetValue("vid_window_scale", v);
        else if (sscanf(line, "vid_window_x \"%f", &v) == 1)     Cvar_SetValue("vid_window_x", v);
        else if (sscanf(line, "vid_window_y \"%f", &v) == 1)     Cvar_SetValue("vid_window_y", v);
        else if (sscanf(line, "vid_scale \"%f", &v) == 1)        Cvar_SetValue("vid_scale", v);
    }
    fclose(f);
}

void VID_Init(unsigned char *palette)
{
    Cvar_RegisterVariable(&vid_scale);
    Cvar_RegisterVariable(&vid_window_scale);
    Cvar_RegisterVariable(&vid_window_x);
    Cvar_RegisterVariable(&vid_window_y);

    vid_preload_cvars_from_config();

    if (!sys_headless)
    {
        // Auto-detect the largest integer scale that fits the desktop; used
        // as fallback for both render and window when their cvars are unset.
        int auto_scale = 3;
        {
            SDL_DisplayID display = SDL_GetPrimaryDisplay();
            SDL_Rect usable;
            if (SDL_GetDisplayUsableBounds(display, &usable))
            {
                int sx = usable.w / VID_WIDTH;
                int sy = usable.h / VID_HEIGHT;
                auto_scale = sx < sy ? sx : sy;
                if (auto_scale < 1) auto_scale = 1;
                if (auto_scale > 4) auto_scale = 4;
            }
        }

        int render_req = (int)vid_scale.value;
        int render_scale = (render_req >= 1 && render_req <= 4) ? render_req : auto_scale;

        int window_req = (int)vid_window_scale.value;
        int window_scale = (window_req >= 1 && window_req <= 4) ? window_req : auto_scale;

        vid_scale_active        = render_scale;
        vid_window_scale_active = window_scale;
        vid_render_w            = VID_WIDTH  * render_scale;
        vid_render_h            = VID_HEIGHT * render_scale;

        {
            int wx = (int)vid_window_x.value;
            int wy = (int)vid_window_y.value;
            int ww = VID_WIDTH  * window_scale;
            int wh = VID_HEIGHT * window_scale;
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "quake1.ai");
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
            if (wx >= 0 && wy >= 0)
            {
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, wx);
                SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, wy);
            }
            sdl_window = SDL_CreateWindowWithProperties(props);
            SDL_DestroyProperties(props);
            if (!sdl_window)
                Sys_Error("SDL_CreateWindow failed: %s", SDL_GetError());
        }

        sdl_renderer = SDL_CreateRenderer(sdl_window, NULL);
        if (!sdl_renderer)
            Sys_Error("SDL_CreateRenderer failed: %s", SDL_GetError());

        SDL_SetRenderLogicalPresentation(sdl_renderer,
            vid_render_w, vid_render_h,
            SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);

        sdl_texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            vid_render_w, vid_render_h);
        if (!sdl_texture)
            Sys_Error("SDL_CreateTexture failed: %s", SDL_GetError());
    }

    // Fill in viddef
    memset(&vid, 0, sizeof(vid));
    vid.width      = vid_render_w;
    vid.height     = vid_render_h;
    vid.rowbytes   = vid_render_w;
    // Pixel aspect for 4:3 CRT at 320x200 — scale-independent since w and h
    // scale by the same factor, preserving the ratio.
    vid.aspect     = ((float)VID_HEIGHT / (float)VID_WIDTH) * (320.0f / 240.0f);
    vid.numpages   = 1;
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer     = vid_buffer;
    vid.conbuffer  = vid_buffer;
    vid.conwidth   = vid_render_w;
    vid.conheight  = vid_render_h;
    vid.conrowbytes = vid_render_w;
    vid.maxwarpwidth  = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.recalc_refdef = 1;

    build_palette(palette);
    vid_load_aux_palette(VID_PAL_DOOM, "gfx/palette_doom.lmp");

    if (!sys_headless)
    {
        ImguiLayer_Init(sdl_window, sdl_renderer);
        RBBox_Init();
        RPaths_Init();
    }

    // Allocate z-buffer and surface cache from the hunk (as vid_win.c does)
    {
        extern short *d_pzbuffer;
        extern int    D_SurfaceCacheForRes(int w, int h);
        extern void   D_InitCaches(void *buffer, int size);
        extern void  *Hunk_HighAllocName(int size, char *name);

        // Pre-allocate for max resolution so VID_ApplyScale never needs hunk realloc.
        int zbuf_bytes_max  = VID_RENDER_MAX_W * VID_RENDER_MAX_H * sizeof(short);
        int cache_bytes_max = D_SurfaceCacheForRes(VID_RENDER_MAX_W, VID_RENDER_MAX_H);
        d_pzbuffer = (short *)Hunk_HighAllocName(zbuf_bytes_max + cache_bytes_max, "video");

        int zbuf_bytes  = vid_render_w * vid_render_h * sizeof(short);
        int cache_bytes = D_SurfaceCacheForRes(vid_render_w, vid_render_h);
        D_InitCaches((byte *)d_pzbuffer + zbuf_bytes, cache_bytes);
    }

    vid_menu_cursor = vid_scale_active - 1;
    vid_menudrawfn  = VID_MenuDraw;
    vid_menukeyfn   = VID_MenuKey;
}

void VID_Shutdown(void)
{
    ImguiLayer_Shutdown();
    if (sdl_texture)  { SDL_DestroyTexture(sdl_texture);   sdl_texture  = NULL; }
    if (sdl_renderer) { SDL_DestroyRenderer(sdl_renderer); sdl_renderer = NULL; }
    if (sdl_window)   { SDL_DestroyWindow(sdl_window);     sdl_window   = NULL; }
}

// ---------------------------------------------------------------------------
// Frame update: expand 8-bit -> 32-bit, upload, present
// ---------------------------------------------------------------------------

void VID_Update(vrect_t *rects)
{
    if (!sdl_texture) return;

    RPaths_Draw();
    RBBox_Draw();
    DebugLines_Draw();

    void *pixels;
    int pitch;
    if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) >= 0)
    {
        // vid_lut[slot][index]: per-pixel palette dispatch.
        // Slot 0 (Quake) mirrors d_8to24table[]; slot 1 (Doom) filled on demand.
        unsigned (*lut)[256] = vid_lut;

        for (int y = 0; y < vid_render_h; y++)
        {
            unsigned *dst     = (unsigned *)((byte *)pixels + y * pitch);
            byte     *src     = vid.buffer     + y * vid.rowbytes;
            byte     *pal_src = vid_palette_id + y * vid_render_w;
            for (int x = 0; x < vid_render_w; x++)
                dst[x] = lut[pal_src[x]][src[x]];
        }

        SDL_UnlockTexture(sdl_texture);
        SDL_RenderClear(sdl_renderer);
        SDL_RenderTexture(sdl_renderer, sdl_texture, NULL, NULL);
        ImguiLayer_Render();
        SDL_RenderPresent(sdl_renderer);
    }

    // Reset palette tags so the next frame's renderer starts clean. Runs
    // unconditionally (even if SDL_LockTexture failed) to preserve the
    // invariant: palette_id is always zero at the start of every frame's
    // renderer pipeline. Must run AFTER the expand step (above) which
    // reads the tags this frame's renderer wrote — clearing earlier
    // would wipe those tags before expand sees them.
    memset(vid_palette_id, 0, vid_render_w * vid_render_h);

    // Force the sbar to redraw on the next SCR_UpdateScreen. Stock Quake
    // uses sb_updates / vid.numpages to skip Sbar_Draw once each VRAM page
    // has the latest sbar — an optimization that assumed the front buffer
    // and back buffer kept stable copies between flips. Our SDL backend
    // re-uploads vid.buffer wholesale every frame, so any region not
    // explicitly redrawn this frame keeps whatever the previous renderer
    // wrote there — most visibly, a bobbing viewmodel sprite writing pixels
    // under the sbar. Resetting sb_updates here makes Sbar_Draw redraw
    // every frame; the redraw cost is a couple of paletted blits and is
    // negligible compared to the framebuffer expand+upload above.
    Sbar_Changed();
}

int VID_SetMode(int modenum, unsigned char *palette)
{
    (void)modenum;
    build_palette(palette);
    return 1;
}

void VID_HandlePause(qboolean pause) { (void)pause; }

// Stubs for DirectDraw surface locking (no-ops in SDL version)
int  VID_ForceUnlockedAndReturnState(void) { return 0; }
void VID_ForceLockState(int lk)            { (void)lk; }
void VID_SetDefaultMode(void)              {}

// These are called by quakedef.h macros on _WIN32 without WINDED
void VID_LockBuffer(void)   {}
void VID_UnlockBuffer(void) {}

SDL_Window   *VID_GetWindow  (void) { return sdl_window;   }
SDL_Renderer *VID_GetRenderer(void) { return sdl_renderer; }

// Loading disc overlays — not needed with SDL renderer
void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height)
    { (void)x; (void)y; (void)pbitmap; (void)width; (void)height; }
void D_EndDirectRect(int x, int y, int width, int height)
    { (void)x; (void)y; (void)width; (void)height; }

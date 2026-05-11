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

static int vid_scale_active = 3;               // scale actually applied; set in VID_Init
static cvar_t vid_scale = {"vid_scale", "0", true}; // 0=auto, 1-4=explicit; archived

#define VID_NUM_SCALES 4
static const int    vid_scale_factors[VID_NUM_SCALES] = {1, 2, 3, 4};
static const char  *vid_scale_labels[VID_NUM_SCALES]  = {
    "1x  320x200",
    "2x  640x400",
    "3x  960x600",
    "4x  1280x800"
};
static int vid_menu_cursor = 0; // index into vid_scale_factors; set in VID_Init

#define VID_WIDTH  320
#define VID_HEIGHT 200

static byte vid_buffer[VID_WIDTH * VID_HEIGHT];
byte vid_palette_id[VID_WIDTH * VID_HEIGHT];   // declared in vid_palette.h

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
// Video Options menu
// ---------------------------------------------------------------------------

static void VID_MenuDraw(void)
{
    qpic_t *p = Draw_CachePic("gfx/vidmodes.lmp");
    M_DrawPic((320 - p->width) / 2, 4, p);

    int base_y = 40;
    for (int i = 0; i < VID_NUM_SCALES; i++)
    {
        if (vid_scale_factors[i] == vid_scale_active)
            M_PrintWhite(64, base_y + i * 16, (char *)vid_scale_labels[i]);
        else
            M_Print(64, base_y + i * 16, (char *)vid_scale_labels[i]);
        if (vid_menu_cursor == i)
            M_DrawCharacter(56, base_y + i * 16, 12 + ((int)(realtime * 4) & 1));
    }
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
        vid_menu_cursor = (vid_menu_cursor - 1 + VID_NUM_SCALES) % VID_NUM_SCALES;
        break;

    case K_DOWNARROW:
        S_LocalSound("misc/menu1.wav");
        vid_menu_cursor = (vid_menu_cursor + 1) % VID_NUM_SCALES;
        break;

    case K_ENTER:
    case K_SPACE:
        {
            int new_scale = vid_scale_factors[vid_menu_cursor];
            vid_scale_active = new_scale;
            Cvar_SetValue("vid_scale", (float)new_scale);
            SDL_SetWindowSize(sdl_window, VID_WIDTH * new_scale, VID_HEIGHT * new_scale);
            S_LocalSound("misc/menu2.wav");
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

extern qboolean sys_headless;

void VID_Init(unsigned char *palette)
{
    Cvar_RegisterVariable(&vid_scale);

    if (!sys_headless)
    {
        // Determine window scale: explicit cvar or auto-detect
        int scale;
        int req = (int)vid_scale.value;
        if (req >= 1 && req <= 4)
        {
            scale = req;
        }
        else
        {
            scale = 3; // fallback
            SDL_DisplayID display = SDL_GetPrimaryDisplay();
            SDL_Rect usable;
            if (SDL_GetDisplayUsableBounds(display, &usable))
            {
                int sx = usable.w / VID_WIDTH;
                int sy = usable.h / VID_HEIGHT;
                scale = sx < sy ? sx : sy;
                if (scale < 1) scale = 1;
                if (scale > 4) scale = 4;
            }
        }
        vid_scale_active = scale;

        sdl_window = SDL_CreateWindow("quake1.ai",
            VID_WIDTH * scale, VID_HEIGHT * scale,
            SDL_WINDOW_RESIZABLE);
        if (!sdl_window)
            Sys_Error("SDL_CreateWindow failed: %s", SDL_GetError());

        sdl_renderer = SDL_CreateRenderer(sdl_window, NULL);
        if (!sdl_renderer)
            Sys_Error("SDL_CreateRenderer failed: %s", SDL_GetError());

        // Scale texture to window, keeping pixel art crisp
        SDL_SetRenderLogicalPresentation(sdl_renderer,
            VID_WIDTH, VID_HEIGHT,
            SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);

        // Streaming texture: we upload the expanded 32-bit framebuffer every frame
        sdl_texture = SDL_CreateTexture(sdl_renderer,
            SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING,
            VID_WIDTH, VID_HEIGHT);
        if (!sdl_texture)
            Sys_Error("SDL_CreateTexture failed: %s", SDL_GetError());
    }

    // Fill in viddef
    memset(&vid, 0, sizeof(vid));
    vid.width      = VID_WIDTH;
    vid.height     = VID_HEIGHT;
    vid.rowbytes   = VID_WIDTH;
    // Match original formula from vid_win.c: pixel aspect for 4:3 CRT at this res
    // (200/320)*(320/240) = 0.8333 for 320x200, keeps weapon/HUD layout correct
    vid.aspect     = ((float)VID_HEIGHT / (float)VID_WIDTH) * (320.0f / 240.0f);
    vid.numpages   = 1;
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer     = vid_buffer;
    vid.conbuffer  = vid_buffer; // same framebuffer — Draw_Character writes here
    vid.conwidth   = VID_WIDTH;
    vid.conheight  = VID_HEIGHT;
    vid.conrowbytes = VID_WIDTH;
    vid.maxwarpwidth  = VID_WIDTH;
    vid.maxwarpheight = VID_HEIGHT;
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

        int zbuf_bytes  = VID_WIDTH * VID_HEIGHT * sizeof(short);
        int cache_bytes = D_SurfaceCacheForRes(VID_WIDTH, VID_HEIGHT);

        d_pzbuffer = (short *)Hunk_HighAllocName(zbuf_bytes + cache_bytes, "video");
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

        for (int y = 0; y < VID_HEIGHT; y++)
        {
            unsigned *dst     = (unsigned *)((byte *)pixels + y * pitch);
            byte     *src     = vid.buffer       + y * vid.rowbytes;
            // vid_palette_id is sized VID_WIDTH * VID_HEIGHT by construction;
            // stride is always VID_WIDTH, independent of any vid.rowbytes padding.
            byte     *pal_src = vid_palette_id   + y * VID_WIDTH;
            for (int x = 0; x < VID_WIDTH; x++)
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
    memset(vid_palette_id, 0, sizeof(vid_palette_id));

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

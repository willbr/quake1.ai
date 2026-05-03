// vid_sdl.c -- SDL3 video driver replacing vid_win.c
// Software framebuffer (8-bit palettized) -> SDL_Texture -> SDL_Renderer

#include <SDL3/SDL.h>
#include "quakedef.h"
#include "winquake.h"

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

unsigned    d_8to24table[256];
unsigned short d_8to16table[256]; // unused in software but declared in vid.h

void (*vid_menudrawfn)(void) = NULL;
void (*vid_menukeyfn)(int key) = NULL;

static SDL_Window   *sdl_window   = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture  *sdl_texture  = NULL;

#define VID_WIDTH  320
#define VID_HEIGHT 200

static byte vid_buffer[VID_WIDTH * VID_HEIGHT];
static byte con_buffer[VID_WIDTH * VID_HEIGHT];

// ---------------------------------------------------------------------------
// Palette helpers
// ---------------------------------------------------------------------------

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

void VID_SetPalette(unsigned char *palette)   { build_palette(palette); }
void VID_ShiftPalette(unsigned char *palette) { build_palette(palette); }

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------

void VID_Init(unsigned char *palette)
{
    // Create window — start at 2× scale so it's not tiny on modern screens
    sdl_window = SDL_CreateWindow("quake1.ai",
        VID_WIDTH * 2, VID_HEIGHT * 2,
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

    // Fill in viddef
    memset(&vid, 0, sizeof(vid));
    vid.width      = VID_WIDTH;
    vid.height     = VID_HEIGHT;
    vid.rowbytes   = VID_WIDTH;
    vid.aspect     = (float)VID_WIDTH / (float)VID_HEIGHT;
    vid.numpages   = 1;
    vid.colormap   = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer     = vid_buffer;
    vid.conbuffer  = con_buffer;
    vid.conwidth   = VID_WIDTH;
    vid.conheight  = VID_HEIGHT;
    vid.conrowbytes = VID_WIDTH;
    vid.maxwarpwidth  = VID_WIDTH;
    vid.maxwarpheight = VID_HEIGHT;
    vid.recalc_refdef = 1;

    build_palette(palette);

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
}

void VID_Shutdown(void)
{
    if (sdl_texture)  { SDL_DestroyTexture(sdl_texture);   sdl_texture  = NULL; }
    if (sdl_renderer) { SDL_DestroyRenderer(sdl_renderer); sdl_renderer = NULL; }
    if (sdl_window)   { SDL_DestroyWindow(sdl_window);     sdl_window   = NULL; }
}

// ---------------------------------------------------------------------------
// Frame update: expand 8-bit -> 32-bit, upload, present
// ---------------------------------------------------------------------------

static int vid_update_count = 0;
void VID_Update(vrect_t *rects)
{
    if (!sdl_texture) return;
    if (++vid_update_count == 1) fputs("VID_Update: first frame\n", stderr);

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
    SDL_RenderPresent(sdl_renderer);
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

SDL_Window *VID_GetWindow(void) { return sdl_window; }

// Loading disc overlays — not needed with SDL renderer
void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height)
    { (void)x; (void)y; (void)pbitmap; (void)width; (void)height; }
void D_EndDirectRect(int x, int y, int width, int height)
    { (void)x; (void)y; (void)width; (void)height; }

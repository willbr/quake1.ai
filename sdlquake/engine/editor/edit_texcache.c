// edit_texcache.c -- 8-bit Quake textures -> RGBA SDL_GPUTextures, sized down
// to a fixed THUMB_SIZE for the editor's visual palette grid.
//
// ImTextureID under the SDL_GPU3 backend is a raw `SDL_GPUTexture*`. We
// create one per unique world / pool texture name, downsample with the
// palette table at upload time, and reuse until the worldmodel changes.

#include "quakedef.h"
#include "edit_texcache.h"
#include "editor_internal.h"
#include "imgui_bridge.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <stdlib.h>
#include <string.h>

extern unsigned        d_8to24table[256];
extern SDL_GPUDevice  *VID_GetGPUDevice(void);

#define THUMB_SIZE   64           // 64x64 RGBA = 16KB per texture, fine

typedef struct cache_entry_s {
    char            name[16];     // matches texture_t.name layout
    SDL_GPUTexture *tex;          // owned
} cache_entry_t;

static cache_entry_t *s_entries = NULL;
static int            s_count   = 0;
static int            s_cap     = 0;
static void          *s_world   = (void *)(intptr_t)-1;

// One reused transfer buffer for thumbnail uploads — sized for one thumbnail
// at a time. Lazily created on first upload.
static SDL_GPUTransferBuffer *s_thumb_xfer      = NULL;
static int                    s_thumb_xfer_size = 0;

static void release_tex(SDL_GPUTexture *t) {
    SDL_GPUDevice *d = VID_GetGPUDevice();
    if (d && t) SDL_ReleaseGPUTexture(d, t);
}

static void invalidate_all(void)
{
    int i;
    for (i = 0; i < s_count; i++)
    {
        if (s_entries[i].tex)
        {
            release_tex(s_entries[i].tex);
            s_entries[i].tex = NULL;
        }
    }
    s_count = 0;
}

void Editor_TexCache_Shutdown(void)
{
    invalidate_all();
    if (s_entries) { free(s_entries); s_entries = NULL; }
    s_cap   = 0;
    s_world = (void *)(intptr_t)-1;
    if (s_thumb_xfer) {
        SDL_GPUDevice *d = VID_GetGPUDevice();
        if (d) SDL_ReleaseGPUTransferBuffer(d, s_thumb_xfer);
        s_thumb_xfer = NULL;
        s_thumb_xfer_size = 0;
    }
}

// Cache invalidates whenever cl.worldmodel changes, matching the
// world_tex_list lifetime in editor_ui.c.
static void check_world(void)
{
    if ((void *)cl.worldmodel != s_world)
    {
        invalidate_all();
        s_world = (void *)cl.worldmodel;
    }
}

static cache_entry_t *find_entry(const char *name)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (!Q_strcasecmp(s_entries[i].name, name))
            return &s_entries[i];
    return NULL;
}

// Find a texture by name: worldmodel first, then the editor texture pool.
static texture_t *find_world_tex(const char *name)
{
    int i;
    if (cl.worldmodel && cl.worldmodel->textures)
    {
        for (i = 0; i < cl.worldmodel->numtextures; i++)
        {
            texture_t *t = cl.worldmodel->textures[i];
            if (t && !Q_strcasecmp(t->name, name)) return t;
        }
    }
    {
        int pool_count;
        texture_t **pool = Editor_TexPool_Get(&pool_count);
        for (i = 0; i < pool_count; i++)
        {
            texture_t *t = pool[i];
            if (t && !Q_strcasecmp(t->name, name)) return t;
        }
    }
    return NULL;
}

// Box-downsample 8-bit indexed `src` (sw x sh) into RGBA `dst` (THUMB_SIZE
// per side) by averaging palette colors over the src region that maps to
// each dst pixel. Output is RGBA8 (R first).
static void downsample_to_rgba(const byte *src, int sw, int sh, byte *dst)
{
    int dx, dy;
    if (sw == 0 || sh == 0) { memset(dst, 0, THUMB_SIZE * THUMB_SIZE * 4); return; }
    for (dy = 0; dy < THUMB_SIZE; dy++)
    {
        int y0 = (dy * sh) / THUMB_SIZE;
        int y1 = ((dy + 1) * sh) / THUMB_SIZE;
        if (y1 <= y0) y1 = y0 + 1;
        for (dx = 0; dx < THUMB_SIZE; dx++)
        {
            int x0 = (dx * sw) / THUMB_SIZE;
            int x1 = ((dx + 1) * sw) / THUMB_SIZE;
            int n = 0, r = 0, g = 0, b = 0;
            int x, y;
            if (x1 <= x0) x1 = x0 + 1;
            for (y = y0; y < y1 && y < sh; y++)
            {
                for (x = x0; x < x1 && x < sw; x++)
                {
                    unsigned c = d_8to24table[src[y * sw + x]];
                    r += (int)( c        & 0xFF);
                    g += (int)((c >>  8) & 0xFF);
                    b += (int)((c >> 16) & 0xFF);
                    n++;
                }
            }
            if (n == 0) n = 1;
            dst[(dy * THUMB_SIZE + dx) * 4 + 0] = (byte)(r / n);
            dst[(dy * THUMB_SIZE + dx) * 4 + 1] = (byte)(g / n);
            dst[(dy * THUMB_SIZE + dx) * 4 + 2] = (byte)(b / n);
            dst[(dy * THUMB_SIZE + dx) * 4 + 3] = 255;
        }
    }
}

// Upload a downsampled thumbnail to a freshly-allocated SDL_GPUTexture.
// Returns NULL on any failure path.
static SDL_GPUTexture *upload_thumbnail(SDL_GPUDevice *dev, texture_t *tx)
{
    static byte rgba[THUMB_SIZE * THUMB_SIZE * 4];
    const byte *src = (const byte *)tx + tx->offsets[0];
    downsample_to_rgba(src, (int)tx->width, (int)tx->height, rgba);

    SDL_GPUTextureCreateInfo info = {0};
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = THUMB_SIZE;
    info.height               = THUMB_SIZE;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;
    info.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(dev, &info);
    if (!tex) return NULL;

    // Lazily (re)allocate the transfer buffer.
    int needed = THUMB_SIZE * THUMB_SIZE * 4;
    if (!s_thumb_xfer || s_thumb_xfer_size < needed) {
        if (s_thumb_xfer) SDL_ReleaseGPUTransferBuffer(dev, s_thumb_xfer);
        SDL_GPUTransferBufferCreateInfo xinfo = {0};
        xinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        xinfo.size  = (Uint32)needed;
        s_thumb_xfer = SDL_CreateGPUTransferBuffer(dev, &xinfo);
        s_thumb_xfer_size = needed;
    }
    if (!s_thumb_xfer) { SDL_ReleaseGPUTexture(dev, tex); return NULL; }

    void *mapped = SDL_MapGPUTransferBuffer(dev, s_thumb_xfer, true);
    if (!mapped) { SDL_ReleaseGPUTexture(dev, tex); return NULL; }
    memcpy(mapped, rgba, (size_t)needed);
    SDL_UnmapGPUTransferBuffer(dev, s_thumb_xfer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) { SDL_ReleaseGPUTexture(dev, tex); return NULL; }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src_info = {0};
    src_info.transfer_buffer = s_thumb_xfer;
    src_info.pixels_per_row  = THUMB_SIZE;
    src_info.rows_per_layer  = THUMB_SIZE;
    SDL_GPUTextureRegion dst_region = {0};
    dst_region.texture = tex;
    dst_region.w = THUMB_SIZE; dst_region.h = THUMB_SIZE; dst_region.d = 1;
    SDL_UploadToGPUTexture(cp, &src_info, &dst_region, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    return tex;
}

IG_TextureID Editor_GetTextureThumbnail(const char *name)
{
    if (!name || !name[0]) return NULL;
    check_world();

    cache_entry_t *e = find_entry(name);
    if (e && e->tex) return (IG_TextureID)e->tex;

    SDL_GPUDevice *dev = VID_GetGPUDevice();
    if (!dev) return NULL;
    texture_t *tx = find_world_tex(name);
    if (!tx) return NULL;

    SDL_GPUTexture *tex = upload_thumbnail(dev, tx);
    if (!tex) return NULL;

    if (s_count >= s_cap)
    {
        int new_cap = s_cap ? s_cap * 2 : 64;
        s_entries = (cache_entry_t *)realloc(s_entries,
            new_cap * sizeof(cache_entry_t));
        s_cap = new_cap;
    }
    e = &s_entries[s_count++];
    Q_strncpy(e->name, name, (int)sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->tex = tex;
    return (IG_TextureID)tex;
}

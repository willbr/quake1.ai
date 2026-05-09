// edit_texcache.h -- on-demand RGBA thumbnail cache for the editor's
// visual texture palette. Built on top of cl.worldmodel->textures (8-bit
// indexed mip-0 pixels) -> d_8to24table -> downsample to a fixed
// thumbnail size -> SDL_CreateTexture (STATIC).
//
// Cache lifetime is keyed on the cl.worldmodel pointer: a map swap
// invalidates everything and forces lazy re-upload on next request.
// Caller (the inspector / browser window) doesn't manage texture
// lifetimes — call Editor_TexCache_Shutdown on engine teardown to be
// tidy, but it'll work without it.

#ifndef EDIT_TEXCACHE_H
#define EDIT_TEXCACHE_H

#include "imgui_bridge.h"   // IG_TextureID

// Look up (or lazy-build) a thumbnail for a worldmodel texture by its
// basename. Returns NULL if the renderer isn't ready, the worldmodel
// has no such texture, or the upload failed. Returned pointer is owned
// by the cache; never freed by the caller.
IG_TextureID Editor_GetTextureThumbnail(const char *name);

// Drop every cached SDL_Texture. Safe to call repeatedly. Called
// implicitly from Editor_GetTextureThumbnail when cl.worldmodel changes.
void Editor_TexCache_Shutdown(void);

#endif // EDIT_TEXCACHE_H

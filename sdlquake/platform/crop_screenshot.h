#ifndef SDLQUAKE_CROP_SCREENSHOT_H
#define SDLQUAKE_CROP_SCREENSHOT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enter modal rect-selection. Snapshots the current framebuffer,
   pauses simulation (cl.paused = true), and shows the OS cursor
   (handled indirectly by the input layer via Crop_Active()).
   `out_path` is the destination filename the eventual commit will
   write to. No-op if `out_path` is NULL/empty or if a session is
   already active. */
void Crop_Enter(const char *out_path);

/* Restore cl.paused, free internal buffers, clear active. Safe to
   call when not active (no-op). */
void Crop_Exit(void);

/* 1 while modal selection is active, 0 otherwise. */
int  Crop_Active(void);

/* Stub for now — Task 7 fills this in. Returns 1 if the event was
   consumed (don't dispatch further). */
int  Crop_HandleEvent(const SDL_Event *ev);

/* Stub for now — Task 6 fills this in. Composites dim + border
   overlay onto the ARGB present buffer. */
void Crop_PresentOverlay(unsigned *argb, int pitch_bytes, int w, int h);

/* Returns the current rect endpoints in framebuffer-local coords,
   normalised so (x0,y0) is top-left and (x1,y1) is bottom-right.
   Values are not clamped to framebuffer bounds — callers should
   clamp before use. Any of the out-params may be NULL. */
void Crop_GetRect(int *x0, int *y0, int *x1, int *y1);

/* Returns the cached frozen 8-bit framebuffer + matching palette-id
   plane so VID_Update can expand them instead of vid.buffer. Returns
   NULL if not active. */
const unsigned char *Crop_FrozenBuffer (int *w, int *h);
const unsigned char *Crop_FrozenPalette(void);

#ifdef __cplusplus
}
#endif

#endif

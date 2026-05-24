#ifndef SDLQUAKE_CROP_SCREENSHOT_H
#define SDLQUAKE_CROP_SCREENSHOT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Request modal rect-selection. Hides the console/menu, pauses
   simulation (cl.paused = true), and defers the actual framebuffer
   snapshot to the next VID_Update via Crop_FrameStart — that way
   the captured frame reflects a render without the console still
   pulled down. `out_path` is the destination filename the eventual
   commit will write to. No-op if `out_path` is NULL/empty or if a
   session is already active or pending. */
void Crop_Enter(const char *out_path);

/* Restore cl.paused, free internal buffers, clear active. Safe to
   call when not active (no-op). */
void Crop_Exit(void);

/* 1 once Crop_FrameStart has taken the snapshot; 0 otherwise. The
   one-frame window between Crop_Enter and the first Crop_FrameStart
   reports 0 — the modal isn't yet capturing input. */
int  Crop_Active(void);

/* 1 during the one-frame window after Crop_Enter and before
   Crop_FrameStart performs the snapshot. Used by chrome drawers
   (e.g. SCR_DrawPause) to skip themselves so the captured frame
   doesn't include them. */
int  Crop_Pending(void);

/* Called once per frame from VID_Update. Performs the deferred
   framebuffer snapshot one frame after Crop_Enter so the captured
   pixels reflect a render with the console/menu already dismissed. */
void Crop_FrameStart(void);

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

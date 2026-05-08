// vid_palette.h -- Multi-palette state for Phase 6 Doom asset rendering.
//
// vid_palette_id rides parallel to vid.buffer (one byte per pixel,
// VID_WIDTH * VID_HEIGHT). VID_Update expands each pixel as:
//   argb[i] = vid_lut[vid_palette_id[i]][vid.buffer[i]]
// where vid_lut is the internal multi-palette LUT defined in vid_sdl.c.
// vid_lut[VID_PAL_QUAKE] is mirrored into d_8to24table[] so engine code
// reading the legacy symbol still gets the Quake palette.
//
// Slot 0 = Quake palette (default; built from gfx/palette.lmp).
// Slot 1 = Doom palette  (built from gfx/palette_doom.lmp).
// Slot 2 = reserved Wolf3D palette (not wired yet).
//
// Tagging rules:
//   - Renderer paths that draw with a non-Quake palette (currently only the
//     screen-space viewmodel blit, for Doom guns) tag pixels with the matching
//     palette_id at write time.
//   - Quake-palette writers that can paint over previously-tagged pixels MUST
//     reset palette_id back to 0 at each pixel they write. The Doom 2D
//     viewmodel sits inside r_refdef.vrect, so anything that draws there
//     (crosshair, centerprint, console messages over the gun, particles)
//     needs the reset. R_DrawParticles avoids the problem by running before
//     the 2D viewmodel pass; draw.c HUD writers (Draw_Character et al.)
//     reset palette_id inline via VID_TAG_QUAKE_AT.

#ifndef SDLQ_VID_PALETTE_H
#define SDLQ_VID_PALETTE_H

// Callers must include quakedef.h (for 'byte') before this header.

#define VID_NUM_PALETTES 3
#define VID_PAL_QUAKE    0
#define VID_PAL_DOOM     1
#define VID_PAL_WOLF3D   2

// Defined in vid_sdl.c. Sized to VID_WIDTH * VID_HEIGHT.
extern byte vid_palette_id[];

#endif

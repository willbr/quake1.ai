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
// Invariant: every renderer path that writes a non-zero palette_id must
// do so AFTER any Quake-palette writes that could overlap. Currently only
// the screen-space viewmodel blit writes a non-zero palette_id, and it
// sits inside r_refdef.vrect above the status bar, so the invariant
// holds trivially.

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

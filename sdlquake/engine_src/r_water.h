// r_water.h — depth-graded sheen for liquid (turbulent) surfaces in the
// software renderer. Mirrors r_fog.c's palette-LUT approach: a 64-row LUT tints
// liquids toward a sheen color, the row chosen per sub-span from 1/z (distance
// ~= grazing angle for horizontal water => fresnel-like sheen), optionally
// offset per-pixel by the warp for ripple glints. Surface tint only — NOT
// see-through (the world's zero-overdraw VSD leaves nothing behind liquids to
// blend against).
#ifndef R_WATER_H
#define R_WATER_H

// Consumers include quakedef.h first; we forward-declare cvar_s like r_fog.h.
struct cvar_s;
extern struct cvar_s r_water_sheen;        // master strength 0..1, 0 = off
extern struct cvar_s r_water_sheen_gain;   // highlight = texel colour * gain (>=1)
extern struct cvar_s r_water_sheen_dist;   // distance falloff (like fog density)
extern struct cvar_s r_water_ripple;       // ripple-relief strength 0..1, 0 = off

// 64-row x 256 palette LUT: row N maps each texel to the nearest palette index of
// lerp(texel, texel*gain, (N/63)*strength) -- the texel's own colour brightened,
// so the sheen reflects the water texture. Row 0 is identity (no sheen).
extern unsigned char r_water_sheenmap[64*256];

extern int r_water_sheen_active;   // sheen on AND not in r_drawflat debug mode
extern int r_water_ripple_scale;   // precomputed per-frame ripple -> row-offset scale

// Registers cvars + builds the initial LUT. Call once at engine startup.
void R_Water_Init   (void);
// Rebuilds the LUT if cvars changed; refreshes active flag + ripple scale.
// Cheap when nothing changed. Call each frame.
void R_Water_Update (void);

// Returns the sheen base row (floor, 0..62) for the given Euclidean distance
// from the eye to the liquid point, plus a 0..3 quantized fractional threshold
// for 2x2 Bayer dithering toward base+1 (smooths the gradient -- no hard
// row-step lines). Distance (not camera-space depth) keeps the gradient
// rotation-invariant, so it doesn't swim as you turn the view. Near = 0
// (clear), far = 63 (full sheen). Assumes r_water_sheen_active; caller checks.
void R_Water_GetRows (float dist, int *base_lo, int *thresh4);

#endif

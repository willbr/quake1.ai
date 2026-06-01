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
extern struct cvar_s r_water_sheen_red;
extern struct cvar_s r_water_sheen_green;
extern struct cvar_s r_water_sheen_blue;
extern struct cvar_s r_water_sheen_dist;   // distance falloff (like fog density)
extern struct cvar_s r_water_ripple;       // ripple-relief strength 0..1, 0 = off

// 64-row x 256 palette LUT: row N maps each index to the nearest palette index
// of lerp(color, sheen, (N/63)*strength). Row 0 is identity (no tint).
extern unsigned char r_water_sheenmap[64*256];

extern int r_water_sheen_active;   // sheen on AND not in r_drawflat debug mode
extern int r_water_ripple_scale;   // precomputed per-frame ripple -> row-offset scale

// Registers cvars + builds the initial LUT. Call once at engine startup.
void R_Water_Init   (void);
// Rebuilds the LUT if cvars changed; refreshes active flag + ripple scale.
// Cheap when nothing changed. Call each frame.
void R_Water_Update (void);

// Maps 1/z to a sheen base row 0..63 (near = 0 clear, far = 63 full sheen).
// Assumes r_water_sheen_active; caller checks.
int  R_Water_RowFromZi (float zi);

#endif

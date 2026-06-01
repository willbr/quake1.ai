// r_fog.h — distance fog for software renderer.
#ifndef R_FOG_H
#define R_FOG_H

// Consumers include quakedef.h first (or include this from a file that does);
// we don't pull in cvar.h directly because it depends on engine typedefs.
struct cvar_s;
extern struct cvar_s r_fog_density;
extern struct cvar_s r_fog_red;
extern struct cvar_s r_fog_green;
extern struct cvar_s r_fog_blue;
extern struct cvar_s r_water_fog_density;

extern int    r_fog_active;        // nonzero when density > 0
extern int    r_fog_color_index;   // palette index closest to fog color (used by sky fill)
extern unsigned char *r_fog_row;   // current span's fog LUT row (set by rasterizers)

// Builds/refreshes the fog colormap LUT and sets r_fog_active based on cvars.
// Cheap when nothing changed (just compares cached values).
void R_Fog_Update (void);

// Called once at engine startup. Registers cvars and builds initial state.
void R_Fog_Init (void);

// Called from R_NewMap. Resets cvars to defaults, then parses worldspawn "fog" key.
void R_Fog_NewMap (void);

// Returns pointer to the 256-byte LUT row corresponding to the given 1/z value.
// Assumes r_fog_active is nonzero; caller must check.
unsigned char *R_Fog_RowFromZi (float zi);

// Returns the two adjacent fog LUT rows (floor and ceil of the fog level) plus
// a 0..3 quantized fractional threshold for 2x2 Bayer dithering. Caller picks
// row_hi when bayer[y&1][x&1] < *thresh4, row_lo otherwise. Assumes r_fog_active.
// (sx,sy) is the pixel's screen position; it converts camera-space 1/zi into true
// (range-based) distance so the fog gradient doesn't swim as the view rotates.
void R_Fog_GetRows (float zi, int sx, int sy, unsigned char **row_lo,
		    unsigned char **row_hi, int *thresh4);

// 2x2 Bayer threshold matrix, flat-indexed by ((y&1)<<1) | (x&1).
// Compare against thresh4 from R_Fog_GetRows.
extern const unsigned char r_fog_bayer2x2[4];

#endif

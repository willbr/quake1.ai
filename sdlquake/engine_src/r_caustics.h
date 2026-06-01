// r_caustics.h — underwater caustics for the software renderer. While the view
// is inside a water leaf, solid surfaces (D_DrawSpans8) get an animated caustic
// brightness pattern projected through their (s,t) texture coords — so it stays
// world-locked and rides the underwater warp instead of swimming. Two scrolling
// samples of a baked tileable ridge texture are multiplied into a 0..63 level
// that picks a row in a brighten-toward-caustic-color LUT (row 0 = unchanged).
#ifndef R_CAUSTICS_H
#define R_CAUSTICS_H

#define CAUSTIC_SIZE 128
#define CAUSTIC_MASK (CAUSTIC_SIZE - 1)

struct cvar_s;
extern struct cvar_s r_caustics;            // master toggle (0 = off)
extern struct cvar_s r_caustics_intensity;  // 0..1 highlight strength (bakes into LUT)
extern struct cvar_s r_caustics_scale;      // cell size: (s,t) >> (16+scale)
extern struct cvar_s r_caustics_speed;      // scroll/animation rate

extern unsigned char r_caustic_tex[CAUSTIC_SIZE * CAUSTIC_SIZE];  // baked ridges, 0..63
extern unsigned char r_caustic_map[64 * 256];                    // brightness LUT

extern int r_caustics_active;   // view in a water leaf AND r_caustics on
extern int r_caustic_shift;     // shift applied to s,t to index the texture
extern int r_caustic_ox1, r_caustic_oy1;   // layer-1 scroll offsets (texture texels)
extern int r_caustic_ox2, r_caustic_oy2;   // layer-2 scroll offsets

void R_Caustics_Init   (void);
// Per frame (after R_SetupFrame sets r_viewleaf): gate, scroll offsets,
// LUT rebuild on cvar change. Cheap when nothing changed.
void R_Caustics_Update (void);

#endif

// r_debugdraw.h -- shared software-renderer primitives for engine debug overlays.
//
// Used by r_bbox.c (entity bbox overlay) and r_paths.c (patrol path overlay).
// Each consumer calls RDD_BeginFrame(density) once per frame; if RDD_Visible()
// returns 0 the consumer should early-out. After that, the projection and
// drawing helpers honour the cached Bayer threshold.

#ifndef SDLQUAKE_R_DEBUGDRAW_H
#define SDLQUAKE_R_DEBUGDRAW_H

// Consumers must include "quakedef.h" before this header so that vec3_t
// and other engine types are already defined.

#define RDD_NEAR_CLIP 0.01f

// Per-frame state. Call once before any other RDD_ function.
// density is clamped to [0,1]; rounded threshold is stored internally.
void RDD_BeginFrame(float density_0_1);

// Returns non-zero if the rounded Bayer threshold is > 0. When zero, skip drawing.
int  RDD_Visible(void);

// World -> view space (right, up, forward).
void RDD_ToView(const vec3_t world, vec3_t out_view);

// View-space -> screen pixel coords. Returns 0 if view[2] < RDD_NEAR_CLIP
// (caller must clip first), 1 otherwise.
int  RDD_Project(const vec3_t view, float *out_sx, float *out_sy);

// Near-plane clip + project + 2D draw for a line whose endpoints are already
// in view space. r_bbox.c passes pre-cached view[a]/view[b]; r_paths.c
// transforms world->view first via RDD_ToView. Returns nothing — fully clipped
// lines (both endpoints behind near plane) are silently dropped.
void RDD_DrawLine3D_View(const vec3_t view_a, const vec3_t view_b, int color);

// Same as RDD_DrawLine3D_View, but when ztest is non-zero each pixel is gated
// on the software renderer's d_pzbuffer — pixels behind world geometry are
// suppressed. Bayer dither (RDD_BeginFrame's density) still applies. ztest=0
// matches the original draw-through-everything behaviour.
void RDD_DrawLine3D_ViewZ(const vec3_t view_a, const vec3_t view_b,
                          int color, int ztest);

// Draw a Bayer-dithered line (clipped to r_refdef.vrect) between two
// already-projected screen-space points.
void RDD_DrawLine2D(int x0, int y0, int x1, int y1, int color);

// Plot a single solid (non-dithered) pixel at the given screen coords,
// clipped to r_refdef.vrect. Use for vertex/corner markers so they read
// clearly even at low density.
void RDD_DrawSolidPixel(int x, int y, int color);

#endif

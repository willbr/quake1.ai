// editor_internal.h -- shared declarations between editor *.c files. Not
// installed; only included by code under sdlquake/engine/editor/.

#ifndef EDITOR_INTERNAL_H
#define EDITOR_INTERNAL_H

// Caller must have already included "quakedef.h" (which defines vec3_t, byte,
// vec_t, and the Vector* macros). We don't include it here because it's a
// non-pragma-once header that redefines qboolean if pulled in twice.
struct edit_brush_s;

// Color palette indices used by render_wire.c and gizmo.c. Tuned to the
// vanilla Quake palette (which has no pure green — 243 is the closest
// distinct hue).
#define EDIT_COLOR_AXIS_X       251     // bright red    (255,  0,  0)
#define EDIT_COLOR_AXIS_Y       243     // yellow-green  (231,227, 87)
#define EDIT_COLOR_AXIS_Z       244     // light blue    (127,191,255)
#define EDIT_COLOR_AXIS_HOT     254     // white         (255,255,255) -- hovered/dragging axis

// render_wire.c
int  Editor_ProjectWorld (const vec3_t world, float *out_sx, float *out_sy);
void Editor_ScreenToRay  (float sx, float sy, vec3_t out_origin, vec3_t out_dir);
void Editor_DrawLine3D   (const vec3_t a, const vec3_t b, byte color);
int  Editor_PickAt       (float sx, float sy, int *out_ent, int *out_brush);

// render_flat.c
void Editor_FlatDrawBrush(const struct edit_brush_s *b);

// gizmo.c
void Editor_GizmoDraw       (void);
int  Editor_GizmoMouseDown  (float sx, float sy);   // 1 if axis grabbed
void Editor_GizmoMouseMove  (float sx, float sy);
void Editor_GizmoMouseUp    (void);
int  Editor_GizmoIsActive   (void);

// Find brush centroid (compiled-face vertex average). Bbox center is used if
// the brush has no faces.
void Editor_BrushCentroid   (const struct edit_brush_s *b, vec3_t out);

#endif // EDITOR_INTERNAL_H

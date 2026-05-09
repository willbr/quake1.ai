// editor_internal.h -- shared declarations between editor *.c files. Not
// installed; only included by code under sdlquake/engine/editor/.

#ifndef EDITOR_INTERNAL_H
#define EDITOR_INTERNAL_H

// Caller must have already included "quakedef.h" (which defines vec3_t, byte,
// vec_t, and the Vector* macros). We don't include it here because it's a
// non-pragma-once header that redefines qboolean if pulled in twice.
struct edit_brush_s;
struct edit_plane_s;

// Color palette indices used by render_wire.c and gizmo.c. Tuned to the
// vanilla Quake palette (which has no pure green — 243 is the closest
// distinct hue).
#define EDIT_COLOR_AXIS_X       251     // bright red    (255,  0,  0)
#define EDIT_COLOR_AXIS_Y       243     // yellow-green  (231,227, 87)
#define EDIT_COLOR_AXIS_Z       244     // light blue    (127,191,255)
#define EDIT_COLOR_AXIS_HOT     254     // white         (255,255,255) -- hovered/dragging axis

// Category colors for point-entity wire bboxes — picked for separation in
// the Quake palette since this is what the user reads to distinguish
// trigger volumes from light markers from spawn points at a glance.
#define EDIT_COLOR_DEFAULT      15      // off-white         (235,235,235)
#define EDIT_COLOR_TRIGGER      251     // bright red        (255,  0,  0) — gameplay volumes
#define EDIT_COLOR_LIGHT        109     // orange-gold       (223,171, 39) — light_*
#define EDIT_COLOR_SPAWN        244     // light blue        (127,191,255) — info_player_*
#define EDIT_COLOR_ITEM         144     // pink-purple       (187,115,159) — item_/weapon_/ammo_
                                        // (Quake palette has no usable green
                                        // — pink-purple is the next-most
                                        // distinct hue from the others.)
#define EDIT_COLOR_MONSTER      232     // dark orange       (183, 51, 15) — monster_*
#define EDIT_COLOR_FUNC         246     // pale cyan         (215,255,255) — func_*
#define EDIT_COLOR_SOUND         47     // lavender          (139,139,203) — ambient_*
#define EDIT_COLOR_PATH         235     // bright orange     (219,127, 59) — path_*
#define EDIT_COLOR_MISC         252     // pale cream        (255,243,147) — misc_*
#define EDIT_COLOR_INFO          13     // light grey        (203,203,203) — info_null/notnull/command

// Category index for filtering / colour-coding. Order is stable; UI
// checkboxes index into a parallel filter array.
enum {
    EDIT_CAT_OTHER = 0,     // worldspawn etc. (anything not below)
    EDIT_CAT_TRIGGER,       // trigger_*
    EDIT_CAT_LIGHT,         // light, light_*
    EDIT_CAT_SPAWN,         // info_player_*, info_teleport_destination, info_intermission
    EDIT_CAT_ITEM,          // item_*, weapon_*, ammo_*
    EDIT_CAT_MONSTER,       // monster_*
    EDIT_CAT_FUNC,          // func_* (doors, plats, buttons, walls, ...)
    EDIT_CAT_SOUND,         // ambient_*
    EDIT_CAT_PATH,          // path_corner, path_*
    EDIT_CAT_MISC,          // misc_*
    EDIT_CAT_INFO,          // info_null, info_notnull, info_command, etc.
    EDIT_CAT_COUNT
};

// render_wire.c
int  Editor_ProjectWorld (const vec3_t world, float *out_sx, float *out_sy);
void Editor_ScreenToRay  (float sx, float sy, vec3_t out_origin, vec3_t out_dir);
void Editor_DrawLine3D   (const vec3_t a, const vec3_t b, byte color);
// Same projection + clip, but bypasses the z-buffer so the line draws over
// whatever's already on screen. Used by the gizmo so axis arrows stay
// visible (and clickable) even when behind a brush.
void Editor_DrawLine3DOver(const vec3_t a, const vec3_t b, byte color);
int  Editor_PickAt       (float sx, float sy, int *out_ent, int *out_brush);
// Map a classname to its alias model path (if known). Returns NULL for
// classes without a registered model. map_io.c uses this when binding
// SV_MakeStatic'd entities to their cl_static_entities[] counterpart.
const char *Editor_ClassnameToModel(const char *classname);

// render_flat.c
void Editor_FlatDrawBrush(const struct edit_brush_s *b);

// render_tex.c
void Editor_TexDrawBrush (const struct edit_brush_s *b);

// Compute scaled+rotated s/t world axes (and shifts) for a plane, matching
// qbsp's QuakeEd-style texinfo. Public so Brush_Translate can keep textures
// pinned to the brush ("texture lock") by adjusting plane shifts.
void Editor_PlaneUVAxes(const struct edit_plane_s *p,
                        vec3_t out_s, vec3_t out_t,
                        float *out_s_shift, float *out_t_shift);

// Unrotated base s/t axes for a plane (the QuakeEd "axial" axes chosen by
// dominant-component of the plane normal). No rotation, no scale, unit
// length. Used by "Fit to face" to project verts onto the canonical UV
// frame before solving for shift/scale.
void Editor_PlaneBaseAxes(const struct edit_plane_s *p,
                          vec3_t out_s, vec3_t out_t);

// Resolve the world texture currently mapped onto a plane's `texname`. NULL
// if no worldmodel, the name doesn't match, or texname is empty. Used by
// the inspector's "Fit to face" math, which needs width/height.
struct texture_s *Editor_PlaneTexture(const struct edit_plane_s *p);

// gizmo.c
void Editor_GizmoDraw       (void);
int  Editor_GizmoMouseDown  (float sx, float sy);   // 1 if axis grabbed
void Editor_GizmoMouseMove  (float sx, float sy);
void Editor_GizmoMouseUp    (void);
int  Editor_GizmoIsActive   (void);

// Find brush centroid (compiled-face vertex average). Bbox center is used if
// the brush has no faces.
void Editor_BrushCentroid   (const struct edit_brush_s *b, vec3_t out);

// editor.c — center the free-fly camera on the (e_idx, b_idx) item and back
// off enough to frame it. Uses brush[b_idx] when b_idx >= 0, else the
// entity's origin. Keeps the current view angles, so direction is preserved.
// b_idx = -1 means "the entity itself" (point entity ref).
void Editor_FrameItem(int e_idx, int b_idx);

// render_wire.c — append fake entity_t records into cl_visedicts so the
// engine renders editor preview models for any point entity that has a
// modelpath but no live counterpart yet. Called from Editor_PreRender,
// inside R_RenderView_ between CL_RelinkEntities and R_EdgeDrawing.
void Editor_PushPreviewEntities(void);

// render_wire.c — classify a classname into one of EDIT_CAT_*. Used for
// bbox color and filter checkboxes.
int  Editor_EntityCategory(const struct edit_entity_s *e);

// editor_ui.c — 1 if the user filtered out this entity's category in the
// Brushes panel checkboxes, or it's outside the camera frustum / behind a
// wall when "visible only" is on. Render / pick / list paths skip filtered
// entities.
int  Editor_EntityHidden  (int e_idx);

// editor_ui.c — sync per-session UI state from cvars on each editor open.
// Currently: difficulty preview filter ← `skill` / `deathmatch` cvars so
// the dropdown matches what the running game would actually spawn.
void Editor_UI_OnOpen     (void);

// render_wire.c — 1 if the entity is in front of the camera, projects into
// the viewport, and isn't occluded by the world BSP. Used by the "visible
// only" filter.
int  Editor_EntityInView  (int e_idx);

// render_wire.c — resolve a single anchor point for an entity. Tries (in
// order): the .map "origin" key, the live edict's absmin/absmax center
// (for BSP-loaded brush entities like func_bossgate which have no .map
// brushes and no origin key but a valid v.absmin/v.absmax), and the
// SV_MakeStatic'd entity's origin. Returns 0 if no resolvable position
// exists (e.g. an empty func_group with no brushes / no kv / no edict).
int  Editor_EntityAnchor  (const struct edit_entity_s *e, vec3_t out);

#endif // EDITOR_INTERNAL_H

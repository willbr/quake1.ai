// edit_scene.h -- in-memory representation of a .map file, plus brush compile.
//
// Brushes are convex polyhedra defined by half-space planes. brush_compile.c
// turns each plane into a clipped convex polygon (the face), shared with the
// renderer and gizmo.
//
// All allocations are malloc/realloc; freed wholesale in Scene_Clear. No hunk.

#ifndef EDIT_SCENE_H
#define EDIT_SCENE_H

#include "mathlib.h" // vec3_t

#define EDIT_MAX_PLANES_PER_BRUSH   64
#define EDIT_MAX_VERTS_PER_FACE     32
#define EDIT_TEX_NAME_LEN           32
#define EDIT_KEY_LEN                64
#define EDIT_VAL_LEN                256

// One half-space in a brush definition. Three points + texture metadata,
// matching the .map plane line.
typedef struct edit_plane_s {
    vec3_t      points[3];          // three points defining the plane
    char        texname[EDIT_TEX_NAME_LEN];
    float       s_shift, t_shift;
    float       rotation;
    float       s_scale, t_scale;
    // computed:
    vec3_t      normal;
    float       dist;
} edit_plane_t;

// One face of a compiled brush: convex polygon clipped from the base winding.
typedef struct edit_face_s {
    int         plane_idx;          // index into edit_brush_t.planes
    int         numverts;
    vec3_t      verts[EDIT_MAX_VERTS_PER_FACE];
} edit_face_t;

// One brush: list of planes (immutable after parse, until edited) + compiled
// faces (rebuilt on every edit) + bbox.
typedef struct edit_brush_s {
    int         numplanes;
    edit_plane_t planes[EDIT_MAX_PLANES_PER_BRUSH];

    int         numfaces;
    edit_face_t *faces;             // malloc'd, length numfaces

    vec3_t      mins, maxs;         // bbox over all face vertices
    int         valid;              // 0 if compile produced no faces
} edit_brush_t;

// One key/value pair on an entity. .map entities have ordered key/value lists.
typedef struct edit_kv_s {
    char        key[EDIT_KEY_LEN];
    char        value[EDIT_VAL_LEN];
} edit_kv_t;

// One .map entity: key/value list + optional brush list (worldspawn or brush
// entities like func_door have brushes; point entities like info_player_start
// don't).
typedef struct edit_entity_s {
    int         numkv;
    edit_kv_t  *kv;                 // malloc'd

    int         numbrushes;
    edit_brush_t *brushes;          // malloc'd

    // Indices into kv where common keys live, -1 if absent. Recomputed on
    // every kv mutation.
    int         classname_idx;
    int         origin_idx;
} edit_entity_t;

// The editor scene: entity list, selection, source filename for save/revert.
typedef struct edit_scene_s {
    int             numentities;
    edit_entity_t  *entities;       // malloc'd

    // Selection. -1 = nothing selected.
    int             sel_entity;
    int             sel_brush;      // index into entities[sel_entity].brushes

    // Round-trip filename. Set by Scene_Load. Empty until a .map has loaded.
    char            filename[256];  // absolute path on disk
    char            mapname[64];    // bare name, used for "map <name>" restart
} edit_scene_t;

extern edit_scene_t edit_scene;

// Lifecycle.
void  Scene_Init    (void);
void  Scene_Clear   (void);             // free everything; reset to empty
void  Scene_Shutdown(void);

// Load / save. Both return 1 on success.
int   Scene_Load    (const char *path); // parses .map, replaces scene
int   Scene_Save    (const char *path); // writes .map text
int   Scene_Revert  (void);             // re-parse current filename

// Brush compile (planes -> windings). Called whenever a brush mutates.
void  Brush_Compile (edit_brush_t *b);
void  Brush_FreeFaces(edit_brush_t *b);

// Translate every plane of a brush by `delta`.
void  Brush_Translate(edit_brush_t *b, const vec3_t delta);

// Selection helpers.
edit_brush_t *Scene_GetSelectedBrush(void);
edit_entity_t *Scene_GetSelectedEntity(void);

// Iterate every brush with its owning entity. cb returns 0 to stop.
typedef int (*Scene_BrushIter_fn)(edit_entity_t *e, int e_idx,
                                  edit_brush_t *b, int b_idx, void *user);
void  Scene_ForEachBrush(Scene_BrushIter_fn cb, void *user);

#endif // EDIT_SCENE_H

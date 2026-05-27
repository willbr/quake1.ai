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

// Forward decl so we can hold a live server edict pointer per entity without
// having to drag in progs.h / server.h.
struct edict_s;
// Forward decl for cl_static_entities[] entries — flame torches and similar
// SV_MakeStatic'd ents live here, not in sv.edicts.
struct entity_s;

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

    // 0 = added by the editor and not yet realised as a server edict.
    // 1 = either parsed from the .map at load time (so already spawned by
    //     the engine's normal load-from-file flow) or already flushed by
    //     Editor_FlushPendingEntities. Not persisted to disk.
    int         spawned;

    // 1 = synthesised on-the-fly to give the user a selection handle on a
    //     runtime-spawned edict (rocket, spike, gib, dropped backpack, …)
    //     that has no .map source. classname + origin are populated from
    //     the live edict; the entry is excluded from .map save and dropped
    //     when the live edict gets ED_Free'd.
    int         transient;

    // Server edict that mirrors this entity, or NULL. Set by the spawn
    // flush, or by the post-load matcher in map_io.c. Gizmo drags + kv
    // edits use this to push live state into the running game.
    struct edict_s *live_ent;

    // cl_static_entities[] entry that mirrors this entity, or NULL. Set by
    // the post-load matcher in map_io.c for ents that called SV_MakeStatic
    // (flame torches, etc) — those have no live edict because makestatic
    // ED_Free'd it, so the *only* runtime presence is in the client static
    // list. Gizmo drags push the new origin here so the model visibly moves.
    struct entity_s *live_static;

    // Snapshot of live_ent->v.classname captured at bind time. QuakeC spawn
    // functions commonly rewrite classname mid-spawn ("func_door" → "door",
    // "func_button" → "button"), so the live edict's v.classname diverges
    // from the .map kv as soon as the spawn function ran. detach_stale_live_ent
    // compares the *current* v.classname against this snapshot, not against
    // the kv — a mismatch means the underlying edict slot got reused by
    // something unrelated (e.g. a spike took a freed monster's slot), which
    // is the case the detach was actually meant to catch.
    char            live_bound_classname[EDIT_KEY_LEN];
} edit_entity_t;

// One entry in the selection list. (entity, brush) is the global address;
// brush is an index into entities[entity].brushes. Stable only between
// scene mutations (Scene_Clear, brush array reallocs).
typedef struct edit_selref_s {
    int entity;
    int brush;
} edit_selref_t;

// The editor scene: entity list, selection, source filename for save/revert.
typedef struct edit_scene_s {
    int             numentities;
    edit_entity_t  *entities;       // malloc'd

    // Selection list. The *primary* selection is the last entry — that's
    // what the inspector/gizmo show as the focal item when many are
    // selected. Empty list means nothing selected.
    edit_selref_t  *selection;
    int             num_selected;
    int             sel_cap;

    // Active face — meaningful only when face mode is on AND exactly one
    // brush is selected AND it matches (active_face_ent, active_face_brush).
    // active_face_plane is a plane index (`b->planes[N]`), NOT a face-array
    // index — plane indices survive Brush_Compile reorderings, face indices
    // don't. All three are -1 when no face is active. Selection mutators
    // call Scene_ClearActiveFace.
    int             active_face_ent;
    int             active_face_brush;
    int             active_face_plane;

    // Round-trip filename. Set by Scene_Load. Empty until a .map has loaded.
    char            filename[256];  // absolute path on disk
    char            mapname[64];    // bare name, used for "map <name>" restart

    // Last sv.spawn_serial editor_check_map_change observed. Different from
    // sv.spawn_serial means SV_SpawnServer ran since we last refreshed, so
    // every cached live_ent / live_static is potentially dangling (Hunk
    // re-allocated sv.edicts and the new num_edicts is smaller than what
    // some pointers were captured against). Mismatch forces a full reset
    // even when sv.name matches edit_scene.mapname.
    int             sv_spawn_serial_seen;
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

// Serialize the current scene to a NUL-terminated malloc'd string in .map
// format. Returns 1 on success; caller free()s *out_text. Length excludes
// the terminator and is written to *out_len if non-NULL.
int   Scene_Serialize       (char **out_text, int *out_len);
// Replace the current scene by parsing text in .map format. The buffer must
// be NUL-terminated; it's not retained or modified by the caller after this
// returns. Returns 1 on success.
int   Scene_DeserializeText (const char *text);

// Brush compile (planes -> windings). Called whenever a brush mutates.
void  Brush_Compile (edit_brush_t *b);
void  Brush_FreeFaces(edit_brush_t *b);

// Translate every plane of a brush by `delta`.
void  Brush_Translate(edit_brush_t *b, const vec3_t delta);

// Rotate every plane of a brush about `pivot` by `angle` radians around
// world axis (0=X, 1=Y, 2=Z). Recompiles after — invalid output (degenerate
// after rotation) leaves the brush with valid=0.
void  Brush_Rotate   (edit_brush_t *b, int axis, float angle,
                      const vec3_t pivot);

// Push a single face along its plane normal by `delta` (positive grows the
// brush, negative shrinks it). Recompiles. If the resulting brush is
// invalid (face would clip through the opposite face), rolls back the move
// and leaves the brush untouched. Identified by plane_idx so it survives
// face-array reordering across recompiles.
void  Brush_TranslateFace(edit_brush_t *b, int plane_idx, float delta);

// Append a 6-plane AABB brush to the worldspawn entity (creating worldspawn
// if absent). Updates selection to the new brush. Returns 1 on success.
// `texname` may be NULL to default to "wbrick1_5".
int   Scene_AddCubeBrush(const vec3_t mins, const vec3_t maxs,
                         const char *texname);

// Replace a single solid brush with 6 wall slabs sharing the same outer
// bbox — the classic "make hollow" room-from-cube operation. Walls inherit
// the source brush's first plane texname; thickness <= 0 defaults to 16
// units; if thickness * 2 >= any axis extent, the call fails. The 6 new
// wall brushes go to worldspawn regardless of the source brush's owning
// entity; selection becomes the 6 new walls. Returns 1 on success.
int   Scene_HollowBrush(int e_idx, int b_idx, float thickness);

// Append a point entity (no brushes) with classname + origin keys. Selection
// becomes the new entity. Returns 1 on success.
int   Scene_AddPointEntity(const char *classname, const vec3_t origin);

// Remove every selected entity (and, for worldspawn, every selected brush)
// from the scene. Live edicts are ED_Free'd so the engine stops simulating
// them. Worldspawn itself is never removed even if it ends up in the
// selection — only its selected brushes go. Selection is cleared on exit
// since indices into the pre-delete layout would be stale. No-op if nothing
// is selected.
void  Scene_DeleteSelected (void);

// Point-entity helpers. "Point entity" here means any entity with no brushes
// (info_player_start, light, monster_*, etc).
int   Entity_IsPoint        (const edit_entity_t *e);
// Reads "origin" key into `out`. Returns 1 if the key existed; 0 (and zeros
// `out`) otherwise.
int   Entity_GetOrigin      (const edit_entity_t *e, vec3_t out);
// Adds `delta` to the entity's origin. Creates the "origin" key if missing.
void  Entity_TranslateOrigin(edit_entity_t *e, const vec3_t delta);
// Upsert a kv pair (replace by key if present, else append). Refreshes the
// classname_idx / origin_idx caches when those keys change.
void  Entity_SetKV          (edit_entity_t *e, const char *key, const char *value);
// Remove a kv pair by key, shifting later entries down. Refreshes the
// classname_idx / origin_idx caches. No-op if the key is absent. Returns
// 1 on removal, 0 if absent.
int   Entity_RemoveKV       (edit_entity_t *e, const char *key);

// Selection helpers. The list-based API treats the *primary* (last-added)
// brush as the focal one — that's what GetSelected{Brush,Entity} return,
// so single-select call sites remain valid.
edit_brush_t *Scene_GetSelectedBrush(void);
edit_entity_t *Scene_GetSelectedEntity(void);
int   Scene_NumSelected     (void);
int   Scene_GetSelected     (int i, int *out_ent, int *out_brush); // 1 = ok
int   Scene_SelectionContains(int ent, int brush);
void  Scene_SelectionClear  (void);
// Add / Remove / Toggle. For non-worldspawn entities, these treat the whole
// entity as one group: adding any one brush adds them all, removing or
// toggling does the same.
void  Scene_SelectionAdd    (int ent, int brush);
void  Scene_SelectionRemove (int ent, int brush);
void  Scene_SelectionToggle (int ent, int brush);

// Active-face state. Validated against current selection — Scene_SetActiveFace
// is a no-op unless `Scene_NumSelected() == 1` and that one selection refers
// to (ent, brush) and `plane_idx` is in range. plane_idx is an index into
// `b->planes[]`; survives Brush_Compile reorderings (face indices don't).
void  Scene_SetActiveFace   (int ent, int brush, int plane_idx);
int   Scene_GetActiveFace   (int *out_ent, int *out_brush, int *out_plane);
void  Scene_ClearActiveFace (void);

// Group / ungroup operations.
//   Group: move every selected brush into a new func_group entity. The
//          new entity becomes the selection.
//   Ungroup: for each selected brush whose owning entity is a func_group,
//            move it into the worldspawn entity. Other classnames left
//            untouched (their grouping has gameplay semantics).
void  Scene_GroupSelected   (void);
void  Scene_UngroupSelected (void);

// Wrap every selected brush into a new entity with the given classname
// (e.g. func_door, func_button, func_train). Origin is the centroid of
// the selected brushes' AABB midpoints — what id maps use for movers'
// rotation pivot. Selection becomes the new entity. Caller pushes
// History before invoking. Returns 1 on success, 0 if no brushes were
// selected or classname is empty.
int   Scene_WrapBrushesIntoEntity(const char *classname);

// Iterate every brush with its owning entity. cb returns 0 to stop.
typedef int (*Scene_BrushIter_fn)(edit_entity_t *e, int e_idx,
                                  edit_brush_t *b, int b_idx, void *user);
void  Scene_ForEachBrush(Scene_BrushIter_fn cb, void *user);

// Serialize the scene to a compact JSON description (open-state, mapname,
// selection, entities with kv pairs, brushes with AABBs). Used by the MCP
// `editor_get_scene` tool so an external agent can introspect the scene.
// Writes up to outsz-1 bytes (always NUL-terminated). Returns bytes written
// excluding terminator. Truncates with a "...":true marker if the output
// doesn't fit.
int   Scene_SerializeJSON  (char *out, int outsz);

#endif // EDIT_SCENE_H

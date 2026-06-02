#ifndef LIBMODEL_IQM_H
#define LIBMODEL_IQM_H

#include <stddef.h>
#include "libmodel.h"   /* lm_result_t + qalloc_t (via qalloc.h) */

/* Neutral in-memory IQM (Inter-Quake Model) representation. R1 captures only
   what a static bind-pose rigid renderer needs: meshes, the bind-pose joint
   skeleton, triangles, and per-vertex position/texcoord/first-bone. Animation
   poses/anims/frames are intentionally skipped (added in R2). */

typedef struct {
    float         pos[3];
    float         st[2];
    unsigned char bone;          /* first blend index (rigid: 1 bone per vertex) */
} lm_iqm_vert_t;

typedef struct {
    char  name[64];
    int   parent;                /* -1 = root */
    float translate[3];
    float rotate[4];             /* quaternion x,y,z,w */
    float scale[3];
} lm_iqm_joint_t;

typedef struct {
    char     name[64];
    char     material[64];
    unsigned first_vertex, num_vertexes;
    unsigned first_triangle, num_triangles;
} lm_iqm_mesh_t;

typedef struct lm_iqm_s {
    lm_iqm_mesh_t  *meshes;   int nummeshes;
    lm_iqm_joint_t *joints;   int numjoints;
    lm_iqm_vert_t  *verts;    int numverts;
    unsigned       *tris;     int numtris;   /* numtris*3 global vertex indices */
    float           mins[3], maxs[3];
    /* animation (R2): decoded per-frame per-joint local TRS. numframes==0 and
       frametrs==NULL means a static (bind-pose-only) model. Layout:
       frametrs[(frame*numjoints + joint)*10 + c], c = 0..2 translate,
       3..6 rotate quat (x,y,z,w), 7..9 scale. */
    int             numframes;
    float           framerate;
    float          *frametrs;
    /* role joints (R3), resolved by name convention at load; -1 / 0 = absent.
       head/chest/jaw match a substring; eyes match the "eye" prefix. */
    int             head_joint;
    int             chest_joint;
    int             jaw_joint;     /* resolved for forward-compat; unused until lipsync */
    int             eye_joint[4];
    int             num_eye;
    int             pony_joint[8];  /* ordered chain root..tip (R4 dynamics) */
    int             num_pony;
    qalloc_t        alloc;
} lm_iqm_t;

/* Parse `len` bytes at `buf` into *out. alloc=NULL uses the default malloc
   allocator. On success returns LM_OK and sets *out; on failure returns an
   error and sets *out to NULL. */
lm_result_t lm_load_iqm(const void *buf, size_t len,
                        const qalloc_t *alloc, lm_iqm_t **out);

/* Free via the remembered allocator (no-op for arena backings). Safe on NULL. */
void lm_iqm_free(lm_iqm_t *m);

/* Serialize geometry + skeleton (no animation yet) to IQM v2 bytes. On success
   returns LM_OK and sets *out_buf to a malloc()'d buffer of *out_len bytes that
   the caller must free() with the C library free(). The output re-parses via
   lm_load_iqm as a static (bind-pose) model. */
lm_result_t lm_write_iqm(const lm_iqm_t *m, void **out_buf, size_t *out_len);

#endif /* LIBMODEL_IQM_H */

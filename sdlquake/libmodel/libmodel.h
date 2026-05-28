#ifndef LIBMODEL_H
#define LIBMODEL_H

#include <stddef.h>
#include "qalloc.h"

/* Neutral, lossless in-memory representation of a Quake MDL. Vertices stay as
   raw packed bytes + the model's scale/scale_origin (decompression is the
   renderer's job). `grouped` distinguishes single vs group entries even when
   the count is 1, because the on-disk format does and consumers may need to
   reproduce that distinction faithfully. */

typedef struct {
    unsigned char v[3];
    unsigned char lightnormalindex;
} lm_trivertx_t;

typedef struct {
    char          name[16];            /* raw 16 bytes from file (may include NUL) */
    lm_trivertx_t bboxmin, bboxmax;
    lm_trivertx_t *verts;              /* [numverts] */
} lm_pose_t;

typedef struct {
    int           grouped;             /* 1 if stored as a frame group in the file */
    int           numposes;            /* 1 for a single frame */
    float        *intervals;           /* [numposes] when grouped, else NULL */
    lm_trivertx_t bboxmin, bboxmax;    /* frame-level bbox (group bbox, or the single pose's) */
    lm_pose_t    *poses;               /* [numposes] */
} lm_frame_t;

typedef struct {
    int            grouped;            /* 1 if stored as a skin group in the file */
    int            numpics;            /* 1 for a single skin */
    float         *intervals;          /* [numpics] when grouped, else NULL */
    unsigned char **pics;              /* [numpics], each skinwidth*skinheight indices */
} lm_skin_t;

typedef struct { int onseam, s, t; } lm_stvert_t;
typedef struct { int facesfront; int vertindex[3]; } lm_triangle_t;

typedef struct {
    /* header scalars (raw from file; no scaling applied) */
    float scale[3], scale_origin[3], eyeposition[3];
    float boundingradius, size;
    int   skinwidth, skinheight;
    int   numskins, numverts, numtris, numframes;
    int   synctype, flags;
    /* data */
    lm_skin_t     *skins;              /* [numskins] */
    lm_stvert_t   *stverts;            /* [numverts] */
    lm_triangle_t *triangles;          /* [numtris] */
    lm_frame_t    *frames;             /* [numframes] */
    /* bookkeeping */
    qalloc_t       alloc;              /* remembered, used by lm_model_free */
} lm_model_t;

typedef enum {
    LM_OK = 0,
    LM_ERR_TRUNCATED,    /* a read would run past the buffer */
    LM_ERR_BAD_MAGIC,    /* not "IDPO" */
    LM_ERR_BAD_VERSION,  /* version != 6 */
    LM_ERR_BAD_COUNT,    /* nonsensical counts or skin dimensions */
    LM_ERR_OOM           /* allocator returned NULL */
} lm_result_t;

/* Parse `len` bytes at `buf` into *out. Pass alloc=NULL for the default malloc
   allocator. On success returns LM_OK and sets *out; on failure returns an
   error and sets *out to NULL (any partial allocations are released). */
lm_result_t lm_load_mdl(const void *buf, size_t len,
                        const qalloc_t *alloc, lm_model_t **out);

/* Free a model via its remembered allocator (no-op for arena backings).
   Safe on NULL. */
void lm_model_free(lm_model_t *m);

const char *lm_strerror(lm_result_t r);

#endif /* LIBMODEL_H */

#include "libmodel.h"
#include "mdl_format.h"
#include <stdint.h>
#include <string.h>

/* trivertx must be exactly 4 bytes for the raw vert/bbox reads below. */
typedef char lm__trivertx_size_check[(sizeof(lm_trivertx_t) == 4) ? 1 : -1];

typedef struct {
    const unsigned char *p;
    size_t               len;
    size_t               pos;
    int                  err;   /* set once a read runs past the buffer */
} lm_cursor_t;

static uint32_t rd_u32(lm_cursor_t *c) {
    uint32_t v;
    if (c->err || c->pos + 4 > c->len) { c->err = 1; return 0; }
    v = (uint32_t)c->p[c->pos]
      | ((uint32_t)c->p[c->pos + 1] << 8)
      | ((uint32_t)c->p[c->pos + 2] << 16)
      | ((uint32_t)c->p[c->pos + 3] << 24);
    c->pos += 4;
    return v;
}
static int rd_i32(lm_cursor_t *c) { return (int)rd_u32(c); }
static float rd_f32(lm_cursor_t *c) {
    uint32_t u = rd_u32(c);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static void rd_bytes(lm_cursor_t *c, void *dst, size_t n) {
    if (c->err || c->pos + n > c->len) { c->err = 1; return; }
    memcpy(dst, c->p + c->pos, n);
    c->pos += n;
}

#define FAIL(code) do { lm_model_free(m); return (code); } while (0)
#define CHECK_TRUNC() do { if (c.err) FAIL(LM_ERR_TRUNCATED); } while (0)

/* Reads bboxmin, bboxmax, name[16], then numv raw trivertx into pose. */
static lm_result_t read_pose(lm_cursor_t *c, qalloc_t *a, lm_pose_t *pose, int numv) {
    rd_bytes(c, &pose->bboxmin, 4);
    rd_bytes(c, &pose->bboxmax, 4);
    rd_bytes(c, pose->name, 16);
    if (c->err) return LM_ERR_TRUNCATED;
    pose->verts = QALLOC_ARR(a, lm_trivertx_t, numv);
    if (!pose->verts) return LM_ERR_OOM;
    rd_bytes(c, pose->verts, (size_t)numv * 4);
    if (c->err) return LM_ERR_TRUNCATED;
    return LM_OK;
}

lm_result_t lm_load_mdl(const void *buf, size_t len,
                        const qalloc_t *ain, lm_model_t **out) {
    qalloc_t a = ain ? *ain : qalloc_malloc();
    lm_cursor_t c;
    lm_model_t *m;
    int ident, version, i, j;
    size_t skinsize;

    c.p = (const unsigned char *)buf; c.len = len; c.pos = 0; c.err = 0;
    *out = NULL;

    ident   = rd_i32(&c);
    version = rd_i32(&c);
    if (c.err) return LM_ERR_TRUNCATED;
    if (ident != MDL_IDENT)     return LM_ERR_BAD_MAGIC;
    if (version != MDL_VERSION) return LM_ERR_BAD_VERSION;

    m = QALLOC(&a, lm_model_t);
    if (!m) return LM_ERR_OOM;
    memset(m, 0, sizeof(*m));
    m->alloc = a;

    for (i = 0; i < 3; i++) m->scale[i]        = rd_f32(&c);
    for (i = 0; i < 3; i++) m->scale_origin[i] = rd_f32(&c);
    m->boundingradius = rd_f32(&c);
    for (i = 0; i < 3; i++) m->eyeposition[i]  = rd_f32(&c);
    m->numskins   = rd_i32(&c);
    m->skinwidth  = rd_i32(&c);
    m->skinheight = rd_i32(&c);
    m->numverts   = rd_i32(&c);
    m->numtris    = rd_i32(&c);
    m->numframes  = rd_i32(&c);
    m->synctype   = rd_i32(&c);
    m->flags      = rd_i32(&c);
    m->size       = rd_f32(&c);
    CHECK_TRUNC();

    if (m->numskins < 1 || m->numverts < 1 || m->numtris < 1 ||
        m->numframes < 1 || m->skinwidth < 1 || m->skinheight < 1)
        FAIL(LM_ERR_BAD_COUNT);

    skinsize = (size_t)m->skinwidth * (size_t)m->skinheight;

    /* skins */
    m->skins = QALLOC_ARR(&a, lm_skin_t, m->numskins);
    if (!m->skins) FAIL(LM_ERR_OOM);
    memset(m->skins, 0, (size_t)m->numskins * sizeof(lm_skin_t));
    for (i = 0; i < m->numskins; i++) {
        lm_skin_t *sk = &m->skins[i];
        int type = rd_i32(&c);
        CHECK_TRUNC();
        if (type == MDL_TYPE_SINGLE) {
            sk->grouped = 0; sk->numpics = 1; sk->intervals = NULL;
            sk->pics = QALLOC_ARR(&a, unsigned char *, 1);
            if (!sk->pics) FAIL(LM_ERR_OOM);
            sk->pics[0] = QALLOC_ARR(&a, unsigned char, skinsize);
            if (!sk->pics[0]) FAIL(LM_ERR_OOM);
            rd_bytes(&c, sk->pics[0], skinsize);
            CHECK_TRUNC();
        } else {
            int nb = rd_i32(&c);
            CHECK_TRUNC();
            if (nb < 1) FAIL(LM_ERR_BAD_COUNT);
            sk->grouped = 1; sk->numpics = nb;
            sk->intervals = QALLOC_ARR(&a, float, nb);
            sk->pics = QALLOC_ARR(&a, unsigned char *, nb);
            if (!sk->intervals || !sk->pics) FAIL(LM_ERR_OOM);
            memset(sk->pics, 0, (size_t)nb * sizeof(unsigned char *));
            for (j = 0; j < nb; j++) sk->intervals[j] = rd_f32(&c);
            CHECK_TRUNC();
            for (j = 0; j < nb; j++) {
                sk->pics[j] = QALLOC_ARR(&a, unsigned char, skinsize);
                if (!sk->pics[j]) FAIL(LM_ERR_OOM);
                rd_bytes(&c, sk->pics[j], skinsize);
                CHECK_TRUNC();
            }
        }
    }

    /* stverts */
    m->stverts = QALLOC_ARR(&a, lm_stvert_t, m->numverts);
    if (!m->stverts) FAIL(LM_ERR_OOM);
    for (i = 0; i < m->numverts; i++) {
        m->stverts[i].onseam = rd_i32(&c);
        m->stverts[i].s      = rd_i32(&c);
        m->stverts[i].t      = rd_i32(&c);
    }
    CHECK_TRUNC();

    /* triangles */
    m->triangles = QALLOC_ARR(&a, lm_triangle_t, m->numtris);
    if (!m->triangles) FAIL(LM_ERR_OOM);
    for (i = 0; i < m->numtris; i++) {
        m->triangles[i].facesfront = rd_i32(&c);
        for (j = 0; j < 3; j++) m->triangles[i].vertindex[j] = rd_i32(&c);
    }
    CHECK_TRUNC();

    /* frames */
    m->frames = QALLOC_ARR(&a, lm_frame_t, m->numframes);
    if (!m->frames) FAIL(LM_ERR_OOM);
    memset(m->frames, 0, (size_t)m->numframes * sizeof(lm_frame_t));
    for (i = 0; i < m->numframes; i++) {
        lm_frame_t *fr = &m->frames[i];
        int type = rd_i32(&c);
        CHECK_TRUNC();
        if (type == MDL_TYPE_SINGLE) {
            lm_result_t pr;
            fr->grouped = 0; fr->numposes = 1; fr->intervals = NULL;
            fr->poses = QALLOC_ARR(&a, lm_pose_t, 1);
            if (!fr->poses) FAIL(LM_ERR_OOM);
            memset(fr->poses, 0, sizeof(lm_pose_t));
            pr = read_pose(&c, &a, &fr->poses[0], m->numverts);
            if (pr != LM_OK) FAIL(pr);
            fr->bboxmin = fr->poses[0].bboxmin;
            fr->bboxmax = fr->poses[0].bboxmax;
        } else {
            int nb = rd_i32(&c);
            CHECK_TRUNC();
            if (nb < 1) FAIL(LM_ERR_BAD_COUNT);
            fr->grouped = 1; fr->numposes = nb;
            rd_bytes(&c, &fr->bboxmin, 4);
            rd_bytes(&c, &fr->bboxmax, 4);
            CHECK_TRUNC();
            fr->intervals = QALLOC_ARR(&a, float, nb);
            fr->poses = QALLOC_ARR(&a, lm_pose_t, nb);
            if (!fr->intervals || !fr->poses) FAIL(LM_ERR_OOM);
            memset(fr->poses, 0, (size_t)nb * sizeof(lm_pose_t));
            for (j = 0; j < nb; j++) fr->intervals[j] = rd_f32(&c);
            CHECK_TRUNC();
            for (j = 0; j < nb; j++) {
                lm_result_t pr = read_pose(&c, &a, &fr->poses[j], m->numverts);
                if (pr != LM_OK) FAIL(pr);
            }
        }
    }

    *out = m;
    return LM_OK;
}

void lm_model_free(lm_model_t *m) {
    qalloc_t *a;
    int i, j;
    if (!m) return;
    a = &m->alloc;
    if (m->skins) {
        for (i = 0; i < m->numskins; i++) {
            lm_skin_t *sk = &m->skins[i];
            if (sk->pics) {
                for (j = 0; j < sk->numpics; j++)
                    if (sk->pics[j]) QFREE(a, sk->pics[j]);
                QFREE(a, sk->pics);
            }
            if (sk->intervals) QFREE(a, sk->intervals);
        }
        QFREE(a, m->skins);
    }
    if (m->stverts)   QFREE(a, m->stverts);
    if (m->triangles) QFREE(a, m->triangles);
    if (m->frames) {
        for (i = 0; i < m->numframes; i++) {
            lm_frame_t *fr = &m->frames[i];
            if (fr->poses) {
                for (j = 0; j < fr->numposes; j++)
                    if (fr->poses[j].verts) QFREE(a, fr->poses[j].verts);
                QFREE(a, fr->poses);
            }
            if (fr->intervals) QFREE(a, fr->intervals);
        }
        QFREE(a, m->frames);
    }
    /* free the model struct last; copy allocator out first since it lives in m */
    {
        qalloc_t self = m->alloc;
        self.free(self.ctx, m);
    }
}

const char *lm_strerror(lm_result_t r) {
    switch (r) {
        case LM_OK:             return "ok";
        case LM_ERR_TRUNCATED:  return "truncated / read past end of buffer";
        case LM_ERR_BAD_MAGIC:  return "bad magic (not IDPO)";
        case LM_ERR_BAD_VERSION:return "bad version (expected 6)";
        case LM_ERR_BAD_COUNT:  return "bad count or skin dimension";
        case LM_ERR_OOM:        return "out of memory";
        default:                return "unknown error";
    }
}

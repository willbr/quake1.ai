#include <string.h>
#include "iqm.h"

/* ---- little-endian readers over a fixed buffer ---- */
static unsigned rdu(const unsigned char *p){
    return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);
}
static int   rdi(const unsigned char *p){ return (int)rdu(p); }
static float rdf(const unsigned char *p){ unsigned u=rdu(p); float f; memcpy(&f,&u,4); return f; }

enum { IQM_POSITION=0, IQM_TEXCOORD=1, IQM_NORMAL=2, IQM_TANGENT=3,
       IQM_BLENDINDEXES=4, IQM_BLENDWEIGHTS=5, IQM_COLOR=6 };
enum { IQM_UBYTE=1, IQM_FLOAT=7 };

#define HDR         124
#define H_VERSION   0x10
#define H_FILESIZE  0x14
#define H_NUM_TEXT  0x1C
#define H_OFS_TEXT  0x20
#define H_NUM_MESH  0x24
#define H_OFS_MESH  0x28
#define H_NUM_VA    0x2C
#define H_NUM_VERT  0x30
#define H_OFS_VA    0x34
#define H_NUM_TRI   0x38
#define H_OFS_TRI   0x3C
#define H_NUM_JOINT 0x44
#define H_OFS_JOINT 0x48
#define H_NUM_POSES 0x4C
#define H_OFS_POSES 0x50
#define H_NUM_ANIMS 0x54
#define H_OFS_ANIMS 0x58
#define H_NUM_FRAMES        0x5C
#define H_NUM_FRAMECHANNELS 0x60
#define H_OFS_FRAMES        0x64
#define IQMJOINT_SZ 48   /* name+parent + 3+4+3 floats = 4+4+12+16+12 */
#define IQMPOSE_SZ  88   /* parent+mask + 10 offset + 10 scale floats */
#define IQMANIM_SZ  20   /* name+first_frame+num_frames+framerate+flags */

/* 1 if [ofs, ofs+count*stride) fits within len */
static int fits(size_t len, unsigned ofs, unsigned count, unsigned stride){
    if (count == 0) return 1;
    if ((size_t)ofs > len) return 0;
    if ((size_t)count > (len - ofs) / stride) return 0;
    return 1;
}

static const char *strat(const unsigned char *base, unsigned ofs_text,
                         unsigned num_text, unsigned nameofs){
    if (nameofs >= num_text) return "";
    return (const char *)(base + ofs_text + nameofs);
}

lm_result_t lm_load_iqm(const void *vbuf, size_t len,
                        const qalloc_t *ain, lm_iqm_t **out){
    qalloc_t a = ain ? *ain : qalloc_malloc();
    const unsigned char *b = (const unsigned char *)vbuf;
    lm_iqm_t *m; int i, j;
    unsigned ofs_text, num_text, ofs_mesh, num_mesh, num_va, num_vert, ofs_va;
    unsigned num_tri, ofs_tri, num_joint, ofs_joint;
    unsigned ofs_pos = 0, ofs_uv = 0, ofs_bi = 0; int have_pos = 0;

    *out = NULL;
    if (len < HDR) return LM_ERR_TRUNCATED;
    if (memcmp(b, "INTERQUAKEMODEL\0", 16) != 0) return LM_ERR_BAD_MAGIC;
    if (rdu(b+H_VERSION) != 2) return LM_ERR_BAD_VERSION;
    if (rdu(b+H_FILESIZE) > len) return LM_ERR_TRUNCATED;

    num_text  = rdu(b+H_NUM_TEXT);  ofs_text  = rdu(b+H_OFS_TEXT);
    num_mesh  = rdu(b+H_NUM_MESH);  ofs_mesh  = rdu(b+H_OFS_MESH);
    num_va    = rdu(b+H_NUM_VA);    num_vert  = rdu(b+H_NUM_VERT);  ofs_va = rdu(b+H_OFS_VA);
    num_tri   = rdu(b+H_NUM_TRI);   ofs_tri   = rdu(b+H_OFS_TRI);
    num_joint = rdu(b+H_NUM_JOINT); ofs_joint = rdu(b+H_OFS_JOINT);

    if (!num_mesh || !num_vert || !num_tri) return LM_ERR_BAD_COUNT;
    if (!fits(len, ofs_text,  num_text,  1)  ||
        !fits(len, ofs_mesh,  num_mesh,  24) ||
        !fits(len, ofs_va,    num_va,    20) ||
        !fits(len, ofs_tri,   num_tri,   12) ||
        !fits(len, ofs_joint, num_joint, IQMJOINT_SZ))
        return LM_ERR_TRUNCATED;

    for (i = 0; i < (int)num_va; i++){
        const unsigned char *va = b + ofs_va + i*20;
        unsigned type = rdu(va), fmt = rdu(va+8), size = rdu(va+12), ofs = rdu(va+16);
        if (type == IQM_POSITION && fmt == IQM_FLOAT && size == 3){
            if (!fits(len, ofs, num_vert, 12)) return LM_ERR_TRUNCATED;
            ofs_pos = ofs; have_pos = 1;
        } else if (type == IQM_TEXCOORD && fmt == IQM_FLOAT && size == 2){
            if (!fits(len, ofs, num_vert, 8)) return LM_ERR_TRUNCATED;
            ofs_uv = ofs;
        } else if (type == IQM_BLENDINDEXES && fmt == IQM_UBYTE && size == 4){
            if (!fits(len, ofs, num_vert, 4)) return LM_ERR_TRUNCATED;
            ofs_bi = ofs;
        }
    }
    if (!have_pos) return LM_ERR_BAD_COUNT;

    m = QALLOC(&a, lm_iqm_t);
    if (!m) return LM_ERR_OOM;
    memset(m, 0, sizeof(*m));
    m->alloc = a;
    m->nummeshes = (int)num_mesh; m->numjoints = (int)num_joint;
    m->numverts  = (int)num_vert; m->numtris   = (int)num_tri;
    m->meshes = QALLOC_ARR(&a, lm_iqm_mesh_t, num_mesh);
    m->joints = num_joint ? QALLOC_ARR(&a, lm_iqm_joint_t, num_joint) : NULL;
    m->verts  = QALLOC_ARR(&a, lm_iqm_vert_t, num_vert);
    m->tris   = QALLOC_ARR(&a, unsigned, num_tri*3);
    if (!m->meshes || !m->verts || !m->tris || (num_joint && !m->joints)){
        lm_iqm_free(m); return LM_ERR_OOM;
    }

    for (i = 0; i < (int)num_mesh; i++){
        const unsigned char *me = b + ofs_mesh + i*24;
        strncpy(m->meshes[i].name,     strat(b, ofs_text, num_text, rdu(me)),   63);
        strncpy(m->meshes[i].material, strat(b, ofs_text, num_text, rdu(me+4)), 63);
        m->meshes[i].name[63] = 0; m->meshes[i].material[63] = 0;
        m->meshes[i].first_vertex   = rdu(me+8);  m->meshes[i].num_vertexes  = rdu(me+12);
        m->meshes[i].first_triangle = rdu(me+16); m->meshes[i].num_triangles = rdu(me+20);
    }
    for (i = 0; i < (int)num_joint; i++){
        const unsigned char *jo = b + ofs_joint + i*IQMJOINT_SZ;
        strncpy(m->joints[i].name, strat(b, ofs_text, num_text, rdu(jo)), 63);
        m->joints[i].name[63] = 0;
        m->joints[i].parent = rdi(jo+4);
        for (j = 0; j < 3; j++) m->joints[i].translate[j] = rdf(jo+8 + j*4);
        for (j = 0; j < 4; j++) m->joints[i].rotate[j]    = rdf(jo+20 + j*4);
        for (j = 0; j < 3; j++) m->joints[i].scale[j]     = rdf(jo+36 + j*4);
    }

    /* ---- resolve role joints by name convention (R3) ---- */
    m->head_joint = m->chest_joint = m->jaw_joint = -1;
    m->num_eye = 0;
    for (i = 0; i < (int)num_joint; i++){
        const char *nm = m->joints[i].name;
        if (m->head_joint  < 0 && strstr(nm, "head"))  m->head_joint  = i;
        if (m->chest_joint < 0 && strstr(nm, "chest")) m->chest_joint = i;
        if (m->jaw_joint   < 0 && strstr(nm, "jaw"))   m->jaw_joint   = i;
        if (strstr(nm, "eye") && m->num_eye < 4)       m->eye_joint[m->num_eye++] = i;
    }
    for (i = 0; i < (int)num_vert; i++){
        const unsigned char *pp = b + ofs_pos + i*12;
        m->verts[i].pos[0] = rdf(pp); m->verts[i].pos[1] = rdf(pp+4); m->verts[i].pos[2] = rdf(pp+8);
        if (ofs_uv){ const unsigned char *uu = b + ofs_uv + i*8;
            m->verts[i].st[0] = rdf(uu); m->verts[i].st[1] = rdf(uu+4); }
        else { m->verts[i].st[0] = m->verts[i].st[1] = 0.f; }
        m->verts[i].bone = ofs_bi ? b[ofs_bi + i*4] : 0;
    }
    for (i = 0; i < (int)num_tri; i++){
        const unsigned char *tt = b + ofs_tri + i*12;
        m->tris[i*3+0] = rdu(tt); m->tris[i*3+1] = rdu(tt+4); m->tris[i*3+2] = rdu(tt+8);
    }

    /* ---- animation (optional): decode per-frame per-joint local TRS ---- */
    {
        unsigned num_poses  = rdu(b+H_NUM_POSES),  ofs_poses  = rdu(b+H_OFS_POSES);
        unsigned num_anims  = rdu(b+H_NUM_ANIMS),  ofs_anims  = rdu(b+H_OFS_ANIMS);
        unsigned nframes    = rdu(b+H_NUM_FRAMES), nfc        = rdu(b+H_NUM_FRAMECHANNELS);
        unsigned ofs_frames = rdu(b+H_OFS_FRAMES);

        if (nframes > 0 && num_poses == (unsigned)m->numjoints && num_poses > 0 &&
            fits(len, ofs_poses, num_poses, IQMPOSE_SZ) &&
            fits(len, ofs_frames, nframes * nfc, 2))
        {
            unsigned *pmask = QALLOC_ARR(&a, unsigned, num_poses);
            float    *poff  = QALLOC_ARR(&a, float, num_poses*10);
            float    *pscl  = QALLOC_ARR(&a, float, num_poses*10);
            float    *ftrs  = QALLOC_ARR(&a, float, (size_t)nframes*num_poses*10);

            if (pmask && poff && pscl && ftrs)
            {
                const unsigned char *fw = b + ofs_frames;
                unsigned pi, fr, cc;
                for (pi = 0; pi < num_poses; pi++)
                {
                    const unsigned char *po = b + ofs_poses + pi*IQMPOSE_SZ;
                    pmask[pi] = rdu(po+4);
                    for (cc = 0; cc < 10; cc++) poff[pi*10+cc] = rdf(po+8  + cc*4);
                    for (cc = 0; cc < 10; cc++) pscl[pi*10+cc] = rdf(po+48 + cc*4);
                }
                for (fr = 0; fr < nframes; fr++)
                    for (pi = 0; pi < num_poses; pi++)
                    {
                        float *dst = ftrs + ((size_t)fr*num_poses + pi)*10;
                        for (cc = 0; cc < 10; cc++)
                        {
                            float v = poff[pi*10+cc];
                            if (pmask[pi] & (1u << cc))
                            {
                                v += (float)(fw[0] | (fw[1] << 8)) * pscl[pi*10+cc];
                                fw += 2;
                            }
                            dst[cc] = v;
                        }
                    }
                m->frametrs  = ftrs;
                m->numframes = (int)nframes;
                m->framerate = (num_anims > 0 && fits(len, ofs_anims, num_anims, IQMANIM_SZ))
                                ? rdf(b + ofs_anims + 12) : 10.0f;
                if (m->framerate <= 0.0f) m->framerate = 10.0f;
            }
            else
            {
                QFREE(&a, ftrs);   /* OOM -> stay static */
            }
            QFREE(&a, pmask); QFREE(&a, poff); QFREE(&a, pscl);
        }
    }

    m->mins[0] = m->mins[1] = m->mins[2] =  1e30f;
    m->maxs[0] = m->maxs[1] = m->maxs[2] = -1e30f;
    for (i = 0; i < (int)num_vert; i++) for (j = 0; j < 3; j++){
        float v = m->verts[i].pos[j];
        if (v < m->mins[j]) m->mins[j] = v;
        if (v > m->maxs[j]) m->maxs[j] = v;
    }

    *out = m;
    return LM_OK;
}

void lm_iqm_free(lm_iqm_t *m){
    qalloc_t a;
    if (!m) return;
    a = m->alloc;
    QFREE(&a, m->meshes); QFREE(&a, m->joints);
    QFREE(&a, m->verts);  QFREE(&a, m->tris);
    QFREE(&a, m->frametrs);
    QFREE(&a, m);
}

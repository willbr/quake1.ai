# Skeletal Actors R1 — IQM Load + Static Render — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render a multi-part rigid IQM "actor" (a cube figure) in bind pose, in-game, via a dev-spawn console command — proving the IQM load + compose + software-rasterize path end to end.

**Architecture:** A portable IQM reader in `libmodel` (`lm_load_iqm` → `lm_iqm_t`) mirrors the existing `lm_load_mdl`. The engine adds a `mod_iqm` model type whose loader (`Mod_LoadIQMModel`, mirroring `Mod_LoadAliasModel`) stores the parsed model on the hunk. A new `R_IQMDrawModel` (added to `r_alias.c` so it reuses the alias transform/projection/rasterizer machinery) renders each mesh's triangles as flat-lit, solid-colour rigid parts via the existing `D_PolysetDraw`. **Key simplification:** at bind pose the rigid-skin transform is identity, so vertices render at their authored actor-space positions — no joint matrices needed for R1 (joints are parsed and stored for R2). A dev module (`iqm_dev.c`) adds `actor_dump`/`actor_spawn`/`actor_clear` console commands and injects a persistent client-side entity into `cl_visedicts` each frame.

**Tech Stack:** C (gnu89 engine, modern-C libmodel), Zig build, Python 3 (one-off asset generator), Quake software renderer.

**Verification note:** This project has **no unit-test framework**; per `CLAUDE.md`, verification is **build success + in-game/console observation**. Each task therefore ends with a concrete build command and an observable check (console-dump output or visual), not a unit test. The IQM parser is verified end-to-end by the `actor_dump` command (Task 4) against the known generated asset.

---

## File Structure

| File | New? | Responsibility |
|---|---|---|
| `scripts/make_test_actor_iqm.py` | new | One-off generator: writes `id1/actors/dummy.iqm` (cube figure) + `dummy.actor` |
| `id1/actors/dummy.iqm` | new (generated) | Bootstrap test actor (committed) |
| `id1/actors/dummy.actor` | new | KV semantics manifest stub |
| `sdlquake/libmodel/iqm.h` | new | `lm_iqm_t` + `lm_load_iqm` declaration (portable) |
| `sdlquake/libmodel/iqm.c` | new | IQM v2 binary parser → `lm_iqm_t` (bind pose; anims skipped) |
| `sdlquake/engine_src/model.h` | modify | Add `mod_iqm` to `modtype_t`; add `iqmdata` ptr to `model_t` |
| `sdlquake/engine_src/model.c` | modify | IQM magic dispatch + `Mod_LoadIQMModel` |
| `sdlquake/engine_src/r_alias.c` | modify | `R_IQMSetUpTransform` + `R_IQMDrawModel` (reuse alias machinery) |
| `sdlquake/engine_src/r_local.h` | modify | Declare `R_IQMDrawModel` |
| `sdlquake/engine_src/r_main.c` | modify | `case mod_iqm:` in `R_DrawEntitiesOnList` |
| `sdlquake/engine_src/iqm_dev.c` | new | `actor_dump`/`actor_spawn`/`actor_clear` + dev entity injection + `.actor` parse |
| `sdlquake/engine_src/iqm_dev.h` | new | `IQMDev_Init`, `IQMDev_AddToScene` decls |
| `sdlquake/engine_src/cl_main.c` | modify | Call `IQMDev_Init` (CL_Init) + `IQMDev_AddToScene` (CL_ReadFromServer) |
| `build.zig` | modify | Compile `libmodel/iqm.c` + `engine_src/iqm_dev.c` |

---

## Task 0: Generate the bootstrap cube-actor IQM

**Files:**
- Create: `scripts/make_test_actor_iqm.py`
- Create (generated): `id1/actors/dummy.iqm`, `id1/actors/dummy.actor`

- [ ] **Step 1: Write the generator script**

Create `scripts/make_test_actor_iqm.py`:

```python
#!/usr/bin/env python3
# Generates id1/actors/dummy.iqm : a 5-part rigid cube "actor" in bind pose.
# IQM v2, no animations. Parts: base, chest, head, eye_l, eye_r.
# Quake axes: +X forward, +Y left, +Z up.
import struct, os

OUT_IQM   = os.path.join(os.path.dirname(__file__), "..", "id1", "actors", "dummy.iqm")
OUT_ACTOR = os.path.join(os.path.dirname(__file__), "..", "id1", "actors", "dummy.actor")

# --- joints: (name, parent, world_translate) ; identity rotation, unit scale ---
JOINTS = [
    ("base",  -1, (0,0,0)),
    ("chest",  0, (0,0,24)),
    ("head",   1, (0,0,28)),
    ("eye.L",  2, (8, 4, 4)),
    ("eye.R",  2, (8,-4, 4)),
]
# world position of each joint (translate is parent-relative; compute absolute)
def joint_world(i):
    name,parent,t = JOINTS[i]
    if parent < 0: return t
    pw = joint_world(parent)
    return (pw[0]+t[0], pw[1]+t[1], pw[2]+t[2])

# --- parts: one box mesh per joint: (joint_index, half_extents, center_offset_from_joint, palette_color) ---
# palette_color is an 8-bit Quake palette index used only by the .actor/dev colours; mesh material name carries it.
PARTS = [
    (0, (16,16,12), (0,0,12),  "p_base"),   # base/legs block, sits on ground
    (1, (12,14,12), (0,0,0),   "p_chest"),  # torso
    (2, (10,10,10), (0,0,0),   "p_head"),   # head
    (3, (2,2,2),    (0,0,0),   "p_eye"),    # left eye
    (4, (2,2,2),    (0,0,0),   "p_eye"),    # right eye
]

# 8 cube corners (unit), 12 triangles (CCW outward).  Order: (-+ per axis)
CORNERS = [(-1,-1,-1),( 1,-1,-1),( 1, 1,-1),(-1, 1,-1),
           (-1,-1, 1),( 1,-1, 1),( 1, 1, 1),(-1, 1, 1)]
# CCW-outward triangles (right-hand normal points outward)
TRIS = [(0,3,2),(0,2,1),  # bottom (-Z)
        (4,5,6),(4,6,7),  # top (+Z)
        (0,1,5),(0,5,4),  # -Y
        (2,3,7),(2,7,6),  # +Y
        (1,2,6),(1,6,5),  # +X
        (0,4,7),(0,7,3)]  # -X
# trivial per-corner texcoords (solid colour parts, so values are cosmetic)
CORNER_UV = [(0,0),(1,0),(1,1),(0,1),(0,0),(1,0),(1,1),(0,1)]

positions=[]; texcoords=[]; blendidx=[]; blendwt=[]; triangles=[]
for (ji, half, off, mat) in PARTS:
    jw = joint_world(ji)
    base_vtx = len(positions)
    for k,(cx,cy,cz) in enumerate(CORNERS):
        positions.append((jw[0]+off[0]+cx*half[0],
                          jw[1]+off[1]+cy*half[1],
                          jw[2]+off[2]+cz*half[2]))
        texcoords.append(CORNER_UV[k])
        blendidx.append((ji,0,0,0))
        blendwt.append((255,0,0,0))
    for (a,b,c) in TRIS:
        triangles.append((base_vtx+a, base_vtx+b, base_vtx+c))

# meshes: each PART is one mesh covering its 8 verts / 12 tris
meshes=[]; vcur=0; tcur=0
for (ji, half, off, mat) in PARTS:
    meshes.append((mat, 8, 12)); # (material, nverts, ntris) ; first computed below

numverts = len(positions); numtris = len(triangles)

# ---- string table ----
strings = b"\0"  # offset 0 is empty string
def add_str(s):
    global strings
    off = len(strings)
    strings += s.encode("ascii") + b"\0"
    return off
joint_name_ofs = [add_str(n) for (n,_,_) in JOINTS]
mesh_name_ofs  = [add_str("mesh%d"%i) for i in range(len(PARTS))]
mesh_mat_ofs   = [add_str(m) for (_,_,_,m) in PARTS]

# ---- build lumps as bytes; track offsets ----
def align(n, a=8):  # pad helper
    return (n + (a-1)) & ~(a-1)

HDR_SIZE = 124
buf = bytearray()
def put(off_list_key=None):
    return len(buf)

# Layout order: header | text | meshes | vertexarrays | vertexdata | triangles | joints
off = HDR_SIZE
# text
ofs_text = off; off += len(strings); off = align(off)
# meshes (24 bytes each)
ofs_meshes = off; off += 24*len(PARTS); off = align(off)
# vertexarrays (5 fields*4 = 20 bytes each); we emit 4 arrays: pos, uv, bidx, bwt
NUM_VA = 4
ofs_vas = off; off += 20*NUM_VA; off = align(off)
# vertex data (planar): pos(float3), uv(float2), bidx(ubyte4), bwt(ubyte4)
ofs_pos = off;  off += numverts*12
ofs_uv  = off;  off += numverts*8
ofs_bi  = off;  off += numverts*4
ofs_bw  = off;  off += numverts*4
off = align(off)
# triangles (12 bytes each)
ofs_tris = off; off += numtris*12; off = align(off)
# joints (56 bytes each)
ofs_joints = off; off += 56*len(JOINTS); off = align(off)
filesize = off

# header
hdr = struct.pack("<16s", b"INTERQUAKEMODEL\0")
hdr += struct.pack("<III", 2, filesize, 0)                 # version, filesize, flags
hdr += struct.pack("<II", len(strings), ofs_text)          # text
hdr += struct.pack("<II", len(PARTS), ofs_meshes)          # meshes
hdr += struct.pack("<III", NUM_VA, numverts, ofs_vas)      # vertexarrays, num_vertexes, ofs
hdr += struct.pack("<III", numtris, ofs_tris, 0)           # triangles, ofs_triangles, ofs_adjacency
hdr += struct.pack("<II", len(JOINTS), ofs_joints)         # joints
hdr += struct.pack("<II", 0, 0)                            # poses
hdr += struct.pack("<II", 0, 0)                            # anims
hdr += struct.pack("<IIII", 0, 0, 0, 0)                    # frames, num_framechannels, ofs_frames, ofs_bounds
hdr += struct.pack("<II", 0, 0)                            # comment
hdr += struct.pack("<II", 0, 0)                            # extensions
assert len(hdr)==HDR_SIZE, len(hdr)

out = bytearray(filesize)
out[0:HDR_SIZE] = hdr
out[ofs_text:ofs_text+len(strings)] = strings
# meshes
p = ofs_meshes; fv=0; ft=0
for i,(mat,nv,nt) in enumerate(meshes):
    out[p:p+24] = struct.pack("<IIIIII", mesh_name_ofs[i], mesh_mat_ofs[i], fv, nv, ft, nt)
    p+=24; fv+=nv; ft+=nt
# vertex arrays: type, flags, format, size, offset   (IQM_FLOAT=7, IQM_UBYTE=1)
IQM_POSITION,IQM_TEXCOORD,IQM_BLENDINDEXES,IQM_BLENDWEIGHTS = 0,1,4,5
IQM_UBYTE,IQM_FLOAT = 1,7
p = ofs_vas
out[p:p+20] = struct.pack("<IIIII", IQM_POSITION,     0, IQM_FLOAT, 3, ofs_pos); p+=20
out[p:p+20] = struct.pack("<IIIII", IQM_TEXCOORD,     0, IQM_FLOAT, 2, ofs_uv);  p+=20
out[p:p+20] = struct.pack("<IIIII", IQM_BLENDINDEXES, 0, IQM_UBYTE, 4, ofs_bi);  p+=20
out[p:p+20] = struct.pack("<IIIII", IQM_BLENDWEIGHTS, 0, IQM_UBYTE, 4, ofs_bw);  p+=20
# vertex data
p=ofs_pos
for (x,y,z) in positions: out[p:p+12]=struct.pack("<fff",x,y,z); p+=12
p=ofs_uv
for (s,t) in texcoords: out[p:p+8]=struct.pack("<ff",s,t); p+=8
p=ofs_bi
for b in blendidx: out[p:p+4]=struct.pack("<BBBB",*b); p+=4
p=ofs_bw
for w in blendwt: out[p:p+4]=struct.pack("<BBBB",*w); p+=4
# triangles
p=ofs_tris
for (a,b,c) in triangles: out[p:p+12]=struct.pack("<III",a,b,c); p+=12
# joints: name, parent, translate3, rotate4(xyzw), scale3
p=ofs_joints
for i,(name,parent,t) in enumerate(JOINTS):
    out[p:p+56]=struct.pack("<iiffffffffff", joint_name_ofs[i], parent,
        t[0],t[1],t[2], 0.0,0.0,0.0,1.0, 1.0,1.0,1.0); p+=56

os.makedirs(os.path.dirname(OUT_IQM), exist_ok=True)
with open(OUT_IQM,"wb") as f: f.write(out)
with open(OUT_ACTOR,"w") as f:
    f.write('actor {\n'
            '    head_joint  "head"\n'
            '    eye_joints  "eye.L eye.R"\n'
            '    chest_joint "chest"\n'
            '    gaze_yaw 50  gaze_pitch 30\n'
            '    eye_yaw 25   eye_pitch 20\n'
            '}\n')
print("wrote", OUT_IQM, filesize, "bytes;", numverts, "verts", numtris, "tris", len(JOINTS), "joints")
```

- [ ] **Step 2: Run it**

Run: `python3 scripts/make_test_actor_iqm.py`
Expected: `wrote .../id1/actors/dummy.iqm 1268 bytes; 40 verts 60 tris 5 joints`

- [ ] **Step 3: Sanity-check the magic**

Run: `head -c 16 id1/actors/dummy.iqm`
Expected: `INTERQUAKEMODEL`

- [ ] **Step 4: Commit**

```bash
git add scripts/make_test_actor_iqm.py id1/actors/dummy.iqm id1/actors/dummy.actor
git commit -m "feat(actors): bootstrap cube test actor IQM generator + asset"
```

---

## Task 1: libmodel IQM reader

**Files:**
- Create: `sdlquake/libmodel/iqm.h`
- Create: `sdlquake/libmodel/iqm.c`
- Modify: `build.zig` (add `iqm.c`)

- [ ] **Step 1: Write `iqm.h`**

Create `sdlquake/libmodel/iqm.h`:

```c
#ifndef LIBMODEL_IQM_H
#define LIBMODEL_IQM_H
#include <stddef.h>
#include "qalloc.h"
#include "libmodel.h"   /* for lm_result_t + qalloc_t */

typedef struct { float pos[3]; float st[2]; unsigned char bone; } lm_iqm_vert_t;
typedef struct {
    char  name[64];
    int   parent;                 /* -1 = root */
    float translate[3];
    float rotate[4];              /* quaternion x,y,z,w */
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
    qalloc_t        alloc;
} lm_iqm_t;

lm_result_t lm_load_iqm(const void *buf, size_t len,
                        const qalloc_t *alloc, lm_iqm_t **out);
void        lm_iqm_free(lm_iqm_t *m);

#endif
```

- [ ] **Step 2: Write `iqm.c`**

Create `sdlquake/libmodel/iqm.c`:

```c
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
enum { IQM_BYTE=0, IQM_UBYTE=1, IQM_SHORT=2, IQM_USHORT=3,
       IQM_INT=4, IQM_UINT=5, IQM_HALF=6, IQM_FLOAT=7, IQM_DOUBLE=8 };

#define HDR 124
/* header field offsets (bytes) */
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

static const char *strat(const unsigned char *base, size_t len,
                         unsigned ofs_text, unsigned num_text, unsigned nameofs){
    if (nameofs >= num_text) return "";
    return (const char *)(base + ofs_text + nameofs);
}

lm_result_t lm_load_iqm(const void *vbuf, size_t len,
                        const qalloc_t *ain, lm_iqm_t **out){
    qalloc_t a = ain ? *ain : qalloc_malloc();
    const unsigned char *b = (const unsigned char*)vbuf;
    lm_iqm_t *m; int i, j;
    unsigned ofs_text,num_text,ofs_mesh,num_mesh,num_va,num_vert,ofs_va;
    unsigned num_tri,ofs_tri,num_joint,ofs_joint;
    unsigned ofs_pos=0,ofs_uv=0,ofs_bi=0; int have_pos=0;

    *out = NULL;
    if (len < HDR) return LM_ERR_TRUNCATED;
    if (memcmp(b, "INTERQUAKEMODEL\0", 16) != 0) return LM_ERR_BAD_MAGIC;
    if (rdu(b+H_VERSION) != 2) return LM_ERR_BAD_VERSION;
    if (rdu(b+H_FILESIZE) > len) return LM_ERR_TRUNCATED;

    num_text =rdu(b+H_NUM_TEXT);  ofs_text =rdu(b+H_OFS_TEXT);
    num_mesh =rdu(b+H_NUM_MESH);  ofs_mesh =rdu(b+H_OFS_MESH);
    num_va   =rdu(b+H_NUM_VA);    num_vert =rdu(b+H_NUM_VERT);  ofs_va=rdu(b+H_OFS_VA);
    num_tri  =rdu(b+H_NUM_TRI);   ofs_tri  =rdu(b+H_OFS_TRI);
    num_joint=rdu(b+H_NUM_JOINT); ofs_joint=rdu(b+H_OFS_JOINT);
    if (!num_mesh || !num_vert || !num_tri) return LM_ERR_BAD_COUNT;

    /* locate the vertex arrays we care about (planar) */
    for (i=0;i<(int)num_va;i++){
        const unsigned char *va = b + ofs_va + i*20;
        unsigned type=rdu(va), fmt=rdu(va+8), size=rdu(va+12), ofs=rdu(va+16);
        if (type==IQM_POSITION && fmt==IQM_FLOAT && size==3){ ofs_pos=ofs; have_pos=1; }
        else if (type==IQM_TEXCOORD && fmt==IQM_FLOAT && size==2){ ofs_uv=ofs; }
        else if (type==IQM_BLENDINDEXES && fmt==IQM_UBYTE && size==4){ ofs_bi=ofs; }
    }
    if (!have_pos) return LM_ERR_BAD_COUNT;

    m = QALLOC(&a, lm_iqm_t);
    if (!m) return LM_ERR_OOM;
    memset(m,0,sizeof(*m));
    m->alloc = a;
    m->nummeshes=(int)num_mesh; m->numjoints=(int)num_joint;
    m->numverts=(int)num_vert;  m->numtris=(int)num_tri;
    m->meshes = QALLOC_N(&a, lm_iqm_mesh_t, num_mesh);
    m->joints = num_joint? QALLOC_N(&a, lm_iqm_joint_t, num_joint) : NULL;
    m->verts  = QALLOC_N(&a, lm_iqm_vert_t, num_vert);
    m->tris   = QALLOC_N(&a, unsigned, num_tri*3);
    if (!m->meshes || !m->verts || !m->tris || (num_joint && !m->joints)){
        lm_iqm_free(m); return LM_ERR_OOM;
    }

    for (i=0;i<(int)num_mesh;i++){
        const unsigned char *me = b + ofs_mesh + i*24;
        strncpy(m->meshes[i].name,     strat(b,len,ofs_text,num_text,rdu(me)),   63);
        strncpy(m->meshes[i].material, strat(b,len,ofs_text,num_text,rdu(me+4)), 63);
        m->meshes[i].first_vertex  =rdu(me+8);  m->meshes[i].num_vertexes =rdu(me+12);
        m->meshes[i].first_triangle=rdu(me+16); m->meshes[i].num_triangles=rdu(me+20);
    }
    for (i=0;i<(int)num_joint;i++){
        const unsigned char *jo = b + ofs_joint + i*56;
        strncpy(m->joints[i].name, strat(b,len,ofs_text,num_text,rdu(jo)), 63);
        m->joints[i].parent = rdi(jo+4);
        for (j=0;j<3;j++) m->joints[i].translate[j]=rdf(jo+8+j*4);
        for (j=0;j<4;j++) m->joints[i].rotate[j]   =rdf(jo+20+j*4);
        for (j=0;j<3;j++) m->joints[i].scale[j]    =rdf(jo+36+j*4);
    }
    for (i=0;i<(int)num_vert;i++){
        const unsigned char *pp = b + ofs_pos + i*12;
        m->verts[i].pos[0]=rdf(pp); m->verts[i].pos[1]=rdf(pp+4); m->verts[i].pos[2]=rdf(pp+8);
        if (ofs_uv){ const unsigned char *uu=b+ofs_uv+i*8; m->verts[i].st[0]=rdf(uu); m->verts[i].st[1]=rdf(uu+4); }
        else { m->verts[i].st[0]=m->verts[i].st[1]=0.f; }
        m->verts[i].bone = ofs_bi ? b[ofs_bi+i*4] : 0;
    }
    for (i=0;i<(int)num_tri;i++){
        const unsigned char *tt = b + ofs_tri + i*12;
        m->tris[i*3+0]=rdu(tt); m->tris[i*3+1]=rdu(tt+4); m->tris[i*3+2]=rdu(tt+8);
    }
    /* bounds from vertices */
    m->mins[0]=m->mins[1]=m->mins[2]= 1e30f;
    m->maxs[0]=m->maxs[1]=m->maxs[2]=-1e30f;
    for (i=0;i<(int)num_vert;i++) for(j=0;j<3;j++){
        float v=m->verts[i].pos[j];
        if (v<m->mins[j]) m->mins[j]=v;
        if (v>m->maxs[j]) m->maxs[j]=v;
    }
    *out = m;
    return LM_OK;
}

void lm_iqm_free(lm_iqm_t *m){
    if (!m) return;
    qalloc_t a = m->alloc;
    QFREE(&a, m->meshes); QFREE(&a, m->joints);
    QFREE(&a, m->verts);  QFREE(&a, m->tris);
    QFREE(&a, m);
}
```

> **Note:** Confirm the exact `QALLOC`/`QALLOC_N`/`QFREE`/`qalloc_malloc` macro/function names by reading `sdlquake/libqalloc/qalloc.h` and `sdlquake/libmodel/mdl.c` (the top of `mdl.c` shows the canonical usage). Match them. If `QALLOC_N(a,T,n)` does not exist, use whatever array-alloc helper `mdl.c` uses (e.g. `qalloc(a, n*sizeof(T))`).

- [ ] **Step 3: Register in build.zig**

In `build.zig`, find the `lib_core_flags` `addCSourceFiles` block (the one listing `sdlquake/libmodel/mdl.c`) and add the IQM source:

```zig
    mod.addCSourceFiles(.{
        .files = &.{
            "sdlquake/libqalloc/qalloc.c",
            "sdlquake/libmodel/mdl.c",
            "sdlquake/libmodel/iqm.c",
        },
        .flags = lib_core_flags,
    });
```

- [ ] **Step 4: Build**

Run: `zig build 2>&1 | tail -20`
Expected: builds with no errors referencing `iqm.c` (link of `lm_load_iqm` happens in Task 2; for now confirm it compiles).

- [ ] **Step 5: Commit**

```bash
git add sdlquake/libmodel/iqm.h sdlquake/libmodel/iqm.c build.zig
git commit -m "feat(libmodel): IQM v2 bind-pose reader (lm_load_iqm)"
```

---

## Task 2: Engine model type + loader glue

**Files:**
- Modify: `sdlquake/engine_src/model.h`
- Modify: `sdlquake/engine_src/model.c`

- [ ] **Step 1: Add the model type + data pointer (model.h)**

In `sdlquake/engine_src/model.h`, change the `modtype_t` enum (line 312):

```c
typedef enum {mod_brush, mod_sprite, mod_alias, mod_iqm} modtype_t;
```

And add a field to `model_t` right after the `cache` member (after line 401, before `palette_id`):

```c
	cache_user_t	cache;		// only access through Mod_Extradata

	struct lm_iqm_s	*iqmdata;	// mod_iqm: parsed IQM (hunk-allocated)
```

- [ ] **Step 2: Implement the loader (model.c)**

At the top of `sdlquake/engine_src/model.c`, near the other includes, add:

```c
#include "iqm.h"   /* libmodel IQM reader; on the lib_core include path */
```

> If `iqm.h` is not on the engine include path, reference it relative to libmodel (match how `model.c` includes `libmodel.h` — read the existing include line and mirror its path).

In `Mod_LoadModel`, the dispatch switch is at model.c:~305 (`switch (LittleLong(*(unsigned *)buf))` with `case IDPOLYHEADER`). Insert an IQM check **before** that switch (IQM's magic is a 16-byte string, not a 4-byte id):

```c
// IQM actors: 16-byte string magic, checked before the 4-byte id switch
	if (com_filesize >= 16 && !Q_strncmp((char *)buf, "INTERQUAKEMODEL", 15))
	{
		Mod_LoadIQMModel (mod, buf);
		return mod;
	}

	switch (LittleLong(*(unsigned *)buf))
	{
	...
```

Add the loader function (place it just before `Mod_LoadAliasModel`):

```c
/*
=================
Mod_LoadIQMModel
=================
*/
void Mod_LoadIQMModel (model_t *mod, void *buffer)
{
	lm_iqm_t	*iqm;
	lm_result_t	r;
	qalloc_t	scratch;
	int			j;

	scratch = qalloc_hunk (loadname);
	r = lm_load_iqm (buffer, (size_t)com_filesize, &scratch, &iqm);
	if (r != LM_OK)
		Sys_Error ("Mod_LoadIQMModel: %s: %s", mod->name, lm_strerror (r));

	mod->type     = mod_iqm;
	mod->iqmdata  = iqm;
	mod->flags    = 0;
	mod->numframes = 1;
	mod->synctype = ST_SYNC;

	for (j = 0; j < 3; j++)
	{
		mod->mins[j] = iqm->mins[j];
		mod->maxs[j] = iqm->maxs[j];
	}
	mod->radius = Length (mod->maxs);   // crude; fine for R1
}
```

Add a forward declaration near the top of model.c (with the other `Mod_Load*` prototypes, or in model.h alongside `Mod_LoadAliasModel`):

```c
void Mod_LoadIQMModel (model_t *mod, void *buffer);
```

> **Verify these symbols exist** by reading `Mod_LoadAliasModel` in model.c: `qalloc_hunk`, `lm_strerror`, `loadname`, `com_filesize`, `Q_strncmp`, `Length`, `ST_SYNC`. They are all used by the alias path / engine; mirror exactly.

- [ ] **Step 3: Build**

Run: `zig build 2>&1 | tail -20`
Expected: builds clean. (`lm_load_iqm` now links.)

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/model.h sdlquake/engine_src/model.c
git commit -m "feat(model): mod_iqm type + Mod_LoadIQMModel glue"
```

---

## Task 3: Dev module + `actor_dump` (parser end-to-end verification)

**Files:**
- Create: `sdlquake/engine_src/iqm_dev.h`
- Create: `sdlquake/engine_src/iqm_dev.c`
- Modify: `sdlquake/engine_src/cl_main.c` (call `IQMDev_Init`)
- Modify: `build.zig`

- [ ] **Step 1: Write `iqm_dev.h`**

```c
#ifndef IQM_DEV_H
#define IQM_DEV_H
void IQMDev_Init (void);        // register console commands
void IQMDev_AddToScene (void);  // inject the persistent dev actor into cl_visedicts
#endif
```

- [ ] **Step 2: Write `iqm_dev.c` (dump command only for this task)**

```c
#include "quakedef.h"
#include "iqm.h"
#include "iqm_dev.h"

static void Actor_Dump_f (void)
{
	char	name[MAX_QPATH];
	model_t	*mod;
	lm_iqm_t *iqm;
	int		i;

	if (Cmd_Argc() < 2) { Con_Printf ("usage: actor_dump <file.iqm>\n"); return; }
	Q_strcpy (name, Cmd_Argv(1));
	mod = Mod_ForName (name, false);
	if (!mod) { Con_Printf ("actor_dump: %s not found\n", name); return; }
	if (mod->type != mod_iqm) { Con_Printf ("actor_dump: %s is not an IQM\n", name); return; }
	iqm = mod->iqmdata;
	Con_Printf ("IQM %s: %d meshes, %d joints, %d verts, %d tris\n",
		name, iqm->nummeshes, iqm->numjoints, iqm->numverts, iqm->numtris);
	Con_Printf ("  mins %g %g %g  maxs %g %g %g\n",
		iqm->mins[0],iqm->mins[1],iqm->mins[2], iqm->maxs[0],iqm->maxs[1],iqm->maxs[2]);
	for (i = 0; i < iqm->numjoints; i++)
		Con_Printf ("  joint %d '%s' parent %d  t(%g %g %g)\n", i,
			iqm->joints[i].name, iqm->joints[i].parent,
			iqm->joints[i].translate[0],iqm->joints[i].translate[1],iqm->joints[i].translate[2]);
	for (i = 0; i < iqm->nummeshes; i++)
		Con_Printf ("  mesh %d '%s' mat '%s' v[%u+%u] t[%u+%u]\n", i,
			iqm->meshes[i].name, iqm->meshes[i].material,
			iqm->meshes[i].first_vertex, iqm->meshes[i].num_vertexes,
			iqm->meshes[i].first_triangle, iqm->meshes[i].num_triangles);
}

void IQMDev_Init (void)
{
	Cmd_AddCommand ("actor_dump", Actor_Dump_f);
}

void IQMDev_AddToScene (void) { /* implemented in Task 6 */ }
```

> Confirm `Q_strcpy`, `Cmd_Argc`, `Cmd_Argv`, `Mod_ForName`, `Con_Printf`, `MAX_QPATH` signatures via any existing `*_f` console command (e.g. in `cl_main.c`/`host_cmd.c`). `Mod_ForName(name, false)` returns NULL on missing when crash=false.

- [ ] **Step 3: Register source + init call**

In `build.zig`, add `"sdlquake/engine_src/iqm_dev.c"` to the engine source list (the `addCSourceFiles` block that lists the other `engine_src/*.c`; match its flags — the gnu89 engine flags).

In `sdlquake/engine_src/cl_main.c`, find `CL_Init` and add at its end:

```c
	IQMDev_Init ();
```

Add `#include "iqm_dev.h"` near the top of `cl_main.c`.

- [ ] **Step 4: Build + run the dump**

Run: `zig build 2>&1 | tail -20` → expect clean build.
Run the game and in the console:
```
actor_dump actors/dummy.iqm
```
Expected console output:
```
IQM actors/dummy.iqm: 5 meshes, 5 joints, 40 verts, 60 tris
  joint 0 'base' parent -1 ...
  joint 1 'chest' parent 0 ...
  ... (head, eye.L, eye.R)
  mesh 0 'mesh0' mat 'p_base' v[0+8] t[0+12]
  ...
```
This confirms the parser is correct end-to-end.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/iqm_dev.c sdlquake/engine_src/iqm_dev.h sdlquake/engine_src/cl_main.c build.zig
git commit -m "feat(iqm): actor_dump console command verifies the IQM reader"
```

---

## Task 4: Render path — `R_IQMDrawModel`

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (add transform + draw)
- Modify: `sdlquake/engine_src/r_local.h` (declare `R_IQMDrawModel`)

- [ ] **Step 1: Declare the entry point (r_local.h)**

In `sdlquake/engine_src/r_local.h`, near the `R_AliasDrawModel` declaration, add:

```c
void R_IQMDrawModel (alight_t *plighting);
```

- [ ] **Step 2: Add the IQM transform + draw (r_alias.c)**

At the bottom of `sdlquake/engine_src/r_alias.c`, add. This reuses the file-local `aliastransform`, `pfinalverts`/`pauxverts`, `ziscale`, `r_affinetridesc`, `acolormap`, and the projection globals (`aliasxscale/aliasxcenter/aliasyscale/aliasycenter`) already in this translation unit, plus `D_PolysetUpdateTables`/`D_PolysetDraw`.

```c
#include "iqm.h"   /* lm_iqm_t */

/*
================
R_IQMSetUpTransform
  Like R_AliasSetUpTransform but identity model scale (IQM verts are float
  model-units, already in actor space at bind pose).
================
*/
static void R_IQMSetUpTransform (void)
{
	int				i;
	float			rotationmatrix[3][4], t2matrix[3][4];
	static float	viewmatrix[3][4];
	vec3_t			angles;

	angles[ROLL]  = currententity->angles[ROLL];
	angles[PITCH] = -currententity->angles[PITCH];
	angles[YAW]   = currententity->angles[YAW];
	AngleVectors (angles, alias_forward, alias_right, alias_up);

	for (i = 0; i < 3; i++)
	{
		t2matrix[i][0] = alias_forward[i];
		t2matrix[i][1] = -alias_right[i];
		t2matrix[i][2] = alias_up[i];
	}
	t2matrix[0][3] = -modelorg[0];
	t2matrix[1][3] = -modelorg[1];
	t2matrix[2][3] = -modelorg[2];

	/* no model-space scale/origin: rotationmatrix == t2matrix */
	memcpy (rotationmatrix, t2matrix, sizeof(rotationmatrix));

	VectorCopy (vright, viewmatrix[0]);
	VectorCopy (vup,    viewmatrix[1]);
	VectorInverse (viewmatrix[1]);
	VectorCopy (vpn,    viewmatrix[2]);

	R_ConcatTransforms (viewmatrix, rotationmatrix, aliastransform);
}

/*
================
R_IQMDrawModel
  Bind-pose rigid render: transform each actor-space vertex to view space,
  project, and rasterize each mesh's triangles with a flat per-mesh skin.
  Skips triangles that touch the near plane or the screen edge (no clipping in
  R1 — keep the test actor centred).
================
*/
void R_IQMDrawModel (alight_t *plighting)
{
	finalvert_t	finalverts[MAXALIASVERTS +
					((CACHE_SIZE - 1) / sizeof(finalvert_t)) + 1];
	auxvert_t	auxverts[MAXALIASVERTS];
	lm_iqm_t	*iqm;
	int			i, mi, t, light;
	static byte	meshskin[16];   /* 4x4 solid-colour skin scratch */
	static const byte meshcolor[8] = { 0x6d, 0x52, 0x0b, 0xf3, 0x40, 0x20, 0xfe, 0x10 };

	r_amodels_drawn++;

	pfinalverts = (finalvert_t *)
		(((size_t)&finalverts[0] + CACHE_SIZE - 1) & ~((size_t)(CACHE_SIZE - 1)));
	pauxverts = &auxverts[0];

	iqm = currententity->model->iqmdata;
	if (!iqm) return;
	if (iqm->numverts > MAXALIASVERTS) return;   // R1 cap

	R_IQMSetUpTransform ();
	R_AliasSetupLighting (plighting);            // sets r_ambientlight/shade/plightvec
	light = r_ambientlight;                        // R1: flat lit

	acolormap = currententity->colormap;
	if (!acolormap) acolormap = vid.colormap;

	/* transform + project every vertex once (global index space) */
	for (i = 0; i < iqm->numverts; i++)
	{
		finalvert_t	*fv = &pfinalverts[i];
		float		*p = iqm->verts[i].pos;
		float		vx, vy, vz, zi;

		vx = DotProduct(p, aliastransform[0]) + aliastransform[0][3];
		vy = DotProduct(p, aliastransform[1]) + aliastransform[1][3];
		vz = DotProduct(p, aliastransform[2]) + aliastransform[2][3];

		fv->flags = 0;
		if (vz < ALIAS_Z_CLIP_PLANE) { fv->flags = ALIAS_Z_CLIP; continue; }

		zi = 1.0f / vz;
		fv->v[5] = (int)(zi * ziscale);
		fv->v[0] = (int)((vx * aliasxscale * zi) + aliasxcenter);
		fv->v[1] = (int)((vy * aliasyscale * zi) + aliasycenter);
		fv->v[2] = (int)(iqm->verts[i].st[0] * 4.0f) << 16;   /* skinwidth=4 */
		fv->v[3] = (int)(iqm->verts[i].st[1] * 4.0f) << 16;   /* skinheight=4 */
		fv->v[4] = light;

		if (fv->v[0] < r_refdef.vrect.x ||
			fv->v[0] >= r_refdef.vrect.x + r_refdef.vrect.width ||
			fv->v[1] < r_refdef.vrect.y ||
			fv->v[1] >= r_refdef.vrect.y + r_refdef.vrect.height)
			fv->flags |= ALIAS_XY_CLIP_MASK;
	}

	r_affinetridesc.drawtype    = 0;
	r_affinetridesc.seamfixupX16= 0;
	r_affinetridesc.pfinalverts = pfinalverts;
	r_affinetridesc.skinwidth   = 4;
	r_affinetridesc.skinheight  = 4;

	for (mi = 0; mi < iqm->nummeshes; mi++)
	{
		lm_iqm_mesh_t	*me = &iqm->meshes[mi];
		byte			col = meshcolor[mi & 7];
		mtriangle_t		tri;

		memset (meshskin, col, sizeof(meshskin));
		r_affinetridesc.pskin = meshskin;
		D_PolysetUpdateTables ();

		tri.facesfront = 1;
		r_affinetridesc.ptriangles  = &tri;
		r_affinetridesc.numtriangles= 1;

		for (t = 0; t < (int)me->num_triangles; t++)
		{
			unsigned	*idx = &iqm->tris[(me->first_triangle + t) * 3];
			finalvert_t	*a = &pfinalverts[idx[0]];
			finalvert_t	*b = &pfinalverts[idx[1]];
			finalvert_t	*c = &pfinalverts[idx[2]];

			if ((a->flags | b->flags | c->flags) & (ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP))
				continue;   /* R1: skip any clipped triangle */

			tri.vertindex[0] = idx[0];
			tri.vertindex[1] = idx[1];
			tri.vertindex[2] = idx[2];
			D_PolysetDraw ();
		}
	}
}
```

> **Symbol checks (read the file/headers to confirm before relying on them):**
> - `MAXALIASVERTS`, `CACHE_SIZE`, `ALIAS_Z_CLIP_PLANE`, `ALIAS_Z_CLIP`, `ALIAS_XY_CLIP_MASK` — in `r_local.h`/`r_shared.h` (used by alias code in this file).
> - `aliasxscale`, `aliasxcenter`, `aliasyscale`, `aliasycenter`, `ziscale` — `ziscale` is the file-static in r_alias.c:48; the `alias*` are externs used by `R_AliasProjectFinalVert` above.
> - `r_refdef.vrect.{x,y,width,height}` — confirm field names in `render.h` (`vrect_t`). If they differ (e.g. `vrectright/vrectbottom`), adjust the bounds test.
> - `vid.colormap`, `currententity->colormap` — colormap base; both valid.
> - Winding: `D_DrawNonSubdiv` back-face culls with `d_xdenom >= 0`. If the cube renders **inside-out** (you see the far faces), swap `tri.vertindex[1]`/`[2]`, or flip `TRIS` winding in the generator and regenerate.

- [ ] **Step 3: Build**

Run: `zig build 2>&1 | tail -20`
Expected: clean. (No visual yet — wired in Task 5/6.)

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_alias.c sdlquake/engine_src/r_local.h
git commit -m "feat(render): R_IQMDrawModel — bind-pose rigid IQM rasterization"
```

---

## Task 5: Hook IQM into the entity render switch

**Files:**
- Modify: `sdlquake/engine_src/r_main.c`

- [ ] **Step 1: Add the dispatch case**

In `R_DrawEntitiesOnList` (r_main.c), the `switch (currententity->model->type)` has a `case mod_alias:` block that computes `lighting` then calls `R_AliasDrawModel (&lighting)`. Add a parallel case. The simplest correct form reuses the same lighting computation:

```c
		case mod_iqm:
			VectorCopy (currententity->origin, r_entorigin);
			VectorSubtract (r_origin, r_entorigin, modelorg);

			j = R_LightPoint (currententity->origin);
			lighting.ambientlight = j;
			lighting.shadelight = j;
			lighting.plightvec = lightvec;

			for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
			{
				if (cl_dlights[lnum].die >= cl.time)
				{
					VectorSubtract (currententity->origin,
									cl_dlights[lnum].origin, dist);
					add = cl_dlights[lnum].radius - Length(dist);
					if (add > 0)
						lighting.ambientlight += add;
				}
			}
			if (lighting.ambientlight > 128)
				lighting.ambientlight = 128;
			if (lighting.ambientlight + lighting.shadelight > 192)
				lighting.shadelight = 192 - lighting.ambientlight;

			R_IQMDrawModel (&lighting);
			break;
```

Place it immediately after the `case mod_alias:` block (so `j`, `lnum`, `dist`, `add`, `lightvec` are already declared in this function — confirm by reading the top of `R_DrawEntitiesOnList`).

- [ ] **Step 2: Build**

Run: `zig build 2>&1 | tail -20`
Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine_src/r_main.c
git commit -m "feat(render): dispatch mod_iqm in R_DrawEntitiesOnList"
```

---

## Task 6: Dev-spawn — persistent client entity

**Files:**
- Modify: `sdlquake/engine_src/iqm_dev.c` (spawn/clear + scene injection)
- Modify: `sdlquake/engine_src/cl_main.c` (call `IQMDev_AddToScene`)

- [ ] **Step 1: Implement spawn/clear + injection (iqm_dev.c)**

Replace the stub `IQMDev_AddToScene` and add the two commands. Add to the top of `iqm_dev.c` (after includes):

```c
static entity_t	dev_actor;
static qboolean	dev_actor_active = false;
```

Add the commands and injection:

```c
static void Actor_Spawn_f (void)
{
	char	name[MAX_QPATH];
	model_t	*mod;
	vec3_t	fwd, right, up;

	if (Cmd_Argc() < 2) { Con_Printf ("usage: actor_spawn <file.iqm> [x y z]\n"); return; }
	if (cls.state != ca_connected || !cl.worldmodel)
	{ Con_Printf ("actor_spawn: load a map first\n"); return; }

	Q_strcpy (name, Cmd_Argv(1));
	mod = Mod_ForName (name, false);
	if (!mod) { Con_Printf ("actor_spawn: %s not found\n", name); return; }

	memset (&dev_actor, 0, sizeof(dev_actor));
	dev_actor.model    = mod;
	dev_actor.colormap = vid.colormap;
	dev_actor.frame    = 0;

	if (Cmd_Argc() >= 5)
	{
		dev_actor.origin[0] = Q_atof (Cmd_Argv(2));
		dev_actor.origin[1] = Q_atof (Cmd_Argv(3));
		dev_actor.origin[2] = Q_atof (Cmd_Argv(4));
	}
	else
	{
		// 96 units in front of the player's view
		AngleVectors (cl.viewangles, fwd, right, up);
		VectorMA (r_refdef.vieworg, 96, fwd, dev_actor.origin);
		dev_actor.angles[YAW] = cl.viewangles[YAW] + 180;  // face the player
	}
	dev_actor_active = true;
	Con_Printf ("actor_spawn: %s at %g %g %g\n", name,
		dev_actor.origin[0], dev_actor.origin[1], dev_actor.origin[2]);
}

static void Actor_Clear_f (void)
{
	dev_actor_active = false;
	Con_Printf ("actor cleared\n");
}

void IQMDev_AddToScene (void)
{
	if (!dev_actor_active || !dev_actor.model) return;
	if (cl_numvisedicts >= MAX_VISEDICTS) return;
	cl_visedicts[cl_numvisedicts++] = &dev_actor;
}
```

Extend `IQMDev_Init`:

```c
void IQMDev_Init (void)
{
	Cmd_AddCommand ("actor_dump",  Actor_Dump_f);
	Cmd_AddCommand ("actor_spawn", Actor_Spawn_f);
	Cmd_AddCommand ("actor_clear", Actor_Clear_f);
}
```

> Confirm: `entity_t`, `cl_visedicts`, `cl_numvisedicts`, `MAX_VISEDICTS` (client.h); `cls.state`, `ca_connected`, `cl.worldmodel`, `cl.viewangles`, `r_refdef.vieworg`; `Q_atof`, `VectorMA`, `AngleVectors`. All standard. `r_refdef.vieworg` is set during rendering; if it reads as stale at command time, use the player entity origin (`cl_entities[cl.viewentity].origin`) instead — for a dev spawn either is acceptable.

- [ ] **Step 2: Call the injection each frame (cl_main.c)**

In `CL_ReadFromServer` (cl_main.c), right after `CL_UpdateTEnts ();` (which also populates `cl_visedicts`), add:

```c
	IQMDev_AddToScene ();
```

- [ ] **Step 3: Build + visual verification (the R1 acceptance test)**

Run: `zig build 2>&1 | tail -20` → clean.
Run the game on a map, e.g.:
```
zig build run -- +map start
```
In the console:
```
actor_spawn actors/dummy.iqm
```
Expected: a **blocky cube figure** (stacked coloured boxes: base, chest, head, two small eye boxes) appears ~96 units in front of you, facing you. Walk around it — it stays put and is solid (not inside-out). `actor_clear` removes it.

Capture a screenshot for the record (MCP `screenshot`, or in-game `screenshot`).

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/iqm_dev.c sdlquake/engine_src/cl_main.c
git commit -m "feat(iqm): actor_spawn dev command renders a cube IQM actor in-game"
```

---

## Task 7: Smoke test + update docs

- [ ] **Step 1: Full smoke test**

Run `zig build run -- +map start`, `actor_spawn actors/dummy.iqm`, confirm:
- figure renders correctly (right-side-out, lit, parts distinguishable),
- moving/rotating the view keeps it stable,
- `actor_clear` then `actor_spawn 0 0 40` (explicit coords) also works,
- stock monsters still render normally (spawn near a monster — `mod_alias` unaffected).

- [ ] **Step 2: Note R1 done in CLAUDE.md**

Add a short line under the Phase/architecture notes recording the new `mod_iqm` type, `libmodel/iqm.c`, `R_IQMDrawModel`, and the `actor_dump`/`actor_spawn` dev commands (mirror the existing terse style).

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude): note mod_iqm R1 (IQM load + static render)"
```

---

## Self-review notes (carried into execution)

- **Spec coverage:** R1 deliverable = "a many-part rigid actor stands assembled on screen" via dev-spawn → covered by Tasks 0–6. `.actor` parse/role-resolution is **deferred to R3** per the design (R1 only needs geometry); the manifest is generated (Task 0) and dumped is optional — do not block R1 on it.
- **Known R1 limitations (intended):** flat lighting (no per-vertex shading), solid-colour synthesized skins (no textures), clipped triangles skipped (keep actor centred), no animation/joint posing (bind pose only), IQM kept on hunk (reloaded across map changes via `needload`). All are picked up by later sub-projects (R2 anim, R3 face/skins, E-track authoring).
- **Biggest execution risks:** (1) generator byte-layout correctness — verified by `actor_dump` (Task 4) before any rendering; (2) triangle winding vs back-face cull — flip in generator if inside-out (Task 4/6 note); (3) exact qalloc macro names — confirm against `mdl.c` (Task 1 note); (4) `r_refdef.vrect` field names — confirm in render.h (Task 4 note).
```

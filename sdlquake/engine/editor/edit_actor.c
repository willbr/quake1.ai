// edit_actor.c -- Actor editor mode. Two sub-modes over the orbit-camera preview:
//   * Preview: load an IQM, watch it animate; the R3 head look-at tracks the
//     orbit camera; per-clip selector + look-at toggle.
//   * Edit geometry: make the loaded actor's parts editable and move/resize each
//     mesh with numeric fields, then re-save as an .iqm (via lm_write_iqm). The
//     editable copy is geometry-only (animation is dropped by the writer; clip
//     authoring is a later slice). Per the user: numeric fields first, edit the
//     loaded actor. Engine-side; mirrors edit_particle.c's mode pattern.

#include "quakedef.h"
#include "iqm.h"
#include "imgui_bridge.h"
#include "editor_mode.h"
#include "editor_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#define IG_TREE_DEFAULTOPEN (1<<5)
#define MAXEDITMESH 64

// ---- preview state --------------------------------------------------------
static entity_t  s_ent;                          // previewed (loaded) actor entity
static model_t  *s_mod;                           // loaded IQM model (NULL = none)
static char      s_path[MAX_QPATH] = "actors/dummy.iqm";

// ---- edit-geometry state --------------------------------------------------
static int       s_editmode;                      // 1 = editing geometry
static lm_iqm_t *s_orig;                           // pristine geometry copy (source of truth)
static lm_iqm_t *s_edit;                           // rendered copy (rebuilt from s_orig + xforms)
static model_t   s_emodel;                         // wraps s_edit for the renderer
static entity_t  s_eent;                           // edit-preview entity
static int       s_selmesh;                        // selected part
static float     s_voff[MAXEDITMESH][3];           // per-mesh move (offset)
static float     s_vscale[MAXEDITMESH][3];         // per-mesh size (scale about centroid)
static float     s_vrot[MAXEDITMESH][3];           // per-mesh rotate (euler pitch/yaw/roll, about centroid)
static char      s_savepath[MAX_QPATH] = "actors/dummy_edit.iqm";
static int       s_seljoint;                       // selected joint (skeleton editing)
static float     s_jnudge[3];                       // incremental joint-move nudge

// E2 animation timeline: sparse per-joint keys (euler rot + translate) over a
// clip; bake interpolates them into frametrs (the per-frame render/save target).
#define E2_MAXKEYS 32
typedef struct { int frame; float eul[3]; float tr[3]; } anim_key_t;
static anim_key_t s_jkeys[MAXEDITMESH][E2_MAXKEYS];
static int        s_jkeycnt[MAXEDITMESH];
static int        s_animclip;                       // clip being authored
static int        s_animframe;                      // current ABSOLUTE frame into frametrs
static float      s_poseeul[3];                      // pose-edit buffer (euler rot)
static float      s_posetr[3];                       // pose-edit buffer (translate)
static float      s_clipeul[3], s_cliptr[3];         // pose clipboard (copy/paste keys)
static int        s_clipvalid;
static int        s_ease;                            // 0 = linear, 1 = ease-in/out (smoothstep)

static void TX (const char *fmt, ...)             // formatted ImGui text helper
{
    char buf[256]; va_list ap;
    va_start (ap, fmt); vsnprintf (buf, sizeof(buf), fmt, ap); va_end (ap);
    IG_TextUnformatted (buf);
}

// ---- edit-geometry backend ------------------------------------------------
static void edit_free (void)
{
    if (s_orig) { lm_iqm_free (s_orig); s_orig = NULL; }
    if (s_edit) { lm_iqm_free (s_edit); s_edit = NULL; }
    s_editmode = 0;
}

// Rebuild s_edit's vertices from s_orig + per-mesh move/size (scale about the
// part's centroid). Cheap (a few hundred verts); run every edit frame.
static void edit_rebuild (void)
{
    int mi;
    if (!s_orig || !s_edit) return;
    for (mi = 0; mi < s_orig->nummeshes && mi < MAXEDITMESH; mi++)
    {
        lm_iqm_mesh_t *me = &s_orig->meshes[mi];
        unsigned a = me->first_vertex, n = me->num_vertexes, k;
        float c[3] = { 0, 0, 0 };
        for (k = 0; k < n; k++)
            { c[0]+=s_orig->verts[a+k].pos[0]; c[1]+=s_orig->verts[a+k].pos[1]; c[2]+=s_orig->verts[a+k].pos[2]; }
        if (n) { c[0]/=n; c[1]/=n; c[2]/=n; }
        {
            // rotation basis (engine convention: columns fwd / -right / up)
            vec3_t fwd, right, up;
            int    rot = (s_vrot[mi][0]||s_vrot[mi][1]||s_vrot[mi][2]);
            if (rot) AngleVectors (s_vrot[mi], fwd, right, up);
            for (k = 0; k < n; k++)
            {
                vec3_t d; int j;
                for (j = 0; j < 3; j++) d[j] = s_vscale[mi][j] * (s_orig->verts[a+k].pos[j] - c[j]);
                if (rot)
                    for (j = 0; j < 3; j++)
                        s_edit->verts[a+k].pos[j] = c[j] + d[0]*fwd[j] - d[1]*right[j] + d[2]*up[j] + s_voff[mi][j];
                else
                    for (j = 0; j < 3; j++)
                        s_edit->verts[a+k].pos[j] = c[j] + d[j] + s_voff[mi][j];
            }
        }
    }
    // refresh model bounds for culling/scale sanity
    {
        int i, j;
        for (j = 0; j < 3; j++) { s_edit->mins[j] = 1e30f; s_edit->maxs[j] = -1e30f; }
        for (i = 0; i < s_edit->numverts; i++) for (j = 0; j < 3; j++)
            { float v = s_edit->verts[i].pos[j];
              if (v < s_edit->mins[j]) s_edit->mins[j] = v;
              if (v > s_edit->maxs[j]) s_edit->maxs[j] = v; }
        VectorCopy (s_edit->mins, s_emodel.mins);
        VectorCopy (s_edit->maxs, s_emodel.maxs);
    }
}

// Enter edit mode: deep-copy the loaded geometry (round-trip through the IQM
// writer/loader gives two independent malloc-backed copies) and reset xforms.
static void edit_begin (void)
{
    void *buf = NULL; size_t len = 0; int mi;
    edit_free ();
    if (!s_mod || !s_mod->iqmdata) return;
    if (lm_write_iqm (s_mod->iqmdata, &buf, &len) != LM_OK) return;
    if (lm_load_iqm (buf, len, 0, &s_orig) != LM_OK) { free (buf); s_orig = NULL; return; }
    if (lm_load_iqm (buf, len, 0, &s_edit) != LM_OK) { free (buf); lm_iqm_free (s_orig); s_orig = NULL; return; }
    free (buf);

    for (mi = 0; mi < MAXEDITMESH; mi++)
        { s_voff[mi][0]=s_voff[mi][1]=s_voff[mi][2]=0.0f;
          s_vscale[mi][0]=s_vscale[mi][1]=s_vscale[mi][2]=1.0f;
          s_vrot[mi][0]=s_vrot[mi][1]=s_vrot[mi][2]=0.0f; }
    s_selmesh = 0;
    memset (s_jkeycnt, 0, sizeof(s_jkeycnt));   // E2: no keys until authored
    s_animclip = 0;
    s_animframe = (s_edit && s_edit->numclips > 0) ? s_edit->clips[0].first_frame : 0;

    memset (&s_emodel, 0, sizeof(s_emodel));
    strncpy (s_emodel.name, "*actor_edit", sizeof(s_emodel.name)-1);
    s_emodel.type    = mod_iqm;
    s_emodel.iqmdata = s_edit;
    memset (&s_eent, 0, sizeof(s_eent));
    s_eent.model    = &s_emodel;
    s_eent.colormap = vid.colormap;
    s_editmode = 1;
    edit_rebuild ();
}

// In-place delete of mesh `mi` from an lm_iqm_t: drop its vertex/triangle ranges,
// reindex the remaining triangles, and fix later meshes' first_vertex/_triangle.
// Counts shrink; the qalloc backing keeps its (now-larger) allocation. Joints and
// animation are untouched (the part's joint just becomes unused).
static void iqm_delete_mesh (lm_iqm_t *m, int mi)
{
    lm_iqm_mesh_t *me;
    unsigned vf, vc, tf, tc, k;
    int i;
    if (!m || mi < 0 || mi >= m->nummeshes || m->nummeshes <= 1) return;
    me = &m->meshes[mi];
    vf = me->first_vertex;   vc = me->num_vertexes;
    tf = me->first_triangle; tc = me->num_triangles;

    for (k = vf; k + vc < (unsigned)m->numverts; k++) m->verts[k] = m->verts[k+vc];
    m->numverts -= (int)vc;
    for (k = tf*3; k + tc*3 < (unsigned)m->numtris*3; k++) m->tris[k] = m->tris[k+tc*3];
    m->numtris -= (int)tc;
    for (k = 0; k < (unsigned)m->numtris*3; k++)
        if (m->tris[k] >= vf+vc) m->tris[k] -= vc;
    for (i = mi; i+1 < m->nummeshes; i++) {
        m->meshes[i] = m->meshes[i+1];
        m->meshes[i].first_vertex   -= vc;
        m->meshes[i].first_triangle -= tc;
    }
    m->nummeshes--;
}

static void edit_delete (int mi)
{
    int t;
    if (!s_editmode || !s_orig || !s_edit) return;
    if (mi < 0 || mi >= s_orig->nummeshes) return;
    if (s_orig->nummeshes <= 1) { Con_Printf ("actor edit: can't delete the last part\n"); return; }
    iqm_delete_mesh (s_orig, mi);
    iqm_delete_mesh (s_edit, mi);
    for (t = mi; t+1 < MAXEDITMESH; t++) {   // shift per-part transforms down
        VectorCopy (s_voff[t+1],   s_voff[t]);
        VectorCopy (s_vscale[t+1], s_vscale[t]);
        VectorCopy (s_vrot[t+1],   s_vrot[t]);
    }
    if (s_selmesh >= s_orig->nummeshes) s_selmesh = s_orig->nummeshes - 1;
    edit_rebuild ();
}

static void edit_add_box (void)
{
    vec3_t c, h; int idx;
    if (!s_editmode || !s_orig || !s_edit) return;
    if (s_orig->nummeshes >= MAXEDITMESH) { Con_Printf ("actor edit: too many parts\n"); return; }
    c[0] = (s_orig->mins[0]+s_orig->maxs[0])*0.5f;
    c[1] = (s_orig->mins[1]+s_orig->maxs[1])*0.5f;
    c[2] = (s_orig->mins[2]+s_orig->maxs[2])*0.5f;
    h[0] = h[1] = h[2] = 8.0f;
    idx = s_orig->nummeshes;
    if (lm_iqm_add_box (s_orig, c, h, 0, "p_new") != LM_OK) return;
    if (lm_iqm_add_box (s_edit, c, h, 0, "p_new") != LM_OK) return;
    s_voff[idx][0]=s_voff[idx][1]=s_voff[idx][2]=0.0f;
    s_vscale[idx][0]=s_vscale[idx][1]=s_vscale[idx][2]=1.0f;
    s_vrot[idx][0]=s_vrot[idx][1]=s_vrot[idx][2]=0.0f;
    s_selmesh = idx;
    edit_rebuild ();
}

// Bind a part to a joint: set every vertex's blend index so the part follows
// that joint under animation (the "parent"/rig op). bone lives in the vert, so
// it persists through edit_rebuild (which only rewrites positions) and the save.
static int edit_part_joint (int mi)
{
    if (!s_orig || mi < 0 || mi >= s_orig->nummeshes) return 0;
    return s_orig->verts[s_orig->meshes[mi].first_vertex].bone;
}
static void edit_bind (int mi, int joint)
{
    lm_iqm_mesh_t *me; unsigned k;
    if (!s_editmode || !s_orig || !s_edit) return;
    if (mi < 0 || mi >= s_orig->nummeshes) return;
    if (joint < 0) joint = 0;
    if (joint >= s_orig->numjoints) joint = s_orig->numjoints - 1;
    me = &s_orig->meshes[mi];
    for (k = 0; k < me->num_vertexes; k++) {
        s_orig->verts[me->first_vertex+k].bone = (unsigned char)joint;
        s_edit->verts[me->first_vertex+k].bone = (unsigned char)joint;
    }
}

static int joint_in_subtree (lm_iqm_t *m, int d, int root)
{
    int guard = 0;
    while (d >= 0 && guard++ < 256) { if (d == root) return 1; d = m->joints[d].parent; }
    return 0;
}

// Move a joint with **move-the-whole-assembly** semantics (vs re-pivot): the
// joint, the part(s) bound to it, and its whole subtree shift by `delta`. We move
// the subtree's *vertices* AND the joint's bind+anim translate together, so the
// skin matrix (curwld . bindinv) stays clean (moving only the joint cancels out —
// that was the no-op). To switch to re-pivot semantics later, move just the bind
// translate and counter-rotate the children — a different op.
static void edit_joint_move (int J, const vec3_t delta)
{
    int v, fr, c, nj;
    if (!s_editmode || !s_orig || !s_edit) return;
    if (J < 0 || J >= s_orig->numjoints) return;
    nj = s_orig->numjoints;
    for (v = 0; v < s_orig->numverts; v++)
        if (joint_in_subtree (s_orig, s_orig->verts[v].bone, J))
            for (c = 0; c < 3; c++) s_orig->verts[v].pos[c] += delta[c];
    for (c = 0; c < 3; c++) { s_orig->joints[J].translate[c]+=delta[c]; s_edit->joints[J].translate[c]+=delta[c]; }
    if (s_orig->frametrs) for (fr=0; fr<s_orig->numframes; fr++) for (c=0;c<3;c++) s_orig->frametrs[((size_t)fr*nj+J)*10+c]+=delta[c];
    if (s_edit->frametrs) for (fr=0; fr<s_edit->numframes; fr++) for (c=0;c<3;c++) s_edit->frametrs[((size_t)fr*nj+J)*10+c]+=delta[c];
    edit_rebuild ();   // propagate the s_orig vertex move into the rendered copy
}

// Re-pivot a joint: move its rotation centre WITHOUT moving the mesh. Shift the
// joint's bind+anim translate (not the verts) — the skin matrix stays identity at
// rest (mesh unchanged), but the joint origin moves, so future rotations of this
// joint pivot around the new point. The complement to edit_joint_move.
static void edit_joint_pivot (int J, const vec3_t d)
{
    int fr, c, nj;
    if (!s_editmode || !s_orig || !s_edit) return;
    if (J < 0 || J >= s_orig->numjoints) return;
    nj = s_orig->numjoints;
    for (c = 0; c < 3; c++) { s_orig->joints[J].translate[c]+=d[c]; s_edit->joints[J].translate[c]+=d[c]; }
    if (s_orig->frametrs) for (fr=0; fr<s_orig->numframes; fr++) for (c=0;c<3;c++) s_orig->frametrs[((size_t)fr*nj+J)*10+c]+=d[c];
    if (s_edit->frametrs) for (fr=0; fr<s_edit->numframes; fr++) for (c=0;c<3;c++) s_edit->frametrs[((size_t)fr*nj+J)*10+c]+=d[c];
}

static void edit_add_joint (int parent, const vec3_t t)
{
    if (!s_editmode || !s_orig || !s_edit) return;
    if (parent < -1 || parent >= s_orig->numjoints) parent = 0;
    if (lm_iqm_add_joint (s_orig, "joint", parent, t) != LM_OK) return;
    if (lm_iqm_add_joint (s_edit, "joint", parent, t) != LM_OK) return;
}

// ---- E2 animation: keys -> frametrs (slerp-free MVP: euler+translate lerp) ----
static void e2_qmul (const float a[4], const float b[4], float o[4])
{
    o[0]=a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1];
    o[1]=a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0];
    o[2]=a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3];
    o[3]=a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2];
}
static void e2_eul2quat (const float e[3], float q[4])  // e=pitch(Y),yaw(Z),roll(X)
{
    float k = (float)(M_PI/180.0)*0.5f;
    float hx=e[2]*k, hy=e[0]*k, hz=e[1]*k, t[4];
    float qx[4]={(float)sin(hx),0,0,(float)cos(hx)};
    float qy[4]={0,(float)sin(hy),0,(float)cos(hy)};
    float qz[4]={0,0,(float)sin(hz),(float)cos(hz)};
    e2_qmul (qy, qx, t);
    e2_qmul (qz, t, q);              // q = qz * qy * qx
}
static void e2_slerp (const float a[4], const float b[4], float u, float o[4])
{
    float d = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3], bb[4], n; int c;
    for (c=0;c<4;c++) bb[c]=b[c];
    if (d < 0.0f) { d = -d; for (c=0;c<4;c++) bb[c] = -bb[c]; }   // shortest path
    if (d > 0.9995f) {                                            // near-parallel: nlerp
        for (c=0;c<4;c++) o[c] = a[c] + (bb[c]-a[c])*u;
    } else {
        float ang = (float)acos(d), s = (float)sin(ang);
        float t0 = (float)sin((1.0f-u)*ang)/s, t1 = (float)sin(u*ang)/s;
        for (c=0;c<4;c++) o[c] = a[c]*t0 + bb[c]*t1;
    }
    n = (float)sqrt(o[0]*o[0]+o[1]*o[1]+o[2]*o[2]+o[3]*o[3]);
    if (n > 0.0f) for (c=0;c<4;c++) o[c]/=n;
}
// Bake joint j's keys into the selected clip's frametrs range: clamp outside the
// key span; inside, **slerp** rotation + lerp translation. No-key joints untouched.
static void e2_bake_joint (int j)
{
    lm_iqm_clip_t *cl; int f0, f1, f, nj, k;
    if (!s_edit || !s_edit->frametrs) return;
    if (s_animclip < 0 || s_animclip >= s_edit->numclips) return;
    if (j < 0 || j >= s_edit->numjoints || j >= MAXEDITMESH || s_jkeycnt[j] < 1) return;
    cl = &s_edit->clips[s_animclip];
    f0 = cl->first_frame; f1 = f0 + cl->num_frames; nj = s_edit->numjoints;
    for (f = f0; f < f1; f++) {
        anim_key_t *A=NULL, *B=NULL; float tr[3], q[4], *row; int c;
        for (k = 0; k < s_jkeycnt[j]; k++) {
            if (s_jkeys[j][k].frame <= f) A = &s_jkeys[j][k];
            if (s_jkeys[j][k].frame >= f && !B) B = &s_jkeys[j][k];
        }
        if (A && B && B->frame > A->frame) {
            float u = (float)(f - A->frame) / (float)(B->frame - A->frame);
            float qa[4], qb[4];
            if (s_ease) u = u*u*(3.0f - 2.0f*u);        // smoothstep ease-in/out
            e2_eul2quat (A->eul, qa); e2_eul2quat (B->eul, qb);
            e2_slerp (qa, qb, u, q);
            for (c=0;c<3;c++) tr[c]=A->tr[c]+(B->tr[c]-A->tr[c])*u;
        } else {
            anim_key_t *K = A ? A : B;
            e2_eul2quat (K->eul, q);
            for (c=0;c<3;c++) tr[c]=K->tr[c];
        }
        row = s_edit->frametrs + ((size_t)f*nj + j)*10;
        row[0]=tr[0]; row[1]=tr[1]; row[2]=tr[2];
        row[3]=q[0];  row[4]=q[1];  row[5]=q[2]; row[6]=q[3];
        row[7]=1.0f;  row[8]=1.0f;  row[9]=1.0f;
    }
}
static void e2_setkey (int j, const float eul[3], const float tr[3])
{
    int i, n;
    if (!s_editmode || !s_edit || j < 0 || j >= s_edit->numjoints || j >= MAXEDITMESH) return;
    n = s_jkeycnt[j];
    for (i = 0; i < n; i++) if (s_jkeys[j][i].frame == s_animframe) break;
    if (i == n) {                       // insert a new key, keeping frames sorted
        if (n >= E2_MAXKEYS) { Con_Printf ("actor anim: key limit\n"); return; }
        i = n; while (i > 0 && s_jkeys[j][i-1].frame > s_animframe) { s_jkeys[j][i]=s_jkeys[j][i-1]; i--; }
        s_jkeys[j][i].frame = s_animframe; s_jkeycnt[j] = n + 1;
    }
    VectorCopy (eul, s_jkeys[j][i].eul);
    VectorCopy (tr,  s_jkeys[j][i].tr);
    e2_bake_joint (j);
}
static void e2_unkey (int j)
{
    int i, n;
    if (j < 0 || j >= MAXEDITMESH) return;
    n = s_jkeycnt[j];
    for (i = 0; i < n; i++)
        if (s_jkeys[j][i].frame == s_animframe) {
            for (; i+1 < n; i++) s_jkeys[j][i] = s_jkeys[j][i+1];
            s_jkeycnt[j] = n - 1; break;
        }
}
static void e2_copykey (int j)   // copy joint j's pose at the current frame
{
    int k;
    if (!s_editmode || j < 0 || j >= MAXEDITMESH) return;
    for (k = 0; k < s_jkeycnt[j]; k++)
        if (s_jkeys[j][k].frame == s_animframe) {
            VectorCopy (s_jkeys[j][k].eul, s_clipeul); VectorCopy (s_jkeys[j][k].tr, s_cliptr);
            s_clipvalid = 1; return;
        }
    VectorCopy (s_poseeul, s_clipeul); VectorCopy (s_posetr, s_cliptr);  // else copy the edit buffer
    s_clipvalid = 1;
}
static void e2_pastekey (int j)  // paste the clipboard pose as a key at the current frame
{
    if (s_clipvalid) e2_setkey (j, s_clipeul, s_cliptr);
}

static void edit_add_clip (int nframes, float fps)
{
    if (!s_editmode || !s_orig || !s_edit) return;
    if (nframes < 2) nframes = 16;
    if (fps <= 0.0f) fps = 10.0f;
    if (lm_iqm_add_clip (s_orig, "newclip", nframes, fps, 1) != LM_OK) return;
    if (lm_iqm_add_clip (s_edit, "newclip", nframes, fps, 1) != LM_OK) return;
    s_animclip  = s_edit->numclips - 1;
    s_animframe = s_edit->clips[s_animclip].first_frame;
    Cvar_SetValue ("actor_clip", (float)s_animclip);
}

static void edit_save (void)
{
    void *buf = NULL; size_t len = 0;
    if (!s_edit) return;
    if (lm_write_iqm (s_edit, &buf, &len) != LM_OK) { Con_Printf ("actor save: write failed\n"); return; }
    COM_WriteFile (s_savepath, buf, (int)len);
    free (buf);
    Con_Printf ("actor save: wrote %s (%d bytes)\n", s_savepath, (int)len);
}

// ---- load (preview) -------------------------------------------------------
static void actor_load (const char *path)
{
    model_t *m = Mod_ForName ((char *)path, false);
    edit_free ();
    if (!m || m->type != mod_iqm) { Con_Printf ("actor editor: %s is not an IQM\n", path); s_mod = NULL; return; }
    s_mod = m;
    memset (&s_ent, 0, sizeof(s_ent));
    s_ent.model    = m;
    s_ent.colormap = vid.colormap;
    s_ent.frame    = 0;
}

static void actor_enter (void) { if (!s_mod) actor_load (s_path); }
static void actor_exit  (void) { edit_free (); }

// Inject the previewed actor (edit copy when editing, else the loaded model)
// into cl_visedicts at the orbit focus each editor frame.
void ActorMode_PushPreview (void)
{
    entity_t *e;
    // Preview the actor the way the particle editor previews an effect: against
    // a clean grid+backdrop, NOT the loaded map. The actor renders through the
    // normal entity pipeline, so the only way to show it alone is to drop every
    // other visedict that CL_RelinkEntities + Editor_PushPreviewEntities queued
    // this frame (map monsters, items, point-entity previews). r_main.c then
    // wipes the world BSP to a backdrop before the actor draws -- see
    // Editor_ActorHideWorld. Only ever called while Actor mode is active.
    cl_numvisedicts = 0;
    if (s_editmode && s_edit) e = &s_eent;
    else if (s_mod)           e = &s_ent;
    else                      return;   // no actor loaded -> empty grid backdrop
    Editor_GetOrbitFocus (e->origin);
    cl_visedicts[cl_numvisedicts++] = e;
}

// ---- UI -------------------------------------------------------------------
static void actor_draw_ui (void)
{
    lm_iqm_t *iqm;
    int       i, edit;

    if (!IG_Begin ("Actor", NULL, IG_WF_None)) { IG_End (); return; }

    IG_SetNextItemWidth (175);
    IG_InputText ("##path", s_path, sizeof(s_path), 0);
    IG_SameLine (0, -1);
    if (IG_Button ("Load")) actor_load (s_path);

    IG_Separator ();
    if (!s_mod || !s_mod->iqmdata) { IG_TextUnformatted ("(no actor loaded)"); IG_End (); return; }
    iqm = s_mod->iqmdata;

    TX ("%s", s_mod->name);
    TX ("meshes %d   joints %d   verts %d   tris %d", iqm->nummeshes, iqm->numjoints, iqm->numverts, iqm->numtris);
    TX ("roles: head %d  chest %d  jaw %d  eyes %d  pony %d",
        iqm->head_joint, iqm->chest_joint, iqm->jaw_joint, iqm->num_eye, iqm->num_pony);

    // ---- Edit geometry toggle ----
    edit = s_editmode;
    if (IG_Checkbox ("Edit geometry (move/resize parts)", &edit))
    {
        if (edit) edit_begin ();
        else      edit_free ();
    }

    // ---- Viewport (like the particle editor): preview the actor against a
    // clean backdrop + reference grid instead of the loaded map. ----
    IG_Separator ();
    {
        cvar_t *cv = Cvar_FindVar ("editor_actor_hide_world");
        int on = cv && cv->value != 0.0f;
        if (IG_Checkbox ("Hide world", &on))
            Cvar_SetValue ("editor_actor_hide_world", on ? 1.0f : 0.0f);
    }
    IG_SameLine (0, -1);
    {
        cvar_t *cv = Cvar_FindVar ("editor_actor_grid");
        int on = cv && cv->value != 0.0f;
        if (IG_Checkbox ("Grid", &on))
            Cvar_SetValue ("editor_actor_grid", on ? 1.0f : 0.0f);
    }
    IG_TextUnformatted ("Grid: 16u cells (major every 64u)");

    if (!s_editmode)
    {
        // ---- preview / inspector ----
        IG_Separator ();
        if (IG_CollapsingHeader ("Joints", IG_TREE_DEFAULTOPEN))
            for (i = 0; i < iqm->numjoints; i++)
                TX ("%2d %-14s parent %d", i, iqm->joints[i].name, iqm->joints[i].parent);
        if (IG_CollapsingHeader ("Meshes", 0))
            for (i = 0; i < iqm->nummeshes; i++)
                TX ("%2d %-10s [%s]", i, iqm->meshes[i].name, iqm->meshes[i].material);

        IG_Separator ();
        IG_TextUnformatted ("Preview");
        if (iqm->numclips > 0) {
            IG_TextUnformatted ("clip (sets previewed entity's frame):");
            for (i = 0; i < iqm->numclips; i++) {
                IG_PushID_Int (i);
                if (IG_Selectable (iqm->clips[i].name, i == s_ent.frame, 0)) s_ent.frame = i;
                IG_PopID ();
            }
        }
        {
            cvar_t *cv = Cvar_FindVar ("actor_lookat");
            int on = cv && cv->value != 0.0f;
            if (IG_Checkbox ("Look at camera (head)", &on))
                Cvar_SetValue ("actor_lookat", on ? 1.0f : 0.0f);
        }
        IG_TextUnformatted ("Camera: RMB-drag orbit; the head tracks it.");
    }
    else if (s_orig)
    {
        // ---- geometry editor (numeric fields) ----
        IG_Separator ();
        if (IG_Button ("Add box")) edit_add_box ();
        IG_TextUnformatted ("Part (select to edit):");
        for (i = 0; i < s_orig->nummeshes && i < MAXEDITMESH; i++) {
            IG_PushID_Int (i);
            if (IG_Selectable (s_orig->meshes[i].name, i == s_selmesh, 0)) s_selmesh = i;
            IG_PopID ();
        }
        if (s_selmesh >= 0 && s_selmesh < s_orig->nummeshes) {
            IG_Separator ();
            TX ("editing part %d (%s)", s_selmesh, s_orig->meshes[s_selmesh].name);
            IG_DragFloat3 ("move",   s_voff[s_selmesh],   0.5f);
            IG_DragFloat3 ("size",   s_vscale[s_selmesh], 0.02f);
            IG_DragFloat3 ("rotate", s_vrot[s_selmesh],   1.0f);
            {   // bind the part to a joint (it follows that joint under animation)
                int   cur = edit_part_joint (s_selmesh);
                float jf  = (float)cur;
                if (IG_DragFloat ("bind joint", &jf, 0.25f, 0.0f, (float)(s_orig->numjoints-1)))
                    edit_bind (s_selmesh, (int)(jf + 0.5f));
                TX ("  bound to joint %d (%s)", cur,
                    (cur>=0 && cur<s_orig->numjoints) ? s_orig->joints[cur].name : "?");
            }
            if (IG_Button ("Reset part")) {
                s_voff[s_selmesh][0]=s_voff[s_selmesh][1]=s_voff[s_selmesh][2]=0.0f;
                s_vscale[s_selmesh][0]=s_vscale[s_selmesh][1]=s_vscale[s_selmesh][2]=1.0f;
                s_vrot[s_selmesh][0]=s_vrot[s_selmesh][1]=s_vrot[s_selmesh][2]=0.0f;
            }
            IG_SameLine (0, -1);
            if (IG_Button ("Delete part")) edit_delete (s_selmesh);
        }
        edit_rebuild ();   // apply numeric edits to the rendered copy each frame

        IG_Separator ();
        if (IG_CollapsingHeader ("Joints (move = joint + its subtree)", 0)) {
            int j;
            if (s_seljoint >= s_orig->numjoints) s_seljoint = 0;
            if (IG_Button ("Add joint")) {       // child of the selected joint, 16u up
                vec3_t off = { 0, 0, 16 };
                edit_add_joint (s_seljoint, off);
                s_seljoint = s_orig->numjoints - 1;
            }
            for (j = 0; j < s_orig->numjoints; j++) {
                IG_PushID_Int (2000 + j);
                if (IG_Selectable (s_orig->joints[j].name, j == s_seljoint, 0)) s_seljoint = j;
                IG_PopID ();
            }
            if (IG_DragFloat3 ("move joint", s_jnudge, 0.5f)) {  // incremental nudge
                edit_joint_move (s_seljoint, s_jnudge);
                s_jnudge[0]=s_jnudge[1]=s_jnudge[2]=0.0f;
            }
            {   // re-pivot: move the rotation centre, mesh stays put
                static float pivn[3] = { 0, 0, 0 };
                if (IG_DragFloat3 ("re-pivot", pivn, 0.5f)) {
                    edit_joint_pivot (s_seljoint, pivn);
                    pivn[0]=pivn[1]=pivn[2]=0.0f;
                }
            }
        }

        IG_Separator ();
        if (s_edit->numclips > 0 && IG_CollapsingHeader ("Animation (keyframes)", 0)) {
            lm_iqm_clip_t *cl; int rel, kk, iskey = 0; float rf;
            if (s_animclip >= s_edit->numclips) s_animclip = 0;
            IG_TextUnformatted ("clip:");
            if (IG_Button ("New clip (16f)")) edit_add_clip (16, 10.0f);
            for (i = 0; i < s_edit->numclips; i++) {
                IG_PushID_Int (3000 + i);
                if (IG_Selectable (s_edit->clips[i].name, i == s_animclip, 0)) {
                    s_animclip = i; s_animframe = s_edit->clips[i].first_frame;
                    Cvar_SetValue ("actor_clip", (float)i);
                }
                IG_PopID ();
            }
            cl = &s_edit->clips[s_animclip];
            rel = s_animframe - cl->first_frame;
            rf = (float)rel;
            if (IG_DragFloat ("frame", &rf, 0.25f, 0.0f, (float)(cl->num_frames-1))) {
                rel = (int)(rf + 0.5f);
                if (rel < 0) rel = 0; if (rel >= cl->num_frames) rel = cl->num_frames-1;
                s_animframe = cl->first_frame + rel;
            }
            if (s_seljoint >= 0 && s_seljoint < MAXEDITMESH)
                for (kk = 0; kk < s_jkeycnt[s_seljoint]; kk++)
                    if (s_jkeys[s_seljoint][kk].frame == s_animframe) { iskey = 1; break; }
            TX ("frame %d/%d  joint %d (%s)  %s%d keys", rel, cl->num_frames, s_seljoint,
                (s_seljoint>=0 && s_seljoint<s_edit->numjoints) ? s_edit->joints[s_seljoint].name : "?",
                iskey ? "[ON KEY] " : "",
                (s_seljoint>=0 && s_seljoint<MAXEDITMESH) ? s_jkeycnt[s_seljoint] : 0);
            IG_DragFloat3 ("rot (key)",   s_poseeul, 1.0f);
            IG_DragFloat3 ("trans (key)", s_posetr,  0.5f);
            if (IG_Button ("Set key"))  e2_setkey (s_seljoint, s_poseeul, s_posetr);
            IG_SameLine (0, -1);
            if (IG_Button ("Del key"))  e2_unkey (s_seljoint);
            IG_SameLine (0, -1);
            if (IG_Button ("Bake"))     { int jj; for (jj=0; jj<s_edit->numjoints && jj<MAXEDITMESH; jj++) e2_bake_joint (jj); }
            if (IG_Button ("Copy"))  e2_copykey (s_seljoint);
            IG_SameLine (0, -1);
            if (IG_Button ("Paste")) e2_pastekey (s_seljoint);
            if (IG_Checkbox ("ease in/out", &s_ease)) { int jj; for (jj=0; jj<s_edit->numjoints && jj<MAXEDITMESH; jj++) e2_bake_joint (jj); }
            IG_TextUnformatted ("(pick joint above; scrub frame; type rot/trans; Set key)");
        }

        IG_Separator ();
        IG_SetNextItemWidth (175);
        IG_InputText ("##savepath", s_savepath, sizeof(s_savepath), 0);
        IG_SameLine (0, -1);
        if (IG_Button ("Save IQM")) edit_save ();
        IG_TextUnformatted ("(saves edited geometry + the original clips)");
    }

    IG_End ();
}

// ---- console interface (scriptable / headless-testable edit backend) ------
static void Cmd_ActorEdit_f (void)
{
    if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_edit <0|1>\n"); return; }
    if (!s_mod) actor_load (s_path);
    if (Q_atoi (Cmd_Argv (1))) edit_begin (); else edit_free ();
    Con_Printf ("actor edit mode %s\n", s_editmode ? "on" : "off");
}
static int actor_edit_part (void)
{
    int mi;
    if (!s_editmode || !s_orig) { Con_Printf ("not editing (actor_edit 1 first)\n"); return -1; }
    mi = Q_atoi (Cmd_Argv (1));
    if (mi < 0 || mi >= s_orig->nummeshes || mi >= MAXEDITMESH) { Con_Printf ("bad mesh index\n"); return -1; }
    return mi;
}
static void Cmd_ActorEditMove_f (void)
{
    int mi;
    if (Cmd_Argc () < 5) { Con_Printf ("usage: actor_edit_move <mesh> <dx> <dy> <dz>\n"); return; }
    if ((mi = actor_edit_part ()) < 0) return;
    s_voff[mi][0]=Q_atof(Cmd_Argv(2)); s_voff[mi][1]=Q_atof(Cmd_Argv(3)); s_voff[mi][2]=Q_atof(Cmd_Argv(4));
    edit_rebuild ();
}
static void Cmd_ActorEditScale_f (void)
{
    int mi;
    if (Cmd_Argc () < 5) { Con_Printf ("usage: actor_edit_scale <mesh> <sx> <sy> <sz>\n"); return; }
    if ((mi = actor_edit_part ()) < 0) return;
    s_vscale[mi][0]=Q_atof(Cmd_Argv(2)); s_vscale[mi][1]=Q_atof(Cmd_Argv(3)); s_vscale[mi][2]=Q_atof(Cmd_Argv(4));
    edit_rebuild ();
}
static void Cmd_ActorEditRot_f (void)
{
    int mi;
    if (Cmd_Argc () < 5) { Con_Printf ("usage: actor_edit_rot <mesh> <pitch> <yaw> <roll>\n"); return; }
    if ((mi = actor_edit_part ()) < 0) return;
    s_vrot[mi][0]=Q_atof(Cmd_Argv(2)); s_vrot[mi][1]=Q_atof(Cmd_Argv(3)); s_vrot[mi][2]=Q_atof(Cmd_Argv(4));
    edit_rebuild ();
}
static void Cmd_ActorEditSel_f (void)   // select the part the gizmo/fields act on
{
    if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_edit_sel <mesh>\n"); return; }
    if (!s_editmode || !s_orig) { Con_Printf ("not editing\n"); return; }
    s_selmesh = Q_atoi (Cmd_Argv (1));
    if (s_selmesh < 0) s_selmesh = 0;
    if (s_selmesh >= s_orig->nummeshes) s_selmesh = s_orig->nummeshes - 1;
    Con_Printf ("actor edit: selected part %d\n", s_selmesh);
}
static void Cmd_ActorEditJMove_f (void)
{
    vec3_t d; int j;
    if (Cmd_Argc () < 5) { Con_Printf ("usage: actor_edit_jmove <joint> <dx> <dy> <dz>\n"); return; }
    if (!s_editmode || !s_orig) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    j = Q_atoi (Cmd_Argv (1));
    if (j < 0 || j >= s_orig->numjoints) { Con_Printf ("bad joint index\n"); return; }
    d[0]=Q_atof(Cmd_Argv(2)); d[1]=Q_atof(Cmd_Argv(3)); d[2]=Q_atof(Cmd_Argv(4));
    edit_joint_move (j, d);
    Con_Printf ("actor edit: moved joint %d (%s) + its subtree\n", j, s_orig->joints[j].name);
}
static void Cmd_ActorAnimClip_f (void)
{
    if (!s_editmode || !s_edit) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    if (Cmd_Argc () < 2) { Con_Printf ("anim: clip %d/%d, frame %d\n", s_animclip, s_edit->numclips, s_animframe); return; }
    s_animclip = Q_atoi (Cmd_Argv (1));
    if (s_animclip < 0) s_animclip = 0;
    if (s_animclip >= s_edit->numclips) s_animclip = s_edit->numclips - 1;
    s_animframe = s_edit->clips[s_animclip].first_frame;
    Cvar_SetValue ("actor_clip", (float)s_animclip);   // preview the edited clip
}
static void Cmd_ActorAnimAddClip_f (void)
{
    int nframes; float fps;
    if (!s_editmode || !s_edit) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    nframes = (Cmd_Argc () >= 2) ? Q_atoi (Cmd_Argv (1)) : 16;
    fps     = (Cmd_Argc () >= 3) ? Q_atof (Cmd_Argv (2)) : 10.0f;
    edit_add_clip (nframes, fps);
    Con_Printf ("actor anim: added clip %d (%d frames @ %g fps)\n", s_animclip, nframes, fps);
}
static void Cmd_ActorAnimFrame_f (void)
{
    lm_iqm_clip_t *cl; int rel;
    if (!s_editmode || !s_edit || s_animclip >= s_edit->numclips) { Con_Printf ("not editing\n"); return; }
    cl = &s_edit->clips[s_animclip];
    if (Cmd_Argc () < 2) { Con_Printf ("anim: frame %d (clip-local %d)\n", s_animframe, s_animframe - cl->first_frame); return; }
    rel = Q_atoi (Cmd_Argv (1));
    if (rel < 0) rel = 0;
    if (rel >= cl->num_frames) rel = cl->num_frames - 1;
    s_animframe = cl->first_frame + rel;
}
static void Cmd_ActorAnimKey_f (void)
{
    int j; float eul[3], tr[3];
    if (Cmd_Argc () < 8) { Con_Printf ("usage: actor_anim_key <joint> <pitch> <yaw> <roll> <tx> <ty> <tz>\n"); return; }
    if (!s_editmode || !s_edit) { Con_Printf ("not editing\n"); return; }
    j = Q_atoi (Cmd_Argv (1));
    eul[0]=Q_atof(Cmd_Argv(2)); eul[1]=Q_atof(Cmd_Argv(3)); eul[2]=Q_atof(Cmd_Argv(4));
    tr[0]=Q_atof(Cmd_Argv(5));  tr[1]=Q_atof(Cmd_Argv(6));  tr[2]=Q_atof(Cmd_Argv(7));
    e2_setkey (j, eul, tr);
    Con_Printf ("actor anim: key joint %d @ frame %d\n", j, s_animframe);
}
static void Cmd_ActorAnimUnkey_f (void)
{
    if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_anim_unkey <joint>\n"); return; }
    e2_unkey (Q_atoi (Cmd_Argv (1)));
}
static void Cmd_ActorAnimBake_f (void)
{
    int j;
    if (!s_editmode || !s_edit) { Con_Printf ("not editing\n"); return; }
    for (j = 0; j < s_edit->numjoints && j < MAXEDITMESH; j++) e2_bake_joint (j);
    Con_Printf ("actor anim: baked clip %d\n", s_animclip);
}
static void Cmd_ActorAnimCopy_f (void)
{
    if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_anim_copy <joint>\n"); return; }
    e2_copykey (Q_atoi (Cmd_Argv (1)));
}
static void Cmd_ActorAnimPaste_f (void)
{
    if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_anim_paste <joint>\n"); return; }
    e2_pastekey (Q_atoi (Cmd_Argv (1)));
}
static void Cmd_ActorEditJAdd_f (void)
{
    vec3_t t = { 0, 0, 16 }; int parent;
    if (!s_editmode || !s_orig) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    parent = (Cmd_Argc () >= 2) ? Q_atoi (Cmd_Argv (1)) : 0;
    if (Cmd_Argc () >= 5) { t[0]=Q_atof(Cmd_Argv(2)); t[1]=Q_atof(Cmd_Argv(3)); t[2]=Q_atof(Cmd_Argv(4)); }
    edit_add_joint (parent, t);
    Con_Printf ("actor edit: added joint under %d; %d joints\n", parent, s_orig->numjoints);
}
static void Cmd_ActorEditJPivot_f (void)
{
    vec3_t d; int j;
    if (Cmd_Argc () < 5) { Con_Printf ("usage: actor_edit_jpivot <joint> <dx> <dy> <dz>\n"); return; }
    if (!s_editmode || !s_orig) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    j = Q_atoi (Cmd_Argv (1));
    if (j < 0 || j >= s_orig->numjoints) { Con_Printf ("bad joint index\n"); return; }
    d[0]=Q_atof(Cmd_Argv(2)); d[1]=Q_atof(Cmd_Argv(3)); d[2]=Q_atof(Cmd_Argv(4));
    edit_joint_pivot (j, d);
    Con_Printf ("actor edit: re-pivoted joint %d (%s) (mesh unchanged)\n", j, s_orig->joints[j].name);
}
static void Cmd_ActorEditBind_f (void)
{
    int mi;
    if (Cmd_Argc () < 3) { Con_Printf ("usage: actor_edit_bind <mesh> <joint>\n"); return; }
    if ((mi = actor_edit_part ()) < 0) return;
    edit_bind (mi, Q_atoi (Cmd_Argv (2)));
}
static void Cmd_ActorEditAdd_f (void)
{
    if (!s_editmode) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    edit_add_box ();
    Con_Printf ("actor edit: added box; %d parts\n", s_orig ? s_orig->nummeshes : 0);
}
static void Cmd_ActorEditDel_f (void)
{
    int mi;
    if (Cmd_Argc () < 2) { Con_Printf ("usage: actor_edit_del <mesh>\n"); return; }
    if ((mi = actor_edit_part ()) < 0) return;
    edit_delete (mi);
    Con_Printf ("actor edit: deleted part; %d left\n", s_orig ? s_orig->nummeshes : 0);
}
static void Cmd_ActorEditSave_f (void)
{
    if (Cmd_Argc () >= 2) { strncpy (s_savepath, Cmd_Argv (1), sizeof(s_savepath)-1); s_savepath[sizeof(s_savepath)-1]=0; }
    if (s_editmode) edit_save (); else Con_Printf ("not editing\n");
}

void ActorMode_RegisterCmds (void)
{
    Cmd_AddCommand ("actor_edit",       Cmd_ActorEdit_f);
    Cmd_AddCommand ("actor_edit_move",  Cmd_ActorEditMove_f);
    Cmd_AddCommand ("actor_edit_scale", Cmd_ActorEditScale_f);
    Cmd_AddCommand ("actor_edit_rot",   Cmd_ActorEditRot_f);
    Cmd_AddCommand ("actor_edit_bind",  Cmd_ActorEditBind_f);
    Cmd_AddCommand ("actor_edit_sel",   Cmd_ActorEditSel_f);
    Cmd_AddCommand ("actor_edit_jmove", Cmd_ActorEditJMove_f);
    Cmd_AddCommand ("actor_edit_jpivot",Cmd_ActorEditJPivot_f);
    Cmd_AddCommand ("actor_edit_jadd",  Cmd_ActorEditJAdd_f);
    Cmd_AddCommand ("actor_anim_clip",  Cmd_ActorAnimClip_f);
    Cmd_AddCommand ("actor_anim_addclip", Cmd_ActorAnimAddClip_f);
    Cmd_AddCommand ("actor_anim_frame", Cmd_ActorAnimFrame_f);
    Cmd_AddCommand ("actor_anim_key",   Cmd_ActorAnimKey_f);
    Cmd_AddCommand ("actor_anim_unkey", Cmd_ActorAnimUnkey_f);
    Cmd_AddCommand ("actor_anim_bake",  Cmd_ActorAnimBake_f);
    Cmd_AddCommand ("actor_anim_copy",  Cmd_ActorAnimCopy_f);
    Cmd_AddCommand ("actor_anim_paste", Cmd_ActorAnimPaste_f);
    Cmd_AddCommand ("actor_edit_add",   Cmd_ActorEditAdd_f);
    Cmd_AddCommand ("actor_edit_del",   Cmd_ActorEditDel_f);
    Cmd_AddCommand ("actor_edit_save",  Cmd_ActorEditSave_f);
}

const editor_mode_t actor_mode = {
    .name    = "Actor",
    .enter   = actor_enter,
    .exit    = actor_exit,
    .draw_ui = actor_draw_ui,
};

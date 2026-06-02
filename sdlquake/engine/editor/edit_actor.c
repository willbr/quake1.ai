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

static void edit_add_joint (int parent, const vec3_t t)
{
    if (!s_editmode || !s_orig || !s_edit) return;
    if (parent < -1 || parent >= s_orig->numjoints) parent = 0;
    if (lm_iqm_add_joint (s_orig, "joint", parent, t) != LM_OK) return;
    if (lm_iqm_add_joint (s_edit, "joint", parent, t) != LM_OK) return;
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
    if (s_editmode && s_edit) e = &s_eent;
    else if (s_mod)           e = &s_ent;
    else                      return;
    if (cl_numvisedicts >= MAX_VISEDICTS) return;
    Editor_GetOrbitFocus (e->origin);
    cl_visedicts[cl_numvisedicts++] = e;
}

// ---- UI -------------------------------------------------------------------
static void actor_draw_ui (void)
{
    lm_iqm_t *iqm;
    int       i, edit;

    IG_SetNextWindowPos (8.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize (286.0f, 520.0f, IG_Cond_FirstUseEver);
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
static void Cmd_ActorEditJAdd_f (void)
{
    vec3_t t = { 0, 0, 16 }; int parent;
    if (!s_editmode || !s_orig) { Con_Printf ("not editing (actor_edit 1 first)\n"); return; }
    parent = (Cmd_Argc () >= 2) ? Q_atoi (Cmd_Argv (1)) : 0;
    if (Cmd_Argc () >= 5) { t[0]=Q_atof(Cmd_Argv(2)); t[1]=Q_atof(Cmd_Argv(3)); t[2]=Q_atof(Cmd_Argv(4)); }
    edit_add_joint (parent, t);
    Con_Printf ("actor edit: added joint under %d; %d joints\n", parent, s_orig->numjoints);
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
    Cmd_AddCommand ("actor_edit_jmove", Cmd_ActorEditJMove_f);
    Cmd_AddCommand ("actor_edit_jadd",  Cmd_ActorEditJAdd_f);
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

// edit_actor.c -- Actor editor mode (E1 skeleton). Loads an IQM actor and
// previews it with the shared orbit camera; the R3 head look-at tracks the orbit
// camera live, so circling the actor shows it watching you. Read-only inspector
// (joints / meshes / roles). The interactive authoring tools (create/size/parent
// box parts, rig joints, animation timeline) build on this foundation. Engine-
// side, like the rest of the editor; mirrors edit_particle.c's mode pattern.

#include "quakedef.h"
#include "iqm.h"
#include "imgui_bridge.h"
#include "editor_mode.h"
#include "editor_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define IG_TREE_DEFAULTOPEN (1<<5)

static entity_t  s_ent;                          // the previewed actor entity
static model_t  *s_mod;                           // loaded IQM model (NULL = none)
static char      s_path[MAX_QPATH] = "actors/dummy.iqm";

static void TX (const char *fmt, ...)             // formatted ImGui text helper
{
    char buf[256];
    va_list ap;
    va_start (ap, fmt);
    vsnprintf (buf, sizeof(buf), fmt, ap);
    va_end (ap);
    IG_TextUnformatted (buf);
}

static void actor_load (const char *path)
{
    model_t *m = Mod_ForName ((char *)path, false);
    if (!m || m->type != mod_iqm) {
        Con_Printf ("actor editor: %s is not an IQM\n", path);
        s_mod = NULL;
        return;
    }
    s_mod = m;
    memset (&s_ent, 0, sizeof(s_ent));
    s_ent.model    = m;
    s_ent.colormap = vid.colormap;
    s_ent.frame    = 0;
}

static void actor_enter (void)
{
    if (!s_mod)
        actor_load (s_path);
}

// Inject the previewed actor into cl_visedicts at the orbit focus each editor
// frame. Called from editor.c after CL_RelinkEntities reset the list and before
// the entity render pass (the same window Editor_PushPreviewEntities uses).
void ActorMode_PushPreview (void)
{
    if (!s_mod)
        return;
    if (cl_numvisedicts >= MAX_VISEDICTS)
        return;
    Editor_GetOrbitFocus (s_ent.origin);
    cl_visedicts[cl_numvisedicts++] = &s_ent;
}

static void actor_draw_ui (void)
{
    lm_iqm_t *iqm;
    int       i;

    IG_SetNextWindowPos (8.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize (270.0f, 470.0f, IG_Cond_FirstUseEver);
    if (!IG_Begin ("Actor", NULL, IG_WF_None)) { IG_End (); return; }

    IG_SetNextItemWidth (175);
    IG_InputText ("##path", s_path, sizeof(s_path), 0);
    IG_SameLine (0, -1);
    if (IG_Button ("Load"))
        actor_load (s_path);

    IG_Separator ();
    if (!s_mod || !s_mod->iqmdata) { IG_TextUnformatted ("(no actor loaded)"); IG_End (); return; }
    iqm = s_mod->iqmdata;

    TX ("%s", s_mod->name);
    TX ("meshes %d   joints %d", iqm->nummeshes, iqm->numjoints);
    TX ("verts %d   tris %d", iqm->numverts, iqm->numtris);
    if (iqm->numframes > 0) TX ("anim: %d frames @ %g fps", iqm->numframes, iqm->framerate);
    else                    IG_TextUnformatted ("anim: (static)");
    TX ("roles: head %d  chest %d  jaw %d  eyes %d  pony %d",
        iqm->head_joint, iqm->chest_joint, iqm->jaw_joint, iqm->num_eye, iqm->num_pony);

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
        IG_TextUnformatted ("clip (sets the previewed entity's frame):");
        for (i = 0; i < iqm->numclips; i++) {
            IG_PushID_Int (i);
            if (IG_Selectable (iqm->clips[i].name, i == s_ent.frame, 0))
                s_ent.frame = i;
            IG_PopID ();
        }
    }
    {   // toggle the procedural head look-at so baked clips are visible
        cvar_t *cv = Cvar_FindVar ("actor_lookat");
        int on = cv && cv->value != 0.0f;
        if (IG_Checkbox ("Look at camera (head)", &on))
            Cvar_SetValue ("actor_lookat", on ? 1.0f : 0.0f);
    }
    IG_TextUnformatted ("Camera: RMB-drag orbit; the head tracks it.");
    IG_TextUnformatted ("Editing tools (box create / rig / timeline): TBD.");
    IG_End ();
}

// Exported vtable -- referenced by editor.c's s_modes[] (extern).
const editor_mode_t actor_mode = {
    .name    = "Actor",
    .enter   = actor_enter,
    .draw_ui = actor_draw_ui,
};

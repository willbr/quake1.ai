// edit_particle.c -- Particle editor mode. Edits emitter_def_t records in the
// r_emitter.c registry live; previews by spawning into the world. Engine-side
// (touches the renderer registry directly), like the rest of the editor.

#include "quakedef.h"
#include "r_emitter.h"
#include "imgui_bridge.h"
#include "editor_mode.h"
#include "editor_internal.h"   // Editor_GetParticlePaused / Editor_SetParticlePaused
#include <stdio.h>
#include <string.h>

extern byte *host_basepal;  // 768 bytes RGB; for palette swatches

#define IG_TREE_DEFAULTOPEN (1<<5)

static int  s_sel = -1;          // selected registry index, -1 = none
static int  s_loop_handle = -1;  // active loop-preview live handle
static char s_new_name[EMIT_NAME_LEN] = "new_effect";

static void panel_effect_list(void);
static void panel_inspector(void);

// ---- mode entry points ---------------------------------------------------
static void particle_enter(void)
{
    int i;
    if (s_sel < 0 || !R_EmitterGetDef(s_sel)) {
        for (i = 0; i < EMIT_MAX_DEFS; i++)
            if (R_EmitterGetDef(i)) { s_sel = i; break; }
    }
}
static void particle_exit(void)
{
    if (s_loop_handle >= 0) { R_EmitterStopHandle(s_loop_handle); s_loop_handle = -1; }
}
static int particle_hide_fx(void) { return 0; } // SHOW particles in this mode

static void particle_draw_ui(void)
{
    panel_effect_list();
    panel_inspector();
}

// Exported vtable -- referenced by editor.c's s_modes[] (extern).
const editor_mode_t particle_mode = {
    .name              = "Particle",
    .enter             = particle_enter,
    .exit              = particle_exit,
    .draw_ui           = particle_draw_ui,
    .hide_transient_fx = particle_hide_fx,
};

// ---- effect list ----------------------------------------------------------
static void panel_effect_list(void)
{
    int i;
    emitter_def_t *d;

    IG_SetNextWindowPos(8.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize(220.0f, 360.0f, IG_Cond_FirstUseEver);
    if (!IG_Begin("Particle Effects", NULL, IG_WF_None)) { IG_End(); return; }

    IG_SetNextItemWidth(120);
    IG_InputText("##newname", s_new_name, sizeof(s_new_name), 0);
    IG_SameLine(0, -1);
    if (IG_Button("New")) {
        int idx = R_EmitterNew(s_new_name);
        if (idx >= 0) s_sel = idx;
    }
    IG_Separator();

    for (i = 0; i < EMIT_MAX_DEFS; i++) {
        d = R_EmitterGetDef(i);
        if (!d) continue;
        IG_PushID_Int(i);
        if (IG_Selectable(d->name, i == s_sel, 0)) s_sel = i;
        IG_PopID();
    }

    IG_Separator();
    d = R_EmitterGetDef(s_sel);
    IG_BeginDisabled(!d);
    if (IG_Button("Save") && d) {
        if (R_EmitterSave(s_sel)) Con_Printf("saved %s.pcl\n", d->name);
    }
    IG_SameLine(0, -1);
    if (IG_Button("Duplicate") && d) {
        char nm[EMIT_NAME_LEN];
        int idx;
        snprintf(nm, sizeof(nm), "%.24s_copy", d->name);
        idx = R_EmitterNew(nm);
        if (idx >= 0) {
            emitter_def_t *n = R_EmitterGetDef(idx);
            emitter_def_t  src = *d;
            *n = src;
            n->used = 1;
            strncpy(n->name, nm, EMIT_NAME_LEN - 1); n->name[EMIT_NAME_LEN - 1] = 0;
            s_sel = idx;
        }
    }
    IG_SameLine(0, -1);
    if (IG_Button("Delete") && d) { R_EmitterDelete(s_sel); s_sel = -1; }
    IG_EndDisabled();
    IG_End();
}

// ---- palette grid (16x16 swatches; returns clicked index or -1) ----------
static int palette_grid(float swatch)
{
    int clicked = -1, i;
    if (!host_basepal) return -1;
    for (i = 0; i < 256; i++) {
        float r = host_basepal[i*3+0] / 255.0f;
        float g = host_basepal[i*3+1] / 255.0f;
        float b = host_basepal[i*3+2] / 255.0f;
        char id[16];
        snprintf(id, sizeof(id), "##pal%d", i);
        if (IG_ColorSwatch(id, r, g, b, swatch)) clicked = i;
        if ((i & 15) != 15) IG_SameLine(0, 2);
    }
    return clicked;
}

// ---- inspector ------------------------------------------------------------
static void panel_inspector(void)
{
    emitter_def_t *d = R_EmitterGetDef(s_sel);
    static const char * const modes[]  = { "burst", "continuous" };
    static const char * const shapes[] = { "point", "sphere", "cone", "box" };
    static const char * const dirs[]   = { "along_shape", "inherit", "up" };
    static const char * const styles[] = { "dot", "blob", "smoke" };

    IG_SetNextWindowPos(236.0f, 120.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize(330.0f, 560.0f, IG_Cond_FirstUseEver);
    if (!IG_Begin("Particle Inspector", NULL, IG_WF_None)) { IG_End(); return; }
    if (!d) { IG_TextUnformatted("(no effect selected)"); IG_End(); return; }

    if (IG_CollapsingHeader("Emit", IG_TREE_DEFAULTOPEN)) {
        IG_Combo("mode", &d->mode, modes, 2);
        { float c = (float)d->count; if (IG_DragFloat("count", &c, 1.0f, 1, 4096)) d->count = (int)c; }
        IG_DragFloat("rate (/s)",    &d->rate,     1.0f,  0, 1000);
        IG_DragFloat("duration (s)", &d->duration, 0.05f, 0, 60);
    }
    if (IG_CollapsingHeader("Spawn", IG_TREE_DEFAULTOPEN)) {
        IG_Combo("shape", &d->shape, shapes, 4);
        IG_DragFloat3("origin offset", d->origin_offset, 0.5f);
        IG_DragFloat("shape size", &d->shape_size, 0.5f, 0, 256);
        IG_DragFloat("cone angle", &d->cone_angle, 0.5f, 0, 90);
    }
    if (IG_CollapsingHeader("Velocity", IG_TREE_DEFAULTOPEN)) {
        IG_Combo("dir mode", &d->dir_mode, dirs, 3);
        IG_DragFloat("speed",        &d->speed,        1.0f, 0,    2000);
        IG_DragFloat("speed jitter", &d->speed_jitter, 1.0f, 0,    1000);
        IG_DragFloat("spread (deg)", &d->spread,       0.5f, 0,    180);
        IG_DragFloat("radial bias",  &d->radial_bias,  1.0f, -500, 500);
    }
    if (IG_CollapsingHeader("Physics / Life", IG_TREE_DEFAULTOPEN)) {
        IG_DragFloat("gravity scale", &d->gravity_scale, 0.01f, -5,    5);
        IG_DragFloat("drag",          &d->drag,          0.05f, 0,     20);
        IG_DragFloat("life min",      &d->life_min,      0.05f, 0.05f, 20);
        IG_DragFloat("life max",      &d->life_max,      0.05f, 0.05f, 20);
    }
    if (IG_CollapsingHeader("Render", IG_TREE_DEFAULTOPEN)) {
        IG_Combo("style", &d->style, styles, 3);

        IG_BeginDisabled(d->style == STYLE_DOT);
        IG_DragFloat("size start", &d->size_start, 0.1f, 0, 32);
        IG_DragFloat("size peak",  &d->size_peak,  0.1f, 0, 32);
        IG_DragFloat("size end",   &d->size_end,   0.1f, 0, 32);
        IG_EndDisabled();
        if (d->style == STYLE_DOT)
            IG_TextUnformatted("(dot size is distance-scaled; envelope N/A)");

        IG_Separator();
        IG_TextUnformatted("Color ramp (palette index per stop)");
        {
            static int armed = 0;
            int i;
            if (armed >= d->ramp_count) armed = 0;
            for (i = 0; i < d->ramp_count; i++) {
                IG_PushID_Int(1000 + i);
                if (IG_RadioButton("##arm", armed == i)) armed = i;
                IG_SameLine(0, -1);
                if (host_basepal) {
                    float r = host_basepal[d->ramp_pal[i]*3+0]/255.0f;
                    float g = host_basepal[d->ramp_pal[i]*3+1]/255.0f;
                    float b = host_basepal[d->ramp_pal[i]*3+2]/255.0f;
                    IG_ColorSwatch("##cur", r, g, b, 16);
                }
                IG_SameLine(0, -1);
                IG_SetNextItemWidth(110);
                IG_DragFloat("frac", &d->ramp_frac[i], 0.01f, 0, 1);
                IG_PopID();
            }
            if (d->ramp_count < EMIT_MAX_RAMP && IG_Button("+ stop")) {
                int nn = d->ramp_count;
                d->ramp_frac[nn] = 1.0f; d->ramp_pal[nn] = 15; d->ramp_count++;
            }
            IG_SameLine(0, -1);
            if (d->ramp_count > 1 && IG_Button("- stop")) d->ramp_count--;

            {
                int picked = palette_grid(12.0f);
                if (picked >= 0 && armed < d->ramp_count) d->ramp_pal[armed] = (byte)picked;
            }
        }
    }

    // ---- preview controls ----
    IG_Separator();
    IG_TextUnformatted("Preview");
    {
        vec3_t org, up = { 0, 0, 1 };
        int paused = Editor_GetParticlePaused();
        Editor_GetOrbitFocus(org);   // spawn at the orbit centre so the camera circles the effect

        // Play/pause toggle for the live preview clock (world sim stays paused).
        if (IG_Button(paused ? "Play"  : "Pause"))
            Editor_SetParticlePaused(!paused);
        IG_SameLine(0, -1);
        if (IG_Button("Spawn"))
            R_SpawnEffectIdx(s_sel, org, up);
        IG_SameLine(0, -1);
        if (s_loop_handle < 0) {
            if (IG_Button("Loop preview")) {
                int saved = d->mode;
                d->mode = EMIT_CONTINUOUS;
                s_loop_handle = R_SpawnEffectIdx(s_sel, org, up);
                d->mode = saved;
            }
        } else {
            if (IG_Button("Stop loop")) { R_EmitterStopHandle(s_loop_handle); s_loop_handle = -1; }
        }
        IG_SameLine(0, -1);
        if (IG_Button("Stop all")) { R_EmitterStopAll(); s_loop_handle = -1; }
        IG_TextUnformatted("Camera: RMB-drag orbit  -  wheel / W,S zoom  -  A,D spin");
    }

    IG_End();
}

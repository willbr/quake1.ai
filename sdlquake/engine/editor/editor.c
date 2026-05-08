// editor.c -- Phase 7 in-game .map editor: top-level state & lifecycle.
//
// F2 toggles editor mode. Editor open implies dev overlay open (ImGui must be
// receiving input) plus the cursor released and the simulation paused.
//
// All other behaviour lives in:
//   edit_scene.c    in-memory scene + selection
//   map_io.c        .map text parse + write
//   brush_compile.c plane-set -> convex polygon windings
//   render_wire.c   wireframe pass (called from r_main.c R_RenderView_)
//   gizmo.c         translate gizmo + drag math
//   editor_ui.c     ImGui panels

#include "quakedef.h"
#include "imgui_layer.h"
#include "imgui_bridge.h"
#include "edit_scene.h"
#include "editor.h"
#include "editor_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern SDL_Window *VID_GetWindow(void);

// Convert a window-coord SDL mouse event into 320x200 vid-space coords.
static void window_to_vid(float wx, float wy, float *vx, float *vy)
{
    SDL_Window *w = VID_GetWindow();
    int ww = 320, wh = 200;
    if (w) SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) { *vx = wx; *vy = wy; return; }

    // SDL3 logical presentation is INTEGER_SCALE (vid_sdl.c). We mirror its
    // letterbox arithmetic: the 320x200 logical area is centred and integer-
    // scaled to fit the window; everything outside maps to negative or
    // out-of-range vid coords.
    float scale_x = (float)ww / (float)vid.width;
    float scale_y = (float)wh / (float)vid.height;
    float scale   = scale_x < scale_y ? floorf(scale_x) : floorf(scale_y);
    if (scale < 1.0f) scale = 1.0f;
    float ox = (ww - vid.width  * scale) * 0.5f;
    float oy = (wh - vid.height * scale) * 0.5f;
    *vx = (wx - ox) / scale;
    *vy = (wy - oy) / scale;
}

static int  s_open = 0;
static int  s_inited = 0;

// -----------------------------------------------------------------------------
// Console commands
// -----------------------------------------------------------------------------

static void Editor_Cmd_Open_f(void)
{
    char path[256];
    const char *name;
    if (Cmd_Argc() < 2)
    {
        Con_Printf("usage: editor_load <mapname>  (loads id1/maps/<name>.map)\n");
        return;
    }
    name = Cmd_Argv(1);
    snprintf(path, sizeof(path), "%s/maps/%s.map", com_gamedir, name);

    if (!Scene_Load(path))
    {
        Con_Printf("editor_load: failed to read %s\n", path);
        return;
    }

    // Track the bare name so editor_save / Restart map can use it.
    {
        size_t n = strlen(name);
        if (n >= sizeof(edit_scene.mapname)) n = sizeof(edit_scene.mapname) - 1;
        memcpy(edit_scene.mapname, name, n);
        edit_scene.mapname[n] = '\0';
    }
    Con_Printf("editor: loaded %d entities from %s\n",
               edit_scene.numentities, path);
}

static void Editor_Cmd_Save_f(void)
{
    const char *name;
    char path[256];
    if (Cmd_Argc() >= 2)
        name = Cmd_Argv(1);
    else if (edit_scene.mapname[0])
        name = edit_scene.mapname;
    else
    {
        Con_Printf("usage: editor_save <mapname>  (no current map loaded)\n");
        return;
    }
    snprintf(path, sizeof(path), "%s/maps/%s.map", com_gamedir, name);

    if (!Scene_Save(path))
    {
        Con_Printf("editor_save: failed to write %s\n", path);
        return;
    }
    Con_Printf("editor: wrote %s\n", path);
}

static void Editor_Cmd_Revert_f(void)
{
    if (!Scene_Revert())
        Con_Printf("editor_revert: nothing to revert (no .map loaded)\n");
    else
        Con_Printf("editor: reverted to last saved %s\n", edit_scene.filename);
}

static void Editor_Cmd_Toggle_f(void)
{
    Editor_Toggle();
}

static void Editor_Cmd_Status_f(void)
{
    extern vec3_t r_origin, vpn;
    int i, j;
    int total_brushes = 0, total_valid = 0;
    Con_Printf("editor: %d entities, file='%s'\n",
               edit_scene.numentities,
               edit_scene.filename[0] ? edit_scene.filename : "(none)");
    Con_Printf("camera origin %.0f %.0f %.0f  forward %.2f %.2f %.2f\n",
               r_origin[0], r_origin[1], r_origin[2],
               vpn[0], vpn[1], vpn[2]);
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = e->classname_idx >= 0
                          ? e->kv[e->classname_idx].value : "(none)";
        Con_Printf("  ent[%d] cls='%s' kvs=%d brushes=%d\n",
                   i, cls, e->numkv, e->numbrushes);
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            total_brushes++;
            if (b->valid) total_valid++;
            Con_Printf("    brush[%d] planes=%d faces=%d valid=%d "
                       "mins=(%.0f %.0f %.0f) maxs=(%.0f %.0f %.0f)\n",
                       j, b->numplanes, b->numfaces, b->valid,
                       b->mins[0], b->mins[1], b->mins[2],
                       b->maxs[0], b->maxs[1], b->maxs[2]);
        }
    }
    Con_Printf("editor: %d/%d brushes compiled successfully\n",
               total_valid, total_brushes);
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void Editor_Init(void)
{
    if (s_inited) return;

    Scene_Init();

    Cmd_AddCommand("editor",        Editor_Cmd_Toggle_f);
    Cmd_AddCommand("editor_load",   Editor_Cmd_Open_f);
    Cmd_AddCommand("editor_save",   Editor_Cmd_Save_f);
    Cmd_AddCommand("editor_revert", Editor_Cmd_Revert_f);
    Cmd_AddCommand("editor_status", Editor_Cmd_Status_f);

    s_inited = 1;
}

void Editor_Shutdown(void)
{
    if (!s_inited) return;
    Scene_Shutdown();
    s_inited = 0;
}

// -----------------------------------------------------------------------------
// Toggle
// -----------------------------------------------------------------------------

void Editor_Toggle(void)
{
    if (!s_inited) return;
    s_open = !s_open;

    // Editor open requires the dev overlay to be open (ImGui receives input,
    // mouse cursor freed). Mirror its state, but only flip if needed.
    if (s_open && !ImguiLayer_IsOpen())
        ImguiLayer_Toggle();
    else if (!s_open && ImguiLayer_IsOpen())
        ImguiLayer_Toggle();
}

int Editor_IsOpen  (void) { return s_open; }
int Editor_IsPaused(void) { return s_open; } // pause sim while editor active

// Editor_RenderScene  -> render_wire.c
// Editor_DrawUI       -> editor_ui.c

// -----------------------------------------------------------------------------
// SDL event hook
// -----------------------------------------------------------------------------

int Editor_ProcessEvent(void *evp)
{
    SDL_Event *ev = (SDL_Event *)evp;
    if (!s_open) return 0;

    switch (ev->type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    {
        if (ev->button.button != SDL_BUTTON_LEFT) return 0;
        // ImGui owns the click if the cursor is over an ImGui window (panel,
        // button). Don't run picking/gizmo in that case.
        if (IG_WantCaptureMouse()) return 0;
        float vx, vy;
        window_to_vid(ev->button.x, ev->button.y, &vx, &vy);
        // Try the gizmo first; if it doesn't grab an axis, treat as a pick.
        if (Editor_GizmoMouseDown(vx, vy)) return 1;
        {
            int e_idx, b_idx;
            if (Editor_PickAt(vx, vy, &e_idx, &b_idx))
            {
                edit_scene.sel_entity = e_idx;
                edit_scene.sel_brush  = b_idx;
            }
            else
            {
                edit_scene.sel_entity = -1;
                edit_scene.sel_brush  = -1;
            }
        }
        return 1;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button == SDL_BUTTON_LEFT && Editor_GizmoIsActive())
        {
            Editor_GizmoMouseUp();
            return 1;
        }
        return 0;
    case SDL_EVENT_MOUSE_MOTION:
        if (Editor_GizmoIsActive())
        {
            float vx, vy;
            window_to_vid(ev->motion.x, ev->motion.y, &vx, &vy);
            Editor_GizmoMouseMove(vx, vy);
            return 1;
        }
        return 0;
    }
    return 0;
}

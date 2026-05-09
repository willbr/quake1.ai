// editor_ui.c -- M1 ImGui panels: toolbar, brush list, inspector.
//
// All panels are drawn inside Editor_DrawUI which is called from
// imgui_layer.c ImguiLayer_Render. The panels only render when the editor is
// open (so they don't clutter the regular F12 dev overlay).

#include "quakedef.h"
#include "imgui_bridge.h"
#include "edit_scene.h"
#include "editor.h"
#include "editor_internal.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Toolbar
// -----------------------------------------------------------------------------

// Layout constants (in window pixels). Toolbar runs across the top; brush
// list anchors the bottom-left, inspector the bottom-right.
#define UI_PAD          10
#define UI_TOOLBAR_H    72
#define UI_LEFT_W       320
#define UI_RIGHT_W      360

static void draw_toolbar(void)
{
    extern cvar_t editor_render_style;
    extern cvar_t editor_camera;
    extern cvar_t editor_grid_snap;
    extern cvar_t editor_grid_size;
    extern cvar_t editor_grid_absolute;
    static const char *style_items[] = {
        "wireframe", "flat", "flat+wire", "textured", "textured+wire"
    };
    static const char *camera_items[] = { "free-fly", "fps" };
    // Quake physics constants drive the gameplay-named entries: 18 = step
    // (max walkable step), 45 = jump apex (270²/(2*800)), 56 = player bbox
    // height (-24 to 32). The rest are powers of 2 for the build grid.
    static const float grid_values[] = { 1, 4, 8, 16, 18, 32, 45, 56, 64, 128 };
    static const char *grid_items[] = {
        "1", "4", "8", "16 (build)", "18 (step)", "32 (door)",
        "45 (jump)", "56 (player)", "64", "128 (room)"
    };
    enum { GRID_N = (int)(sizeof(grid_values) / sizeof(grid_values[0])) };

    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    IG_SetNextWindowPos((float)UI_PAD, (float)UI_PAD, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize(disp_w - 2 * UI_PAD, (float)UI_TOOLBAR_H, IG_Cond_FirstUseEver);
    if (!IG_Begin("Editor", NULL, IG_WF_None)) { IG_End(); return; }

    if (IG_Button("Save"))         Cbuf_AddText("editor_save\n");
    IG_SameLine(0, -1);
    if (IG_Button("Revert edits")) Cbuf_AddText("editor_revert\n");
    IG_SameLine(0, -1);
    if (IG_Button("Restart map") && edit_scene.mapname[0])
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "map %s\n", edit_scene.mapname);
        Cbuf_AddText(buf);
    }
    IG_SameLine(0, -1);
    // Spawn a 64-unit cube ~128 units in front of the camera, snapped to a
    // 16-unit grid. The console command does the camera math too — keep them
    // in sync.
    if (IG_Button("Add cube"))     Cbuf_AddText("editor_brush_add_cube\n");
    IG_SameLine(0, -1);
    if (IG_Button("Group"))        Cbuf_AddText("editor_group\n");
    IG_SameLine(0, -1);
    if (IG_Button("Ungroup"))      Cbuf_AddText("editor_ungroup\n");
    IG_SameLine(0, -1);
    if (IG_Button("Undo"))         Cbuf_AddText("editor_undo\n");
    IG_SameLine(0, -1);
    if (IG_Button("Redo"))         Cbuf_AddText("editor_redo\n");
    IG_SameLine(0, -1);
    if (IG_Button("Close (F2)"))   Cbuf_AddText("editor\n");

    {
        int style = (int)editor_render_style.value;
        if (style < 0) style = 0;
        if (style >= (int)(sizeof(style_items) / sizeof(style_items[0])))
            style = (int)(sizeof(style_items) / sizeof(style_items[0])) - 1;
        IG_SetNextItemWidth(160);
        if (IG_Combo("render style", &style, style_items,
                     (int)(sizeof(style_items) / sizeof(style_items[0]))))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "editor_render_style %d\n", style);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Snap toggle. Cvar is float; treat any non-zero as on.
        int snap = editor_grid_snap.value != 0.0f;
        if (IG_Checkbox("snap", &snap))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_grid_snap %d\n", snap ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Absolute snap mode — snap the brush centroid to world grid lines
        // (useful for stairs / lining brushes up across drags). Off ⇒ snap
        // is relative to the drag-start position.
        int abs = editor_grid_absolute.value != 0.0f;
        if (IG_Checkbox("abs", &abs))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_grid_absolute %d\n", abs ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Grid size dropdown. Find current size in the preset list; if it's
        // not a preset (user set a custom value), select index 0 visually but
        // don't overwrite the cvar unless they pick something.
        int sel = -1, k;
        float cur = editor_grid_size.value;
        for (k = 0; k < GRID_N; k++)
            if (grid_values[k] == cur) { sel = k; break; }
        if (sel < 0) sel = 0;
        IG_SetNextItemWidth(140);
        if (IG_Combo("grid", &sel, grid_items, GRID_N))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "editor_grid_size %g\n", grid_values[sel]);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        int cam = (int)editor_camera.value;
        if (cam < 0) cam = 0;
        if (cam >= (int)(sizeof(camera_items) / sizeof(camera_items[0])))
            cam = (int)(sizeof(camera_items) / sizeof(camera_items[0])) - 1;
        IG_SetNextItemWidth(110);
        if (IG_Combo("camera (Tab)", &cam, camera_items,
                     (int)(sizeof(camera_items) / sizeof(camera_items[0]))))
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "editor_camera %d\n", cam);
            Cbuf_AddText(buf);
        }
    }
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "loaded: %s    hold RMB to look + WASD to move",
                 edit_scene.mapname[0] ? edit_scene.mapname : "(none)");
        IG_TextUnformatted(buf);
    }
    IG_End();
}

// -----------------------------------------------------------------------------
// Brush list
// -----------------------------------------------------------------------------

static void draw_brush_list(void)
{
    int i, j;
    char buf[128];
    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    float y    = (float)(UI_PAD + UI_TOOLBAR_H + UI_PAD);
    float h    = disp_h - y - UI_PAD;

    IG_SetNextWindowPos((float)UI_PAD, y, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize((float)UI_LEFT_W, h, IG_Cond_FirstUseEver);
    if (!IG_Begin("Brushes", NULL, IG_WF_None)) { IG_End(); return; }

    snprintf(buf, sizeof(buf), "%d entities, ? brushes",
             edit_scene.numentities);
    {
        int total = 0;
        for (i = 0; i < edit_scene.numentities; i++)
            total += edit_scene.entities[i].numbrushes;
        snprintf(buf, sizeof(buf), "%d entities, %d brushes",
                 edit_scene.numentities, total);
    }
    IG_TextUnformatted(buf);
    IG_Separator();

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = "(no classname)";
        if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;

        IG_PushID_Int(i);
        snprintf(buf, sizeof(buf), "[%d] %s", i, cls);
        IG_TextUnformatted(buf);

        for (j = 0; j < e->numbrushes; j++)
        {
            int sel = Scene_SelectionContains(i, j);
            edit_brush_t *b = &e->brushes[j];
            snprintf(buf, sizeof(buf),
                     "  brush %d (%d planes, %d faces)##b%d_%d",
                     j, b->numplanes, b->numfaces, i, j);
            if (IG_Selectable(buf, sel, 0))
            {
                // Match the 3D-viewport semantics: shift toggles, plain
                // click replaces. SDL_GetModState reads OS keyboard state
                // so it works regardless of which window has focus.
                SDL_Keymod mod = SDL_GetModState();
                int shift = (mod & SDL_KMOD_SHIFT) != 0;
                if (shift)
                {
                    Scene_SelectionToggle(i, j);
                }
                else
                {
                    Scene_SelectionClear();
                    Scene_SelectionAdd(i, j);
                }
            }
        }
        IG_PopID();
    }
    IG_End();
}

// -----------------------------------------------------------------------------
// Inspector
// -----------------------------------------------------------------------------

static void draw_inspector(void)
{
    edit_entity_t *e;
    edit_brush_t  *b;
    int i;
    char buf[128];
    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    float y = (float)(UI_PAD + UI_TOOLBAR_H + UI_PAD);
    float h = disp_h - y - UI_PAD;
    float x = disp_w - UI_RIGHT_W - UI_PAD;

    IG_SetNextWindowPos(x, y, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize((float)UI_RIGHT_W, h, IG_Cond_FirstUseEver);
    if (!IG_Begin("Inspector", NULL, IG_WF_None)) { IG_End(); return; }

    e = Scene_GetSelectedEntity();
    b = Scene_GetSelectedBrush();
    if (!e)
    {
        IG_TextUnformatted("(no selection)");
        IG_End();
        return;
    }

    IG_TextUnformatted("entity keys");
    IG_Separator();

    for (i = 0; i < e->numkv; i++)
    {
        IG_PushID_Int(i);
        snprintf(buf, sizeof(buf), "%s##key", e->kv[i].key);
        IG_SetNextItemWidth(180);
        IG_InputText(buf, e->kv[i].value, EDIT_VAL_LEN, IG_ITF_EnterReturnsTrue);
        IG_PopID();
    }
    IG_Separator();

    if (b)
    {
        vec3_t centroid;
        int n_sel = Scene_NumSelected();
        Editor_BrushCentroid(b, centroid);
        if (n_sel > 1)
        {
            snprintf(buf, sizeof(buf),
                     "%d brushes selected (showing primary)",
                     n_sel);
            IG_TextUnformatted(buf);
        }
        snprintf(buf, sizeof(buf),
                 "brush planes=%d  faces=%d",
                 b->numplanes, b->numfaces);
        IG_TextUnformatted(buf);
        snprintf(buf, sizeof(buf), "centroid: %.0f %.0f %.0f",
                 centroid[0], centroid[1], centroid[2]);
        IG_TextUnformatted(buf);
        snprintf(buf, sizeof(buf), "mins: %.0f %.0f %.0f",
                 b->mins[0], b->mins[1], b->mins[2]);
        IG_TextUnformatted(buf);
        snprintf(buf, sizeof(buf), "maxs: %.0f %.0f %.0f",
                 b->maxs[0], b->maxs[1], b->maxs[2]);
        IG_TextUnformatted(buf);
    }
    else
    {
        IG_TextUnformatted("(point entity — no brushes)");
    }

    IG_End();
}

// -----------------------------------------------------------------------------
// Public entry
// -----------------------------------------------------------------------------

void Editor_DrawUI(void)
{
    if (!Editor_IsOpen()) return;
    draw_toolbar();
    draw_brush_list();
    draw_inspector();
}

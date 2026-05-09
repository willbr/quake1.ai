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
#define UI_TOOLBAR_H    100
#define UI_LEFT_W       320
#define UI_RIGHT_W      360

// Entity-palette classnames offered in the toolbar combo. Anything outside
// this list is reachable via the editor_entity_add console command.
static const char *s_entity_classes[] = {
    "info_player_start",
    "info_player_deathmatch",
    "light",
    "monster_army",
    "monster_dog",
    "monster_ogre",
    "monster_demon1",
    "monster_shambler",
    "monster_knight",
    "monster_wizard",
    "monster_zombie",
    "weapon_supershotgun",
    "weapon_nailgun",
    "weapon_supernailgun",
    "weapon_grenadelauncher",
    "weapon_rocketlauncher",
    "weapon_lightning",
    "item_health",
    "item_armor1",
    "item_armor2",
    "item_armorInv",
    "item_shells",
    "item_spikes",
    "item_rockets",
    "item_cells",
};
enum { S_ENTITY_CLASSES_N
       = (int)(sizeof(s_entity_classes) / sizeof(s_entity_classes[0])) };

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
    // Entity palette: classname combo + Add button. Sits on the next row
    // below the main button strip. Selection persists across frames so the
    // user can spam Add to drop several of the same kind.
    {
        static int sel_class = 0;
        if (sel_class < 0) sel_class = 0;
        if (sel_class >= S_ENTITY_CLASSES_N) sel_class = S_ENTITY_CLASSES_N - 1;
        IG_SetNextItemWidth(220);
        IG_Combo("entity class", &sel_class, s_entity_classes,
                 S_ENTITY_CLASSES_N);
        IG_SameLine(0, -1);
        if (IG_Button("Add entity"))
        {
            char buf[96];
            snprintf(buf, sizeof(buf), "editor_entity_add %s\n",
                     s_entity_classes[sel_class]);
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

// Per-category hide flag, indexed by EDIT_CAT_*. Persists across panel
// re-opens within a session. Default is everything visible. The
// "OTHER" slot covers worldspawn / misc_* / etc — we don't expose a
// checkbox for it (hiding worldspawn would render the level invisible),
// but the array entry exists so EDIT_CAT_OTHER indexing is uniform.
static int s_hide_cat[EDIT_CAT_COUNT] = { 0 };

int Editor_EntityHidden(int e_idx)
{
    int cat;
    edit_entity_t *e;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];
    cat = Editor_EntityCategory(e);
    if (cat <= 0 || cat >= EDIT_CAT_COUNT) return 0;
    return s_hide_cat[cat];
}

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

    // Filter checkboxes — "Hide X". Inverted so unchecked = visible
    // (the default state) reads naturally. Lay them out two per row.
    {
        int hide_trig    = s_hide_cat[EDIT_CAT_TRIGGER];
        int hide_light   = s_hide_cat[EDIT_CAT_LIGHT];
        int hide_spawn   = s_hide_cat[EDIT_CAT_SPAWN];
        int hide_item    = s_hide_cat[EDIT_CAT_ITEM];
        int hide_monster = s_hide_cat[EDIT_CAT_MONSTER];
        int hide_func    = s_hide_cat[EDIT_CAT_FUNC];
        int hide_sound   = s_hide_cat[EDIT_CAT_SOUND];
        int hide_path    = s_hide_cat[EDIT_CAT_PATH];
        int hide_misc    = s_hide_cat[EDIT_CAT_MISC];
        int hide_info    = s_hide_cat[EDIT_CAT_INFO];
        IG_TextUnformatted("Hide:");
        if (IG_Checkbox("triggers", &hide_trig))    s_hide_cat[EDIT_CAT_TRIGGER] = hide_trig;
        IG_SameLine(0, -1);
        if (IG_Checkbox("lights",   &hide_light))   s_hide_cat[EDIT_CAT_LIGHT]   = hide_light;
        if (IG_Checkbox("spawns",   &hide_spawn))   s_hide_cat[EDIT_CAT_SPAWN]   = hide_spawn;
        IG_SameLine(0, -1);
        if (IG_Checkbox("items",    &hide_item))    s_hide_cat[EDIT_CAT_ITEM]    = hide_item;
        if (IG_Checkbox("monsters", &hide_monster)) s_hide_cat[EDIT_CAT_MONSTER] = hide_monster;
        IG_SameLine(0, -1);
        if (IG_Checkbox("funcs",    &hide_func))    s_hide_cat[EDIT_CAT_FUNC]    = hide_func;
        if (IG_Checkbox("sounds",   &hide_sound))   s_hide_cat[EDIT_CAT_SOUND]   = hide_sound;
        IG_SameLine(0, -1);
        if (IG_Checkbox("paths",    &hide_path))    s_hide_cat[EDIT_CAT_PATH]    = hide_path;
        if (IG_Checkbox("misc",     &hide_misc))    s_hide_cat[EDIT_CAT_MISC]    = hide_misc;
        IG_SameLine(0, -1);
        if (IG_Checkbox("info",     &hide_info))    s_hide_cat[EDIT_CAT_INFO]    = hide_info;
    }
    IG_Separator();

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = "(no classname)";
        if (Editor_EntityHidden(i)) continue;
        if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;

        IG_PushID_Int(i);
        if (Entity_IsPoint(e))
        {
            // Point entity: header itself is the selectable. Clicking it
            // selects (i, -1) so the gizmo anchors at its origin.
            // Double-click frames the camera on it.
            int sel = Scene_SelectionContains(i, -1);
            snprintf(buf, sizeof(buf), "[%d] %s##e%d", i, cls, i);
            if (IG_Selectable(buf, sel, IG_SF_AllowDoubleClick))
            {
                SDL_Keymod mod = SDL_GetModState();
                int shift = (mod & SDL_KMOD_SHIFT) != 0;
                if (shift)
                {
                    Scene_SelectionToggle(i, -1);
                }
                else
                {
                    Scene_SelectionClear();
                    Scene_SelectionAdd(i, -1);
                }
                if (IG_IsMouseDoubleClicked(0))
                    Editor_FrameItem(i, -1);
            }
        }
        else
        {
            snprintf(buf, sizeof(buf), "[%d] %s", i, cls);
            IG_TextUnformatted(buf);

            for (j = 0; j < e->numbrushes; j++)
            {
                int sel = Scene_SelectionContains(i, j);
                edit_brush_t *b = &e->brushes[j];
                snprintf(buf, sizeof(buf),
                         "  brush %d (%d planes, %d faces)##b%d_%d",
                         j, b->numplanes, b->numfaces, i, j);
                if (IG_Selectable(buf, sel, IG_SF_AllowDoubleClick))
                {
                    // Match the 3D-viewport semantics: shift toggles,
                    // plain click replaces. SDL_GetModState reads OS
                    // keyboard state so it works regardless of which
                    // window has focus.
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
                    if (IG_IsMouseDoubleClicked(0))
                        Editor_FrameItem(i, j);
                }
            }
        }
        IG_PopID();
    }
    IG_End();
}

// -----------------------------------------------------------------------------
// spawnflags tables — class-specific bits, plus the universal skill/DM bits.
// -----------------------------------------------------------------------------

typedef struct { unsigned bit; const char *label; } spawnflag_def_t;

typedef struct {
    const char *classname;       // exact match (NULL → use prefix)
    const char *prefix;          // prefix match (NULL → use classname)
    const spawnflag_def_t *flags;
    int n;
} class_flag_table_t;

static const spawnflag_def_t s_universal_flags[] = {
    {  256, "Not on Easy" },
    {  512, "Not on Normal" },
    { 1024, "Not on Hard" },
    { 2048, "Not in Deathmatch" },
};

static const spawnflag_def_t s_light_flags[]   = { { 1, "Start off" } };
static const spawnflag_def_t s_door_flags[]    = {
    { 1,  "Start open" }, { 4,  "Don't link" }, { 8,  "Gold key" },
    { 16, "Silver key" }, { 32, "Toggle" },
};
static const spawnflag_def_t s_plat_flags[]    = { { 1, "Low trigger" } };
static const spawnflag_def_t s_trigger_flags[] = { { 1, "No touch" } };
static const spawnflag_def_t s_health_flags[]  = {
    { 1, "Rotten (15)" }, { 2, "Mega (100)" },
};
static const spawnflag_def_t s_changelevel_flags[] = { { 1, "Silent" } };

static const class_flag_table_t s_class_flag_tables[] = {
    { NULL,                  "light",      s_light_flags,        1 },
    { "func_door",           NULL,         s_door_flags,         5 },
    { "func_door_secret",    NULL,         s_door_flags,         5 },
    { "func_plat",           NULL,         s_plat_flags,         1 },
    { "trigger_multiple",    NULL,         s_trigger_flags,      1 },
    { "trigger_once",        NULL,         s_trigger_flags,      1 },
    { "trigger_changelevel", NULL,         s_changelevel_flags,  1 },
    { "item_health",         NULL,         s_health_flags,       2 },
};

static const class_flag_table_t *find_class_flags(const char *classname)
{
    int i;
    if (!classname) return NULL;
    for (i = 0; i < (int)(sizeof(s_class_flag_tables)
                          / sizeof(s_class_flag_tables[0])); i++)
    {
        const class_flag_table_t *t = &s_class_flag_tables[i];
        if (t->classname && !strcmp(t->classname, classname)) return t;
        if (t->prefix && !strncmp(t->prefix, classname, strlen(t->prefix)))
            return t;
    }
    return NULL;
}

static int kv_lookup(const edit_entity_t *e, const char *key)
{
    int i;
    for (i = 0; i < e->numkv; i++)
        if (!strcmp(e->kv[i].key, key)) return i;
    return -1;
}

// Render the spawnflags section: class-specific checkboxes (if known),
// universal skill/DM checkboxes, and a raw integer fallback. Mutates the
// "spawnflags" kv (creating it on first toggle) and pushes the new value to
// e->live_ent->v.spawnflags so the live entity sees the change too.
static void draw_spawnflags_section(edit_entity_t *e)
{
    char buf[32];
    char label[80];
    int  changed = 0, on, j;
    int  current;
    const char *cls = e->classname_idx >= 0
                      ? e->kv[e->classname_idx].value : NULL;
    const class_flag_table_t *cf = find_class_flags(cls);
    int  sf_idx = kv_lookup(e, "spawnflags");

    current = sf_idx >= 0 ? atoi(e->kv[sf_idx].value) : 0;

    IG_TextUnformatted("spawnflags");

    // Class-specific bits first (more interesting per-entity).
    if (cf)
    {
        for (j = 0; j < cf->n; j++)
        {
            on = (current & cf->flags[j].bit) != 0;
            snprintf(label, sizeof(label), "%s##cf%d", cf->flags[j].label, j);
            if (IG_Checkbox(label, &on))
            {
                if (on) current |=  cf->flags[j].bit;
                else    current &= ~cf->flags[j].bit;
                changed = 1;
            }
        }
    }

    // Universal skill / DM filter bits — apply to every entity.
    for (j = 0; j < (int)(sizeof(s_universal_flags) / sizeof(s_universal_flags[0])); j++)
    {
        on = (current & s_universal_flags[j].bit) != 0;
        snprintf(label, sizeof(label), "%s##uf%d",
                 s_universal_flags[j].label, j);
        if (IG_Checkbox(label, &on))
        {
            if (on) current |=  s_universal_flags[j].bit;
            else    current &= ~s_universal_flags[j].bit;
            changed = 1;
        }
    }

    // Raw integer — handles bits the table doesn't know about, and lets
    // power users type a value directly.
    snprintf(buf, sizeof(buf), "%d", current);
    IG_SetNextItemWidth(120);
    if (IG_InputText("raw##sfraw", buf, sizeof(buf), IG_ITF_EnterReturnsTrue))
    {
        current = atoi(buf);
        changed = 1;
    }

    if (changed)
    {
        snprintf(buf, sizeof(buf), "%d", current);
        Entity_SetKV(e, "spawnflags", buf);
        if (e->live_ent && !e->live_ent->free)
            e->live_ent->v.spawnflags = (float)current;
    }
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

    // classname first (always), then origin, then everything else in kv
    // array order. Most parsers happen to put classname first already, but
    // for entities populated from worldmodel->entities the order can vary,
    // and reading "classname" should never require scanning a long list.
    // spawnflags gets its own dedicated checkbox section below — skip it
    // in the generic kv loop.
    {
        int sf_idx = kv_lookup(e, "spawnflags");
        int order[3] = { e->classname_idx, e->origin_idx, -1 };
        int j;
        for (j = 0; j < 2; j++)
        {
            int idx = order[j];
            if (idx < 0 || idx >= e->numkv) continue;
            IG_PushID_Int(idx);
            snprintf(buf, sizeof(buf), "%s##key", e->kv[idx].key);
            IG_SetNextItemWidth(180);
            IG_InputText(buf, e->kv[idx].value, EDIT_VAL_LEN,
                         IG_ITF_EnterReturnsTrue);
            IG_PopID();
        }
        for (i = 0; i < e->numkv; i++)
        {
            if (i == e->classname_idx) continue;
            if (i == e->origin_idx)    continue;
            if (i == sf_idx)           continue;
            IG_PushID_Int(i);
            snprintf(buf, sizeof(buf), "%s##key", e->kv[i].key);
            IG_SetNextItemWidth(180);
            IG_InputText(buf, e->kv[i].value, EDIT_VAL_LEN,
                         IG_ITF_EnterReturnsTrue);
            IG_PopID();
        }
    }
    IG_Separator();
    draw_spawnflags_section(e);
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

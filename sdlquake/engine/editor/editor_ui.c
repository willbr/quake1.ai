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
#include <math.h>
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
    extern cvar_t editor_view_mode;
    static const char *style_items[] = {
        "wireframe", "flat", "flat+wire", "textured", "textured+wire"
    };
    static const char *camera_items[] = { "free-fly", "fps" };
    static const char *view_items[]   = { "live", "map" };
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
    if (IG_Button("Delete (Del)")) Cbuf_AddText("editor_delete\n");
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
        // Fullbright toggle. Maps to the engine's r_fullbright cvar — zeroes
        // the lightmap so world surfaces draw at full intensity (alias
        // models still vertex-shaded; that's vanilla software-renderer
        // behaviour). Cheat-protected only in multiplayer; r_misc.c forces
        // it back to 0 if cl.maxclients > 1.
        cvar_t *fb = Cvar_FindVar("r_fullbright");
        int on = fb && fb->value != 0.0f;
        if (IG_Checkbox("fullbright", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "r_fullbright %d\n", on ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Per-entity facing / movedir arrow visibility. Selection always
        // overrides — a clicked entity shows its arrow either way.
        extern cvar_t editor_show_angles;
        int on = editor_show_angles.value != 0.0f;
        if (IG_Checkbox("angles", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_show_angles %d\n", on ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Target / killtarget link visibility. Selection overrides per
        // pair — a link from or to a selected ent is always drawn.
        extern cvar_t editor_show_links;
        int on = editor_show_links.value != 0.0f;
        if (IG_Checkbox("links", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_show_links %d\n", on ? 1 : 0);
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
    IG_SameLine(0, -1);
    {
        // View mode: show running edicts ("live") vs .map text ("map").
        // Eliminates double-rendering when AI has moved a monster away
        // from its .map origin.
        int vm = (int)editor_view_mode.value;
        if (vm < 0) vm = 0;
        if (vm >= (int)(sizeof(view_items) / sizeof(view_items[0])))
            vm = (int)(sizeof(view_items) / sizeof(view_items[0])) - 1;
        IG_SetNextItemWidth(90);
        if (IG_Combo("view", &vm, view_items,
                     (int)(sizeof(view_items) / sizeof(view_items[0]))))
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "editor_view_mode %d\n", vm);
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
// re-opens within a session. The "OTHER" slot covers worldspawn /
// misc_* / etc — we don't expose a checkbox for it (hiding worldspawn
// would render the level invisible), but the array entry exists so
// EDIT_CAT_OTHER indexing is uniform.
//
// Default hides triggers, lights, ambient sounds, path corners, and
// info_* metadata — none of these have a model the engine actually
// renders, so they just clutter the view; the user can re-enable
// them from the panel checkboxes.
static int s_hide_cat[EDIT_CAT_COUNT] = {
    [EDIT_CAT_TRIGGER] = 1,
    [EDIT_CAT_LIGHT]   = 1,
    [EDIT_CAT_SOUND]   = 1,
    [EDIT_CAT_PATH]    = 1,
    [EDIT_CAT_INFO]    = 1,
};

// "Visible only" toggle — when on, anything outside the camera frustum
// or occluded by world geometry is hidden, regardless of category. Useful
// for "what's actually in front of me right now" while authoring a room.
static int s_visible_only = 0;

// Difficulty / mode preview. -1 = no filter (show all entities); 0/1/2 =
// skill 0/1/2 (easy/normal/hard, where hard also covers nightmare since
// vanilla shares the bit); 3 = deathmatch. Filters against the universal
// NOT_EASY/NOT_NORMAL/NOT_HARD/NOT_DEATHMATCH spawnflag bits so the user
// can preview which entities will actually spawn at each difficulty.
static int s_skill_filter = -1;

// Called from Editor_Toggle when the editor opens. Resets s_skill_filter
// to match the game's current `skill` (and `deathmatch`) cvars so the
// preview dropdown lines up with what would actually spawn if the user
// hit Restart map. The user can override after the sync — the override
// stays until the next time the editor opens.
void Editor_UI_OnOpen(void)
{
    extern cvar_t skill, deathmatch;
    int s = (int)skill.value;
    if (deathmatch.value != 0.0f) { s_skill_filter = 3; return; }
    if (s < 0) s = 0;
    if (s > 2) s = 2;       // collapse nightmare onto hard (vanilla
                            // shares the NOT_HARD spawnflag bit)
    s_skill_filter = s;
}

static int entity_hidden_by_skill(const edit_entity_t *e)
{
    int idx, sf;
    if (s_skill_filter < 0) return 0;
    idx = -1;
    {
        int k;
        for (k = 0; k < e->numkv; k++)
            if (!strcmp(e->kv[k].key, "spawnflags")) { idx = k; break; }
    }
    if (idx < 0) return 0;
    sf = atoi(e->kv[idx].value);
    switch (s_skill_filter)
    {
    case 0: return (sf &  256) != 0;   // NOT_EASY
    case 1: return (sf &  512) != 0;   // NOT_NORMAL
    case 2: return (sf & 1024) != 0;   // NOT_HARD (and nightmare)
    case 3: return (sf & 2048) != 0;   // NOT_DEATHMATCH
    }
    return 0;
}

int Editor_EntityHidden(int e_idx)
{
    int cat;
    edit_entity_t *e;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];
    cat = Editor_EntityCategory(e);
    if (cat > 0 && cat < EDIT_CAT_COUNT && s_hide_cat[cat]) return 1;
    if (s_visible_only && !Editor_EntityInView(e_idx)) return 1;
    if (entity_hidden_by_skill(e)) return 1;
    return 0;
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

    // Difficulty preview dropdown. Index 0 = "All" (no filter); indices
    // 1..4 map to skill_filter 0..3.
    {
        static const char *preview_items[] = {
            "All", "Easy", "Normal", "Hard / Nightmare", "Deathmatch"
        };
        int sel = s_skill_filter + 1;
        if (sel < 0) sel = 0;
        if (sel >= 5) sel = 4;
        IG_SetNextItemWidth(160);
        if (IG_Combo("preview", &sel, preview_items, 5))
            s_skill_filter = sel - 1;
    }
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
        // "Visible only" — orthogonal to category filters; AND'd with them.
        if (IG_Checkbox("visible only", &s_visible_only)) { /* applied next frame */ }
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

// Inspector edits update e->kv[i].value directly via IG_InputText. For
// most keys that's enough — the engine reads them at next map load. But
// for fields the running engine + renderer is already using (origin,
// angle/angles), the live edict and the cl_entities mirror need a poke
// or the typed value won't visibly take effect until close+reopen. The
// gizmo paths already sync these; this helper does the same for direct
// kv edits.
static void inspector_sync_live(edit_entity_t *e, const char *key,
                                const char *val)
{
    if (!e->live_ent || e->live_ent->free) return;

    if (!strcmp(key, "origin"))
    {
        float x = 0, y = 0, z = 0;
        if (sscanf(val, "%f %f %f", &x, &y, &z) < 3) return;
        e->live_ent->v.origin[0] = x;
        e->live_ent->v.origin[1] = y;
        e->live_ent->v.origin[2] = z;
        SV_LinkEdict(e->live_ent, false);
        {
            int en = NUM_FOR_EDICT(e->live_ent);
            if (en > 0 && en < cl.num_entities)
            {
                entity_t *ce = &cl_entities[en];
                ce->origin[0] = x; ce->origin[1] = y; ce->origin[2] = z;
                VectorCopy(ce->origin, ce->msg_origins[0]);
                VectorCopy(ce->origin, ce->msg_origins[1]);
                ce->msgtime  = cl.mtime[0];
                ce->forcelink = true;
            }
        }
        return;
    }

    if (!strcmp(key, "angle") || !strcmp(key, "angles"))
    {
        float pitch = 0, yaw = 0, roll = 0;
        int parsed = 0;
        if (!strcmp(key, "angle"))
        {
            float a = (float)atof(val);
            // -1 / -2 sentinels: SetMovedir would have set vertical
            // movedir at spawn. Mirror that here so movers re-aim
            // correctly.
            if (a == -1.0f || a == -2.0f)
            {
                e->live_ent->v.angles[0] = 0;
                e->live_ent->v.angles[1] = 0;
                e->live_ent->v.angles[2] = 0;
                e->live_ent->v.movedir[0] = 0;
                e->live_ent->v.movedir[1] = 0;
                e->live_ent->v.movedir[2] = (a == -1.0f) ? 1.0f : -1.0f;
                goto sync_cl;
            }
            yaw = a;
            parsed = 1;
        }
        else
        {
            if (sscanf(val, "%f %f %f", &pitch, &yaw, &roll) >= 1) parsed = 1;
        }
        if (!parsed) return;
        e->live_ent->v.angles[0] = pitch;
        e->live_ent->v.angles[1] = yaw;
        e->live_ent->v.angles[2] = roll;

        // Rebuild v.movedir from the new angles for movers (any entity
        // whose movedir was already non-zero has been through SetMovedir).
        // Non-movers leave movedir at (0,0,0) so this is a no-op for them.
        {
            float *md = e->live_ent->v.movedir;
            if (md[0] != 0.0f || md[1] != 0.0f || md[2] != 0.0f)
            {
                vec3_t fwd, right, up;
                AngleVectors(e->live_ent->v.angles, fwd, right, up);
                VectorCopy(fwd, md);
            }
        }
sync_cl:
        {
            int en = NUM_FOR_EDICT(e->live_ent);
            if (en > 0 && en < cl.num_entities)
            {
                entity_t *ce = &cl_entities[en];
                VectorCopy(e->live_ent->v.angles, ce->angles);
                VectorCopy(e->live_ent->v.angles, ce->msg_angles[0]);
                VectorCopy(e->live_ent->v.angles, ce->msg_angles[1]);
                ce->msgtime  = cl.mtime[0];
                ce->forcelink = true;
            }
        }
    }
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
// Live state readout — runtime fields that aren't in the .map kv. Read-only:
// gives the user a debugging view of what the engine is actually doing with
// the selected entity right now (animation frame, AI target, velocity, …).
// Refreshes every UI tick — values move while the sim is running.
// -----------------------------------------------------------------------------

static const char *solid_name(int s)
{
    switch (s)
    {
    case SOLID_NOT:      return "NOT";
    case SOLID_TRIGGER:  return "TRIGGER";
    case SOLID_BBOX:     return "BBOX";
    case SOLID_SLIDEBOX: return "SLIDEBOX";
    case SOLID_BSP:      return "BSP";
    default:             return "?";
    }
}

static const char *movetype_name(int mt)
{
    static const char *names[] = {
        "NONE", "ANGLENOCLIP", "ANGLECLIP", "WALK", "STEP", "FLY",
        "TOSS", "PUSH", "NOCLIP", "FLYMISSILE", "BOUNCE"
    };
    if (mt >= 0 && mt < (int)(sizeof(names) / sizeof(names[0])))
        return names[mt];
    return "?";
}

static const char *deadflag_name(int df)
{
    switch (df)
    {
    case DEAD_NO:    return "NO";
    case DEAD_DYING: return "DYING";
    case DEAD_DEAD:  return "DEAD";
    case 3:          return "RESPAWNABLE";
    default:         return "?";
    }
}

static void format_flags(int flags, char *out, int out_sz)
{
    static const struct { int bit; const char *name; } tab[] = {
        {FL_FLY,           "FLY"},
        {FL_SWIM,          "SWIM"},
        {FL_CONVEYOR,      "CONVEYOR"},
        {FL_CLIENT,        "CLIENT"},
        {FL_INWATER,       "INWATER"},
        {FL_MONSTER,       "MONSTER"},
        {FL_GODMODE,       "GODMODE"},
        {FL_NOTARGET,      "NOTARGET"},
        {FL_ITEM,          "ITEM"},
        {FL_ONGROUND,      "ONGROUND"},
        {FL_PARTIALGROUND, "PARTGND"},
        {FL_WATERJUMP,     "WATERJUMP"},
        {FL_JUMPRELEASED,  "JMPREL"},
    };
    int i, n = 0;
    out[0] = 0;
    for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++)
    {
        if (flags & tab[i].bit)
        {
            int w = snprintf(out + n, out_sz - n, "%s%s",
                             n ? "|" : "", tab[i].name);
            if (w < 0 || w >= out_sz - n) break;
            n += w;
        }
    }
    if (out[0] == 0) snprintf(out, out_sz, "(none)");
}

// Resolve a v.enemy / v.owner / v.goalentity link to a printable label.
// Sentinel: edict 0 is the world edict, treated as "(none)" here so the
// reader doesn't see a meaningless "worldspawn" link on every monster
// without a current target.
static void format_edict_link(edict_t *ed, char *out, int out_sz)
{
    int idx;
    if (!ed || ed->free) { snprintf(out, out_sz, "(none)"); return; }
    idx = NUM_FOR_EDICT(ed);
    if (idx == 0) { snprintf(out, out_sz, "(none)"); return; }
    snprintf(out, out_sz, "%s [#%d]",
             ed->v.classname ? ed->v.classname : "?", idx);
}

static void draw_live_state(edit_entity_t *e)
{
    extern cvar_t editor_view_mode;
    edict_t *ed;
    char buf[160];
    int en;

    // Live state only makes sense in live mode + with a real edict bound.
    // Map mode would be misleading — the user is reading what the .map
    // says, not what the engine is doing.
    if ((int)editor_view_mode.value != 0) return;
    ed = e->live_ent;
    if (!ed || ed->free) return;

    IG_Separator();
    IG_TextUnformatted("live state");

    en = NUM_FOR_EDICT(ed);
    snprintf(buf, sizeof(buf), "edict #%d  %s / %s",
             en, solid_name((int)ed->v.solid),
             movetype_name((int)ed->v.movetype));
    IG_TextUnformatted(buf);

    if (ed->v.health != 0 || ed->v.max_health != 0)
    {
        snprintf(buf, sizeof(buf), "health %g / %g  takedamage %d  deadflag %s",
                 ed->v.health, ed->v.max_health,
                 (int)ed->v.takedamage,
                 deadflag_name((int)ed->v.deadflag));
        IG_TextUnformatted(buf);
    }

    {
        const float *v = ed->v.velocity;
        float mag = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        if (mag > 0.5f)
        {
            snprintf(buf, sizeof(buf), "vel %.0f  (%.0f %.0f %.0f)",
                     mag, v[0], v[1], v[2]);
            IG_TextUnformatted(buf);
        }
    }

    {
        char flagsbuf[128];
        format_flags((int)ed->v.flags, flagsbuf, sizeof(flagsbuf));
        snprintf(buf, sizeof(buf), "frame %d  flags %s",
                 (int)ed->v.frame, flagsbuf);
        IG_TextUnformatted(buf);
    }

    if (ed->v.nextthink > 0)
    {
        float dt = ed->v.nextthink - sv.time;
        snprintf(buf, sizeof(buf), "nextthink in %.2fs", dt);
        IG_TextUnformatted(buf);
    }

    if (ed->v.enemy)
    {
        char who[64];
        format_edict_link(ed->v.enemy, who, sizeof(who));
        snprintf(buf, sizeof(buf), "enemy: %s", who);
        IG_TextUnformatted(buf);
    }

    if (ed->v.owner)
    {
        char who[64];
        format_edict_link(ed->v.owner, who, sizeof(who));
        snprintf(buf, sizeof(buf), "owner: %s", who);
        IG_TextUnformatted(buf);
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
            if (IG_InputText(buf, e->kv[idx].value, EDIT_VAL_LEN,
                             IG_ITF_EnterReturnsTrue))
                inspector_sync_live(e, e->kv[idx].key, e->kv[idx].value);
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
            if (IG_InputText(buf, e->kv[i].value, EDIT_VAL_LEN,
                             IG_ITF_EnterReturnsTrue))
                inspector_sync_live(e, e->kv[i].key, e->kv[i].value);
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

    draw_live_state(e);

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

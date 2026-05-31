// editor_ui.c -- M1 ImGui panels: toolbar, brush list, inspector.
//
// All panels are drawn inside Editor_DrawUI which is called from
// imgui_layer.c ImguiLayer_Render. The panels only render when the editor is
// open (so they don't clutter the regular F12 dev overlay).

#include "quakedef.h"
#include "imgui_bridge.h"
#include "edit_scene.h"
#include "edit_history.h"
#include "edit_texcache.h"
#include "editor_classlist.h"
#include "editor.h"
#include "editor_internal.h"
#include "hotreload.h"          // g_game_api — ai_inspect / Wind_SampleVelocity

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

// Platform headers for resolve_function_symbol — dbghelp on Windows
// (already linked by sys_crash.c), dlfcn on POSIX.
#if defined(_WIN32)
  #include <windows.h>
  #include <dbghelp.h>
#else
  #include <dlfcn.h>
#endif

// Toolbar Textures... button toggles this; consumed by draw_texture_browser.
static int s_show_tex_browser = 0;
// Toolbar "Add Entity..." button toggles this; consumed by draw_spawn_dialog.
static int s_show_spawn_dialog = 0;
// Toolbar "Wrap..." button toggles this; consumed by draw_wrap_dialog.
static int s_show_wrap_dialog = 0;
// Toolbar "Light..." button toggles this; consumed by draw_light_opts_window.
static int s_show_light_opts = 0;

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

// -----------------------------------------------------------------------------
// Tiny UI helpers — kill the toolbar's checkbox / combo repetition.
// -----------------------------------------------------------------------------

// Checkbox wired to a float cvar treated as a boolean. Sets the cvar
// directly via Cvar_SetValue (no Cbuf round-trip).
static void ui_cvar_checkbox(const char *label, const char *cvar_name)
{
    cvar_t *cv = Cvar_FindVar((char *)cvar_name);
    int on = cv && cv->value != 0.0f;
    if (IG_Checkbox(label, &on))
        Cvar_SetValue((char *)cvar_name, on ? 1.0f : 0.0f);
}

// SameLine + checkbox — toolbar widgets all chain horizontally.
static void ui_cvar_checkbox_same(const char *label, const char *cvar_name)
{
    IG_SameLine(0, -1);
    ui_cvar_checkbox(label, cvar_name);
}

// Combo bound to an integer cvar with a fixed item list. Clamps the
// cvar's value to a valid index before display.
static void ui_cvar_combo_int(const char *label, const char *cvar_name,
                              const char *const *items, int n, float width)
{
    cvar_t *cv = Cvar_FindVar((char *)cvar_name);
    int sel = cv ? (int)cv->value : 0;
    if (sel < 0)  sel = 0;
    if (sel >= n) sel = n - 1;
    if (width > 0) IG_SetNextItemWidth(width);
    if (IG_Combo(label, &sel, items, n))
        Cvar_SetValue((char *)cvar_name, (float)sel);
}

// Combo bound to a float cvar with a discrete preset list (e.g. grid
// sizes). Selection mirrors current cvar value when it matches a preset
// exactly; otherwise displays index 0 without overwriting the cvar.
static void ui_cvar_combo_preset(const char *label, const char *cvar_name,
                                 const char *const *items,
                                 const float *values, int n, float width)
{
    cvar_t *cv  = Cvar_FindVar((char *)cvar_name);
    float  cur  = cv ? cv->value : values[0];
    int    sel  = 0;
    for (int k = 0; k < n; k++)
        if (values[k] == cur) { sel = k; break; }
    if (width > 0) IG_SetNextItemWidth(width);
    if (IG_Combo(label, &sel, items, n))
        Cvar_SetValue((char *)cvar_name, values[sel]);
}

// -----------------------------------------------------------------------------
// Toolbar
// -----------------------------------------------------------------------------

// Layout constants (in window pixels). Toolbar runs across the top; brush
// list anchors the bottom-left, inspector the bottom-right.
#define UI_PAD          10
#define UI_TOOLBAR_H    160
#define UI_LEFT_W       320
#define UI_RIGHT_W      360

// Case-insensitive substring search. Used by the texture filter UI so
// "wood" matches "Wood" and "BWOOD" alike. Empty needle matches anything.
static int strstri_simple(const char *hay, const char *needle)
{
    int hi, ni, hL, nL;
    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    hL = (int)strlen(hay);
    nL = (int)strlen(needle);
    if (nL > hL) return 0;
    for (hi = 0; hi <= hL - nL; hi++)
    {
        for (ni = 0; ni < nL; ni++)
        {
            char a = hay[hi + ni], b = needle[ni];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (ni == nL) return 1;
    }
    return 0;
}

// World-texture name list, lazily rebuilt when cl.worldmodel changes. Both
// the toolbar brush-tex picker and the inspector face-tex pickers feed off
// this. Pointers borrow into worldmodel->textures[i]->name so we pay one
// ptr-array realloc per map load.
static const char *const *world_tex_list(int *out_count)
{
    static const char **names         = NULL;
    static int          cap           = 0;
    static int          count         = 0;
    static void        *cached_world  = (void *)(intptr_t)-1;
    static int          cached_pool   = -1;
    int pool_count;
    texture_t **pool = Editor_TexPool_Get(&pool_count);

    if ((void *)cl.worldmodel != cached_world || pool_count != cached_pool)
    {
        int i;
        cached_world = (void *)cl.worldmodel;
        cached_pool  = pool_count;
        count = 0;

        /* Worldmodel textures first. */
        if (cl.worldmodel && cl.worldmodel->textures)
        {
            for (i = 0; i < cl.worldmodel->numtextures; i++)
            {
                texture_t *t = cl.worldmodel->textures[i];
                if (!t || !t->name[0]) continue;
                if (count >= cap)
                {
                    cap = cap ? cap * 2 : 128;
                    names = (const char **)realloc(names,
                                (size_t)cap * sizeof(*names));
                }
                names[count++] = t->name;
            }
        }

        /* Pool textures not already in the list. */
        for (i = 0; i < pool_count; i++)
        {
            texture_t *t = pool[i];
            int j, found = 0;
            if (!t || !t->name[0]) continue;
            for (j = 0; j < count; j++)
                if (!Q_strcasecmp(names[j], t->name)) { found = 1; break; }
            if (found) continue;
            if (count >= cap)
            {
                cap = cap ? cap * 2 : 128;
                names = (const char **)realloc(names,
                            (size_t)cap * sizeof(*names));
            }
            names[count++] = t->name;
        }
    }
    *out_count = count;
    return names;
}

// Linear search for a name in the world-texture list. Returns the index, or
// -1 if absent. Used to seed combo selection from the current texname so
// the dropdown opens already showing the in-use texture.
static int world_tex_index(const char *name)
{
    int i, n;
    const char *const *list = world_tex_list(&n);
    if (!name || !name[0]) return -1;
    for (i = 0; i < n; i++) if (!strcmp(list[i], name)) return i;
    return -1;
}

// Reflow condition for the three docked panels (toolbar, brushes, inspector).
// IG_Cond_Always on the first frame and on any frame where the display size
// just changed, so they snap to the formula-based layout that hugs the window
// edges. IG_Cond_FirstUseEver otherwise so a manual drag/resize the user did
// since the last reflow stays where they put it. Refreshed once per frame at
// the top of Editor_DrawUI.
static int s_dock_cond = IG_Cond_FirstUseEver;

// Run a simple console command (no args). Wraps the Cbuf_AddText
// boilerplate for toolbar buttons that don't need to interpolate.
static void ui_exec(const char *cmd_with_newline) { Cbuf_AddText((char *)cmd_with_newline); }

// "Label | next-button-on-same-line | ..." helper. Returns the click.
static int ui_btn_same(const char *label)
{
    IG_SameLine(0, -1);
    return IG_Button(label);
}

static void draw_toolbar(void)
{
    // Quake physics constants drive the gameplay-named grid entries:
    // 18 = step (max walkable), 45 = jump apex (270²/(2·800)),
    // 56 = player bbox height (-24..32). Others are powers of two.
    static const char *style_items[]  = { "wireframe", "flat", "flat+wire", "textured", "textured+wire" };
    static const char *camera_items[] = { "free-fly", "fps" };
    static const char *view_items[]   = { "live", "map" };
    static const char *grid_items[]   = { "1", "4", "8", "16 (build)", "18 (step)", "32 (door)",
                                          "45 (jump)", "56 (player)", "64", "128 (room)" };
    static const float grid_values[]  = { 1, 4, 8, 16, 18, 32, 45, 56, 64, 128 };
    static const char *rsnap_items[]  = { "5", "10", "15", "22.5", "30", "45", "90" };
    static const float rsnap_values[] = { 5, 10, 15, 22.5f, 30, 45, 90 };

    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    IG_SetNextWindowPos((float)UI_PAD, (float)UI_PAD, s_dock_cond);
    IG_SetNextWindowSize(disp_w - 2 * UI_PAD, (float)UI_TOOLBAR_H, s_dock_cond);
    if (!IG_Begin("Editor", NULL, IG_WF_None)) { IG_End(); return; }

    // -- File / build ----------------------------------------------------
    if (IG_Button("Save"))                      ui_exec("editor_save\n");
    if (ui_btn_same("Revert edits"))            ui_exec("editor_revert\n");
    if (ui_btn_same("Restart map") && edit_scene.mapname[0])
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "map %s\n", edit_scene.mapname);
        Cbuf_AddText(buf);
    }
    // qbsp-only fast iteration: geometry recompiled, existing .lit reused.
    if (ui_btn_same("Compile"))                 ui_exec("editor_compile\n");
    // qbsp + light: re-bakes from current `light*` entities. Slower but
    // the only way to see colour/intensity edits in the rendered .bsp.
    if (ui_btn_same("Compile + Light"))         ui_exec("editor_compile_full\n");
    // Background re-bake on an SDL_Thread; needs Compile+Light to have run
    // at least once this session to populate qbsp's globals.
    {
        extern int Editor_LightBake_InProgress(void);
        int busy = Editor_LightBake_InProgress();
        if (ui_btn_same(busy ? "Re-baking..." : "Refresh Lighting") && !busy)
            ui_exec("editor_relight\n");
    }
    if (ui_btn_same("Light..."))                s_show_light_opts = !s_show_light_opts;
    if (ui_btn_same("Close (F2)"))              ui_exec("editor\n");

    // -- Selection / undo ------------------------------------------------
    if (IG_Button("Undo"))                      ui_exec("editor_undo\n");
    if (ui_btn_same("Redo"))                    ui_exec("editor_redo\n");
    if (ui_btn_same("Delete (Del)"))            ui_exec("editor_delete\n");
    if (ui_btn_same("Group"))                   ui_exec("editor_group\n");
    if (ui_btn_same("Ungroup"))                 ui_exec("editor_ungroup\n");

    // -- Dialogs ---------------------------------------------------------
    if (ui_btn_same("Textures..."))             s_show_tex_browser  = !s_show_tex_browser;
    if (ui_btn_same("Add Entity..."))           s_show_spawn_dialog = !s_show_spawn_dialog;
    if (ui_btn_same("Wrap..."))                 s_show_wrap_dialog  = !s_show_wrap_dialog;

    // -- Brush palette ---------------------------------------------------
    {
        extern cvar_t editor_brush_tex;
        int n;
        const char *const *names = world_tex_list(&n);
        IG_SetNextItemWidth(180);
        if (n > 0)
        {
            int sel = world_tex_index(editor_brush_tex.string);
            if (sel < 0) sel = 0;
            if (IG_Combo("brush tex", &sel, names, n))
                Cvar_Set("editor_brush_tex", (char *)names[sel]);
        }
        else IG_TextUnformatted("brush tex (no map loaded)");
    }
    if (ui_btn_same("Add cube"))                ui_exec("editor_brush_add_cube\n");
    if (ui_btn_same("Hollow"))                  ui_exec("editor_brush_hollow\n");

    // -- Render style / overlays -----------------------------------------
    ui_cvar_combo_int("render style", "editor_render_style",
                      style_items, ARRAY_LEN(style_items), 160);
    // Fullbright zeroes the lightmap so world surfaces draw at full
    // intensity. Cheat-protected in multiplayer (r_misc.c forces it
    // back to 0 if cl.maxclients > 1).
    ui_cvar_checkbox_same("fullbright",  "r_fullbright");
    // Trigger / clip render mode: AABB shell vs textured face.
    ui_cvar_checkbox_same("trigger tex", "editor_trigger_render");
    ui_cvar_checkbox_same("clip tex",    "editor_clip_render");
    // Per-entity facing arrow + target/killtarget link overlay.
    ui_cvar_checkbox_same("angles",      "editor_show_angles");
    ui_cvar_checkbox_same("links",       "editor_show_links");
    // Navmesh debug overlay + its z-test companion (game DLL cvars,
    // only render while the sim is ticking).
    ui_cvar_checkbox_same("navmesh",     "sim_nav_debug");
    ui_cvar_checkbox_same("z-test",      "sim_nav_ztest");
    // Face mode: clicks pick a face instead of replacing the brush
    // selection. Gates the inspector's alignment widgets.
    ui_cvar_checkbox_same("faces",       "editor_face_mode");

    // -- Snap (translate) ------------------------------------------------
    // Plain (non-_same) checkbox here drops onto a new row so the render
    // overlays above don't share a line with the snap/rotate/camera/view
    // widgets — the combined line would extend past the toolbar's right edge.
    ui_cvar_checkbox("snap", "editor_grid_snap");
    ui_cvar_checkbox_same("abs",  "editor_grid_absolute");
    ui_cvar_checkbox_same("surface", "editor_snap_surface");
    IG_SameLine(0, -1);
    ui_cvar_combo_preset("grid", "editor_grid_size",
                         grid_items, grid_values, ARRAY_LEN(grid_items), 140);

    // -- Snap (rotate) ---------------------------------------------------
    ui_cvar_checkbox_same("rsnap",    "editor_rotate_snap");
    ui_cvar_checkbox_same("abs##rot", "editor_rotate_snap_absolute");
    IG_SameLine(0, -1);
    ui_cvar_combo_preset("rangle", "editor_rotate_snap_size",
                         rsnap_items, rsnap_values, ARRAY_LEN(rsnap_items), 60);

    // -- Camera / view ---------------------------------------------------
    IG_SameLine(0, -1);
    ui_cvar_combo_int("camera (Tab)", "editor_camera",
                      camera_items, ARRAY_LEN(camera_items), 110);
    IG_SameLine(0, -1);
    ui_cvar_combo_int("view", "editor_view_mode",
                      view_items, ARRAY_LEN(view_items), 90);

    // -- Status line -----------------------------------------------------
    {
        char buf[200];
        float fps = IG_GetFramerate();
        if (Editor_IsPlacementPending())
            snprintf(buf, sizeof(buf),
                     "click viewport to place '%s'   (ESC to cancel)   "
                     "[%.0f fps]",
                     Editor_PendingClassname(), fps);
        else
            snprintf(buf, sizeof(buf),
                     "loaded: %s    hold RMB to look + WASD to move   "
                     "[%.0f fps  %.1f ms]",
                     edit_scene.mapname[0] ? edit_scene.mapname : "(none)",
                     fps, 1000.0f / fps);
        IG_TextUnformatted(buf);
    }
    IG_End();
}

// -----------------------------------------------------------------------------
// Brush list
// -----------------------------------------------------------------------------

// Per-category hide flag, indexed by EDIT_CAT_*. Persists across panel
// re-opens within a session. The "OTHER" slot covers worldspawn plus
// uncategorised runtime junk (projectiles, backpacks, bubbles, ...). It
// has an "other" checkbox, but worldspawn is special-cased never-hideable
// in Editor_EntityHiddenByCategory (hiding it would blank the geometry).
//
// Default hides triggers, lights, ambient sounds, path corners, and
// info_* metadata -- none of these have a model the engine actually
// renders, so they just clutter the view (start.bsp alone carries 267
// `light*` entities). The user re-enables them from the panel
// checkboxes when authoring lights / triggers / etc.
static int s_hide_cat[EDIT_CAT_COUNT] = {
    [EDIT_CAT_TRIGGER] = 1,
    [EDIT_CAT_LIGHT]   = 1,
    [EDIT_CAT_SOUND]   = 1,
    [EDIT_CAT_PATH]    = 1,
    [EDIT_CAT_INFO]    = 1,
};

// Cross-cutting "sim" hide. Phase 8 immersive-sim entities live in their
// natural categories (func_grate is a func, misc_oilbarrel is a misc, ...),
// so this is a flag OR'd alongside the per-category filter rather than a
// category of its own. Off by default.
static int s_hide_sim = 0;

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

// True if `e` is a Phase 8 immersive-sim entity (wind / smoke / oil /
// breakables / grates). Drives the cross-cutting "sim" hide toggle, which
// hides these regardless of their natural category.
static int entity_is_sim(const edit_entity_t *e)
{
    const char *cls;
    if (!e || e->classname_idx < 0) return 0;
    cls = e->kv[e->classname_idx].value;
    if (!cls) return 0;
    return !strncmp(cls, "sim_", 4)        // sim_walkgoal (AI walk target) + any future sim_* ent
        || !strcmp(cls, "info_wind_source")
        || !strcmp(cls, "misc_smokegrenade")
        || !strcmp(cls, "misc_oilbarrel")
        || !strcmp(cls, "misc_oilslick")
        || !strcmp(cls, "func_grate")
        || !strcmp(cls, "func_breakable")
        || !strcmp(cls, "misc_breakable");
}

int Editor_EntityHiddenByCategory(int e_idx)
{
    int cat;
    edit_entity_t *e;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];
    cat = Editor_EntityCategory(e);
    if (cat >= 0 && cat < EDIT_CAT_COUNT && s_hide_cat[cat]) {
        // worldspawn lands in OTHER but owns the level's editable brushes,
        // so hiding it would blank the geometry — keep it always visible.
        const char *cls = (e->classname_idx >= 0) ? e->kv[e->classname_idx].value : 0;
        if (cat != EDIT_CAT_OTHER || !cls || strcmp(cls, "worldspawn"))
            return 1;
    }
    if (s_hide_sim && entity_is_sim(e)) return 1;
    if (s_visible_only && !Editor_EntityInView(e_idx)) return 1;
    if (entity_hidden_by_skill(e)) return 1;
    return 0;
}

int Editor_EntityHidden(int e_idx)
{
    edit_entity_t *e;
    if (Editor_EntityHiddenByCategory(e_idx)) return 1;
    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 0;
    e = &edit_scene.entities[e_idx];

    // Triggers are always authoring-only — InitTrigger zeroes v.modelindex
    // so the engine never draws them. Skipping the "engine isn't rendering"
    // filter below keeps them visible + pickable in both live and map view.
    if (Editor_EntityCategory(e) == EDIT_CAT_TRIGGER) return 0;

    // Live mode: hide entities the engine isn't visibly rendering. Once
    // realised as a live edict the engine's model decision is the truth —
    // info_player_*, info_intermission, info_null and similar metadata
    // edicts have v.model=NULL and the engine draws nothing for them, so
    // bbox + arrows + picking should also draw nothing. SV_MakeStatic'd
    // ents (live_static) keep their efrag chain so they stay visible.
    // Pending entities (no live_ent yet) and BSP-loaded brush entities
    // both pass through — the former is the user's authoring intent, the
    // latter has v.model="*N" which sets cl_entities[N].model.
    {
        extern cvar_t editor_view_mode;
        int view_live = (int)editor_view_mode.value == 0;
        if (view_live && e->live_ent)
        {
            // Bounds-check before NUM_FOR_EDICT: a same-map respawn can
            // leave live_ent pointing past the new (smaller) num_edicts
            // even if the spawn-serial gate in editor_check_map_change
            // hasn't yet swept this frame.
            if (!Editor_LiveEntInRange(e->live_ent))
            {
                e->live_ent = NULL;
            }
            else if (!e->live_ent->free)
            {
                int en = NUM_FOR_EDICT(e->live_ent);
                int engine_renders =
                    (en > 0 && en < cl.num_entities && cl_entities[en].model);
                if (!engine_renders) return 1;
            }
        }
    }
    return 0;
}

// Combine category-filter visibility with view-mode visibility. The
// Brushes panel uses this rather than calling Editor_EntityHidden
// directly, so metadata edicts (info_player_*, info_intermission)
// still show in live view's list — they're alive engine edicts even
// though Editor_EntityHidden filters them from render/pick.
// Brush-list click handling: plain click replaces selection, shift
// toggles, double-click frames the camera on the item. Same semantics
// as the 3D viewport. SDL_GetModState reads OS keyboard state directly
// so modifiers work regardless of which window has focus.
static void brush_list_pick(int i, int j)
{
    SDL_Keymod mod = SDL_GetModState();
    if (mod & SDL_KMOD_SHIFT) {
        Scene_SelectionToggle(i, j);
    } else {
        Scene_SelectionClear();
        Scene_SelectionAdd(i, j);
    }
    if (IG_IsMouseDoubleClicked(0)) Editor_FrameItem(i, j);
}

static int brush_list_visible(int e_idx)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    edit_entity_t *e = &edit_scene.entities[e_idx];

    if (Editor_EntityHiddenByCategory(e_idx)) return 0;

    if (view_live)
    {
        if (e->live_ent && !e->live_ent->free) return 1;
        if (e->live_static) return 1;
        /* `light*` entities are compile-time only -- the engine drops
         * them on map load (qrad consumes them at bake time, the live
         * server never spawns them). Without this branch the brushes
         * panel hides 224 of start.bsp's 267 lights even with the
         * category checkbox enabled, because none of them have a
         * live_ent / live_static. */
        if (Editor_EntityCategory(e) == EDIT_CAT_LIGHT) return 1;
        return 0;
    }
    if (e->transient) return 0;
    return 1;
}

static void draw_brush_list(void)
{
    int i, j;
    char buf[128];
    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    float y    = (float)(UI_PAD + UI_TOOLBAR_H + UI_PAD);
    float h    = disp_h - y - UI_PAD;

    IG_SetNextWindowPos((float)UI_PAD, y, s_dock_cond);
    IG_SetNextWindowSize((float)UI_LEFT_W, h, s_dock_cond);
    if (!IG_Begin("Brushes", NULL, IG_WF_None)) { IG_End(); return; }

    {
        extern cvar_t editor_view_mode;
        int view_live = (int)editor_view_mode.value == 0;
        int visible_ents = 0, total_brushes = 0;
        for (i = 0; i < edit_scene.numentities; i++)
        {
            if (!brush_list_visible(i)) continue;
            visible_ents++;
            total_brushes += edit_scene.entities[i].numbrushes;
        }
        if (view_live)
            snprintf(buf, sizeof(buf), "%d live edicts, %d brushes",
                     visible_ents, total_brushes);
        else
            snprintf(buf, sizeof(buf), "%d entities, %d brushes",
                     visible_ents, total_brushes);
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

    // Filter checkboxes — "Hide X" per category. Two per row.
    {
        static const struct { int cat; const char *label; } hide_cats[] = {
            { EDIT_CAT_TRIGGER, "triggers" },
            { EDIT_CAT_LIGHT,   "lights"   },
            { EDIT_CAT_SPAWN,   "spawns"   },
            { EDIT_CAT_ITEM,    "items"    },
            { EDIT_CAT_MONSTER, "monsters" },
            { EDIT_CAT_FUNC,    "funcs"    },
            { EDIT_CAT_SOUND,   "sounds"   },
            { EDIT_CAT_PATH,    "paths"    },
            { EDIT_CAT_MISC,    "misc"     },
            { EDIT_CAT_INFO,    "info"     },
            { EDIT_CAT_GIB,     "gibs"     },
            { EDIT_CAT_TRAP,    "traps"    },
            { EDIT_CAT_PLAYER,  "player"   },
            { EDIT_CAT_OTHER,   "other"    },
        };
        IG_TextUnformatted("Hide:");
        for (int k = 0; k < ARRAY_LEN(hide_cats); k++) {
            if (k & 1) IG_SameLine(0, -1);
            int v = s_hide_cat[hide_cats[k].cat];
            if (IG_Checkbox(hide_cats[k].label, &v))
                s_hide_cat[hide_cats[k].cat] = v;
        }
        // "sim" hides Phase 8 sim entities regardless of their natural
        // category; "visible only" is orthogonal too. Both AND with the
        // per-category filters above.
        IG_Checkbox("sim", &s_hide_sim);
        IG_Checkbox("visible only", &s_visible_only);
    }
    IG_Separator();

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = "(no classname)";
        if (!brush_list_visible(i)) continue;
        if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;

        IG_PushID_Int(i);
        if (Entity_IsPoint(e))
        {
            // Point entity: header itself is the selectable. (i, -1)
            // anchors the gizmo at its origin.
            int sel = Scene_SelectionContains(i, -1);
            snprintf(buf, sizeof(buf), "[%d] %s##e%d", i, cls, i);
            if (IG_Selectable(buf, sel, IG_SF_AllowDoubleClick))
                brush_list_pick(i, -1);
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
                    brush_list_pick(i, j);
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
static const spawnflag_def_t s_door_secret_flags[] = {
    { 1, "Open once" }, { 2, "1st left" }, { 4, "1st down" },
    { 8, "No shoot" }, { 16, "Always shoot" },
};
static const spawnflag_def_t s_plat_flags[]    = { { 1, "Low trigger" } };
static const spawnflag_def_t s_trigger_flags[] = { { 1, "No touch" } };
static const spawnflag_def_t s_teleport_flags[] = {
    { 1, "Player only" }, { 2, "Silent" },
};
static const spawnflag_def_t s_push_flags[]    = { { 1, "Push once" } };
static const spawnflag_def_t s_button_flags[]  = { { 1, "Don't move" } };
static const spawnflag_def_t s_wall_flags[]    = { { 1, "Toggle" } };
static const spawnflag_def_t s_path_flags[]    = {
    { 1, "Wait for trigger" }, { 2, "Teleport" },
};
static const spawnflag_def_t s_monster_flags[] = { { 1, "Ambush" } };
static const spawnflag_def_t s_health_flags[]  = {
    { 1, "Rotten (15)" }, { 2, "Mega (100)" },
};
static const spawnflag_def_t s_changelevel_flags[] = { { 1, "Silent" } };

#define FLAGS_OF(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))

static const class_flag_table_t s_class_flag_tables[] = {
    { NULL,                  "light",      FLAGS_OF(s_light_flags) },
    { "func_door_secret",    NULL,         FLAGS_OF(s_door_secret_flags) },
    { "func_door",           NULL,         FLAGS_OF(s_door_flags) },
    { "func_plat",           NULL,         FLAGS_OF(s_plat_flags) },
    { "func_button",         NULL,         FLAGS_OF(s_button_flags) },
    { "func_wall",           NULL,         FLAGS_OF(s_wall_flags) },
    { "func_illusionary",    NULL,         FLAGS_OF(s_wall_flags) },
    { "trigger_multiple",    NULL,         FLAGS_OF(s_trigger_flags) },
    { "trigger_once",        NULL,         FLAGS_OF(s_trigger_flags) },
    { "trigger_teleport",    NULL,         FLAGS_OF(s_teleport_flags) },
    { "trigger_push",        NULL,         FLAGS_OF(s_push_flags) },
    { "trigger_changelevel", NULL,         FLAGS_OF(s_changelevel_flags) },
    { "path_corner",         NULL,         FLAGS_OF(s_path_flags) },
    { "item_health",         NULL,         FLAGS_OF(s_health_flags) },
    { NULL,                  "monster_",   FLAGS_OF(s_monster_flags) },
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

// Render the effects section: EF_* dynamic-light bits as checkboxes plus a
// raw integer. Round-trips through the "effects" kv and pokes the live
// edict's v.effects so dlights re-evaluate the next server frame.
static const spawnflag_def_t s_effects_bits[] = {
    { EF_BRIGHTFIELD, "Brightfield" },
    { EF_MUZZLEFLASH, "Muzzleflash" },
    { EF_BRIGHTLIGHT, "Bright light" },
    { EF_DIMLIGHT,    "Dim light" },
};

static void draw_effects_section(edit_entity_t *e)
{
    char buf[32];
    char label[80];
    int  changed = 0, on, j;
    int  current;
    int  ef_idx = kv_lookup(e, "effects");

    current = ef_idx >= 0 ? atoi(e->kv[ef_idx].value) : 0;

    IG_TextUnformatted("effects");

    for (j = 0; j < (int)(sizeof(s_effects_bits) / sizeof(s_effects_bits[0])); j++)
    {
        on = (current & s_effects_bits[j].bit) != 0;
        snprintf(label, sizeof(label), "%s##ef%d", s_effects_bits[j].label, j);
        if (IG_Checkbox(label, &on))
        {
            if (on) current |=  s_effects_bits[j].bit;
            else    current &= ~s_effects_bits[j].bit;
            changed = 1;
        }
    }

    snprintf(buf, sizeof(buf), "%d", current);
    IG_SetNextItemWidth(120);
    if (IG_InputText("raw##efraw", buf, sizeof(buf), IG_ITF_EnterReturnsTrue))
    {
        current = atoi(buf);
        changed = 1;
    }

    if (changed)
    {
        snprintf(buf, sizeof(buf), "%d", current);
        Entity_SetKV(e, "effects", buf);
        if (e->live_ent && !e->live_ent->free)
            e->live_ent->v.effects = (float)current;
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

// Print bits set in `mask` as a pipe-joined token list ("ABC|XYZ"), or
// "(none)" when no bits are set. Shared by flags / effects / items.
static void format_bitmask(int mask,
                           const struct { int bit; const char *name; } *tab,
                           int ntab, char *out, int out_sz)
{
    int i, n = 0;
    out[0] = 0;
    for (i = 0; i < ntab; i++)
    {
        if (mask & tab[i].bit)
        {
            int w = snprintf(out + n, out_sz - n, "%s%s",
                             n ? "|" : "", tab[i].name);
            if (w < 0 || w >= out_sz - n) break;
            n += w;
        }
    }
    if (out[0] == 0) snprintf(out, out_sz, "(none)");
}

// Active-weapon names: v.weapon holds a single IT_* bit when the player has
// a Quake weapon out; v.weapon2 holds a Doom/Wolf IT2_* bit when a Phase 6
// gun is selected (and v.weapon is then forced to 0). NULL = no match.
static const char *quake_weapon_name(int w)
{
    switch (w)
    {
    case IT_AXE:             return "Axe";
    case IT_SHOTGUN:         return "Shotgun";
    case IT_SUPER_SHOTGUN:   return "Super Shotgun";
    case IT_NAILGUN:         return "Nailgun";
    case IT_SUPER_NAILGUN:   return "Super Nailgun";
    case IT_GRENADE_LAUNCHER:return "Grenade Launcher";
    case IT_ROCKET_LAUNCHER: return "Rocket Launcher";
    case IT_LIGHTNING:       return "Lightning";
    default:                 return NULL;
    }
}

// IT2_* bits live in game_defs.h on the DLL side; engine doesn't include
// it. The values are stable Phase-6 ABI — hard-coded here so the engine
// inspector can decode without a header dependency.
#define ED_IT2_DOOM_FIST       (1 << 0)
#define ED_IT2_DOOM_PISTOL     (1 << 1)
#define ED_IT2_DOOM_SHOTGUN    (1 << 2)
#define ED_IT2_DOOM_CHAINGUN   (1 << 3)
#define ED_IT2_DOOM_ROCKET     (1 << 4)
#define ED_IT2_DOOM_CHAINSAW   (1 << 5)
#define ED_IT2_WOLF_KNIFE      (1 << 6)
#define ED_IT2_WOLF_PISTOL     (1 << 7)
#define ED_IT2_WOLF_MACHINEGUN (1 << 8)
#define ED_IT2_WOLF_CHAINGUN   (1 << 9)

static const char *bonus_weapon_name(int w)
{
    switch (w)
    {
    case ED_IT2_DOOM_FIST:       return "Doom Fist";
    case ED_IT2_DOOM_PISTOL:     return "Doom Pistol";
    case ED_IT2_DOOM_SHOTGUN:    return "Doom Shotgun";
    case ED_IT2_DOOM_CHAINGUN:   return "Doom Chaingun";
    case ED_IT2_DOOM_ROCKET:     return "Doom Rocket";
    case ED_IT2_DOOM_CHAINSAW:   return "Doom Chainsaw";
    case ED_IT2_WOLF_KNIFE:      return "Wolf Knife";
    case ED_IT2_WOLF_PISTOL:     return "Wolf Pistol";
    case ED_IT2_WOLF_MACHINEGUN: return "Wolf MG";
    case ED_IT2_WOLF_CHAINGUN:   return "Wolf Chaingun";
    default:                     return NULL;
    }
}

static const char *contents_name(int c)
{
    switch (c)
    {
    case 0:                return "(none)";
    case CONTENTS_EMPTY:   return "EMPTY";
    case CONTENTS_SOLID:   return "SOLID";
    case CONTENTS_WATER:   return "WATER";
    case CONTENTS_SLIME:   return "SLIME";
    case CONTENTS_LAVA:    return "LAVA";
    case CONTENTS_SKY:     return "SKY";
    default:               return "?";
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

// Resolve a function pointer to "name+0xOFFSET" via dladdr (POSIX) or
// dbghelp SymFromAddr (Windows). Static functions are often not in the
// dynamic symbol table, so on POSIX we fall back to "<module>+0xOFFSET"
// — the user can resolve with `atos -o zig-out/bin/game.dll 0xOFFSET`.
// Used in the editor inspector to show what a corpse's nextthink will
// actually fire.
static void resolve_function_symbol(void *ptr, char *out, int out_sz)
{
    if (!ptr) { snprintf(out, out_sz, "(null)"); return; }
#if defined(_WIN32)
    static int s_sym_inited = 0;
    if (!s_sym_inited) {
        SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        s_sym_inited = 1;
    }
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    memset(symbuf, 0, sizeof(symbuf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)ptr, &disp, sym))
    {
        if (disp == 0) snprintf(out, out_sz, "%s", sym->Name);
        else           snprintf(out, out_sz, "%s+0x%llx",
                                sym->Name, (unsigned long long)disp);
        return;
    }
    snprintf(out, out_sz, "%p", ptr);
#else
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(ptr, &info) && info.dli_sname)
    {
        ptrdiff_t off = (char *)ptr - (char *)info.dli_saddr;
        if (off == 0) snprintf(out, out_sz, "%s", info.dli_sname);
        else          snprintf(out, out_sz, "%s+0x%lx",
                               info.dli_sname, (long)off);
        return;
    }
    if (info.dli_fbase && info.dli_fname)
    {
        ptrdiff_t off = (char *)ptr - (char *)info.dli_fbase;
        const char *base = info.dli_fname;
        const char *slash = strrchr(base, '/');
        if (slash) base = slash + 1;
        snprintf(out, out_sz, "%s+0x%lx", base, (long)off);
        return;
    }
    snprintf(out, out_sz, "%p", ptr);
#endif
}

static const char *ai_state_name(int s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "SUSPICIOUS";
    case 2: return "SEARCHING";
    case 3: return "COMBAT";
    default: return "?";
    }
}

// Per-inspector velocity watch: tracks the peak speed seen on the
// currently selected edict over the recent past. Single-slot — the
// inspector only shows one entity at a time, so a ring keyed by edict
// number would be wasted memory. Resets when the selection changes.
#define VEL_WATCH_WINDOW_SEC 5.0f
typedef struct {
    int   edict_num;
    float peak_mag;
    float peak_time;       // sv.time when peak was observed
    float peak_v[3];
} vel_watch_t;
static vel_watch_t s_vel_watch = { -1, 0, 0, {0,0,0} };

static void vel_watch_update(edict_t *ed)
{
    int en = NUM_FOR_EDICT(ed);
    const float *v = ed->v.velocity;
    float mag = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);

    // Selection changed or watch window elapsed since last peak → reset.
    if (s_vel_watch.edict_num != en ||
        sv.time - s_vel_watch.peak_time > VEL_WATCH_WINDOW_SEC)
    {
        s_vel_watch.edict_num = en;
        s_vel_watch.peak_mag  = mag;
        s_vel_watch.peak_time = sv.time;
        s_vel_watch.peak_v[0] = v[0];
        s_vel_watch.peak_v[1] = v[1];
        s_vel_watch.peak_v[2] = v[2];
        return;
    }
    if (mag > s_vel_watch.peak_mag)
    {
        s_vel_watch.peak_mag  = mag;
        s_vel_watch.peak_time = sv.time;
        s_vel_watch.peak_v[0] = v[0];
        s_vel_watch.peak_v[1] = v[1];
        s_vel_watch.peak_v[2] = v[2];
    }
}

static void draw_live_state(edit_entity_t *e)
{
    extern cvar_t editor_view_mode;
    edict_t *ed;
    char buf[256];
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

    // Origin — always shown. Useful for spotting tiny drift (z creeping up
    // by 0.01 each frame, for instance) which is invisible from the gizmo.
    {
        const float *o = ed->v.origin;
        snprintf(buf, sizeof(buf), "origin (%.2f %.2f %.2f)", o[0], o[1], o[2]);
        IG_TextUnformatted(buf);
    }

    // Bounding box — the one gib-relevant field that wasn't surfaced. The
    // world AABB (absmin..absmax) is exactly what hitscan (Corpse_BulletTrace)
    // and explosions (T_RadiusDamage/CanDamage) test against; size.z ~16 on a
    // corpse is the flattened prone slab from Corpse_LayProne. A corpse that
    // refuses to gib despite TRIGGER + takedamage + DEAD usually has that slab
    // embedded in / occluded by world geometry, so shots hit the world first
    // and never reach it.
    {
        const float *mn = ed->v.absmin;
        const float *mx = ed->v.absmax;
        const float *sz = ed->v.size;
        snprintf(buf, sizeof(buf),
                 "bbox abs (%.1f %.1f %.1f)..(%.1f %.1f %.1f)  size (%.0f %.0f %.0f)",
                 mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], sz[0], sz[1], sz[2]);
        IG_TextUnformatted(buf);
    }

    // Velocity — always shown, even sub-unit. Sliding-corpse debugging
    // hinges on seeing drift that would have been hidden by the old
    // `> 0.5` gate.
    vel_watch_update(ed);
    {
        const float *v = ed->v.velocity;
        float mag = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
        snprintf(buf, sizeof(buf), "vel %.3f  (%.3f %.3f %.3f)",
                 mag, v[0], v[1], v[2]);
        IG_TextUnformatted(buf);

        if (s_vel_watch.edict_num == en && s_vel_watch.peak_mag > 0)
        {
            float age = sv.time - s_vel_watch.peak_time;
            snprintf(buf, sizeof(buf),
                     "vel peak (last %.0fs): %.3f  (%.2f %.2f %.2f)  %.2fs ago",
                     VEL_WATCH_WINDOW_SEC,
                     s_vel_watch.peak_mag,
                     s_vel_watch.peak_v[0], s_vel_watch.peak_v[1],
                     s_vel_watch.peak_v[2], age);
            IG_TextUnformatted(buf);
        }
    }

    // Angular velocity — gibs spin; nonzero avelocity on a "settled"
    // corpse is a tell that something is still ticking it.
    {
        const float *av = ed->v.avelocity;
        float amag = sqrtf(av[0]*av[0] + av[1]*av[1] + av[2]*av[2]);
        if (amag > 0.01f)
        {
            snprintf(buf, sizeof(buf), "avel %.2f  (%.2f %.2f %.2f)",
                     amag, av[0], av[1], av[2]);
            IG_TextUnformatted(buf);
        }
    }

    // Ground entity link + ONGROUND status. A corpse without ONGROUND is
    // a candidate for spurious motion — gravity (MOVETYPE_STEP) or
    // bounce (MOVETYPE_BOUNCE) will keep ticking displacement.
    {
        int onground = ((int)ed->v.flags & FL_ONGROUND) != 0;
        char who[80];
        format_edict_link(ed->v.groundentity, who, sizeof(who));
        snprintf(buf, sizeof(buf), "ground: %s  [%s]",
                 who, onground ? "ONGROUND" : "AIRBORNE");
        IG_TextUnformatted(buf);
    }

    if (ed->v.health != 0 || ed->v.max_health != 0)
    {
        snprintf(buf, sizeof(buf), "health %g / %g  deadflag %s",
                 ed->v.health, ed->v.max_health,
                 deadflag_name((int)ed->v.deadflag));
        IG_TextUnformatted(buf);
    }

    if (ed->v.takedamage != 0 || ed->v.dmg_take != 0 || ed->v.dmg_save != 0)
    {
        snprintf(buf, sizeof(buf), "takedamage %g  dmg_take %g  dmg_save %g",
                 ed->v.takedamage, ed->v.dmg_take, ed->v.dmg_save);
        IG_TextUnformatted(buf);
    }

    if (ed->v.dmg_inflictor)
    {
        char who[80];
        format_edict_link(ed->v.dmg_inflictor, who, sizeof(who));
        snprintf(buf, sizeof(buf), "dmg_inflictor: %s", who);
        IG_TextUnformatted(buf);
    }

    {
        char flagsbuf[128];
        format_flags((int)ed->v.flags, flagsbuf, sizeof(flagsbuf));
        snprintf(buf, sizeof(buf), "frame %d  flags %s",
                 (int)ed->v.frame, flagsbuf);
        IG_TextUnformatted(buf);
    }

    // Effects (dynamic-light bits + nodraw). The setter UI lives in the
    // kv panel — this is just the live readout.
    if (ed->v.effects != 0)
    {
        static const struct { int bit; const char *name; } eftab[] = {
            { EF_BRIGHTFIELD, "BRIGHTFIELD" }, { EF_MUZZLEFLASH, "MUZZLEFLASH" },
            { EF_BRIGHTLIGHT, "BRIGHTLIGHT" }, { EF_DIMLIGHT,    "DIMLIGHT" },
        };
        char efbuf[96];
        format_bitmask((int)ed->v.effects, eftab,
                       (int)(sizeof(eftab) / sizeof(eftab[0])),
                       efbuf, sizeof(efbuf));
        snprintf(buf, sizeof(buf), "effects %s", efbuf);
        IG_TextUnformatted(buf);
    }

    // Water — useful when AI seems to be drowning, sliding, or refusing
    // to path through liquids. watertype is a CONTENTS_* code (negative).
    if (ed->v.waterlevel != 0 || ed->v.watertype != 0)
    {
        snprintf(buf, sizeof(buf), "water level %d  type %s",
                 (int)ed->v.waterlevel, contents_name((int)ed->v.watertype));
        IG_TextUnformatted(buf);
    }

    // Player-only readouts: inventory bitfields and live input buttons.
    // `cl.viewentity` is the local client's edict index; the inspector can
    // target any entity, so most monsters won't take this branch.
    if (en == cl.viewentity)
    {
        // v.weapon = active Quake gun (single IT_* bit). v.weapon2 = active
        // Doom/Wolf gun (single IT2_* bit, Phase 6). If both are zero the
        // player isn't holding anything (in_blink / clamp during teleport).
        int w  = (int)ed->v.weapon;
        int w2 = (int)ed->v.weapon2;
        const char *wn  = w  ? quake_weapon_name(w)   : NULL;
        const char *w2n = w2 ? bonus_weapon_name(w2)  : NULL;
        if (w2n)
            snprintf(buf, sizeof(buf), "weapon %s  (weapon2=%d)", w2n, w2);
        else if (wn)
            snprintf(buf, sizeof(buf), "weapon %s  (%d)", wn, w);
        else
            snprintf(buf, sizeof(buf), "weapon %d / weapon2 %d", w, w2);
        IG_TextUnformatted(buf);

        // v.items mask. Decode by category for readability.
        {
            static const struct { int bit; const char *name; } itab[] = {
                { IT_SHOTGUN, "SHOT" }, { IT_SUPER_SHOTGUN, "SSHOT" },
                { IT_NAILGUN, "NAIL" }, { IT_SUPER_NAILGUN, "SNAIL" },
                { IT_GRENADE_LAUNCHER, "GL" }, { IT_ROCKET_LAUNCHER, "RL" },
                { IT_LIGHTNING, "LG" }, { IT_AXE, "AXE" },
                { IT_ARMOR1, "GA" }, { IT_ARMOR2, "YA" }, { IT_ARMOR3, "RA" },
                { IT_KEY1, "SKEY" }, { IT_KEY2, "GKEY" },
                { IT_INVISIBILITY, "RING" }, { IT_INVULNERABILITY, "PENT" },
                { IT_SUIT, "SUIT" }, { IT_QUAD, "QUAD" },
                { IT_SIGIL1, "S1" }, { IT_SIGIL2, "S2" },
                { IT_SIGIL3, "S3" }, { IT_SIGIL4, "S4" },
            };
            char itbuf[160];
            format_bitmask((int)ed->v.items, itab,
                           (int)(sizeof(itab) / sizeof(itab[0])),
                           itbuf, sizeof(itbuf));
            snprintf(buf, sizeof(buf), "items %s", itbuf);
            IG_TextUnformatted(buf);
        }

        // v.items2 — Phase 6 Doom/Wolf3D bonus roster. Only show if any
        // bit is set to avoid clutter on vanilla play.
        if (ed->v.items2 != 0)
        {
            static const struct { int bit; const char *name; } i2tab[] = {
                { ED_IT2_DOOM_FIST,       "DFIST" },
                { ED_IT2_DOOM_PISTOL,     "DPIST" },
                { ED_IT2_DOOM_SHOTGUN,    "DSHOT" },
                { ED_IT2_DOOM_CHAINGUN,   "DCHAIN" },
                { ED_IT2_DOOM_ROCKET,     "DROCK" },
                { ED_IT2_DOOM_CHAINSAW,   "DSAW" },
                { ED_IT2_WOLF_KNIFE,      "WKNF" },
                { ED_IT2_WOLF_PISTOL,     "WPIST" },
                { ED_IT2_WOLF_MACHINEGUN, "WMG" },
                { ED_IT2_WOLF_CHAINGUN,   "WCHN" },
            };
            char itbuf[160];
            format_bitmask((int)ed->v.items2, i2tab,
                           (int)(sizeof(i2tab) / sizeof(i2tab[0])),
                           itbuf, sizeof(itbuf));
            snprintf(buf, sizeof(buf), "items2 %s", itbuf);
            IG_TextUnformatted(buf);
        }

        // Buttons — live input. button3 = Blink, button4 = Gust (Phase 8
        // M3). Useful to confirm the new clc_move bits are reaching the
        // server when a bind seems unresponsive.
        if (ed->v.button0 || ed->v.button1 || ed->v.button2
            || ed->v.button3 || ed->v.button4)
        {
            snprintf(buf, sizeof(buf),
                     "buttons %s%s%s%s%s",
                     ed->v.button0 ? "ATK " : "",
                     ed->v.button1 ? "JMP " : "",
                     ed->v.button2 ? "B2 "  : "",
                     ed->v.button3 ? "BLNK ": "",
                     ed->v.button4 ? "GUST ": "");
            IG_TextUnformatted(buf);
        }
    }

    // Think function + nextthink countdown. Symbol resolution is best-
    // effort: static functions in game.dll often won't have a dynamic
    // symbol entry; on POSIX we then print module+offset so the user
    // can `atos` it manually. On Windows dbghelp usually resolves PDB
    // symbols correctly. See resolve_function_symbol above.
    if (ed->v.think)
    {
        char name[128];
        resolve_function_symbol((void *)ed->v.think, name, sizeof(name));
        if (ed->v.nextthink > 0)
        {
            float dt = ed->v.nextthink - sv.time;
            snprintf(buf, sizeof(buf), "think %s  in %.3fs", name, dt);
        }
        else
        {
            snprintf(buf, sizeof(buf), "think %s  (no nextthink)", name);
        }
        IG_TextUnformatted(buf);
    }
    else if (ed->v.nextthink > 0)
    {
        float dt = ed->v.nextthink - sv.time;
        snprintf(buf, sizeof(buf), "nextthink in %.3fs  (no think fn)", dt);
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

    // AI brain (sim-side side table). The brain lives in s_brains[]
    // inside the game DLL even after the monster dies — if a corpse
    // shows a brain that's still ticking, that's the bug.
    if (g_game_api && g_game_api->ai_inspect)
    {
        int   ai_state, target_n, stuck;
        float alert, next_dt;
        float last_pos[3];
        if (g_game_api->ai_inspect(ed, &ai_state, &alert, last_pos,
                                   &target_n, &next_dt, &stuck))
        {
            snprintf(buf, sizeof(buf),
                     "ai: %s  alert %.2f  next tick in %+.2fs  stuck %d",
                     ai_state_name(ai_state), alert, next_dt, stuck);
            IG_TextUnformatted(buf);
            snprintf(buf, sizeof(buf),
                     "ai last_known (%.0f %.0f %.0f)  target #%d",
                     last_pos[0], last_pos[1], last_pos[2], target_n);
            IG_TextUnformatted(buf);
        }
    }

    // Wind sample at the entity's origin. Gust / info_wind_source can
    // push corpses (movetype != NONE/PUSH); a nonzero wind here while a
    // corpse is sliding is the smoking gun.
    if (g_game_api && g_game_api->Wind_SampleVelocity)
    {
        float w[3];
        g_game_api->Wind_SampleVelocity(ed->v.origin, w);
        float wm = sqrtf(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
        if (wm > 0.01f)
        {
            snprintf(buf, sizeof(buf), "wind here %.2f  (%.2f %.2f %.2f)",
                     wm, w[0], w[1], w[2]);
            IG_TextUnformatted(buf);
        }
    }
}

// -----------------------------------------------------------------------------
// Spawn dialog (Add Entity...)
// -----------------------------------------------------------------------------

// Modeless picker for the next-click placement. Source list is the running
// game DLL's spawn table — every classname the engine actually knows how
// to spawn shows up here. Click a row → arms Editor_BeginPlaceEntity, the
// next viewport LMB drops the entity at the cursor.
static void draw_spawn_dialog(void)
{
    static char s_filter[64] = "";
    int n = 0, i;
    const char *const *names;

    if (!s_show_spawn_dialog) return;

    IG_SetNextWindowSize(360, 480, IG_Cond_FirstUseEver);
    if (!IG_Begin("Spawn Entity", &s_show_spawn_dialog, IG_WF_None))
    {
        IG_End();
        return;
    }

    names = Editor_ClassList_Get(&n);
    if (!names || n == 0)
    {
        IG_TextUnformatted("(game DLL not loaded — start a server then retry)");
        IG_End();
        return;
    }

    if (Editor_IsPlacementPending())
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "* armed: %s  (click viewport, ESC cancels)",
                 Editor_PendingClassname());
        IG_TextUnformatted(buf);
    }
    else
    {
        IG_TextUnformatted("pick a classname, then click in the viewport.");
    }

    IG_SetNextItemWidth(220);
    IG_InputText("filter##spawn", s_filter, sizeof(s_filter), 0);

    IG_BeginChild("##spawn_grid", 0, 0, 0, 0);
    for (i = 0; i < n; i++)
    {
        if (s_filter[0] && !strstri_simple(names[i], s_filter)) continue;
        IG_PushID_Int(i);
        if (IG_Selectable(names[i], 0, 0))
            Editor_BeginPlaceEntity(names[i]);
        IG_PopID();
    }
    IG_EndChild();
    IG_End();
}

// Brush-entity classname heuristic: func_*, trigger_*, misc_teleporttrain.
// Anything else in s_spawns[] is point-entity territory (lights, monsters,
// items, info_*, ambient_*, ...). The handful of trigger_* point entities
// (trigger_relay, trigger_setskill) leak through but a user wrapping
// brushes into them just gets a no-op spawn function — harmless.
static int classname_is_brush_entity(const char *cls)
{
    if (!cls) return 0;
    if (!strncmp(cls, "func_",    5)) return 1;
    if (!strncmp(cls, "trigger_", 8)) return 1;
    return 0;
}

// -----------------------------------------------------------------------------
// Wrap dialog (selected brushes -> new func_*/trigger_* entity)
// -----------------------------------------------------------------------------

static void draw_wrap_dialog(void)
{
    static char s_filter[64] = "";
    int n = 0, i, n_brush_classes = 0;
    const char *const *names;
    edit_brush_t *primary;

    if (!s_show_wrap_dialog) return;

    IG_SetNextWindowSize(360, 480, IG_Cond_FirstUseEver);
    if (!IG_Begin("Wrap Brushes", &s_show_wrap_dialog, IG_WF_None))
    {
        IG_End();
        return;
    }

    primary = Scene_GetSelectedBrush();
    if (!primary)
    {
        IG_TextUnformatted("(select at least one brush, then pick a classname)");
        IG_End();
        return;
    }

    {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "wrapping %d brush%s into a new entity",
                 Scene_NumSelected(),
                 Scene_NumSelected() == 1 ? "" : "es");
        IG_TextUnformatted(buf);
    }

    names = Editor_ClassList_Get(&n);
    if (!names || n == 0)
    {
        IG_TextUnformatted("(game DLL not loaded — start a server then retry)");
        IG_End();
        return;
    }

    IG_SetNextItemWidth(220);
    IG_InputText("filter##wrap", s_filter, sizeof(s_filter), 0);

    IG_BeginChild("##wrap_grid", 0, 0, 0, 0);
    for (i = 0; i < n; i++)
    {
        if (!classname_is_brush_entity(names[i])) continue;
        if (s_filter[0] && !strstri_simple(names[i], s_filter)) continue;
        n_brush_classes++;
        IG_PushID_Int(i);
        if (IG_Selectable(names[i], 0, 0))
        {
            History_Push("wrap entity");
            if (Scene_WrapBrushesIntoEntity(names[i]))
            {
                int new_idx = edit_scene.numentities - 1;
                if (new_idx >= 0)
                    Editor_ApplyClassnameDefaults(
                        &edit_scene.entities[new_idx], names[i]);
            }
            s_show_wrap_dialog = 0;
        }
        IG_PopID();
    }
    IG_EndChild();
    if (n_brush_classes == 0)
        IG_TextUnformatted("(no func_*/trigger_* classes match your filter)");
    IG_End();
}

// -----------------------------------------------------------------------------
// Texture browser (thumbnail palette)
// -----------------------------------------------------------------------------

// (s_show_tex_browser declared up by draw_toolbar — see top of file.)

// Click dispatcher shared by the inspector picker and the browser. Same
// rule both places: with face mode + active face on the primary brush,
// write to that face's plane only; otherwise rewrite all planes of the
// primary brush; if no brush is selected, just update the new-cube
// default cvar.
static void apply_texture_pick(const char *name)
{
    edit_brush_t  *b;
    int af_e, af_b, af_p;
    int p_ent, p_brush;
    int has_active;
    if (!name || !name[0]) return;
    has_active = Scene_GetActiveFace(&af_e, &af_b, &af_p);
    b = Scene_GetSelectedBrush();
    if (b
     && has_active
     && Scene_NumSelected() == 1
     && Scene_GetSelected(0, &p_ent, &p_brush)
     && p_ent == af_e && p_brush == af_b
     && af_p >= 0 && af_p < b->numplanes)
    {
        edit_plane_t *pl = &b->planes[af_p];
        History_Push("face texture");
        Q_strncpy(pl->texname, name, EDIT_TEX_NAME_LEN - 1);
        pl->texname[EDIT_TEX_NAME_LEN - 1] = '\0';
        return;
    }
    if (b)
    {
        int i;
        History_Push("brush texture");
        for (i = 0; i < b->numplanes; i++)
        {
            edit_plane_t *pl = &b->planes[i];
            Q_strncpy(pl->texname, name, EDIT_TEX_NAME_LEN - 1);
            pl->texname[EDIT_TEX_NAME_LEN - 1] = '\0';
        }
        return;
    }
    Cvar_Set("editor_brush_tex", (char *)name);
}

static void draw_texture_browser(void)
{
    static char s_filter[64] = "";
    int n, i;
    const char *const *names;
    float thumb = 64.0f, pad = 8.0f, x_avail;
    int thumbs_per_row, col;

    if (!s_show_tex_browser) return;

    IG_SetNextWindowSize(560, 480, IG_Cond_FirstUseEver);
    if (!IG_Begin("Textures", &s_show_tex_browser, IG_WF_None))
    {
        IG_End();
        return;
    }

    names = world_tex_list(&n);
    if (n == 0)
    {
        IG_TextUnformatted("(no map loaded — load a .map then come back)");
        IG_End();
        return;
    }

    {
        edit_brush_t *b = Scene_GetSelectedBrush();
        int af_e, af_b, af_p;
        int has_active = Scene_GetActiveFace(&af_e, &af_b, &af_p);
        const char *target = (b && has_active) ? "active face only"
                            : (b ? "ALL brush faces"
                                 : "default for new cubes");
        char buf[96];
        snprintf(buf, sizeof(buf), "click writes to: %s", target);
        IG_TextUnformatted(buf);
    }

    IG_SetNextItemWidth(220);
    IG_InputText("filter##browser", s_filter, sizeof(s_filter), 0);

    x_avail = IG_GetContentRegionAvailX();
    thumbs_per_row = (int)((x_avail + pad) / (thumb + pad));
    if (thumbs_per_row < 1) thumbs_per_row = 1;

    IG_BeginChild("##texgrid", 0, 0, 0, 0);
    col = 0;
    for (i = 0; i < n; i++)
    {
        IG_TextureID tex;
        if (s_filter[0] && !strstri_simple(names[i], s_filter)) continue;
        tex = Editor_GetTextureThumbnail(names[i]);
        IG_PushID_Int(i);
        if (tex)
        {
            if (IG_ImageButton("t", tex, thumb, thumb))
                apply_texture_pick(names[i]);
        }
        else
        {
            // Fallback: text-only Selectable when the thumbnail upload
            // failed (renderer not ready, OOM, etc.).
            if (IG_Selectable(names[i], 0, 0))
                apply_texture_pick(names[i]);
        }
        if (IG_IsItemHovered())
        {
            IG_BeginTooltip();
            if (tex) IG_Image(tex, 192, 192);
            IG_TextUnformatted(names[i]);
            IG_EndTooltip();
        }
        IG_PopID();
        col++;
        if (col >= thumbs_per_row) col = 0;
        else                       IG_SameLine(0, pad);
    }
    IG_EndChild();
    IG_End();
}

// -----------------------------------------------------------------------------
// Per-face alignment widgets
// -----------------------------------------------------------------------------

// Cross-face clipboard for "Copy alignment / Paste alignment" — five floats
// matching plane fields s_shift, t_shift, rotation, s_scale, t_scale.
// Survives face changes; reset on editor close is fine (static = zero/one).
static float s_clip_align[5] = { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f };
static int   s_clip_align_set = 0;

// Resolve which plane the alignment widgets target: the active face if set
// and consistent with the primary brush selection, else fall back to plane
// 0 so the widgets are usable even before face selection ships (M2).
// Returns -1 if no plane can be targeted (empty brush).
static int alignment_target_plane(edit_brush_t *b, int *out_is_fallback)
{
    int af_e, af_b, af_p;
    int p_ent, p_brush;
    if (!b || b->numplanes == 0) { if (out_is_fallback) *out_is_fallback = 0; return -1; }
    if (Scene_GetActiveFace(&af_e, &af_b, &af_p)
     && Scene_NumSelected() == 1
     && Scene_GetSelected(0, &p_ent, &p_brush)
     && p_ent == af_e && p_brush == af_b
     && af_p >= 0 && af_p < b->numplanes)
    {
        if (out_is_fallback) *out_is_fallback = 0;
        return af_p;
    }
    if (out_is_fallback) *out_is_fallback = 1;
    return b->numfaces > 0 ? b->faces[0].plane_idx : 0;
}

// Find the face whose plane_idx matches `plane_idx`, return NULL if none
// (degenerate plane that produced no face).
static const edit_face_t *face_for_plane(const edit_brush_t *b, int plane_idx)
{
    int k;
    for (k = 0; k < b->numfaces; k++)
        if (b->faces[k].plane_idx == plane_idx) return &b->faces[k];
    return NULL;
}

// "Fit to face": stretch the texture to span the face's projected extent
// exactly once. Resets rotation to 0 (fit math is undefined under non-zero
// rotation; document and revisit if it bites).
static void apply_fit_to_face(edit_brush_t *b, int plane_idx)
{
    edit_plane_t *p;
    const edit_face_t *f;
    texture_t *tex;
    vec3_t s_ax, t_ax;
    float s_min, s_max, t_min, t_max, s_extent, t_extent;
    int   k;
    if (!b || plane_idx < 0 || plane_idx >= b->numplanes) return;
    p = &b->planes[plane_idx];
    f = face_for_plane(b, plane_idx);
    tex = Editor_PlaneTexture(p);
    if (!f || !tex || tex->width == 0 || tex->height == 0) return;

    Editor_PlaneBaseAxes(p, s_ax, t_ax);

    s_min =  1e30f; s_max = -1e30f;
    t_min =  1e30f; t_max = -1e30f;
    for (k = 0; k < f->numverts; k++)
    {
        float s = DotProduct(f->verts[k], s_ax);
        float t = DotProduct(f->verts[k], t_ax);
        if (s < s_min) s_min = s;
        if (s > s_max) s_max = s;
        if (t < t_min) t_min = t;
        if (t > t_max) t_max = t;
    }
    s_extent = s_max - s_min;
    t_extent = t_max - t_min;
    if (s_extent < 1e-3f || t_extent < 1e-3f) return;   // zero-area face

    p->rotation = 0.0f;
    p->s_scale  = s_extent / (float)tex->width;
    p->t_scale  = t_extent / (float)tex->height;
    // u(v) = DotProduct(v,s_ax)/s_scale + s_shift, want u=0 at s_min:
    p->s_shift  = -s_min / p->s_scale;
    p->t_shift  = -t_min / p->t_scale;
}

// One drag-float row with snapshot-on-activate so a single drag becomes one
// undo step. `desc` ends up in the history label.
static void draw_align_drag(const char *label, float *v, float speed,
                            float vmin, float vmax, const char *desc)
{
    IG_SetNextItemWidth(140);
    IG_DragFloat(label, v, speed, vmin, vmax);
    if (IG_IsItemActivated())
        History_Push(desc);
}

static void draw_face_alignment(edit_brush_t *b)
{
    int   plane_idx;
    int   is_fallback;
    edit_plane_t *p;
    char  buf[96];

    plane_idx = alignment_target_plane(b, &is_fallback);
    if (plane_idx < 0) return;
    p = &b->planes[plane_idx];

    IG_Separator();
    if (is_fallback)
        snprintf(buf, sizeof(buf),
                 "alignment (face plane %d — fallback; face mode coming)",
                 plane_idx);
    else
        snprintf(buf, sizeof(buf), "alignment (active face plane %d)", plane_idx);
    IG_TextUnformatted(buf);

    IG_PushID_Int(plane_idx + 5000);
    draw_align_drag("s_shift",  &p->s_shift,  0.1f,  -1.0e6f, 1.0e6f, "face s_shift");
    draw_align_drag("t_shift",  &p->t_shift,  0.1f,  -1.0e6f, 1.0e6f, "face t_shift");
    draw_align_drag("rotation", &p->rotation, 0.5f, -1.0e4f, 1.0e4f, "face rotation");
    draw_align_drag("s_scale",  &p->s_scale,  0.01f, -1.0e4f, 1.0e4f, "face s_scale");
    draw_align_drag("t_scale",  &p->t_scale,  0.01f, -1.0e4f, 1.0e4f, "face t_scale");

    if (IG_Button("Reset"))
    {
        History_Push("face align reset");
        p->s_shift = 0.0f;
        p->t_shift = 0.0f;
        p->rotation = 0.0f;
        p->s_scale = 1.0f;
        p->t_scale = 1.0f;
    }
    IG_SameLine(0, -1);
    if (IG_Button("Fit"))
    {
        History_Push("face fit");
        apply_fit_to_face(b, plane_idx);
    }
    IG_SameLine(0, -1);
    if (IG_Button("Copy"))
    {
        s_clip_align[0] = p->s_shift;
        s_clip_align[1] = p->t_shift;
        s_clip_align[2] = p->rotation;
        s_clip_align[3] = p->s_scale;
        s_clip_align[4] = p->t_scale;
        s_clip_align_set = 1;
    }
    IG_SameLine(0, -1);
    if (s_clip_align_set)
    {
        if (IG_Button("Paste"))
        {
            History_Push("face align paste");
            p->s_shift  = s_clip_align[0];
            p->t_shift  = s_clip_align[1];
            p->rotation = s_clip_align[2];
            p->s_scale  = s_clip_align[3];
            p->t_scale  = s_clip_align[4];
        }
    }
    else
    {
        IG_TextUnformatted("(Paste: empty)");
    }
    IG_PopID();
}

// -----------------------------------------------------------------------------
// Inspector
// -----------------------------------------------------------------------------

/* Light entity rich controls — drawn above the generic kv list when the
 * selected entity's classname starts with "light". Each widget reads the
 * current kv into native form, lets the user edit it, and writes back as
 * the canonical Quake key string ("_color" "r g b" / "light" "300" /
 * "style" "1" / etc.). The generic kv list below still works for anything
 * outside the special cases. */

static int is_light_entity(const edit_entity_t *e)
{
    int idx;
    if (!e) return 0;
    idx = e->classname_idx;
    if (idx < 0 || idx >= e->numkv) return 0;
    return strncmp(e->kv[idx].value, "light", 5) == 0;
}

static void parse_color_kv(const char *s, float out[3])
{
    double v[3] = { 1.0, 1.0, 1.0 };
    double mx;
    int n;
    out[0] = out[1] = out[2] = 1.0f;
    if (!s || !s[0]) return;
    n = sscanf(s, "%lf %lf %lf", &v[0], &v[1], &v[2]);
    if (n < 1) return;
    /* If only one component was supplied, broadcast it. Patched ericw
     * tools occasionally write a single scalar for legacy mono fixtures. */
    if (n == 1) { v[1] = v[0]; v[2] = v[0]; }
    /* 0..255 detection -- anything above 1.5 (with some safety margin
     * over 1.0 floating point fuzz) is read as 0..255 byte triple. */
    mx = v[0]; if (v[1] > mx) mx = v[1]; if (v[2] > mx) mx = v[2];
    if (mx > 1.5)
    {
        v[0] /= 255.0; v[1] /= 255.0; v[2] /= 255.0;
    }
    if (v[0] < 0) v[0] = 0; if (v[0] > 1) v[0] = 1;
    if (v[1] < 0) v[1] = 0; if (v[1] > 1) v[1] = 1;
    if (v[2] < 0) v[2] = 0; if (v[2] > 1) v[2] = 1;
    out[0] = (float)v[0]; out[1] = (float)v[1]; out[2] = (float)v[2];
}

static const char *s_light_style_names[] = {
    "0 normal",
    "1 flicker A",
    "2 slow strong pulse",
    "3 candle A",
    "4 fast strobe",
    "5 gentle pulse",
    "6 flicker B",
    "7 candle B",
    "8 candle C",
    "9 slow strobe",
    "10 fluorescent flicker",
    "11 slow pulse not fade",
};
enum { LIGHT_STYLE_N = (int)(sizeof(s_light_style_names)/sizeof(s_light_style_names[0])) };

static const char *s_light_falloff_names[] = {
    "0 linear (default)",
    "1 1/x",
    "2 1/x^2",
    "3 no falloff",
};

static void draw_light_inspector_panel(edit_entity_t *e)
{
    float color[3] = { 1, 1, 1 };
    float intensity = 300.0f;
    int   style = 0;
    int   delay = 0;
    char  buf[64];
    int   idx;

    idx = kv_lookup(e, "_color");
    if (idx >= 0) parse_color_kv(e->kv[idx].value, color);
    idx = kv_lookup(e, "light");
    if (idx >= 0) intensity = (float)atof(e->kv[idx].value);
    idx = kv_lookup(e, "style");
    if (idx >= 0) style = atoi(e->kv[idx].value);
    idx = kv_lookup(e, "delay");
    if (idx >= 0) delay = atoi(e->kv[idx].value);

    if (style < 0 || style >= LIGHT_STYLE_N) style = 0;
    if (delay < 0 || delay > 3) delay = 0;

    IG_TextUnformatted("light controls");
    IG_Separator();

    IG_SetNextItemWidth(220);
    if (IG_ColorEdit3("colour", color))
    {
        snprintf(buf, sizeof(buf), "%.3f %.3f %.3f", color[0], color[1], color[2]);
        Entity_SetKV(e, "_color", buf);
    }
    if (IG_IsItemDeactivatedAfterEdit()) History_Push("light colour");

    IG_SetNextItemWidth(220);
    if (IG_DragFloat("intensity", &intensity, 1.0f, 0.0f, 4000.0f))
    {
        snprintf(buf, sizeof(buf), "%d", (int)intensity);
        Entity_SetKV(e, "light", buf);
    }
    if (IG_IsItemDeactivatedAfterEdit()) History_Push("light intensity");

    IG_SetNextItemWidth(220);
    if (IG_Combo("style", &style, s_light_style_names, LIGHT_STYLE_N))
    {
        snprintf(buf, sizeof(buf), "%d", style);
        History_Push("light style");
        Entity_SetKV(e, "style", buf);
    }

    IG_SetNextItemWidth(220);
    if (IG_Combo("falloff", &delay, s_light_falloff_names, 4))
    {
        snprintf(buf, sizeof(buf), "%d", delay);
        History_Push("light falloff");
        Entity_SetKV(e, "delay", buf);
    }

    if (IG_CollapsingHeader("spotlight", 0))
    {
        float cone = 0;
        float angle = 0;
        char  target[64] = "";
        idx = kv_lookup(e, "_cone");
        if (idx >= 0) cone = (float)atof(e->kv[idx].value);
        idx = kv_lookup(e, "_angle");
        if (idx >= 0) angle = (float)atof(e->kv[idx].value);
        idx = kv_lookup(e, "target");
        if (idx >= 0)
        {
            int n = (int)strlen(e->kv[idx].value);
            if (n >= (int)sizeof(target)) n = (int)sizeof(target) - 1;
            memcpy(target, e->kv[idx].value, n);
            target[n] = '\0';
        }

        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("cone half-angle", &cone, 0, 90, "%.1f deg"))
        {
            snprintf(buf, sizeof(buf), "%.1f", cone);
            Entity_SetKV(e, "_cone", buf);
        }
        if (IG_IsItemDeactivatedAfterEdit()) History_Push("light cone");

        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("angle (pitch)", &angle, -90, 90, "%.1f deg"))
        {
            snprintf(buf, sizeof(buf), "%.1f", angle);
            Entity_SetKV(e, "_angle", buf);
        }
        if (IG_IsItemDeactivatedAfterEdit()) History_Push("light angle");

        IG_SetNextItemWidth(220);
        if (IG_InputText("target", target, sizeof(target),
                         IG_ITF_EnterReturnsTrue))
        {
            History_Push("light target");
            Entity_SetKV(e, "target", target);
        }
    }

    IG_TextUnformatted("(Compile + Light re-bakes lighting from these.)");
    IG_Separator();
}

// Floating popup for the light bake's tunables. Hidden by default; shown
// while s_show_light_opts is set. All four widgets are bound to cvars so
// the values survive across map loads and editor open/close cycles. The
// changes flow into the next bake via editor_light_opts_from_cvars on
// the C side -- this window is pure UI.
//
// We deliberately don't trigger an automatic relight when a slider
// moves; the user clicks "Refresh Lighting" when they want to see the
// result. Auto-rebaking on every drag tick would queue dozens of
// SDL_Thread workers in a fraction of a second.
static void draw_light_opts_window(void)
{
    if (!s_show_light_opts) return;

    IG_SetNextWindowSize(360, 0, IG_Cond_FirstUseEver);
    if (!IG_Begin("Light bake options", &s_show_light_opts, IG_WF_None))
    {
        IG_End();
        return;
    }

    IG_TextUnformatted("Bake knobs (apply on next Compile+Light / Refresh Lighting):");
    IG_Separator();

    {
        cvar_t *cv;
        float v;

        cv = Cvar_FindVar("editor_light_scaledist");
        v = cv ? cv->value : 1.0f;
        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("scaledist", &v, 0.1f, 4.0f, "%.2f"))
            Cvar_SetValue("editor_light_scaledist", v);

        cv = Cvar_FindVar("editor_light_scalecos");
        v = cv ? cv->value : 0.5f;
        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("scalecos", &v, 0.0f, 1.0f, "%.2f"))
            Cvar_SetValue("editor_light_scalecos", v);

        cv = Cvar_FindVar("editor_light_rangescale");
        v = cv ? cv->value : 0.5f;
        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("rangescale", &v, 0.05f, 4.0f, "%.2f"))
            Cvar_SetValue("editor_light_rangescale", v);

        ui_cvar_checkbox("extrasamples (2x2 supersample, slow)",
                         "editor_light_extrasamples");
    }

    IG_Separator();
    IG_TextUnformatted("Ambient occlusion (dirt). ~Nx slowdown where N=samples.");
    {
        cvar_t *cv;
        float v;

        ui_cvar_checkbox("dirt enable", "editor_light_dirt");

        cv = Cvar_FindVar("editor_light_dirt_gain");
        v = cv ? cv->value : 1.0f;
        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("dirt gain (attenuation)", &v, 0.0f, 1.0f, "%.2f"))
            Cvar_SetValue("editor_light_dirt_gain", v);

        cv = Cvar_FindVar("editor_light_dirt_depth");
        v = cv ? cv->value : 128.0f;
        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("dirt depth (units)", &v, 8.0f, 1024.0f, "%.0f"))
            Cvar_SetValue("editor_light_dirt_depth", v);

        cv = Cvar_FindVar("editor_light_dirt_samples");
        v = cv ? cv->value : 32.0f;
        IG_SetNextItemWidth(220);
        if (IG_SliderFloat("dirt samples (rays/sample)", &v, 1.0f, 128.0f, "%.0f"))
            Cvar_SetValue("editor_light_dirt_samples", v);

        ui_cvar_checkbox("dirt debug (write AO mask as grey)",
                         "editor_light_dirt_debug");
    }

    IG_Separator();
    /* Render preview toggles. These don't affect what gets baked --
     * they only change how the engine paints the current lightmap on
     * screen, so flipping them costs nothing (no relight needed). */
    IG_TextUnformatted("Render preview (no re-bake needed):");
    ui_cvar_checkbox("r_lightmap        (show lightmap only, no textures)",
                     "r_lightmap");
    ui_cvar_checkbox("r_dynamic         (muzzle flashes / explosions / etc.)",
                     "r_dynamic");
    ui_cvar_checkbox("r_coloredlight    (RGB lightmap from .lit)",
                     "r_coloredlight");
    ui_cvar_checkbox("r_colored_dlights (RGB dynamic lights)",
                     "r_colored_dlights");
    ui_cvar_checkbox("paint_light_preview (editor: light entities as dlights)",
                     "paint_light_preview");

    IG_Separator();
    IG_TextUnformatted("Defaults: scaledist 1.0  scalecos 0.5  rangescale 0.5");
    IG_TextUnformatted("          dirt off  gain 0.5  depth 384  samples 32");
    IG_TextUnformatted("Worldspawn key:  _minlight <value>  sets a brightness floor.");

    /* light_apply re-bakes the running map's BSP from disk and paints
     * the result onto cl.worldmodel; works on stock id1 maps without
     * needing an editor scene. */
    if (IG_Button("Apply now (re-bake current map)"))
        ui_exec("light_apply\n");
    IG_SameLine(0, -1);
    if (IG_Button("Reset to defaults"))
    {
        Cvar_SetValue("editor_light_scaledist",  1.0f);
        Cvar_SetValue("editor_light_scalecos",   0.5f);
        Cvar_SetValue("editor_light_rangescale", 0.5f);
        Cvar_SetValue("editor_light_extrasamples", 0.0f);
        Cvar_SetValue("editor_light_dirt",         0.0f);
        Cvar_SetValue("editor_light_dirt_gain",    0.5f);
        Cvar_SetValue("editor_light_dirt_depth",   384.0f);
        Cvar_SetValue("editor_light_dirt_samples", 32.0f);
        Cvar_SetValue("editor_light_dirt_debug",   0.0f);
    }

    IG_End();
}

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

    IG_SetNextWindowPos(x, y, s_dock_cond);
    IG_SetNextWindowSize((float)UI_RIGHT_W, h, s_dock_cond);
    if (!IG_Begin("Inspector", NULL, IG_WF_None)) { IG_End(); return; }

    e = Scene_GetSelectedEntity();
    b = Scene_GetSelectedBrush();
    if (!e)
    {
        IG_TextUnformatted("(no selection)");
        IG_End();
        return;
    }

    if (is_light_entity(e))
        draw_light_inspector_panel(e);

    // Resolved engine model: what cl_entities[].model->name actually points
    // at right now. Differs from the authored "model" kv when the engine
    // substituted a brush model ("*N") or precache redirected the path.
    {
        const char *mname = NULL;
        if (e->live_ent && Editor_LiveEntInRange(e->live_ent)
            && !e->live_ent->free)
        {
            int en = NUM_FOR_EDICT(e->live_ent);
            if (en > 0 && en < cl.num_entities && cl_entities[en].model)
                mname = cl_entities[en].model->name;
        }
        snprintf(buf, sizeof(buf), "model: %s", mname ? mname : "(none)");
        IG_TextUnformatted(buf);
    }

    IG_TextUnformatted("entity keys");
    IG_Separator();

    // classname first (always), then origin, then everything else in kv
    // array order. Most parsers happen to put classname first already, but
    // for entities populated from worldmodel->entities the order can vary,
    // and reading "classname" should never require scanning a long list.
    // spawnflags gets its own dedicated checkbox section below — skip it
    // in the generic kv loop.
    //
    // Each non-classname row gets an [x] button to remove it; classname
    // is locked because the engine spawn dispatch needs it to function.
    // Removing during iteration breaks the index, so the loop stops on
    // the first removal — UI redraws the next frame against the shrunk
    // array. After the rows, a footer row with [+] + key input appends
    // a new kv (with empty value, ready for the row's text edit). All
    // mutations push their own History_Push so undo round-trips.
    {
        int sf_idx = kv_lookup(e, "spawnflags");
        int ef_idx = kv_lookup(e, "effects");
        int order[2] = { e->classname_idx, e->origin_idx };
        int j;
        int removed_any = 0;
        for (j = 0; j < 2 && !removed_any; j++)
        {
            int idx = order[j];
            int is_classname;
            if (idx < 0 || idx >= e->numkv) continue;
            is_classname = (idx == e->classname_idx);
            IG_PushID_Int(idx);
            snprintf(buf, sizeof(buf), "%s##key", e->kv[idx].key);
            IG_SetNextItemWidth(180);
            if (IG_InputText(buf, e->kv[idx].value, EDIT_VAL_LEN,
                             IG_ITF_EnterReturnsTrue))
                inspector_sync_live(e, e->kv[idx].key, e->kv[idx].value);
            if (!is_classname)
            {
                IG_SameLine(0, -1);
                if (IG_SmallButton("x##rm"))
                {
                    char keycopy[EDIT_KEY_LEN];
                    Q_strncpy(keycopy, e->kv[idx].key, EDIT_KEY_LEN - 1);
                    keycopy[EDIT_KEY_LEN - 1] = '\0';
                    History_Push("remove kv");
                    Entity_RemoveKV(e, keycopy);
                    removed_any = 1;
                }
            }
            IG_PopID();
        }
        for (i = 0; i < e->numkv && !removed_any; i++)
        {
            if (i == e->classname_idx) continue;
            if (i == e->origin_idx)    continue;
            if (i == sf_idx)           continue;
            if (i == ef_idx)           continue;
            IG_PushID_Int(i);
            snprintf(buf, sizeof(buf), "%s##key", e->kv[i].key);
            IG_SetNextItemWidth(180);
            if (IG_InputText(buf, e->kv[i].value, EDIT_VAL_LEN,
                             IG_ITF_EnterReturnsTrue))
                inspector_sync_live(e, e->kv[i].key, e->kv[i].value);
            IG_SameLine(0, -1);
            if (IG_SmallButton("x##rm"))
            {
                char keycopy[EDIT_KEY_LEN];
                Q_strncpy(keycopy, e->kv[i].key, EDIT_KEY_LEN - 1);
                keycopy[EDIT_KEY_LEN - 1] = '\0';
                History_Push("remove kv");
                Entity_RemoveKV(e, keycopy);
                removed_any = 1;
            }
            IG_PopID();
        }

        // Footer: add a new kv. Trims whitespace, rejects collisions
        // (Entity_SetKV upserts, so a duplicate would silently re-target
        // the existing row instead of appending — surprising UX).
        {
            static char s_new_key[EDIT_KEY_LEN] = "";
            int submit;
            IG_SetNextItemWidth(140);
            submit = IG_InputText("##newkey", s_new_key, sizeof(s_new_key),
                                  IG_ITF_EnterReturnsTrue);
            IG_SameLine(0, -1);
            if (IG_SmallButton("+ add kv") || submit)
            {
                if (s_new_key[0] && kv_lookup(e, s_new_key) < 0)
                {
                    History_Push("add kv");
                    Entity_SetKV(e, s_new_key, "");
                }
                s_new_key[0] = '\0';
            }
        }
    }
    IG_Separator();
    draw_spawnflags_section(e);
    IG_Separator();
    draw_effects_section(e);
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

        // Per-face textures. With face mode on, the picker writes only to
        // the active face's plane; with face mode off, it writes to every
        // face plane on the brush (so a new cube can be textured in one
        // click). render_tex.c looks up textures per-frame, so any write
        // here is immediately visible.
        {
            int n;
            const char *const *names = world_tex_list(&n);
            int k;
            IG_Separator();
            IG_TextUnformatted("face textures");
            if (n == 0)
            {
                IG_TextUnformatted("(no map loaded)");
            }
            else
            {
                static char s_filter[64] = "";
                int af_e, af_b, af_p;
                int has_active = Scene_GetActiveFace(&af_e, &af_b, &af_p);
                int p_ent, p_brush;
                int aligned_to_active =
                    has_active
                    && Scene_NumSelected() == 1
                    && Scene_GetSelected(0, &p_ent, &p_brush)
                    && p_ent == af_e && p_brush == af_b;
                int i;

                snprintf(buf, sizeof(buf),
                         "picker writes to: %s",
                         aligned_to_active ? "active face only"
                                           : "ALL faces");
                IG_TextUnformatted(buf);
                IG_SetNextItemWidth(180);
                IG_InputText("filter##facetex", s_filter, sizeof(s_filter), 0);
                IG_BeginChild("##texlist", 0, 130, 0, 0);
                for (i = 0; i < n; i++)
                {
                    if (s_filter[0] && !strstri_simple(names[i], s_filter))
                        continue;
                    if (IG_Selectable(names[i], 0, 0))
                    {
                        if (aligned_to_active)
                        {
                            edit_brush_t *bb = b;
                            edit_plane_t *pl;
                            if (af_p >= 0 && af_p < bb->numplanes)
                            {
                                History_Push("face texture");
                                pl = &bb->planes[af_p];
                                Q_strncpy(pl->texname, names[i],
                                          EDIT_TEX_NAME_LEN - 1);
                                pl->texname[EDIT_TEX_NAME_LEN - 1] = '\0';
                            }
                        }
                        else
                        {
                            int kk;
                            History_Push("brush texture");
                            for (kk = 0; kk < b->numplanes; kk++)
                            {
                                edit_plane_t *pl = &b->planes[kk];
                                Q_strncpy(pl->texname, names[i],
                                          EDIT_TEX_NAME_LEN - 1);
                                pl->texname[EDIT_TEX_NAME_LEN - 1] = '\0';
                            }
                        }
                    }
                }
                IG_EndChild();
            }
            // Per-face read-out so the user can see which face has what.
            // Face mode click targets a face; this confirms the result.
            for (k = 0; k < b->numfaces; k++)
            {
                edit_face_t  *f = &b->faces[k];
                edit_plane_t *p = &b->planes[f->plane_idx];
                int af_e, af_b, af_p;
                int is_active =
                    Scene_GetActiveFace(&af_e, &af_b, &af_p)
                    && af_p == f->plane_idx;
                snprintf(buf, sizeof(buf), "%sface %d  %s",
                         is_active ? "* " : "  ", k, p->texname);
                IG_TextUnformatted(buf);
            }
        }

        draw_face_alignment(b);
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

    // Refresh the dock-panel reflow condition for this frame: Always on the
    // first frame ever and on any frame where the SDL window size changed,
    // FirstUseEver otherwise so manual drags between resizes stay put.
    {
        static float s_last_w = -1, s_last_h = -1;
        float disp_w = 1280, disp_h = 720;
        IG_GetDisplaySize(&disp_w, &disp_h);
        if (disp_w != s_last_w || disp_h != s_last_h)
        {
            s_last_w = disp_w;
            s_last_h = disp_h;
            s_dock_cond = IG_Cond_Always;
        }
        else
        {
            s_dock_cond = IG_Cond_FirstUseEver;
        }
    }

    draw_toolbar();
    draw_brush_list();
    draw_inspector();
    draw_texture_browser();
    draw_spawn_dialog();
    draw_wrap_dialog();
    draw_light_opts_window();
}

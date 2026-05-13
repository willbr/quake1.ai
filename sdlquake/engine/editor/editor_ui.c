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

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Toolbar Textures... button toggles this; consumed by draw_texture_browser.
static int s_show_tex_browser = 0;
// Toolbar "Add Entity..." button toggles this; consumed by draw_spawn_dialog.
static int s_show_spawn_dialog = 0;
// Toolbar "Wrap..." button toggles this; consumed by draw_wrap_dialog.
static int s_show_wrap_dialog = 0;

// -----------------------------------------------------------------------------
// Toolbar
// -----------------------------------------------------------------------------

// Layout constants (in window pixels). Toolbar runs across the top; brush
// list anchors the bottom-left, inspector the bottom-right.
#define UI_PAD          10
#define UI_TOOLBAR_H    100
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

static void draw_toolbar(void)
{
    extern cvar_t editor_render_style;
    extern cvar_t editor_camera;
    extern cvar_t editor_grid_snap;
    extern cvar_t editor_grid_size;
    extern cvar_t editor_grid_absolute;
    extern cvar_t editor_rotate_snap;
    extern cvar_t editor_rotate_snap_size;
    extern cvar_t editor_rotate_snap_absolute;
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
    static const float rotate_snap_values[] = { 5, 10, 15, 22.5f, 30, 45, 90 };
    static const char *rotate_snap_items[]  = { "5", "10", "15", "22.5", "30", "45", "90" };
    enum { RSNAP_N = (int)(sizeof(rotate_snap_values) / sizeof(rotate_snap_values[0])) };

    float disp_w = 1280, disp_h = 720;
    IG_GetDisplaySize(&disp_w, &disp_h);

    IG_SetNextWindowPos((float)UI_PAD, (float)UI_PAD, s_dock_cond);
    IG_SetNextWindowSize(disp_w - 2 * UI_PAD, (float)UI_TOOLBAR_H, s_dock_cond);
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
    // (Add cube moved to row 2, paired with the texture picker.)
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
    IG_SameLine(0, -1);
    /* qbsp-only fast iteration on geometry: the map reloads with the
     * existing .lit (if any), so lighting stays whatever the last bake
     * produced. */
    if (IG_Button("Compile"))      Cbuf_AddText("editor_compile\n");
    IG_SameLine(0, -1);
    /* qbsp + light: re-bakes lightmaps from the current `light*` entities
     * in the scene. Slower (single-threaded direct lighting), but the
     * only way to see colour/intensity edits in the rendered .bsp. */
    if (IG_Button("Compile + Light")) Cbuf_AddText("editor_compile_full\n");
    IG_SameLine(0, -1);
    if (IG_Button("Textures..."))  s_show_tex_browser = !s_show_tex_browser;
    IG_SameLine(0, -1);
    if (IG_Button("Add Entity..."))s_show_spawn_dialog = !s_show_spawn_dialog;
    IG_SameLine(0, -1);
    if (IG_Button("Wrap..."))      s_show_wrap_dialog  = !s_show_wrap_dialog;

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
        // Trigger render mode: 0 = single red AABB per trigger volume
        // (clean authoring view), 1 = render trigger brushes textured
        // (face-level inspection). Independent of editor_render_style.
        extern cvar_t editor_trigger_render;
        int on = editor_trigger_render.value != 0.0f;
        if (IG_Checkbox("trigger tex", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_trigger_render %d\n", on ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Clip render mode: 0 = teal AABB per clip brush (clean view),
        // 1 = textured (procedural-grid fallback since clip has no
        // miptex). Per-brush, not per-entity — most clip brushes live
        // in worldspawn alongside visible geometry.
        extern cvar_t editor_clip_render;
        int on = editor_clip_render.value != 0.0f;
        if (IG_Checkbox("clip tex", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_clip_render %d\n", on ? 1 : 0);
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
        // Navmesh debug overlay. Drives sim_nav_debug, which the game
        // DLL registers; lines only appear while the sim is ticking.
        int on = Cvar_VariableValue("sim_nav_debug") != 0.0f;
        if (IG_Checkbox("navmesh", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "sim_nav_debug %d\n", on ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Navmesh z-test. When on, edges are occluded by world geometry
        // (only the portion the camera can directly see). Companion to the
        // navmesh toggle — has no effect when navmesh is off.
        int on = Cvar_VariableValue("sim_nav_ztest") != 0.0f;
        if (IG_Checkbox("z-test", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "sim_nav_ztest %d\n", on ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        // Face mode: clicks set the active face on the singly-selected
        // brush instead of replacing the brush selection. Active face
        // gates the inspector's alignment widgets and gets a white
        // outline overlay in the wireframe pass.
        extern cvar_t editor_face_mode;
        int on = editor_face_mode.value != 0.0f;
        if (IG_Checkbox("faces", &on))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_face_mode %d\n", on ? 1 : 0);
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
        int rsnap = editor_rotate_snap.value != 0.0f;
        if (IG_Checkbox("rsnap", &rsnap))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_rotate_snap %d\n", rsnap ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        int rabs = editor_rotate_snap_absolute.value != 0.0f;
        if (IG_Checkbox("abs##rot", &rabs))
        {
            char buf[40];
            snprintf(buf, sizeof(buf), "editor_rotate_snap_absolute %d\n", rabs ? 1 : 0);
            Cbuf_AddText(buf);
        }
    }
    IG_SameLine(0, -1);
    {
        int sel = 5, k;
        float cur = editor_rotate_snap_size.value;
        for (k = 0; k < RSNAP_N; k++)
            if (rotate_snap_values[k] == cur) { sel = k; break; }
        IG_SetNextItemWidth(60);
        if (IG_Combo("rangle", &sel, rotate_snap_items, RSNAP_N))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "editor_rotate_snap_size %g\n", rotate_snap_values[sel]);
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
    // Brush palette: texture combo + Add cube. Names are pulled from
    // world_tex_list(); selection mirrors the editor_brush_tex cvar.
    {
        extern cvar_t editor_brush_tex;
        int n;
        const char *const *names = world_tex_list(&n);
        int sel = world_tex_index(editor_brush_tex.string);
        if (sel < 0) sel = 0;
        IG_SetNextItemWidth(180);
        if (n > 0)
        {
            if (IG_Combo("brush tex", &sel, names, n))
                Cvar_Set("editor_brush_tex", (char *)names[sel]);
        }
        else
        {
            IG_TextUnformatted("brush tex (no map loaded)");
        }
        IG_SameLine(0, -1);
        if (IG_Button("Add cube"))
            Cbuf_AddText("editor_brush_add_cube\n");
        IG_SameLine(0, -1);
        if (IG_Button("Hollow"))
            Cbuf_AddText("editor_brush_hollow\n");
    }
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
// re-opens within a session. The "OTHER" slot covers worldspawn /
// misc_* / etc — we don't expose a checkbox for it (hiding worldspawn
// would render the level invisible), but the array entry exists so
// EDIT_CAT_OTHER indexing is uniform.
//
// Default hides triggers, ambient sounds, path corners, and info_*
// metadata -- none of these have a model the engine actually renders,
// so they just clutter the view; the user can re-enable them from the
// panel checkboxes. Lights stay visible by default now that the editor
// has first-class light-entity authoring (Compile + Light, the light
// inspector panel, viewport gizmos) -- hiding them would defeat the
// whole flow.
static int s_hide_cat[EDIT_CAT_COUNT] = {
    [EDIT_CAT_TRIGGER] = 1,
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

int Editor_EntityHiddenByCategory(int e_idx)
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
        if (view_live && e->live_ent && !e->live_ent->free)
        {
            int en = NUM_FOR_EDICT(e->live_ent);
            int engine_renders =
                (en > 0 && en < cl.num_entities && cl_entities[en].model);
            if (!engine_renders) return 1;
        }
    }
    return 0;
}

// Combine category-filter visibility with view-mode visibility. The
// Brushes panel uses this rather than calling Editor_EntityHidden
// directly, so metadata edicts (info_player_*, info_intermission)
// still show in live view's list — they're alive engine edicts even
// though Editor_EntityHidden filters them from render/pick.
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
        if (!brush_list_visible(i)) continue;
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
}

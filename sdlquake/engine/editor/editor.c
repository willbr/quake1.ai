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
#include "edit_history.h"
#include "editor.h"
#include "editor_internal.h"
#include "hotreload.h"          // g_game_api

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

// Free-fly / FPS camera support (M5 dev work).
//
// editor_camera 0 = free-fly: sim is paused, the editor owns r_refdef and
//                  WASD + mouse-look (RMB held) drive a detached camera.
// editor_camera 1 = fps: sim runs while RMB is held (mouse-look + WASD route
//                  to the player); cursor is free for clicking gizmos / UI
//                  the rest of the time. Sim pauses again when RMB is up.
//
// Tab toggles between modes while the editor is open. RMB enters/leaves
// "look mode" (relative-mouse capture).
cvar_t      editor_camera = { "editor_camera", "0" };

// Grid snap state. When grid_snap is 1, gizmo drags + new-cube placement
// snap to multiples of grid_size in world units. Default 16 matches the
// QuakeEd build grid. When grid_absolute is 1, the snapped position is the
// brush centroid's *world coordinate* on the dragged axis (so brushes line
// up across drag operations — useful for stairs); when 0, snapping is
// relative to the centroid at drag start.
cvar_t      editor_grid_snap     = { "editor_grid_snap", "1" };
cvar_t      editor_grid_size     = { "editor_grid_size", "16" };
cvar_t      editor_grid_absolute = { "editor_grid_absolute", "1" };

static int     s_lookmode      = 0;
static int     s_camera_inited = 0;
static vec3_t  s_cam_origin;
static vec3_t  s_cam_angles;
static int     s_cam_mouse_dx  = 0;
static int     s_cam_mouse_dy  = 0;

static void set_lookmode(int on);

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

    // Loading a new scene invalidates any prior history — undo would
    // suddenly restore from a different map.
    History_Clear();

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

static void Editor_Cmd_Group_f  (void)
{
    History_Push("group");
    Scene_GroupSelected();
}

static void Editor_Cmd_Ungroup_f(void)
{
    History_Push("ungroup");
    Scene_UngroupSelected();
}

static void Editor_Cmd_Undo_f(void)
{
    if (!History_Undo()) Con_Printf("editor: nothing to undo\n");
}

static void Editor_Cmd_Redo_f(void)
{
    if (!History_Redo()) Con_Printf("editor: nothing to redo\n");
}

// Compute the focal point ~128 units in front of the camera, snapped to the
// active grid. Used by Add cube + Add entity so newly-created items land in
// the same place the user was looking.
static void compute_camera_focal(vec3_t out)
{
    extern vec3_t r_origin, vpn;
    int i;
    float grid = editor_grid_snap.value ? editor_grid_size.value : 0.0f;
    const float DIST = 128.0f;
    for (i = 0; i < 3; i++)
    {
        float c = r_origin[i] + vpn[i] * DIST;
        if (grid > 0.0f) c = floorf(c / grid + 0.5f) * grid;
        out[i] = c;
    }
}

// Spawn a point entity with the given classname at the camera focal point.
// Usage: editor_entity_add <classname>
static void Editor_Cmd_AddEntity_f(void)
{
    vec3_t origin;
    const char *classname;
    if (Cmd_Argc() < 2)
    {
        Con_Printf("usage: editor_entity_add <classname>\n");
        return;
    }
    classname = Cmd_Argv(1);
    compute_camera_focal(origin);
    History_Push("add entity");
    if (Scene_AddPointEntity(classname, origin))
        Con_Printf("editor: added %s at %.0f %.0f %.0f\n",
                   classname, origin[0], origin[1], origin[2]);
}

// Spawn a 64-unit cube at the camera focal point. New brush is appended to
// worldspawn and becomes the current selection.
static void Editor_Cmd_AddCube_f(void)
{
    vec3_t center, mins, maxs;
    int i;
    const float HALF = 32.0f;       // 64-unit cube
    compute_camera_focal(center);
    for (i = 0; i < 3; i++)
    {
        mins[i] = center[i] - HALF;
        maxs[i] = center[i] + HALF;
    }
    History_Push("add cube");
    if (Scene_AddCubeBrush(mins, maxs, NULL))
        Con_Printf("editor: added cube at %.0f %.0f %.0f\n",
                   center[0], center[1], center[2]);
}

// List every texture in cl.worldmodel — these are the names that brush face
// texnames in a .map will resolve against. Anything not in this list falls
// back to the procedural grid in the textured render style.
static void Editor_Cmd_Textures_f(void)
{
    int i, n = 0;
    if (!cl.worldmodel || !cl.worldmodel->textures)
    {
        Con_Printf("editor_textures: no worldmodel loaded\n");
        return;
    }
    for (i = 0; i < cl.worldmodel->numtextures; i++)
    {
        texture_t *t = cl.worldmodel->textures[i];
        if (!t) continue;
        Con_Printf("  %-16s %3ux%-3u\n", t->name, t->width, t->height);
        n++;
    }
    Con_Printf("editor_textures: %d textures in '%s'\n",
               n, cl.worldmodel->name);
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
// Populate edit_scene from a running server (editor opened without editor_load)
// -----------------------------------------------------------------------------
//
// When the user does `+map start` and opens the editor without first running
// editor_load, edit_scene is empty and they see no entities. Build a virtual
// scene directly from sv.edicts so monsters / lights / players / triggers
// are immediately selectable and draggable. Brushes from the BSP stay
// outside edit_scene (the engine renders them as part of cl.worldmodel).

static void populate_entity_from_edict(edict_t *ed, edit_entity_t *e)
{
    char buf[256];
    memset(e, 0, sizeof(*e));
    e->classname_idx = -1;
    e->origin_idx    = -1;
    e->live_ent      = ed;
    e->spawned       = 1;       // already in the world

    if (ed->v.classname && ed->v.classname[0])
        Entity_SetKV(e, "classname", ed->v.classname);

    snprintf(buf, sizeof(buf), "%g %g %g",
             ed->v.origin[0], ed->v.origin[1], ed->v.origin[2]);
    Entity_SetKV(e, "origin", buf);

    if (ed->v.angles[1] != 0)
    {
        snprintf(buf, sizeof(buf), "%g", ed->v.angles[1]);
        Entity_SetKV(e, "angle", buf);
    }
    if (ed->v.spawnflags != 0)
    {
        snprintf(buf, sizeof(buf), "%g", ed->v.spawnflags);
        Entity_SetKV(e, "spawnflags", buf);
    }
    if (ed->v.target && ed->v.target[0])
        Entity_SetKV(e, "target", ed->v.target);
    if (ed->v.targetname && ed->v.targetname[0])
        Entity_SetKV(e, "targetname", ed->v.targetname);
}

static void Editor_PopulateFromServer(void)
{
    int i, n;
    if (!sv.active) return;
    Scene_Clear();
    History_Clear();

    // Conservative upper bound for the entities array; we'll only fill the
    // slots that pass the filter loop below.
    edit_scene.entities = (edit_entity_t *)malloc(
        sv.num_edicts * sizeof(edit_entity_t));
    n = 0;
    for (i = 1; i < sv.num_edicts; i++)         // 1: skip worldspawn
    {
        edict_t *ed = EDICT_NUM(i);
        if (ed->free) continue;
        if (!ed->v.classname || !ed->v.classname[0]) continue;
        // Brush entities (func_door etc) own a BSP submodel — kv editing is
        // useful but we don't pull their brushes into edit_scene; their
        // visuals come from the engine's bmodel pipeline.
        populate_entity_from_edict(ed, &edit_scene.entities[n]);
        n++;
    }
    edit_scene.numentities = n;

    // Track the current map so the Restart button + a future explicit
    // editor_save know what to write to. Filename stays empty so auto-save
    // on close stays a no-op — overwriting id1/maps/<name>.map silently
    // would destroy any existing source .map (test.map etc.).
    if (sv.name[0])
    {
        size_t l = strlen(sv.name);
        if (l >= sizeof(edit_scene.mapname)) l = sizeof(edit_scene.mapname) - 1;
        memcpy(edit_scene.mapname, sv.name, l);
        edit_scene.mapname[l] = '\0';
    }
    edit_scene.filename[0] = '\0';

    Con_Printf("editor: populated %d entities from running server (%s)\n",
               n, edit_scene.mapname[0] ? edit_scene.mapname : "(no name)");
}

// -----------------------------------------------------------------------------
// Spawn pending point entities (closing the editor → live edicts)
// -----------------------------------------------------------------------------
//
// When the user adds a point entity in the editor (Add entity / editor_entity_add)
// it lives in edit_scene with spawned=0. The editor renders its model preview,
// but the engine has no edict for it — so the player can't touch it. On close
// we walk those pending entities and run the same flow ED_LoadFromFile uses
// at map load: ED_Alloc, ED_ParseEdict, then game.dll's entity_spawn dispatch
// which fires the classname's spawn function (sets model/size/touch/etc).
//
// Brushes added via Add cube don't need this — they overlay through
// Editor_RenderScene and collide via the editor's brute-force trace path,
// neither of which involves a server edict.

// Synthesize the ED_ParseEdict-consumable text for one entity's kv list and
// hand the resulting edict to game.dll's spawn dispatch. Returns 1 on
// successful spawn, 0 if skipped or failed.
static int spawn_one_pending(edit_entity_t *e)
{
    edict_t *ent;
    char     buf[2048];
    int      len = 0;
    int      i;
    char    *parsed;
    const char *classname;

    if (e->classname_idx < 0) return 0;
    classname = e->kv[e->classname_idx].value;

    // info_player_start / info_player_deathmatch / info_intermission /
    // info_teleport_destination etc are level metadata, not touchable
    // entities — game_entity_spawn ignores them anyway, but skipping
    // here avoids burning an edict slot for nothing.
    if (!strncmp(classname, "info_", 5)) return 0;

    // Emit "key" "value" lines followed by a closing brace; ED_ParseEdict
    // expects to start *inside* a brace and consume until '}'.
    for (i = 0; i < e->numkv; i++)
    {
        int wrote = snprintf(buf + len, sizeof(buf) - len,
                             "\"%s\" \"%s\"\n",
                             e->kv[i].key, e->kv[i].value);
        if (wrote <= 0 || (size_t)(len + wrote) >= sizeof(buf) - 4) break;
        len += wrote;
    }
    snprintf(buf + len, sizeof(buf) - len, "}\n");

    ent = ED_Alloc();
    parsed = ED_ParseEdict(buf, ent);
    if (!parsed)
    {
        ED_Free(ent);
        Con_Printf("editor: spawn failed (parse) for '%s'\n", classname);
        return 0;
    }

    if (!g_game_api || !g_game_api->entity_spawn)
    {
        ED_Free(ent);
        return 0;
    }

    // Many spawn functions read pr_global_struct->self while running.
    pr_global_struct->self = EDICT_TO_PROG(ent);
    g_game_api->entity_spawn(ent, ent->v.classname);

    // The spawn function usually calls SV_SetOrigin / SV_SetSize which link
    // automatically; relink defensively for the rare cases that don't.
    SV_LinkEdict(ent, false);

    // Cache the edict so subsequent gizmo drags / kv edits push live state
    // into this same entity (otherwise re-spawning is the only way to
    // affect a placed monster's origin).
    e->live_ent = ent;
    return 1;
}

static void Editor_FlushPendingEntities(void)
{
    int i, n_spawned = 0;
    if (!g_game_api) return;
    if (!sv.active)  return;        // no live server to spawn into

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (!Entity_IsPoint(e)) continue;
        if (e->spawned) continue;
        if (spawn_one_pending(e)) n_spawned++;
        // Mark spawned even on skip so we don't try every frame — the
        // skip reasons (info_*, missing classname) won't change without
        // a kv edit, which the user can re-trigger by re-adding.
        e->spawned = 1;
    }
    if (n_spawned > 0)
        Con_Printf("editor: spawned %d pending entit%s into the world\n",
                   n_spawned, n_spawned == 1 ? "y" : "ies");
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
    Cmd_AddCommand("editor_textures", Editor_Cmd_Textures_f);
    Cmd_AddCommand("editor_brush_add_cube", Editor_Cmd_AddCube_f);
    Cmd_AddCommand("editor_entity_add",     Editor_Cmd_AddEntity_f);
    Cmd_AddCommand("editor_group",   Editor_Cmd_Group_f);
    Cmd_AddCommand("editor_ungroup", Editor_Cmd_Ungroup_f);
    Cmd_AddCommand("editor_undo",    Editor_Cmd_Undo_f);
    Cmd_AddCommand("editor_redo",    Editor_Cmd_Redo_f);

    History_Init();

    Cvar_RegisterVariable(&editor_camera);
    Cvar_RegisterVariable(&editor_grid_snap);
    Cvar_RegisterVariable(&editor_grid_size);
    Cvar_RegisterVariable(&editor_grid_absolute);

    {
        extern void Editor_RegisterCvars(void);
        Editor_RegisterCvars();
    }

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

    // Opening the editor without an explicit editor_load: build edit_scene
    // from the running server's edicts so monsters / lights / players are
    // selectable and editable. Re-runs when the map name changes (so a
    // mid-session +map command picks up the new world). Skipped if the
    // user already loaded a .map manually (filename non-empty), which we
    // treat as "they explicitly chose the source".
    if (!s_open && sv.active)
    {
        int need_populate = (edit_scene.numentities == 0)
            || (edit_scene.filename[0] == '\0'
                && strcmp(edit_scene.mapname, sv.name) != 0);
        if (need_populate)
            Editor_PopulateFromServer();
    }

    // Closing the editor: spawn any newly-placed point entities into the
    // live server so the player can actually touch them, then auto-save
    // the .map. Order matters — flushing first means the save reflects
    // post-spawn kv defaults (e.g. spawn functions sometimes back-fill
    // spawnflags), keeping the file authoritative.
    if (s_open)
    {
        Editor_FlushPendingEntities();
        if (edit_scene.filename[0])
        {
            if (Scene_Save(edit_scene.filename))
                Con_Printf("editor: auto-saved %s\n", edit_scene.filename);
            else
                Con_Printf("editor: auto-save failed for %s\n",
                           edit_scene.filename);
        }
    }

    s_open = !s_open;

    // Editor open requires the dev overlay to be open (ImGui receives input,
    // mouse cursor freed). Mirror its state, but only flip if needed.
    if (s_open && !ImguiLayer_IsOpen())
        ImguiLayer_Toggle();
    else if (!s_open && ImguiLayer_IsOpen())
        ImguiLayer_Toggle();

    // Re-latch the camera on the next pre-render and drop any residual
    // look-mode capture.
    s_camera_inited = 0;
    if (s_lookmode) set_lookmode(0);
}

int Editor_IsOpen  (void) { return s_open; }
int Editor_IsPaused(void)
{
    if (!s_open) return 0;
    if ((int)editor_camera.value == 0) return 1;   // free-fly: always paused
    return !s_lookmode;                            // fps: paused when not looking
}
int Editor_LookmodeActive (void) { return s_open && s_lookmode; }
int Editor_AllowGameInput (void)
{
    return s_open && s_lookmode && (int)editor_camera.value == 1;
}
int Editor_ShouldDrawPlayer(void)
{
    // Only in free-fly: in FPS mode the camera is at the player's eyes so
    // their own model would block the view.
    return s_open && (int)editor_camera.value == 0;
}

// -----------------------------------------------------------------------------
// Camera mode + lookmode helpers
// -----------------------------------------------------------------------------

// Stock Quake1's Key_ClearStates only zeros the keydown[] array, so it
// doesn't fire the "-forward 119" etc. release commands that actually clear
// in_forward.state. We need to walk the array and call Key_Event(i, false)
// per held key so the player input system's bind state matches reality.
extern qboolean keydown[256];

static void release_held_keys(void)
{
    int i;
    for (i = 0; i < 256; i++)
        if (keydown[i]) Key_Event(i, false);
}

static void set_lookmode(int on)
{
    SDL_Window *w = VID_GetWindow();
    int was_on = s_lookmode;
    s_lookmode = on ? 1 : 0;
    s_cam_mouse_dx = s_cam_mouse_dy = 0;
    if (w) SDL_SetWindowRelativeMouseMode(w, on ? true : false);

    // Leaving look mode: fire fake key-up events for everything currently
    // held. Otherwise the in_sdl.c gate (`if (ImguiLayer_IsOpen() &&
    // !Editor_AllowGameInput()) break;`) silently swallows the real key-up
    // events when the user releases RMB before the WASD key, leaving
    // +forward / +moveright / etc. latched. The next time we re-enter look
    // mode the player walks "by itself".
    if (was_on && !on)
        release_held_keys();
}

void Editor_CycleCameraMode(void)
{
    int next = ((int)editor_camera.value == 0) ? 1 : 0;
    Cvar_SetValue("editor_camera", (float)next);
    // If we changed mode mid-look, drop look so the player input doesn't get
    // a jolt of accumulated delta from the wrong consumer.
    if (s_lookmode) set_lookmode(0);
    Con_Printf("editor camera: %s\n", next == 0 ? "free-fly" : "fps");
}

// -----------------------------------------------------------------------------
// Pre-render hook: called from R_RenderView_ before R_SetupFrame
// -----------------------------------------------------------------------------

void Editor_PreRender(void)
{
    extern double host_frametime;
    extern cvar_t sensitivity, m_pitch, m_yaw;

    if (!s_open) return;

    // First frame after opening the editor: latch the current view into the
    // camera state so free-fly mode starts where the player was standing.
    if (!s_camera_inited)
    {
        VectorCopy(r_refdef.vieworg,    s_cam_origin);
        VectorCopy(r_refdef.viewangles, s_cam_angles);
        s_camera_inited = 1;
    }

    if ((int)editor_camera.value != 0) return;     // FPS mode: no override

    // Free-fly mode: drive camera from input while RMB held, then override
    // r_refdef so the renderer sees the editor camera.
    if (s_lookmode)
    {
        // Mouse-look (uses Quake's same mouse params for consistency).
        s_cam_angles[YAW]   -= m_yaw.value   * (float)s_cam_mouse_dx * sensitivity.value;
        s_cam_angles[PITCH] += m_pitch.value * (float)s_cam_mouse_dy * sensitivity.value;
        if (s_cam_angles[PITCH] >  89) s_cam_angles[PITCH] =  89;
        if (s_cam_angles[PITCH] < -89) s_cam_angles[PITCH] = -89;
        s_cam_mouse_dx = s_cam_mouse_dy = 0;

        // WASD + Space/Ctrl from raw keyboard state (ImGui's input is gated;
        // we don't go through Quake's key system at all here). Use the
        // SDL3-native bool* type so we don't rely on sizeof(bool)==1.
        // Modifier keys also come from SDL_GetModState — more reliable than
        // reading individual scancodes for Ctrl/Shift on Windows.
        {
            const bool *keys = SDL_GetKeyboardState(NULL);
            SDL_Keymod  mods = SDL_GetModState();
            vec3_t fwd, right, up;
            float speed = 240.0f * (float)host_frametime;
            int   i;
            if (mods & SDL_KMOD_SHIFT) speed *= 4.0f;
            AngleVectors(s_cam_angles, fwd, right, up);
            if (keys[SDL_SCANCODE_W])
                for (i = 0; i < 3; i++) s_cam_origin[i] += fwd[i] * speed;
            if (keys[SDL_SCANCODE_S])
                for (i = 0; i < 3; i++) s_cam_origin[i] -= fwd[i] * speed;
            if (keys[SDL_SCANCODE_D])
                for (i = 0; i < 3; i++) s_cam_origin[i] += right[i] * speed;
            if (keys[SDL_SCANCODE_A])
                for (i = 0; i < 3; i++) s_cam_origin[i] -= right[i] * speed;
            if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_E])
                s_cam_origin[2] += speed;
            if ((mods & SDL_KMOD_CTRL) || keys[SDL_SCANCODE_Q])
                s_cam_origin[2] -= speed;
        }
    }

    VectorCopy(s_cam_origin, r_refdef.vieworg);
    VectorCopy(s_cam_angles, r_refdef.viewangles);
}

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
        if (ev->button.button == SDL_BUTTON_RIGHT)
        {
            // RMB enters look mode. Don't grab if the cursor is over an
            // ImGui panel — let the panel take the click instead.
            if (IG_WantCaptureMouse()) return 0;
            set_lookmode(1);
            return 1;
        }
        if (ev->button.button != SDL_BUTTON_LEFT) return 0;
        // ImGui owns the click if the cursor is over an ImGui window (panel,
        // button). Don't run picking/gizmo in that case.
        if (IG_WantCaptureMouse()) return 0;
        // Don't pick or drag while looking around.
        if (s_lookmode) return 0;
        float vx, vy;
        window_to_vid(ev->button.x, ev->button.y, &vx, &vy);
        // Try the gizmo first; if it doesn't grab an axis, treat as a pick.
        if (Editor_GizmoMouseDown(vx, vy)) return 1;
        {
            int e_idx, b_idx;
            SDL_Keymod mod = SDL_GetModState();
            int shift = (mod & SDL_KMOD_SHIFT) != 0;
            if (Editor_PickAt(vx, vy, &e_idx, &b_idx))
            {
                if (shift)
                {
                    Scene_SelectionToggle(e_idx, b_idx);
                }
                else
                {
                    Scene_SelectionClear();
                    Scene_SelectionAdd(e_idx, b_idx);
                }
            }
            else if (!shift)
            {
                // Click on empty space with no shift = clear.
                Scene_SelectionClear();
            }
        }
        return 1;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ev->button.button == SDL_BUTTON_RIGHT && s_lookmode)
        {
            set_lookmode(0);
            return 1;
        }
        if (ev->button.button == SDL_BUTTON_LEFT && Editor_GizmoIsActive())
        {
            Editor_GizmoMouseUp();
            return 1;
        }
        return 0;
    case SDL_EVENT_MOUSE_MOTION:
        // Free-fly look: accumulate the delta locally; the pre-render hook
        // converts it to camera angles. FPS look: leave the event alone so
        // in_sdl.c's player-mouse path picks it up.
        if (s_lookmode && (int)editor_camera.value == 0)
        {
            s_cam_mouse_dx += (int)ev->motion.xrel;
            s_cam_mouse_dy += (int)ev->motion.yrel;
            return 1;
        }
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

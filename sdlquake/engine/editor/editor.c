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
#include "virtual_fs.h"         // Editor_VFS_*
#include "qbsp_lib.h"           // qbsp_compile_to_memory

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
cvar_t      editor_rotate_snap          = { "editor_rotate_snap",          "1"  };
cvar_t      editor_rotate_snap_size     = { "editor_rotate_snap_size",     "45" };
cvar_t      editor_rotate_snap_absolute = { "editor_rotate_snap_absolute", "1"  };

// View mode: 0 = "live" (show what's running — engine renders edicts at
// AI-moved positions, bbox follows live, gizmo anchors on live). 1 = "map"
// (show what the .map says — preview entities at .map origin, live edicts
// scrubbed from cl_visedicts so they don't double-render, bbox + gizmo at
// .map origin). Default live so the editor opens looking like the game.
cvar_t      editor_view_mode     = { "editor_view_mode", "0" };

// Overlay toggles for the per-entity facing/movedir arrows and the
// target/killtarget link arrows. Selection always overrides — clicking
// an entity reveals its own arrow + the links touching it even when
// the global toggle is off. Angles default off (a screenful of arrows
// over every monster + spawn is more clutter than insight); links
// default on since they're rarer and authoring-relevant.
cvar_t      editor_show_angles   = { "editor_show_angles", "0" };
cvar_t      editor_show_links    = { "editor_show_links",  "1" };

// Face mode: 0 = clicks select brushes (default), 1 = clicks set the active
// face on the singly-selected brush. Toggled via toolbar Faces button.
// Active-face state lives in edit_scene; this cvar is just the UI flag.
cvar_t      editor_face_mode     = { "editor_face_mode",   "0" };

// Default texture for newly-spawned cube brushes. The toolbar texture
// picker writes here; Editor_Cmd_AddCube_f reads it. Falls through to
// Scene_AddCubeBrush's "wbrick1_5" hardcoded fallback if blanked out
// or set to a name that isn't in the loaded worldmodel.
cvar_t      editor_brush_tex     = { "editor_brush_tex", "wbrick1_5" };

// Wall thickness used by the "hollow" operation (Scene_HollowBrush).
// 16 units fits the standard build grid and keeps a 64-unit cube
// hollow-able (thickness * 2 must stay below the smallest extent).
cvar_t      editor_brush_hollow_thickness = { "editor_brush_hollow_thickness", "16" };

static int     s_lookmode      = 0;
static int     s_camera_inited = 0;
static vec3_t  s_cam_origin;
static vec3_t  s_cam_angles;
static int     s_cam_mouse_dx  = 0;
static int     s_cam_mouse_dy  = 0;

// Pending entity placement (set by Editor_BeginPlaceEntity, consumed by
// the next viewport LMB click). Empty string means nothing is pending.
static char    s_pending_classname[64] = { 0 };
// While the user is holding LMB after a place-click, the entity origin
// tracks the cursor's raycast hit so they can drag the new entity to a
// final spot in one motion. Released on LMB up.
static int     s_placing_drag    = 0;
static int     s_placing_ent_idx = -1;

static void set_lookmode(int on);

// -----------------------------------------------------------------------------
// Console commands
// -----------------------------------------------------------------------------

// Build a minimal playable scaffold: a sealed 256-cube hollow room with
// 16-unit walls, an info_player_start above the floor, and a func_door
// brush waiting inside. Provides a known-good starting scene so
// editor_compile + walk-into-door works end-to-end without the user
// needing to author from scratch each session. Caller has already run
// History_Clear + Scene_Clear if appropriate.
static void scaffold_build_test_room(const char *mapname)
{
    vec3_t r_mins = { -128, -128,    0 };  /* floor at z=0 */
    vec3_t r_maxs = {  128,  128,  128 };
    vec3_t door_mins = {  16,   80,    0 };
    vec3_t door_maxs = {  48,  112,   96 };
    vec3_t spawn     = { -64,    0,   24 };
    int    door_ent_idx;

    /* Big solid cube; Hollow turns it into 6 wall slabs. */
    Scene_AddCubeBrush(r_mins, r_maxs, "wbrick1_5");
    Scene_HollowBrush(0 /*worldspawn*/, 0, 16);

    /* Brush 0 = floor slab, brush 1 = ceiling slab (see Scene_HollowBrush
     * ordering: floor=z:mins..mins+thick, ceiling=z:maxs-thick..maxs). */
    {
        edit_entity_t *ws = &edit_scene.entities[0];
        int p;
        if (ws->numbrushes >= 2)
        {
            for (p = 0; p < ws->brushes[0].numplanes; p++)
                Q_strncpy(ws->brushes[0].planes[p].texname, "floor0_1",
                          EDIT_TEX_NAME_LEN - 1);
            Brush_Compile(&ws->brushes[0]);

            for (p = 0; p < ws->brushes[1].numplanes; p++)
                Q_strncpy(ws->brushes[1].planes[p].texname, "sky1",
                          EDIT_TEX_NAME_LEN - 1);
            Brush_Compile(&ws->brushes[1]);
        }
    }

    /* qbsp's WriteMiptex needs worldspawn to declare a "wad" key or
     * it warns "no wadfile specified" and ships the .bsp with zero
     * textures embedded. cl.worldmodel->textures then comes back
     * empty and the next editor_compile's WAD synthesis has nothing
     * to dump. Set it explicitly so the loop closes. */
    if (edit_scene.numentities > 0)
        Entity_SetKV(&edit_scene.entities[0], "wad", "gfx/base.wad");

    /* Door brush — added to worldspawn, then wrapped into a func_door.
     * Scene_AddCubeBrush clears the selection on exit, so we must
     * re-select the new brush before calling WrapBrushesIntoEntity,
     * which requires a non-empty selection. */
    Scene_AddCubeBrush(door_mins, door_maxs, "wbrick1_5");
    Scene_SelectionAdd(0, edit_scene.entities[0].numbrushes - 1);
    Scene_WrapBrushesIntoEntity("func_door");
    door_ent_idx = edit_scene.numentities - 1;
    Editor_ApplyClassnameDefaults(&edit_scene.entities[door_ent_idx],
                                  "func_door");
    /* angle -1 = "moves up", more obviously a working door than the
     * default east slide. */
    Entity_SetKV(&edit_scene.entities[door_ent_idx], "angle", "-1");
    /* qbsp does NOT subtract entity origin from brush vertex coordinates,
     * so a non-zero origin (set by WrapBrushesIntoEntity to the brush
     * centroid) would double-offset the door: geometry is compiled at
     * absolute world coords, then the engine shifts by origin again at
     * spawn → door lands outside the room.  Brush entities require
     * origin "0 0 0" so pos1/pos2 are correct and the model renders
     * at its compiled absolute position. */
    Entity_SetKV(&edit_scene.entities[door_ent_idx], "origin", "0 0 0");

    /* Player spawn inside the room, above the floor at eye height. */
    Scene_AddPointEntity("info_player_start", spawn);

    Q_strncpy(edit_scene.mapname, mapname,
              sizeof(edit_scene.mapname) - 1);
    edit_scene.mapname[sizeof(edit_scene.mapname) - 1] = '\0';
    edit_scene.filename[0] = '\0';   /* not yet saved to disk */

    Scene_SelectionClear();
    Con_Printf("editor: built scaffold scene (mapname=%s, "
               "%d entities)\n", mapname, edit_scene.numentities);
}

static void Editor_Cmd_New_f(void)
{
    const char *name = (Cmd_Argc() >= 2) ? Cmd_Argv(1) : "test";

    /* Stop whatever's currently running so the scaffold isn't drawn
     * over a live demo. CL_Disconnect_f handles demo playback + active
     * server shutdown; cls.demonum = -1 stops the demo auto-advance
     * loop from immediately starting demo2. */
    cls.demonum = -1;
    CL_Disconnect_f();

    Editor_TexPool_Init();
    History_Clear();
    Scene_Clear();
    scaffold_build_test_room(name);

    /* Auto-compile so the user lands on a rendered playable map
     * instead of the bare console — the editor's render hooks need a
     * cl.worldmodel, which only exists after SV_SpawnServer loads a
     * .bsp. The Cbuf defers editor_compile by one frame, which gives
     * CL_Disconnect_f's state changes time to settle. M1 limit
     * applies: this consumes the session's one qbsp call, so a
     * subsequent editor_compile after edits may crash until M2's
     * globals-reset ships. */
    Cbuf_AddText("editor_compile\n");
}

static void Editor_Cmd_Open_f(void)
{
    char path[256];
    const char *name;
    if (Cmd_Argc() < 2)
    {
        Con_Printf("usage: editor_load <mapname>  (loads id1/maps/<name>.map)\n");
        Con_Printf("       editor_new [<mapname>] (build scaffold scene)\n");
        return;
    }
    name = Cmd_Argv(1);
    snprintf(path, sizeof(path), "%s/maps/%s.map", com_gamedir, name);

    if (!Scene_Load(path))
    {
        /* Fall through to the scaffold so the user has something
         * usable instead of a load error. Auto-compile + load so the
         * editor's render path has a worldmodel to draw against
         * (otherwise the user lands on the empty console with no
         * viewport). editor_save will later write a fresh <name>.map. */
        Con_Printf("editor_load: %s not found — building scaffold scene\n",
                   path);
        cls.demonum = -1;
        CL_Disconnect_f();
        History_Clear();
        Scene_Clear();
        scaffold_build_test_room(name);
        Cbuf_AddText("editor_compile\n");
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

static void Editor_Cmd_Delete_f(void)
{
    if (Scene_NumSelected() == 0)
    {
        Con_Printf("editor: nothing selected\n");
        return;
    }
    History_Push("delete");
    Scene_DeleteSelected();
}

static void Editor_Cmd_Undo_f(void)
{
    if (!History_Undo()) Con_Printf("editor: nothing to undo\n");
}

// -----------------------------------------------------------------------------
// Texture pool — persistent collection of texture_t loaded from game BSPs,
// so the texture browser and WAD synthesis have the full Quake palette even
// when the current worldmodel only has a handful of textures compiled in.
// -----------------------------------------------------------------------------

static texture_t **s_texpool       = NULL;
static int         s_texpool_count = 0;
static int         s_texpool_cap   = 0;
static int         s_texpool_inited = 0;

static int texpool_has(const char *name)
{
    int i;
    for (i = 0; i < s_texpool_count; i++)
        if (s_texpool[i] && !Q_strcasecmp(s_texpool[i]->name, name)) return 1;
    return 0;
}

static void texpool_add(texture_t *t)
{
    if (s_texpool_count >= s_texpool_cap)
    {
        int new_cap = s_texpool_cap ? s_texpool_cap * 2 : 256;
        s_texpool = (texture_t **)realloc(s_texpool,
                                          new_cap * sizeof(texture_t *));
        s_texpool_cap = new_cap;
    }
    s_texpool[s_texpool_count++] = t;
}

static void texpool_load_bsp(const char *path)
{
    int h, len, i;
    byte *data;
    dheader_t *hdr;
    dmiptexlump_t *ml;
    int texofs, texlen;

    len = COM_OpenFile((char *)path, &h);
    if (h == -1 || len < (int)sizeof(dheader_t)) return;

    data = (byte *)malloc((size_t)len);
    if (!data) { COM_CloseFile(h); return; }
    Sys_FileRead(h, data, len);
    COM_CloseFile(h);

    hdr = (dheader_t *)data;
    if (LittleLong(hdr->version) != BSPVERSION) { free(data); return; }

    texofs = LittleLong(hdr->lumps[LUMP_TEXTURES].fileofs);
    texlen = LittleLong(hdr->lumps[LUMP_TEXTURES].filelen);
    if (texofs + texlen > len || texlen < (int)sizeof(int)) { free(data); return; }

    ml = (dmiptexlump_t *)(data + texofs);
    ml->nummiptex = LittleLong(ml->nummiptex);

    for (i = 0; i < ml->nummiptex; i++)
    {
        int ofs = LittleLong(ml->dataofs[i]);
        miptex_t *mt;
        texture_t *tx;
        int pixels, j;

        if (ofs < 0 || ofs + (int)sizeof(miptex_t) > texlen) continue;
        mt = (miptex_t *)((byte *)ml + ofs);
        if (!mt->name[0]) continue;
        if (texpool_has(mt->name)) continue;

        mt->width  = LittleLong(mt->width);
        mt->height = LittleLong(mt->height);
        for (j = 0; j < MIPLEVELS; j++)
            mt->offsets[j] = LittleLong(mt->offsets[j]);

        if ((mt->width & 15) || (mt->height & 15)
         || mt->width == 0 || mt->height == 0) continue;

        pixels = (int)(mt->width * mt->height / 64 * 85);
        tx = (texture_t *)malloc(sizeof(texture_t) + (size_t)pixels);
        if (!tx) continue;
        memset(tx, 0, sizeof(texture_t));
        memcpy(tx->name, mt->name, sizeof(tx->name));
        tx->width  = mt->width;
        tx->height = mt->height;
        for (j = 0; j < MIPLEVELS; j++)
            tx->offsets[j] = mt->offsets[j]
                           + (unsigned)(sizeof(texture_t) - sizeof(miptex_t));
        memcpy(tx + 1, mt + 1, (size_t)pixels);
        texpool_add(tx);
    }
    free(data);
}

void Editor_TexPool_Init(void)
{
    static const char *maps[] = {
        "maps/start.bsp",
        "maps/e1m1.bsp", "maps/e1m2.bsp", "maps/e1m3.bsp", "maps/e1m4.bsp",
        "maps/e1m5.bsp", "maps/e1m6.bsp", "maps/e1m7.bsp",
        "maps/e2m1.bsp", "maps/e2m2.bsp", "maps/e2m3.bsp", "maps/e2m4.bsp",
        "maps/e2m5.bsp", "maps/e2m6.bsp", "maps/e2m7.bsp",
        "maps/e3m1.bsp", "maps/e3m2.bsp", "maps/e3m3.bsp", "maps/e3m4.bsp",
        "maps/e3m5.bsp", "maps/e3m6.bsp", "maps/e3m7.bsp",
        "maps/e4m1.bsp", "maps/e4m2.bsp", "maps/e4m3.bsp", "maps/e4m4.bsp",
        "maps/e4m5.bsp", "maps/e4m6.bsp", "maps/e4m7.bsp",
        NULL
    };
    int i;
    if (s_texpool_inited) return;
    s_texpool_inited = 1;
    for (i = 0; maps[i]; i++)
        texpool_load_bsp(maps[i]);
    Con_Printf("editor: texture pool: %d textures from game BSPs\n",
               s_texpool_count);
}

texture_t **Editor_TexPool_Get(int *out_count)
{
    if (out_count) *out_count = s_texpool_count;
    return s_texpool;
}

// qbsp's WriteMiptex needs <gamedir>/<wad> on disk to look up texture
// sizes for surface UV math + to embed pixel data into the .bsp. id
// never shipped gfx/base.wad in the runtime PAK — that file is the
// qbsp-time texture source, used only by level-design tools. The
// runtime engine has the same textures embedded in each .bsp's miptex
// lump, exposed through cl.worldmodel->textures[].
//
// Synthesize a WAD2 from the running worldmodel: every texture the
// engine has loaded becomes a lump. Any brush the user authored in
// the editor's texture picker references one of these (the picker
// sources from the same array), so qbsp will resolve every texture
// the .map names. Re-emitted on each editor_compile in case the
// engine has switched maps.
/* WAD2 lumpinfo type tag for "miptex". File-scope so gnu89 is happy. */
#define WAD2_TYP_MIPTEX 0x44

/* WAD2 lumpinfo_t — 32 bytes on disk. */
typedef struct wad2_lumpinfo_s {
    int  filepos;
    int  disksize;
    int  size;
    char type;
    char compression;
    char pad[2];
    char name[16];
} wad2_lumpinfo_t;

static int write_wad2_from_worldmodel(const char *out_path)
{
    FILE      *f;
    int        i, n_lumps = 0, body_size = 0;
    int        header_size = 12;
    int       *lump_size = NULL;
    int       *lump_pos  = NULL;
    texture_t **all_tex  = NULL;
    int        all_count = 0, all_cap = 0;

    /* Build a unified, deduplicated texture list: worldmodel first, then
     * any pool textures not already present. This lets qbsp resolve any
     * texture the user picks from the browser, even if it hasn't been
     * compiled into the current worldmodel yet. */
    if (cl.worldmodel && cl.worldmodel->textures)
    {
        for (i = 0; i < cl.worldmodel->numtextures; i++)
        {
            texture_t *t = cl.worldmodel->textures[i];
            if (!t || !t->name[0]) continue;
            if (all_count >= all_cap)
            {
                all_cap = all_cap ? all_cap * 2 : 64;
                all_tex = (texture_t **)realloc(all_tex,
                              (size_t)all_cap * sizeof(texture_t *));
            }
            all_tex[all_count++] = t;
        }
    }
    {
        int pool_count;
        texture_t **pool = Editor_TexPool_Get(&pool_count);
        for (i = 0; i < pool_count; i++)
        {
            texture_t *t = pool[i];
            int j, found = 0;
            if (!t || !t->name[0]) continue;
            for (j = 0; j < all_count; j++)
                if (!Q_strcasecmp(all_tex[j]->name, t->name)) { found = 1; break; }
            if (!found)
            {
                if (all_count >= all_cap)
                {
                    all_cap = all_cap ? all_cap * 2 : 64;
                    all_tex = (texture_t **)realloc(all_tex,
                                  (size_t)all_cap * sizeof(texture_t *));
                }
                all_tex[all_count++] = t;
            }
        }
    }

    if (all_count == 0)
    {
        Con_Printf("editor_compile: no textures — run editor_new first\n");
        if (all_tex) free(all_tex);
        return 1;
    }

    /* First pass: compute per-lump sizes and file positions. */
    lump_size = (int *)calloc((size_t)all_count, sizeof(int));
    lump_pos  = (int *)calloc((size_t)all_count, sizeof(int));
    for (i = 0; i < all_count; i++)
    {
        texture_t *t = all_tex[i];
        int sz = 40
               + (int)(t->width * t->height)
               + (int)((t->width >> 1) * (t->height >> 1))
               + (int)((t->width >> 2) * (t->height >> 2))
               + (int)((t->width >> 3) * (t->height >> 3));
        lump_size[i] = sz;
        lump_pos [i] = header_size + body_size;
        body_size   += sz;
        n_lumps++;
    }

    f = fopen(out_path, "wb");
    if (!f)
    {
        Con_Printf("editor_compile: can't open %s for write\n", out_path);
        free(all_tex); free(lump_size); free(lump_pos);
        return 1;
    }

    /* WAD2 header. */
    {
        char hdr[12];
        int  infotableofs = header_size + body_size;
        memcpy(hdr, "WAD2", 4);
        memcpy(hdr + 4, &n_lumps,       4);
        memcpy(hdr + 8, &infotableofs, 4);
        fwrite(hdr, 1, 12, f);
    }

    /* Body — one miptex per texture. */
    for (i = 0; i < all_count; i++)
    {
        texture_t *t = all_tex[i];
        unsigned   off[4];
        int        k;
        off[0] = 40;
        off[1] = off[0] + t->width * t->height;
        off[2] = off[1] + (t->width >> 1) * (t->height >> 1);
        off[3] = off[2] + (t->width >> 2) * (t->height >> 2);
        fwrite(t->name,    1, 16, f);
        fwrite(&t->width,  1,  4, f);
        fwrite(&t->height, 1,  4, f);
        fwrite(off,        1, 16, f);
        for (k = 0; k < 4; k++)
        {
            const byte *src  = (const byte *)t + t->offsets[k];
            int         npix = (int)((t->width >> k) * (t->height >> k));
            fwrite(src, 1, (size_t)npix, f);
        }
    }

    /* Directory. */
    for (i = 0; i < all_count; i++)
    {
        texture_t      *t  = all_tex[i];
        wad2_lumpinfo_t li;
        memset(&li, 0, sizeof(li));
        li.filepos     = lump_pos [i];
        li.disksize    = lump_size[i];
        li.size        = lump_size[i];
        li.type        = (char)WAD2_TYP_MIPTEX;
        li.compression = 0;
        memcpy(li.name, t->name, 16);
        fwrite(&li, 1, sizeof(li), f);
    }

    fclose(f);
    free(all_tex);
    free(lump_size);
    free(lump_pos);
    Con_Printf("editor_compile: wrote %s (%d textures, %d bytes)\n",
               out_path, n_lumps, header_size + body_size + n_lumps * 32);
    return 0;
}

// Compile the current edit_scene's .map through the in-process qbsp
// library, register the resulting .bsp bytes as a virtual file, and
// restart the map so the engine loads the freshly compiled geometry.
// No .bsp ever hits disk on the engine side — the .map is the source
// of truth (Scene_Save writes it first), the .bsp lives in RAM only.
//
// v1 limit: qbsp's globals are not reset between calls; running
// editor_compile a second time in the same session may crash. Quit
// and relaunch between attempts. M2 fixes this.
static void Editor_Cmd_Compile_f(void)
{
    char  map_path[256];
    char  bsp_vpath[64];
    void *bytes = NULL;
    int   size  = 0;
    int   rc;
    qbsp_options_t opts;

    if (!edit_scene.mapname[0])
    {
        Con_Printf("editor_compile: no map loaded\n");
        return;
    }

    snprintf(map_path,  sizeof(map_path),
             "%s/maps/%s.map", com_gamedir, edit_scene.mapname);
    snprintf(bsp_vpath, sizeof(bsp_vpath),
             "maps/%s.bsp", edit_scene.mapname);

    if (!Scene_Save(map_path))
    {
        Con_Printf("editor_compile: save to %s failed\n", map_path);
        return;
    }

    /* qbsp's WAD lookup uses raw fopen. id never shipped gfx/base.wad
     * in the PAK — runtime textures live in each .bsp's miptex lump.
     * Synthesize a WAD2 from the running worldmodel's textures so qbsp
     * has something to read from disk. The .map's "wad" key is a stock
     * "gfx/base.wad" that the editor's serializer emits. */
    {
        char wad_path[1024];
        snprintf(wad_path, sizeof(wad_path), "%s/gfx/base.wad", com_gamedir);
        Sys_mkdir(va("%s/gfx", com_gamedir));
        if (write_wad2_from_worldmodel(wad_path) != 0)
        {
            Con_Printf("editor_compile: WAD synthesis failed\n");
            return;
        }
    }

    Con_Printf("editor_compile: saved %s; running qbsp...\n", map_path);

    memset(&opts, 0, sizeof(opts));
    opts.gamedir = com_gamedir;
    rc = qbsp_compile_to_memory(map_path, &bytes, &size, &opts);
    if (rc != 0 || !bytes)
    {
        Con_Printf("editor_compile: qbsp failed (rc=%d)\n", rc);
        return;
    }

    Editor_VFS_Register(bsp_vpath, bytes, size);
    Con_Printf("editor_compile: %s = %d bytes (virtual); reloading\n",
               bsp_vpath, size);

    {
        char buf[160];
        snprintf(buf, sizeof(buf), "map %s\n", edit_scene.mapname);
        Cbuf_AddText(buf);
    }
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

// Center the free-fly camera on the chosen item and back off enough to
// frame it. Called by editor_ui.c when the user double-clicks a row in the
// Brushes panel — we don't change view angles, so the user's looking
// direction is preserved; only camera origin moves so the item sits
// directly ahead. In FPS mode this still updates the latched cam state, so
// switching back to free-fly puts you on the framed view.
void Editor_FrameItem(int e_idx, int b_idx)
{
    extern vec3_t vpn;
    edit_entity_t *e;
    vec3_t target = {0,0,0};
    float  diag = 64.0f;
    float  dist;
    int    i;

    if (e_idx < 0 || e_idx >= edit_scene.numentities) return;
    e = &edit_scene.entities[e_idx];

    if (b_idx < 0)
    {
        // Whole-entity ref. Use Editor_EntityAnchor so BSP-loaded brush
        // entities (func_bossgate / func_door from a .bsp without .map
        // source — numbrushes==0, no "origin" key, geometry only in
        // live_ent->v.absmin/absmax) frame correctly. Falling back to
        // Entity_GetOrigin alone teleported the camera to world origin
        // because brush entities don't have an origin key.
        if (!Editor_EntityAnchor(e, target)) return;
        // Diagonal: live edict's absmin/absmax for brush entities;
        // otherwise the friendly default for point entity items.
        if (e->live_ent && !e->live_ent->free)
        {
            const float *amn = e->live_ent->v.absmin;
            const float *amx = e->live_ent->v.absmax;
            if (amx[0] > amn[0] || amx[1] > amn[1] || amx[2] > amn[2])
            {
                int i;
                for (i = 0; i < 3; i++)
                {
                    float d = amx[i] - amn[i];
                    if (d > diag) diag = d;
                }
            }
            else diag = 96.0f;
        }
        else diag = 96.0f;
    }
    else
    {
        edit_brush_t *b;
        if (b_idx >= e->numbrushes) return;
        b = &e->brushes[b_idx];
        if (!b->valid) return;
        Editor_BrushCentroid(b, target);
        for (i = 0; i < 3; i++)
        {
            float d = b->maxs[i] - b->mins[i];
            if (d > diag) diag = d;
        }
    }

    dist = diag * 1.5f + 64.0f;
    for (i = 0; i < 3; i++)
        s_cam_origin[i] = target[i] - vpn[i] * dist;

    // Mark the cam state initialised so the next PreRender doesn't latch
    // back to the player's view and overwrite our work. Angles untouched.
    s_camera_inited = 1;
}

// Per-classname default keyvalue prefill. Heuristic — matches the most
// common spawn-function expectations so newly-placed entities are usable
// without a trip through the inspector. Anything not matched gets just
// classname + origin (Scene_AddPointEntity already wrote those).
//
// Public so editor_ui.c can call it after Scene_WrapBrushesIntoEntity —
// brush entities need an angle key the same way point movers/items do.
void Editor_ApplyClassnameDefaults(edit_entity_t *e, const char *cls)
{
    // Most facing-aware classes (monsters, items, spawn points, brush
    // movers) need an angle key; their spawn functions read v.angles[YAW]
    // and SetMovedir(e) converts it to a unit vector along world axes.
    // Special values: -1 = up, -2 = down. We default to 0 (east) which
    // matches the engine's own SetMovedir behaviour when angle is absent;
    // making it explicit means the user sees a value to edit.
    //
    // func_plat / func_train / func_door_secret intentionally omitted —
    // they don't read angle (plat is always vertical, train follows
    // path_corners, secret door has its own SECRET_1ST_LEFT spawnflag
    // for direction).
    if (!strcmp(cls, "info_player_start")
     || !strcmp(cls, "info_player_start2")
     || !strcmp(cls, "info_player_deathmatch")
     || !strcmp(cls, "info_player_coop")
     || !strcmp(cls, "info_intermission")
     || !strcmp(cls, "info_teleport_destination")
     || !strcmp(cls, "func_door")
     || !strcmp(cls, "func_button")
     || !strcmp(cls, "trigger_push")
     || !strncmp(cls, "monster_",    8)
     || !strncmp(cls, "item_",       5)
     || !strncmp(cls, "weapon_",     7)
     || !strncmp(cls, "ammo_",       5)
     || !strncmp(cls, "trap_",       5))
        Entity_SetKV(e, "angle", "0");

    // Lights default to 200 — reasonable medium intensity, dim enough not
    // to wash out a small room, bright enough not to vanish in vis.
    if (!strcmp(cls, "light")
     || !strncmp(cls, "light_", 6))
        Entity_SetKV(e, "light", "200");
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
    {
        int idx = edit_scene.numentities - 1;
        Editor_ApplyClassnameDefaults(&edit_scene.entities[idx], classname);
        Con_Printf("editor: added %s at %.0f %.0f %.0f\n",
                   classname, origin[0], origin[1], origin[2]);
    }
}

// Arm placement of `classname` for the next viewport LMB click. The toolbar
// "Add Entity..." dialog calls this when the user picks a row. Pre-existing
// pending placement is overwritten — useful for "actually I wanted a knight,
// not an ogre".
void Editor_BeginPlaceEntity(const char *classname)
{
    if (!classname || !classname[0]) { s_pending_classname[0] = '\0'; return; }
    Q_strncpy(s_pending_classname, classname, (int)sizeof(s_pending_classname) - 1);
    s_pending_classname[sizeof(s_pending_classname) - 1] = '\0';
}

void Editor_CancelPlaceEntity(void) { s_pending_classname[0] = '\0'; }
int  Editor_IsPlacementPending(void) { return s_pending_classname[0] != '\0'; }
const char *Editor_PendingClassname(void) { return s_pending_classname; }

// Hit point for cursor placement. If the ray misses everything, falls back
// to 128 units in front of the camera (snapped). Origin is grid-snapped on
// success too — keeps newly-placed entities aligned with the rest of the
// scene without making the user fiddle with snap toggles per-spawn.
static void resolve_place_origin(float vx, float vy, vec3_t out)
{
    vec3_t hit, nrm;
    float  grid = editor_grid_snap.value ? editor_grid_size.value : 0.0f;
    int    i;
    if (Editor_RaycastForPlacement(vx, vy, hit, nrm))
    {
        // Offset slightly along the surface normal so the new entity
        // doesn't z-fight or end up half-inside the wall.
        for (i = 0; i < 3; i++) out[i] = hit[i] + nrm[i] * 16.0f;
    }
    else
    {
        compute_camera_focal(out);
        return;     // already snapped by compute_camera_focal
    }
    if (grid > 0.0f)
        for (i = 0; i < 3; i++)
            out[i] = floorf(out[i] / grid + 0.5f) * grid;
}

// Spawn the pending entity at the cursor and arm the place-drag state so
// the user keeps dragging it until LMB-up. Called from the mouse-down
// handler when s_pending_classname is non-empty.
static void place_pending_at_cursor(float vx, float vy)
{
    vec3_t origin;
    int idx;
    char cls[64];
    Q_strncpy(cls, s_pending_classname, (int)sizeof(cls) - 1);
    cls[sizeof(cls) - 1] = '\0';
    s_pending_classname[0] = '\0';

    resolve_place_origin(vx, vy, origin);
    History_Push("place entity");
    if (!Scene_AddPointEntity(cls, origin)) return;
    idx = edit_scene.numentities - 1;
    Editor_ApplyClassnameDefaults(&edit_scene.entities[idx], cls);
    s_placing_drag    = 1;
    s_placing_ent_idx = idx;
}

// Track-cursor update during the place-drag. Snaps to grid the same way
// resolve_place_origin does so the drag feels like the gizmo's translate
// drag — no surprise un-snap on release.
static void place_drag_update(float vx, float vy)
{
    edit_entity_t *e;
    vec3_t origin;
    if (!s_placing_drag) return;
    if (s_placing_ent_idx < 0
     || s_placing_ent_idx >= edit_scene.numentities)
    {
        s_placing_drag = 0;
        s_placing_ent_idx = -1;
        return;
    }
    resolve_place_origin(vx, vy, origin);
    e = &edit_scene.entities[s_placing_ent_idx];
    {
        char buf[96];
        snprintf(buf, sizeof(buf), "%g %g %g", origin[0], origin[1], origin[2]);
        Entity_SetKV(e, "origin", buf);
    }
}

// Spawn a 64-unit cube at the camera focal point. New brush is appended to
// worldspawn and becomes the current selection.
static void Editor_Cmd_AddCube_f(void)
{
    vec3_t center, mins, maxs;
    int i;
    const char *tex;
    const float HALF = 32.0f;       // 64-unit cube
    compute_camera_focal(center);
    for (i = 0; i < 3; i++)
    {
        mins[i] = center[i] - HALF;
        maxs[i] = center[i] + HALF;
    }
    tex = editor_brush_tex.string;
    if (!tex || !tex[0]) tex = NULL;     // Scene_AddCubeBrush picks default
    History_Push("add cube");
    if (Scene_AddCubeBrush(mins, maxs, tex))
        Con_Printf("editor: added cube at %.0f %.0f %.0f tex='%s'\n",
                   center[0], center[1], center[2],
                   tex ? tex : "(default)");
}

// Convert the primary-selected brush into a hollow room (6 wall slabs with
// thickness `editor_brush_hollow_thickness`). Source brush is replaced; new
// walls become the selection.
static void Editor_Cmd_Hollow_f(void)
{
    int e_idx, b_idx;
    float thickness;
    extern cvar_t editor_brush_hollow_thickness;
    if (Scene_NumSelected() == 0
     || !Scene_GetSelected(Scene_NumSelected() - 1, &e_idx, &b_idx))
    {
        Con_Printf("editor: hollow: nothing selected\n");
        return;
    }
    if (b_idx < 0)
    {
        Con_Printf("editor: hollow: select a brush, not an entity\n");
        return;
    }
    thickness = editor_brush_hollow_thickness.value;
    History_Push("hollow brush");
    if (Scene_HollowBrush(e_idx, b_idx, thickness))
        Con_Printf("editor: hollowed brush (thickness %g)\n", thickness);
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
// editor_load, edit_scene is empty and they see no entities. We populate by
// parsing the BSP's embedded entity string (sv.worldmodel->entities) — that
// gives us the original .map kv lists for *every* entity, including lights
// that the spawn function freed (no-targetname static lights are deliberately
// dropped at server start) and brush entities (doors/buttons) whose actual
// brushmodel lives in the BSP. The post-parse linker in map_io.c matches each
// entity to its live sv.edicts entry by classname + nearest origin so live_ent
// is set where available.

static void Editor_PopulateFromServer(void);

// Detect mid-session map changes (trigger_changelevel, +map, etc.) and react.
// Live edict pointers from the previous map are now garbage — at minimum we
// have to drop them and clear the selection. For a server-populated scene
// (no .map filename — we built it from sv.edicts originally) we go further
// and rebuild the whole scene from the new world.
static void editor_check_map_change(void)
{
    if (!sv.active) return;
    if (strcmp(edit_scene.mapname, sv.name) == 0
        && edit_scene.numentities > 0) return;       // same map, scene live

    if (edit_scene.filename[0])
    {
        // User-loaded scene (editor_load): keep their entities AND
        // their mapname (the .map the user explicitly loaded is their
        // authoring target — even when the engine is running a
        // different .bsp). Just null the live_ent / live_static
        // pointers since they reference the old sv.edicts /
        // cl_static_entities which has been reused.
        int i;
        Scene_SelectionClear();
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_scene.entities[i].live_ent    = NULL;
            edit_scene.entities[i].live_static = NULL;
        }
        Con_Printf("editor: engine map changed to %s; "
                   "cleared selection + live links "
                   "(authoring target stays %s)\n",
                   sv.name, edit_scene.mapname[0] ? edit_scene.mapname : "(none)");
        return;
    }

    // Server-populated mode (or empty scene): rebuild from the live edicts.
    Editor_PopulateFromServer();
}

static void Editor_PopulateFromServer(void)
{
    if (!sv.active || !sv.worldmodel || !sv.worldmodel->entities) return;
    History_Clear();

    // parse_scene_text inside Scene_DeserializeText calls Scene_Clear, marks
    // every parsed entity spawned=1, and runs the live-edict matcher (so
    // brush entities' live_ent points at the func_door edict whose absmin/
    // absmax we use for picking).
    if (!Scene_DeserializeText(sv.worldmodel->entities))
    {
        Con_Printf("editor: failed to parse worldmodel entities\n");
        return;
    }

    // mapname for Restart map + a future explicit editor_save. Filename
    // stays empty so close-time auto-save is a no-op — overwriting an
    // existing id1/maps/<name>.map source file silently would destroy work.
    if (sv.name[0])
    {
        size_t l = strlen(sv.name);
        if (l >= sizeof(edit_scene.mapname)) l = sizeof(edit_scene.mapname) - 1;
        memcpy(edit_scene.mapname, sv.name, l);
        edit_scene.mapname[l] = '\0';
    }
    edit_scene.filename[0] = '\0';

    Con_Printf("editor: populated %d entities from worldmodel (%s)\n",
               edit_scene.numentities,
               edit_scene.mapname[0] ? edit_scene.mapname : "(no name)");
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
    Cmd_AddCommand("editor_new",    Editor_Cmd_New_f);
    Cmd_AddCommand("editor_save",   Editor_Cmd_Save_f);
    Cmd_AddCommand("editor_revert", Editor_Cmd_Revert_f);
    Cmd_AddCommand("editor_status", Editor_Cmd_Status_f);
    Cmd_AddCommand("editor_textures", Editor_Cmd_Textures_f);
    Cmd_AddCommand("editor_brush_add_cube", Editor_Cmd_AddCube_f);
    Cmd_AddCommand("editor_brush_hollow",   Editor_Cmd_Hollow_f);
    Cmd_AddCommand("editor_entity_add",     Editor_Cmd_AddEntity_f);
    Cmd_AddCommand("editor_group",   Editor_Cmd_Group_f);
    Cmd_AddCommand("editor_ungroup", Editor_Cmd_Ungroup_f);
    Cmd_AddCommand("editor_delete",  Editor_Cmd_Delete_f);
    Cmd_AddCommand("editor_undo",    Editor_Cmd_Undo_f);
    Cmd_AddCommand("editor_redo",    Editor_Cmd_Redo_f);
    Cmd_AddCommand("editor_compile", Editor_Cmd_Compile_f);

    History_Init();

    Cvar_RegisterVariable(&editor_camera);
    Cvar_RegisterVariable(&editor_grid_snap);
    Cvar_RegisterVariable(&editor_grid_size);
    Cvar_RegisterVariable(&editor_grid_absolute);
    Cvar_RegisterVariable(&editor_rotate_snap);
    Cvar_RegisterVariable(&editor_rotate_snap_size);
    Cvar_RegisterVariable(&editor_rotate_snap_absolute);
    Cvar_RegisterVariable(&editor_view_mode);
    Cvar_RegisterVariable(&editor_show_angles);
    Cvar_RegisterVariable(&editor_show_links);
    Cvar_RegisterVariable(&editor_face_mode);
    Cvar_RegisterVariable(&editor_brush_tex);
    Cvar_RegisterVariable(&editor_brush_hollow_thickness);

    {
        extern void Editor_RegisterCvars(void);
        Editor_RegisterCvars();
    }

    s_inited = 1;
}

void Editor_Shutdown(void)
{
    extern void Editor_TexCache_Shutdown(void);
    if (!s_inited) return;
    Editor_TexCache_Shutdown();
    Editor_VFS_Shutdown();
    Scene_Shutdown();
    s_inited = 0;
}

// -----------------------------------------------------------------------------
// Toggle
// -----------------------------------------------------------------------------

void Editor_Toggle(void)
{
    if (!s_inited) return;

    // Opening the editor: refresh edit_scene against the live server.
    // Server-populated scenes get rebuilt against new sv.edicts when the
    // map changed; user-loaded scenes drop their live_ent pointers so a
    // post-map-change drag can't move a random edict. Then sync the UI's
    // per-session filter state from cvars (difficulty preview ← skill).
    if (!s_open)
    {
        editor_check_map_change();
        Editor_UI_OnOpen();
    }

    // Closing the editor: spawn any newly-placed point entities into the
    // live server so the player can actually touch them, then auto-save
    // the .map. Order matters — flushing first means the save reflects
    // post-spawn kv defaults (e.g. spawn functions sometimes back-fill
    // spawnflags), keeping the file authoritative.
    if (s_open)
    {
        // Drop any pending placement so reopening doesn't surprise-spawn
        // on the next viewport click.
        s_pending_classname[0] = '\0';
        s_placing_drag    = 0;
        s_placing_ent_idx = -1;
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

    // SCR_CalcRefdef inspects Editor_IsOpen() to expand scr_vrect to
    // fullscreen (sb_lines = 0) while we're open, so the world view
    // overdraws the HUD strip and bbox lines don't smear on stale Sbar
    // pixels. Without this poke the cached sb_lines / scr_vrect from
    // before the toggle stays in effect until viewsize/fov changes.
    vid.recalc_refdef = 1;

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

    // Mid-session map change while the editor is open (e.g. trigger_changelevel
    // tripped, or the user typed `map foo` in the console) — refresh before
    // the gizmo / selection touch any stale state.
    editor_check_map_change();

    // Push fake editor preview entities into cl_visedicts so the engine
    // renders alias + brushmodel previews via its normal entity pipeline.
    // Must happen after CL_RelinkEntities reset the list (which is in
    // host.c earlier in the frame) and before R_EdgeDrawing dispatches
    // brushmodel entries.
    Editor_PushPreviewEntities();

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
        // Pending entity placement intercepts the click before gizmo/pick.
        // The same mouse-down arms a follow-the-cursor drag so the user can
        // refine the position in one motion.
        if (s_pending_classname[0])
        {
            place_pending_at_cursor(vx, vy);
            return 1;
        }
        // Try the gizmo first; if it doesn't grab an axis, treat as a pick.
        if (Editor_GizmoMouseDown(vx, vy)) return 1;
        {
            extern cvar_t editor_face_mode;
            int e_idx, b_idx, plane_idx;
            SDL_Keymod mod = SDL_GetModState();
            int shift = (mod & SDL_KMOD_SHIFT) != 0;
            int face_mode = editor_face_mode.value != 0.0f;
            if (Editor_PickAt(vx, vy, &e_idx, &b_idx, &plane_idx))
            {
                if (face_mode && b_idx >= 0 && plane_idx >= 0)
                {
                    // Face mode replaces selection with the picked brush
                    // and pins active_face to the clicked face.
                    // Shift is intentionally ignored — multi-face select
                    // is out of scope.
                    Scene_SelectionClear();
                    Scene_SelectionAdd(e_idx, b_idx);
                    Scene_SetActiveFace(e_idx, b_idx, plane_idx);
                }
                else if (shift)
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
        if (ev->button.button == SDL_BUTTON_LEFT && s_placing_drag)
        {
            // Drag finishes implicitly — origin already updated in the last
            // motion event. History was pushed at place time, so the spawn
            // + drag is one undo step.
            s_placing_drag    = 0;
            s_placing_ent_idx = -1;
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
        if (s_placing_drag)
        {
            float vx, vy;
            window_to_vid(ev->motion.x, ev->motion.y, &vx, &vy);
            place_drag_update(vx, vy);
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
    case SDL_EVENT_KEY_DOWN:
        // ESC cancels pending placement (or an in-progress place-drag, in
        // which case we also wipe the just-placed entity via undo).
        if (ev->key.scancode == SDL_SCANCODE_ESCAPE)
        {
            if (s_placing_drag)
            {
                s_placing_drag    = 0;
                s_placing_ent_idx = -1;
                History_Undo();
                return 1;
            }
            if (s_pending_classname[0])
            {
                s_pending_classname[0] = '\0';
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

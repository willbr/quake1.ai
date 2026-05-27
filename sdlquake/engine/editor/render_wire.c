// render_wire.c -- M1 wireframe pass + screen<->world projection helpers.
//
// Project each face's vertex loop, clip line segments at the near plane,
// Bresenham-draw 8-bit lines into vid.buffer. No z-buffer, no occlusion;
// editor brushes overdraw the world (intentional — they need to be visible
// when authoring inside an existing room).
//
// Also exposes Editor_ScreenToRay used by picking + gizmo.

#include "quakedef.h"
#include "r_local.h"
#include "edit_scene.h"
#include "editor.h"
#include "editor_internal.h"

#include <math.h>

// Engine projection state (defined in r_main.c / r_misc.c).
extern float        xcenter, ycenter;
extern float        xscale, yscale;
extern vec3_t       vpn, vright, vup;
extern vec3_t       r_origin;
extern short       *d_pzbuffer;
extern unsigned int d_zwidth;

// -----------------------------------------------------------------------------
// Projection
// -----------------------------------------------------------------------------

// World point -> view space (X right, Y up, Z forward).
static void world_to_view(const vec3_t world, vec3_t view)
{
    vec3_t local;
    VectorSubtract(world, r_origin, local);
    view[0] = DotProduct(local, vright);
    view[1] = DotProduct(local, vup);
    view[2] = DotProduct(local, vpn);
}

// View -> screen. Returns 1 if in front of near plane.
static int view_to_screen(const vec3_t view, float *sx, float *sy)
{
    float z = view[2];
    if (z < NEAR_CLIP) return 0;
    *sx = xcenter + (xscale / z) * view[0];
    *sy = ycenter - (yscale / z) * view[1];
    return 1;
}

int Editor_ProjectWorld(const vec3_t world, float *out_sx, float *out_sy)
{
    vec3_t view;
    world_to_view(world, view);
    return view_to_screen(view, out_sx, out_sy);
}

// Build a world-space ray from a screen pixel.
void Editor_ScreenToRay(float sx, float sy, vec3_t out_origin, vec3_t out_dir)
{
    float ux = (sx - xcenter) / xscale;
    float uy = -(sy - ycenter) / yscale;
    int i;
    for (i = 0; i < 3; i++)
        out_dir[i] = ux * vright[i] + uy * vup[i] + vpn[i];
    {
        float l = sqrtf(out_dir[0]*out_dir[0] + out_dir[1]*out_dir[1] + out_dir[2]*out_dir[2]);
        if (l > 1e-6f) { out_dir[0] /= l; out_dir[1] /= l; out_dir[2] /= l; }
    }
    VectorCopy(r_origin, out_origin);
}

// -----------------------------------------------------------------------------
// Line drawing
// -----------------------------------------------------------------------------

// Bresenham line into vid.buffer with optional per-pixel depth test against
// d_pzbuffer. iz0/iz1 are the endpoint inv_z values; we interpolate linearly
// across the (1 + max(|dx|,|dy|)) Bresenham steps. When ztest is 0 the line
// is drawn unconditionally (and doesn't disturb the z-buffer).
static void draw_line8(int x0, int y0, float iz0,
                       int x1, int y1, float iz1,
                       byte color, int ztest)
{
    int W = (int)vid.width, H = (int)vid.height;
    int dx, dy, sx, sy, err, e2;
    int adx, ady, steps;
    float iz, di;
    byte *base = vid.buffer;
    int  rb   = (int)vid.rowbytes;

    if ((x0 < 0 && x1 < 0) || (x0 >= W && x1 >= W) ||
        (y0 < 0 && y1 < 0) || (y0 >= H && y1 >= H))
        return;

    adx = abs(x1 - x0);
    ady = abs(y1 - y0);
    dx = adx;
    dy = -ady;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;
    steps = adx > ady ? adx : ady;
    iz = iz0;
    di = steps > 0 ? (iz1 - iz0) / (float)steps : 0.0f;

    for (;;)
    {
        if ((unsigned)x0 < (unsigned)W && (unsigned)y0 < (unsigned)H)
        {
            if (ztest)
            {
                int izi = (int)(iz * 32768.0f);
                short *zp = d_pzbuffer + y0 * d_zwidth + x0;
                // Wireframe wins ties against opaque surfaces sharing the
                // same plane (a brush face you've outlined draws right on
                // top of its own fill). "<=" lets equal depth still draw.
                if (*zp <= izi)
                {
                    base[y0 * rb + x0] = color;
                    *zp = (short)izi;
                }
            }
            else
            {
                base[y0 * rb + x0] = color;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        iz += di;
    }
}

// Internal worker — both Editor_DrawLine3D and Editor_DrawLine3DOver share
// this clip + project. ztest is forwarded to draw_line8.
static void draw_line3d(const vec3_t a_world, const vec3_t b_world,
                        byte color, int ztest)
{
    vec3_t va, vb;
    float sx0, sy0, sx1, sy1;
    world_to_view(a_world, va);
    world_to_view(b_world, vb);

    if (va[2] < NEAR_CLIP && vb[2] < NEAR_CLIP) return;

    if (va[2] < NEAR_CLIP)
    {
        float t = (NEAR_CLIP - va[2]) / (vb[2] - va[2]);
        va[0] = va[0] + (vb[0] - va[0]) * t;
        va[1] = va[1] + (vb[1] - va[1]) * t;
        va[2] = NEAR_CLIP;
    }
    else if (vb[2] < NEAR_CLIP)
    {
        float t = (NEAR_CLIP - vb[2]) / (va[2] - vb[2]);
        vb[0] = vb[0] + (va[0] - vb[0]) * t;
        vb[1] = vb[1] + (va[1] - vb[1]) * t;
        vb[2] = NEAR_CLIP;
    }

    // Both endpoints are at z >= NEAR_CLIP after the clip above. Project
    // inline rather than going through view_to_screen — the engine's
    // NEAR_CLIP is the double literal 0.01, and assigning it to a float
    // (va[2] = NEAR_CLIP) rounds down to ~0.0099999977. view_to_screen's
    // `z < NEAR_CLIP` then promotes that float back to double, which IS
    // less than 0.01, so a freshly-clipped endpoint fails the test and
    // the entire line gets culled. Long target/path arrows with one end
    // behind the camera disappeared entirely because of this.
    {
        float iz0 = 1.0f / va[2];
        float iz1 = 1.0f / vb[2];
        float diz;
        int   i2;
        float t0, t1, p2, q2, r2;
        float ps[4], qs[4];
        float W, H;
        sx0 = xcenter + xscale * va[0] * iz0;
        sy0 = ycenter - yscale * va[1] * iz0;
        sx1 = xcenter + xscale * vb[0] * iz1;
        sy1 = ycenter - yscale * vb[1] * iz1;

        // Liang-Barsky 2-D screen clip. Near-plane clipping can leave a
        // vertex with va[2] == NEAR_CLIP (0.01), giving iz == 100 and a
        // screen X/Y millions of pixels off-screen. draw_line8's trivial
        // reject only catches the case where *both* endpoints are on the
        // same side; one on-screen + one at ±2M causes ~2M Bresenham
        // iterations (drawing nothing). Clip to screen rect first.
        W = (float)vid.width  - 1.0f;
        H = (float)vid.height - 1.0f;
        diz = iz1 - iz0;
        {
            float dx = sx1 - sx0, dy = sy1 - sy0;
            ps[0] = -dx; qs[0] = sx0;
            ps[1] =  dx; qs[1] = W - sx0;
            ps[2] = -dy; qs[2] = sy0;
            ps[3] =  dy; qs[3] = H - sy0;
        }
        t0 = 0.0f; t1 = 1.0f;
        for (i2 = 0; i2 < 4; i2++) {
            p2 = ps[i2]; q2 = qs[i2];
            if (p2 == 0.0f) { if (q2 < 0.0f) return; continue; }
            r2 = q2 / p2;
            if (p2 < 0.0f) { if (r2 > t1) return; if (r2 > t0) t0 = r2; }
            else            { if (r2 < t0) return; if (r2 < t1) t1 = r2; }
        }
        if (t0 > t1) return;
        /* Apply in terms of the *original* dx/dy so t0 and t1 are both
         * relative to the same baseline — adjust t1 (far end) before t0
         * (near end) so the t0 delta still uses the original span. */
        {
            float odx = sx1 - sx0, ody = sy1 - sy0;
            if (t1 < 1.0f) { sx1 = sx0 + t1*odx; sy1 = sy0 + t1*ody; iz1 = iz0 + t1*diz; }
            if (t0 > 0.0f) { sx0 += t0*odx;       sy0 += t0*ody;       iz0 += t0*diz;     }
        }

        draw_line8((int)(sx0 + 0.5f), (int)(sy0 + 0.5f), iz0,
                   (int)(sx1 + 0.5f), (int)(sy1 + 0.5f), iz1, color, ztest);
    }
}

void Editor_DrawLine3D(const vec3_t a, const vec3_t b, byte color)
{
    draw_line3d(a, b, color, 1);
}

void Editor_DrawLine3DOver(const vec3_t a, const vec3_t b, byte color)
{
    draw_line3d(a, b, color, 0);
}

// -----------------------------------------------------------------------------
// Cull + draw a brush
// -----------------------------------------------------------------------------

// Cheap reject: every face vertex behind near plane => skip. Not a true frustum
// reject (left/right/top/bottom planes not tested), but good enough for M1 —
// off-screen lines are clipped by draw_line8.
static int brush_visible(const edit_brush_t *b)
{
    int i, k;
    for (i = 0; i < b->numfaces; i++)
    {
        for (k = 0; k < b->faces[i].numverts; k++)
        {
            vec3_t v;
            world_to_view(b->faces[i].verts[k], v);
            if (v[2] >= NEAR_CLIP) return 1;
        }
    }
    return 0;
}

static void draw_brush(const edit_brush_t *b, byte color, int through)
{
    int i, k;
    for (i = 0; i < b->numfaces; i++)
    {
        const edit_face_t *f = &b->faces[i];
        for (k = 0; k < f->numverts; k++)
        {
            const float *a = f->verts[k];
            const float *bb = f->verts[(k + 1) % f->numverts];
            vec3_t aw, bw;
            VectorCopy(a, aw); VectorCopy(bb, bw);
            if (through) Editor_DrawLine3DOver(aw, bw, color);
            else         Editor_DrawLine3D    (aw, bw, color);
        }
    }
}

// -----------------------------------------------------------------------------
// Public entry point — called from r_main.c R_RenderView_
// -----------------------------------------------------------------------------

#define EDIT_COLOR_BRUSH        15      // off-white      (235,235,235)
#define EDIT_COLOR_SELECTED     192     // bright yellow  (255,243, 27)

// Classify by classname for bbox color + filter checkboxes. Single source
// of truth — classname_color and the UI's category-skip logic both index
// into this.
int Editor_EntityCategory(const edit_entity_t *e)
{
    const char *cls;
    if (!e || e->classname_idx < 0) return EDIT_CAT_OTHER;
    cls = e->kv[e->classname_idx].value;
    if (!cls) return EDIT_CAT_OTHER;
    if (!strncmp(cls, "trigger_", 8))                return EDIT_CAT_TRIGGER;
    if (!strncmp(cls, "info_player_", 12))           return EDIT_CAT_SPAWN;
    if (!strcmp (cls, "info_teleport_destination")) return EDIT_CAT_SPAWN;
    // info_intermission is the post-level camera — metadata, not a
    // gameplay spawn point. Group with the rest of the info_* metadata
    // so the "info" filter checkbox actually hides it.
    if (!strncmp(cls, "info_",    5))                return EDIT_CAT_INFO;
    if (!strncmp(cls, "light",    5))                return EDIT_CAT_LIGHT;
    if (!strncmp(cls, "monster_", 8))                return EDIT_CAT_MONSTER;
    if (!strncmp(cls, "item_",    5))                return EDIT_CAT_ITEM;
    if (!strncmp(cls, "weapon_",  7))                return EDIT_CAT_ITEM;
    if (!strncmp(cls, "ammo_",    5))                return EDIT_CAT_ITEM;
    if (!strncmp(cls, "func_",    5))                return EDIT_CAT_FUNC;
    if (!strncmp(cls, "ambient_", 8))                return EDIT_CAT_SOUND;
    if (!strncmp(cls, "path_",    5))                return EDIT_CAT_PATH;
    if (!strncmp(cls, "misc_",    5))                return EDIT_CAT_MISC;
    return EDIT_CAT_OTHER;
}

// Pick the bbox color for an entity by category.
static byte category_color(const edit_entity_t *e)
{
    switch (Editor_EntityCategory(e))
    {
        case EDIT_CAT_TRIGGER: return EDIT_COLOR_TRIGGER;
        case EDIT_CAT_LIGHT:   return EDIT_COLOR_LIGHT;
        case EDIT_CAT_SPAWN:   return EDIT_COLOR_SPAWN;
        case EDIT_CAT_ITEM:    return EDIT_COLOR_ITEM;
        case EDIT_CAT_MONSTER: return EDIT_COLOR_MONSTER;
        case EDIT_CAT_FUNC:    return EDIT_COLOR_FUNC;
        case EDIT_CAT_SOUND:   return EDIT_COLOR_SOUND;
        case EDIT_CAT_PATH:    return EDIT_COLOR_PATH;
        case EDIT_CAT_MISC:    return EDIT_COLOR_MISC;
        case EDIT_CAT_INFO:    return EDIT_COLOR_INFO;
        default:               return EDIT_COLOR_DEFAULT;
    }
}

// Render styles: index into editor_render_style cvar.
enum {
    EDIT_STYLE_WIRE = 0,
    EDIT_STYLE_FLAT = 1,
    EDIT_STYLE_FLAT_WIRE = 2,
    EDIT_STYLE_TEX  = 3,
    EDIT_STYLE_TEX_WIRE = 4,
    EDIT_STYLE_COUNT
};

cvar_t editor_render_style = { "editor_render_style", "3" };

// Override for trigger_* brush entities: 0 = single AABB (the readable
// authoring view — uncluttered red box per volume), 1 = textured brushes
// (shows the actual brush geometry with whatever texture the .map source
// has, useful when sizing a trigger against world geometry). Applies to
// trigger entities only; non-trigger brushes always follow
// editor_render_style.
cvar_t editor_trigger_render = { "editor_trigger_render", "0" };

// Same idea but per-brush, for clip brushes (brushes with all faces using
// the "clip" texture — invisible-in-game collision volumes). 0 = AABB per
// brush in teal, 1 = textured (procedural-grid fallback since the BSP has
// no "clip" miptex). Independent of editor_trigger_render so the user can
// reveal one without the other.
cvar_t editor_clip_render    = { "editor_clip_render",    "0" };

void Editor_RegisterCvars(void)
{
    Cvar_RegisterVariable(&editor_render_style);
    Cvar_RegisterVariable(&editor_trigger_render);
    Cvar_RegisterVariable(&editor_clip_render);
}

// True if every plane on this brush uses the "clip" texture (case-insensitive
// prefix match — catches "clip", "Clip", "clip2", etc.). A clip brush has no
// rendered geometry in vanilla Quake (qbsp strips clip faces from the visible
// BSP, keeping them only in the clip hull), so the editor visualisation is
// the only way to see / move them.
static int brush_is_clip(const edit_brush_t *b)
{
    int i;
    if (!b || b->numplanes <= 0) return 0;
    for (i = 0; i < b->numplanes; i++)
    {
        const char *t = b->planes[i].texname;
        if (!t || !t[0]) return 0;
        if (Q_strncasecmp(t, "clip", 4) != 0) return 0;
    }
    return 1;
}

// Draw the 12 edges of an AABB. `through` = 1 → depth-bypass overlay (read
// through walls); 0 → depth-tested (occluded by world geometry like normal).
static void draw_aabb_ex(const vec3_t mins, const vec3_t maxs,
                         byte color, int through)
{
    vec3_t c[8];
    int i;
    static const int edges[12][2] = {
        {0,1},{1,3},{3,2},{2,0},      // bottom rectangle
        {4,5},{5,7},{7,6},{6,4},      // top rectangle
        {0,4},{1,5},{2,6},{3,7}       // verticals
    };
    for (i = 0; i < 8; i++)
    {
        c[i][0] = (i & 1) ? maxs[0] : mins[0];
        c[i][1] = (i & 2) ? maxs[1] : mins[1];
        c[i][2] = (i & 4) ? maxs[2] : mins[2];
    }
    for (i = 0; i < 12; i++)
    {
        if (through) Editor_DrawLine3DOver(c[edges[i][0]], c[edges[i][1]], color);
        else         Editor_DrawLine3D    (c[edges[i][0]], c[edges[i][1]], color);
    }
}

static void draw_aabb_over(const vec3_t mins, const vec3_t maxs, byte color)
{
    draw_aabb_ex(mins, maxs, color, 1);
}

// Per-classname info: model path + entity-local bbox. Bbox values mirror
// SV_SetSize calls in sdlquake/game so the editor's wire box wraps the
// actual model in the same way the runtime entity will. Anything missing
// from the table falls back to a centered ±16 cube.
typedef struct {
    const char *classname;
    const char *modelpath;      // NULL => no model preview, just bbox
    vec3_t      mins, maxs;     // entity-local
} edit_class_info_t;

// Hull-2 (large monsters): mirrors VEC_HULL2_* in the engine.
#define BBM2_MIN { -32, -32, -24 }
#define BBM2_MAX {  32,  32,  64 }
// Player / hull-1 default.
#define BB1_MIN  { -16, -16, -24 }
#define BB1_MAX  {  16,  16,  32 }
// Soldier-class monsters.
#define BBM1_MIN { -16, -16, -24 }
#define BBM1_MAX {  16,  16,  40 }
// Weapons / armor (origin at base).
#define BBW_MIN  { -16, -16,   0 }
#define BBW_MAX  {  16,  16,  56 }
// Default unknown class.
#define BBDEF_MIN { -16, -16, -16 }
#define BBDEF_MAX {  16,  16,  16 }

static const edit_class_info_t s_class_info[] = {
    {"info_player_start",       "progs/player.mdl",  BB1_MIN,  BB1_MAX},
    {"info_player_start2",      "progs/player.mdl",  BB1_MIN,  BB1_MAX},
    {"info_player_coop",        "progs/player.mdl",  BB1_MIN,  BB1_MAX},
    {"info_player_deathmatch",  "progs/player.mdl",  BB1_MIN,  BB1_MAX},
    {"info_teleport_destination","progs/player.mdl", BB1_MIN,  BB1_MAX},
    {"monster_army",            "progs/soldier.mdl", BBM1_MIN, BBM1_MAX},
    {"monster_dog",             "progs/dog.mdl",     {-32,-32,-24}, {32,32,40}},
    {"monster_ogre",            "progs/ogre.mdl",    BBM2_MIN, BBM2_MAX},
    {"monster_demon1",          "progs/demon.mdl",   BBM2_MIN, BBM2_MAX},
    {"monster_shambler",        "progs/shambler.mdl",BBM2_MIN, BBM2_MAX},
    {"monster_knight",          "progs/knight.mdl",  BBM1_MIN, BBM1_MAX},
    {"monster_wizard",          "progs/wizard.mdl",  BBM1_MIN, BBM1_MAX},
    {"monster_zombie",          "progs/zombie.mdl",  BBM1_MIN, BBM1_MAX},
    {"monster_enforcer",        "progs/enforcer.mdl",BBM1_MIN, BBM1_MAX},
    {"monster_hell_knight",     "progs/hknight.mdl", BBM1_MIN, BBM1_MAX},
    {"monster_fish",            "progs/fish.mdl",    {-16,-16,-24}, {16,16,24}},
    {"monster_boss",            "progs/boss.mdl",    {-128,-128,-24}, {128,128,256}},
    {"weapon_supershotgun",     "progs/g_shot.mdl",  BBW_MIN, BBW_MAX},
    {"weapon_nailgun",          "progs/g_nail.mdl",  BBW_MIN, BBW_MAX},
    {"weapon_supernailgun",     "progs/g_nail2.mdl", BBW_MIN, BBW_MAX},
    {"weapon_grenadelauncher",  "progs/g_rock.mdl",  BBW_MIN, BBW_MAX},
    {"weapon_rocketlauncher",   "progs/g_rock2.mdl", BBW_MIN, BBW_MAX},
    {"weapon_lightning",        "progs/g_light.mdl", BBW_MIN, BBW_MAX},
    {"item_armor1",             "progs/armor.mdl",   BBW_MIN, BBW_MAX},
    {"item_armor2",             "progs/armor.mdl",   BBW_MIN, BBW_MAX},
    {"item_armorInv",           "progs/armor.mdl",   BBW_MIN, BBW_MAX},
    {"item_artifact_super_damage",   "progs/quaddama.mdl", BB1_MIN, BB1_MAX},
    {"item_artifact_invisibility",   "progs/invisibl.mdl", BB1_MIN, BB1_MAX},
    {"item_artifact_invulnerability","progs/invulner.mdl", BB1_MIN, BB1_MAX},
    {"item_artifact_envirosuit",     "progs/suit.mdl",     BB1_MIN, BB1_MAX},
    {"item_torch_small_walltorch",   "progs/flame.mdl",    {-8,-8,-8}, {8,8,8}},
    // Flame torches (light_flame_*, light_torch_*) all SV_MakeStatic, so
    // their runtime presence lives in cl_static_entities[]. Editor preview
    // model + bbox lookup keys off these; the post-load matcher in
    // map_io.c uses the modelpath here to bind each edit_entity to its
    // static-entity counterpart so gizmo drags actually move the flame.
    {"light_torch_small_walltorch",  "progs/flame.mdl",    {-8,-8,-8}, {8,8,8}},
    {"light_flame_large_yellow",     "progs/flame2.mdl",   {-8,-8,-8}, {8,8,8}},
    {"light_flame_small_yellow",     "progs/flame2.mdl",   {-8,-8,-8}, {8,8,8}},
    {"light_flame_small_white",      "progs/flame2.mdl",   {-8,-8,-8}, {8,8,8}},
    // .bsp ammo / health boxes. Spawn functions pick a variant by spawn
    // flag (small vs big); we use the small/medium default — the user
    // can always set spawnflags 1 for the alternate at game time, the
    // editor preview is just an at-a-glance "what is this".
    {"item_health",             "maps/b_bh25.bsp",     {0,0,0},  {32,32,56}},
    {"item_shells",             "maps/b_shell0.bsp",   {0,0,0},  {32,32,56}},
    {"item_spikes",             "maps/b_nail0.bsp",    {0,0,0},  {32,32,56}},
    {"item_rockets",            "maps/b_rock0.bsp",    {0,0,0},  {32,32,56}},
    {"item_cells",              "maps/b_batt0.bsp",    {0,0,0},  {32,32,56}},
    // End-game runes.
    {"item_sigil",              "progs/end1.mdl",      {-16,-16,-24}, {16,16,32}},
};

static const edit_class_info_t *find_class(const char *classname)
{
    int i;
    if (!classname) return NULL;
    for (i = 0; i < (int)(sizeof(s_class_info) / sizeof(s_class_info[0])); i++)
        if (!strcmp(s_class_info[i].classname, classname)) return &s_class_info[i];
    return NULL;
}

// Compute the world-space AABB used to display + pick a point entity.
// First preference: live edict's absmin/absmax (set by SV_LinkEdict).
// This is what makes brush entities like func_door pickable — their
// origin is (0,0,0) but their brushmodel lives elsewhere in the world,
// and SV_LinkEdict put the real bbox into v.absmin/absmax. Falls back
// to the per-class table (weapons / monsters / players) and then to a
// centred ±16 cube for unrecognised classnames.
// Compute model-local bounds for any model the editor cares about. For
// alias models WinQuake hardcodes mod->mins/maxs to ±16 ("FIXME: do this
// right" — model.c:1641), so we have to read the per-frame trivertx_t
// bboxmin/bboxmax and de-quantise via mdl_t scale + scale_origin. For
// brush models (b_shell0.bsp etc) mod->mins/maxs are real, set by
// Mod_LoadBrushModel from the BSP submodel.
static int model_local_bbox(model_t *m, vec3_t out_mins, vec3_t out_maxs)
{
    aliashdr_t        *pahdr;
    mdl_t             *pmdl;
    maliasframedesc_t *frame;
    int                i;
    if (!m) return 0;
    if (m->type == mod_brush)
    {
        if (m->maxs[0] <= m->mins[0]) return 0;
        for (i = 0; i < 3; i++) { out_mins[i] = m->mins[i]; out_maxs[i] = m->maxs[i]; }
        return 1;
    }
    if (m->type != mod_alias) return 0;
    pahdr = (aliashdr_t *)Mod_Extradata(m);
    if (!pahdr) return 0;
    pmdl  = (mdl_t *)((byte *)pahdr + pahdr->model);
    frame = &pahdr->frames[0];
    for (i = 0; i < 3; i++)
    {
        out_mins[i] = (float)frame->bboxmin.v[i] * pmdl->scale[i] + pmdl->scale_origin[i];
        out_maxs[i] = (float)frame->bboxmax.v[i] * pmdl->scale[i] + pmdl->scale_origin[i];
    }
    return 1;
}

static int try_live_anchor(const edit_entity_t *e, vec3_t out)
{
    if (e->live_ent && !e->live_ent->free)
    {
        const float *amn = e->live_ent->v.absmin;
        const float *amx = e->live_ent->v.absmax;
        if (amx[0] > amn[0] || amx[1] > amn[1] || amx[2] > amn[2])
        {
            out[0] = (amn[0] + amx[0]) * 0.5f;
            out[1] = (amn[1] + amx[1]) * 0.5f;
            out[2] = (amn[2] + amx[2]) * 0.5f;
            return 1;
        }
    }
    return 0;
}

int Editor_EntityAnchor(const edit_entity_t *e, vec3_t out)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    if (!e) return 0;
    // In live mode, the engine-known position wins so the gizmo + bbox
    // track an AI-moved monster. Map mode keeps the original priority
    // (origin key first) so editing pins the gizmo to the .map source.
    if (view_live && try_live_anchor(e, out)) return 1;
    if (Entity_GetOrigin(e, out)) return 1;
    if (try_live_anchor(e, out)) return 1;
    if (e->live_static)
    {
        VectorCopy(e->live_static->origin, out);
        return 1;
    }
    return 0;
}

void Editor_PointEntityBBox(const edit_entity_t *e,
                            vec3_t out_mins, vec3_t out_maxs)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    static const vec3_t default_min = BBDEF_MIN;
    static const vec3_t default_max = BBDEF_MAX;
    const float *mn = default_min, *mx = default_max;
    const edit_class_info_t *ci = NULL;
    vec3_t o, am, ax;
    int i;

    // Live mode: prefer the visible model's bbox so the box hugs what's
    // actually rendered. Items (item_health, item_shells, …) call
    // SV_SetSize with a generous touch volume (e.g. 32×32×56) so the
    // player can grab them easily, but the user wants the bbox to match
    // the small ammo-box graphic, not the touch trigger. Monsters look
    // tighter against the silhouette, and brush ents (func_door etc.)
    // read identically since their submodel bbox equals absmin/absmax.
    // Falls back to absmin/absmax when no model is loaded.
    if (view_live && e->live_ent && !e->live_ent->free)
    {
        int      mi = (int)e->live_ent->v.modelindex;
        model_t *m  = (mi > 0 && mi < MAX_MODELS) ? sv.models[mi] : NULL;
        if (m && model_local_bbox(m, am, ax))
        {
            const float *org = e->live_ent->v.origin;
            const float *ang = e->live_ent->v.angles;
            if (ang[0] != 0.0f || ang[1] != 0.0f || ang[2] != 0.0f)
            {
                vec3_t fwd, right, up;
                int c;
                AngleVectors((float *)ang, fwd, right, up);
                out_mins[0] = out_mins[1] = out_mins[2] =  1e30f;
                out_maxs[0] = out_maxs[1] = out_maxs[2] = -1e30f;
                for (c = 0; c < 8; c++)
                {
                    float lx = (c & 1) ? ax[0] : am[0];
                    float ly = (c & 2) ? ax[1] : am[1];
                    float lz = (c & 4) ? ax[2] : am[2];
                    int k;
                    for (k = 0; k < 3; k++)
                    {
                        float w = org[k] + fwd[k]*lx - right[k]*ly + up[k]*lz;
                        if (w < out_mins[k]) out_mins[k] = w;
                        if (w > out_maxs[k]) out_maxs[k] = w;
                    }
                }
            }
            else
            {
                for (i = 0; i < 3; i++)
                {
                    out_mins[i] = org[i] + am[i];
                    out_maxs[i] = org[i] + ax[i];
                }
            }
            return;
        }
        {
            const float *amn = e->live_ent->v.absmin;
            const float *amx = e->live_ent->v.absmax;
            if (amx[0] > amn[0] || amx[1] > amn[1] || amx[2] > amn[2])
            {
                for (i = 0; i < 3; i++) { out_mins[i] = amn[i]; out_maxs[i] = amx[i]; }
                return;
            }
        }
    }

    // SV_MakeStatic'd ents (flame torches) — read the alias model's actual
    // frame-0 vertex bounds and translate to the static entity's origin.
    if (e->live_static && e->live_static->model
        && model_local_bbox(e->live_static->model, am, ax))
    {
        for (i = 0; i < 3; i++)
        {
            out_mins[i] = e->live_static->origin[i] + am[i];
            out_maxs[i] = e->live_static->origin[i] + ax[i];
        }
        return;
    }

    // .map origin path (used in map mode, or when no live edict). For BSP-
    // loaded brush entities (no .map origin key) fall back to the live
    // edict's bbox so map mode doesn't snap them to world origin.
    if (!Entity_GetOrigin(e, o) && e->live_ent && !e->live_ent->free)
    {
        const float *amn = e->live_ent->v.absmin;
        const float *amx = e->live_ent->v.absmax;
        if (amx[0] > amn[0] || amx[1] > amn[1] || amx[2] > amn[2])
        {
            for (i = 0; i < 3; i++) { out_mins[i] = amn[i]; out_maxs[i] = amx[i]; }
            return;
        }
    }
    if (e->classname_idx >= 0)
        ci = find_class(e->kv[e->classname_idx].value);

    // Editor preview path: load the model and use its real vertex bounds
    // (Mod_ForName is a cache hit — draw_point_entity_model already
    // loaded it this frame).
    if (ci && ci->modelpath)
    {
        model_t *m = Mod_ForName((char *)ci->modelpath, false);
        if (m && model_local_bbox(m, am, ax))
        {
            for (i = 0; i < 3; i++)
            {
                out_mins[i] = o[i] + am[i];
                out_maxs[i] = o[i] + ax[i];
            }
            return;
        }
    }

    if (ci) { mn = ci->mins; mx = ci->maxs; }
    for (i = 0; i < 3; i++)
    {
        out_mins[i] = o[i] + mn[i];
        out_maxs[i] = o[i] + mx[i];
    }
}

// -----------------------------------------------------------------------------
// Point entity model preview
// -----------------------------------------------------------------------------
//
// Spawned monsters/items normally only appear after `map <name>` runs the
// QuakeC spawn function. While editing we want to *see* what the user
// placed, so we render the alias model directly through the engine's
// software pipeline using a transient currententity. Loaded on demand via
// Mod_ForName — anything not in s_class_info (or with NULL modelpath) falls
// back to the plain wire AABB.

static const char *classname_to_model(const char *classname)
{
    const edit_class_info_t *ci = find_class(classname);
    return ci ? ci->modelpath : NULL;
}

// Public wrapper. Used by map_io.c's static-entity matcher to bind each
// edit_entity to its cl_static_entities[] counterpart by classname → model.
const char *Editor_ClassnameToModel(const char *classname)
{
    return classname_to_model(classname);
}

// Read the entity's "angle" key into out_angles[YAW]. Quake .map convention:
// -1 = up, -2 = down, otherwise yaw degrees. We treat the up/down sentinels
// as a flat yaw=0 for preview (player-model arrow points along +X).
static void parse_entity_angles(const edit_entity_t *e, vec3_t out_angles)
{
    int k;
    out_angles[0] = out_angles[1] = out_angles[2] = 0;
    for (k = 0; k < e->numkv; k++)
    {
        if (!strcmp(e->kv[k].key, "angle"))
        {
            float a = (float)atof(e->kv[k].value);
            if (a >= 0) out_angles[YAW] = a;
            return;
        }
    }
}

// Per-classname translated colormaps for player.mdl previews — lets the user
// tell info_player_start (singleplayer brown), _coop (green), _deathmatch
// (red) and _teleport_destination (purple) apart at a glance. Each entry
// caches a full VID_GRADES*256 colormap built lazily from vid.colormap by
// remapping TOP_RANGE/BOTTOM_RANGE the same way CL_NewTranslation does for
// network player skins. The 4-bit top/bottom values map onto 16-color bands
// in the Quake palette (1=brown, 3=green, 4=red, 11=purple).

typedef struct {
    const char *classname;
    byte top, bottom;
    int  initialised;
    byte buffer[VID_GRADES * 256];
} editor_player_cmap_t;

static editor_player_cmap_t s_player_cmaps[] = {
    {"info_player_start",         1,  1,  0, {0}},  // brown — singleplayer
    {"info_player_start2",        1,  1,  0, {0}},  // brown — return start
    {"info_player_coop",          3,  3,  0, {0}},  // green — coop
    {"info_player_deathmatch",    4,  4,  0, {0}},  // red   — DM
    {"info_teleport_destination", 11, 11, 0, {0}},  // purple — telefrag pad
};

static byte *editor_player_colormap(const char *classname)
{
    int i;
    if (!classname) return NULL;
    for (i = 0; i < (int)(sizeof(s_player_cmaps) / sizeof(s_player_cmaps[0])); i++)
    {
        editor_player_cmap_t *m = &s_player_cmaps[i];
        if (strcmp(m->classname, classname) != 0) continue;
        if (!m->initialised)
        {
            int row, j;
            int top_pal = m->top    << 4;
            int bot_pal = m->bottom << 4;
            byte *src = vid.colormap;
            byte *dst = m->buffer;
            memcpy(dst, vid.colormap, sizeof(m->buffer));
            for (row = 0; row < VID_GRADES; row++, dst += 256, src += 256)
            {
                // CL_NewTranslation reverses bands at index >= 128 because
                // the artists laid those palette ranges out backwards. We
                // mirror that quirk here so the result reads naturally.
                if (top_pal < 128)
                    memcpy(dst + TOP_RANGE, src + top_pal, 16);
                else
                    for (j = 0; j < 16; j++)
                        dst[TOP_RANGE + j] = src[top_pal + 15 - j];
                if (bot_pal < 128)
                    memcpy(dst + BOTTOM_RANGE, src + bot_pal, 16);
                else
                    for (j = 0; j < 16; j++)
                        dst[BOTTOM_RANGE + j] = src[bot_pal + 15 - j];
            }
            m->initialised = 1;
        }
        return m->buffer;
    }
    return NULL;
}

// Pool of fake entity_t records pushed into cl_visedicts each frame so the
// engine renders editor previews via its normal alias / brushmodel pipeline.
// Sized generously — most maps have <100 entities and we only push those
// without a live counterpart.
#define EDIT_PREVIEW_MAX 256
static entity_t s_preview_pool[EDIT_PREVIEW_MAX];

// Editor_PushPreviewEntities — append fake entities to cl_visedicts so that
// the engine's R_DrawBEntitiesOnList (mod_brush) and R_DrawEntitiesOnList
// (mod_alias / mod_sprite) render every editor entity that has a model but
// no live counterpart yet. Skip ents the engine already renders (live edict
// via cl_entities[N], or static via cl_static_entities → R_StoreEfrags).
//
// Called from Editor_PreRender, which fires inside R_RenderView_ AFTER
// CL_RelinkEntities has reset cl_numvisedicts and BEFORE R_EdgeDrawing
// processes brushmodels — exactly the window the engine itself uses to
// fill the visedicts list.
void Editor_PushPreviewEntities(void)
{
    int i;
    int n_pushed = 0;
    extern int       cl_numvisedicts;
    extern entity_t *cl_visedicts[];
    extern cvar_t    editor_view_mode;
    int view_map = (int)editor_view_mode.value == 1;

    if (!Editor_IsOpen()) return;
    if (!cl.worldmodel) return;

    // Live view: don't push any .map preview models. Live view is pure
    // game — engine-rendered entities only, no editor authoring overlay.
    if (!view_map) return;

    // Map view: the engine has already filled cl_visedicts with every live
    // entity (runtime-spawned rockets/gibs/monsters, the player body, …).
    // Map view wants to show only the .map authoring source, so drop the
    // engine's choices wholesale — the loop below re-pushes a preview for
    // each .map entity that has a model. Static-entity efrag chains and
    // the BSP world render through separate pipelines and are unaffected.
    cl_numvisedicts = 0;

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char    *cls;
        const edit_class_info_t *ci;
        model_t       *m;
        entity_t      *ent;
        byte          *player_cmap = NULL;

        if (!Entity_IsPoint(e)) continue;
        if (e->classname_idx < 0) continue;
        if (Editor_EntityHidden(i)) continue;

        // View modes:
        //   live: show what's running. Once an entity has a live edict the
        //         engine's decision is the truth — if it draws something,
        //         that's what we see (no preview); if it draws nothing
        //         (info_player_*, info_intermission, info_null — metadata
        //         edicts with v.model=NULL), we also draw nothing. Map
        //         mode is where authoring markers belong.
        //   map:  show what the .map text says. Push a preview at the .map
        //         origin and scrub the engine's cl_visedicts entry for
        //         this entity so the live model doesn't double-draw.
        if (e->live_ent)
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
            if (!view_map) continue;        // live: defer to engine entirely
            if (engine_renders)
            {
                int k;
                for (k = 0; k < cl_numvisedicts; k++)
                {
                    if (cl_visedicts[k] == &cl_entities[en])
                    {
                        cl_visedicts[k] = cl_visedicts[--cl_numvisedicts];
                        break;
                    }
                }
                // fall through to push preview at .map origin
            }
            }   // end of `else if (!e->live_ent->free)`
        }       // end of `if (e->live_ent)`
        // SV_MakeStatic'd ents (torches) don't move so .map == efrag
        // position; the efrag chain draws them in both modes, no preview
        // needed and we'd have to walk the chain to scrub them anyway.
        if (e->live_static) continue;

        cls = e->kv[e->classname_idx].value;
        ci  = find_class(cls);
        if (!ci || !ci->modelpath) continue;

        m = Mod_ForName((char *)ci->modelpath, false);
        if (!m) continue;
        if (m->type != mod_alias && m->type != mod_brush && m->type != mod_sprite)
            continue;

        if (n_pushed >= EDIT_PREVIEW_MAX) break;
        if (cl_numvisedicts >= MAX_VISEDICTS) break;

        ent = &s_preview_pool[n_pushed++];
        memset(ent, 0, sizeof(*ent));
        Entity_GetOrigin(e, ent->origin);
        parse_entity_angles(e, ent->angles);
        ent->model   = m;
        ent->frame   = 0;
        ent->skinnum = 0;

        // Per-classname player skin tinting (info_player_start brown,
        // _coop green, _deathmatch red, _teleport_destination purple).
        player_cmap   = editor_player_colormap(cls);
        ent->colormap = player_cmap ? player_cmap : vid.colormap;

        cl_visedicts[cl_numvisedicts++] = ent;
    }
}

// Union bbox of every selected brush + point entity. Returns 1 if at least
// one valid item contributed.
int Editor_SelectionBBox(vec3_t out_mins, vec3_t out_maxs)
{
    int i, n = 0, e_idx, b_idx, k;
    vec3_t pmin, pmax;
    out_mins[0] = out_mins[1] = out_mins[2] =  1e30f;
    out_maxs[0] = out_maxs[1] = out_maxs[2] = -1e30f;
    for (i = 0; i < Scene_NumSelected(); i++)
    {
        edit_brush_t *b;
        edit_entity_t *e;
        const float *mn, *mx;
        if (!Scene_GetSelected(i, &e_idx, &b_idx)) continue;
        if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
        e = &edit_scene.entities[e_idx];
        if (b_idx < 0)
        {
            // Point entity ref. Skip if there's no resolvable position
            // (e.g. an empty func_group with no origin and no edict) so
            // the union doesn't get pulled toward (0,0,0). BSP-loaded
            // brush entities (no .map brushes, no origin key, but a live
            // edict) flow through point_entity_bbox which uses the
            // edict's absmin/absmax.
            vec3_t anchor;
            if (!Editor_EntityAnchor(e, anchor)) continue;
            Editor_PointEntityBBox(e, pmin, pmax);
            mn = pmin; mx = pmax;
        }
        else
        {
            if (b_idx >= e->numbrushes) continue;
            b = &e->brushes[b_idx];
            if (!b->valid) continue;
            mn = b->mins; mx = b->maxs;
        }
        for (k = 0; k < 3; k++)
        {
            if (mn[k] < out_mins[k]) out_mins[k] = mn[k];
            if (mx[k] > out_maxs[k]) out_maxs[k] = mx[k];
        }
        n++;
    }
    return n > 0;
}

// Line + small V-arrowhead at the target end. The arrowhead lets the user
// read direction on patrol-path chains (path_corner → path_corner) at a
// glance — a plain line would be ambiguous about which end is the source.
// `through` = 1 → bypass depth (used when either endpoint is selected, so
// the user can trace selected ents' links even when occluded by walls);
// 0 → depth-tested so walls hide the line and the viewport stays readable
// on dense maps.
static void draw_link_arrow(const vec3_t a, const vec3_t b, byte color,
                            int through)
{
    vec3_t dir, right, world_up = {0, 0, 1};
    float len, r2, head;
    int j;
    void (*line)(const vec3_t, const vec3_t, byte)
        = through ? Editor_DrawLine3DOver : Editor_DrawLine3D;

    line(a, b, color);

    VectorSubtract(b, a, dir);
    len = sqrtf(DotProduct(dir, dir));
    if (len < 4.0f) return;
    for (j = 0; j < 3; j++) dir[j] /= len;

    // Perpendicular to dir, in the plane containing world up. Falls back
    // to a world-X cross when the line is nearly vertical.
    CrossProduct(dir, world_up, right);
    r2 = DotProduct(right, right);
    if (r2 < 0.01f)
    {
        vec3_t world_x = {1, 0, 0};
        CrossProduct(dir, world_x, right);
        r2 = DotProduct(right, right);
        if (r2 < 0.01f) return;
    }
    {
        float r = sqrtf(r2);
        for (j = 0; j < 3; j++) right[j] /= r;
    }

    // Pixel-ish arrowhead size: 10% of line length, clamped so it stays
    // readable on tiny links and doesn't dominate long ones.
    head = len * 0.1f;
    if (head > 32.0f) head = 32.0f;
    if (head <  4.0f) head = 4.0f;
    {
        vec3_t tail, lp, rp;
        VectorMA(b, -head, dir, tail);
        VectorMA(tail,  head * 0.4f, right, rp);
        VectorMA(tail, -head * 0.4f, right, lp);
        line(b, rp, color);
        line(b, lp, color);
    }
}

static int entity_is_selected(int e_idx)
{
    int i, e_sel, b_sel;
    for (i = 0; i < Scene_NumSelected(); i++)
        if (Scene_GetSelected(i, &e_sel, &b_sel) && e_sel == e_idx)
            return 1;
    return 0;
}

// Resolve an entity's angle into a unit direction vector. Returns 1 with
// `out` filled, 0 if the entity has no angle to visualize.
//
// Live mode prefers the running edict: SetMovedir zeroes v.angles and
// fills v.movedir for movers, and v.angles[1] holds the yaw for facers.
// Map mode (and pending entities, and live-mode fallback) reads the .map
// "angle" key directly so the user sees what they're authoring. Sentinel
// values map to vertical: -1 → up, -2 → down — matches SetMovedir's own
// decode in sdlquake/game/misc.c:28.
static int entity_angle_dir(const edit_entity_t *e, vec3_t out)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    int k;

    if (view_live && e->live_ent && !e->live_ent->free)
    {
        const float *md  = e->live_ent->v.movedir;
        const float *ang = e->live_ent->v.angles;
        if (md[0] != 0.0f || md[1] != 0.0f || md[2] != 0.0f)
        {
            VectorCopy(md, out);
            return 1;
        }
        if (ang[1] != 0.0f)
        {
            float r = ang[1] * (3.14159265f / 180.0f);
            out[0] = cosf(r); out[1] = sinf(r); out[2] = 0;
            return 1;
        }
        // Fall through to the .map kv — covers facers whose live angle
        // happens to be 0 but the .map authored a non-zero value, and
        // pending entities with no edict yet.
    }

    for (k = 0; k < e->numkv; k++)
    {
        if (!strcmp(e->kv[k].key, "angle"))
        {
            float a = (float)atof(e->kv[k].value);
            float r;
            if (a == -1.0f) { out[0] = 0; out[1] = 0; out[2] =  1; return 1; }
            if (a == -2.0f) { out[0] = 0; out[1] = 0; out[2] = -1; return 1; }
            r = a * (3.14159265f / 180.0f);
            out[0] = cosf(r); out[1] = sinf(r); out[2] = 0;
            return 1;
        }
    }
    return 0;
}

// Per-entity facing / movedir arrow. Reads as orientation for monsters,
// info_player_*, light torches; as motion direction for func_doors,
// buttons, plats, triggers. Same arrow primitive as the target-link
// pass; selected ents bypass depth so the arrow stays readable.
static void draw_angle_arrows(void)
{
    int i, j;
    vec3_t a, dir, b;
    const float arrow_len = 48.0f;     // 3 build grid cells; reads well
                                       // beyond a typical 32-unit bbox

    extern cvar_t editor_show_angles;
    int show_all = editor_show_angles.value != 0.0f;

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        byte color;
        int sel, through;
        if (Editor_EntityHidden(i)) continue;
        // Both point ents (monsters, spawns — facing) and brush ents
        // (func_door, button, plat — slide direction) get an arrow.
        if (!Editor_EntityAnchor(e, a)) continue;
        if (!entity_angle_dir(e, dir)) continue;

        sel = entity_is_selected(i);

        // Toolbar toggle gates everything except selected ents — clicking
        // an entity always reveals its facing/movedir even when the
        // global "angles" checkbox is off.
        if (!show_all && !sel) continue;

        for (j = 0; j < 3; j++) b[j] = a[j] + dir[j] * arrow_len;

        // White when selected — the bright-yellow EDIT_COLOR_SELECTED reads
        // too close to the gizmo's yellow Y-axis line on top of the same
        // entity. White stands clear of the axis colours.
        color   = sel ? EDIT_COLOR_AXIS_HOT : category_color(e);
        through = sel;
        draw_link_arrow(a, b, color, through);
    }
}

// Walk every entity's "target" / "killtarget" keys, find each match by
// "targetname", and draw a coloured arrow from source to target. Patrol
// paths emerge naturally from this — a path_corner with target=next
// chains to the next path_corner, etc. The check is O(N²) per pair but
// runs only while the editor is open + paused, on ≤ a few hundred ents.
// Per-frame cache of entity indices that share a target/targetname link
// with any currently-selected entity. Populated once per Editor_RenderScene
// by compute_link_partners(); read by the wire + bbox passes so the *other*
// end of a selected link draws with depth-bypass (matching the link arrow's
// own through-wall path). Cap is generous for big maps but bounded.
#define EDIT_LINK_PARTNERS_MAX 512
static int s_link_partners[EDIT_LINK_PARTNERS_MAX];
static int s_link_partners_n;

static int entity_is_link_partner(int e_idx)
{
    int k;
    for (k = 0; k < s_link_partners_n; k++)
        if (s_link_partners[k] == e_idx) return 1;
    return 0;
}

static void link_partners_add(int idx)
{
    int k;
    if (s_link_partners_n >= EDIT_LINK_PARTNERS_MAX) return;
    for (k = 0; k < s_link_partners_n; k++)
        if (s_link_partners[k] == idx) return;
    s_link_partners[s_link_partners_n++] = idx;
}

// Walk every selected entity's target/killtarget/targetname kvs and pool
// the indices of every other entity matching the other half of the link.
// "Selected as source" (has target/killtarget) → partners are anything
// with a matching targetname. "Selected as dest" (has targetname) →
// partners are anything pointing at it.
static void compute_link_partners(void)
{
    int s, i, j, k;
    int nsel = Scene_NumSelected();
    s_link_partners_n = 0;
    if (nsel == 0) return;

    for (s = 0; s < nsel; s++)
    {
        int e_idx, b_idx;
        edit_entity_t *se;
        if (!Scene_GetSelected(s, &e_idx, &b_idx)) continue;
        if (e_idx < 0 || e_idx >= edit_scene.numentities) continue;
        se = &edit_scene.entities[e_idx];

        for (j = 0; j < se->numkv; j++)
        {
            const char *key = se->kv[j].key;
            const char *val = se->kv[j].value;
            int sel_is_src, sel_is_dst;
            if (!val[0]) continue;
            sel_is_src = (!strcmp(key, "target") || !strcmp(key, "killtarget"));
            sel_is_dst = !strcmp(key, "targetname");
            if (!sel_is_src && !sel_is_dst) continue;

            for (i = 0; i < edit_scene.numentities; i++)
            {
                edit_entity_t *t;
                if (i == e_idx) continue;
                t = &edit_scene.entities[i];
                for (k = 0; k < t->numkv; k++)
                {
                    const char *tk = t->kv[k].key;
                    const char *tv = t->kv[k].value;
                    int hit = 0;
                    if (sel_is_src
                        && !strcmp(tk, "targetname")
                        && !strcmp(tv, val))
                        hit = 1;
                    else if (sel_is_dst
                        && (!strcmp(tk, "target") || !strcmp(tk, "killtarget"))
                        && !strcmp(tv, val))
                        hit = 1;
                    if (hit) { link_partners_add(i); break; }
                }
            }
        }
    }
}

static void draw_target_links(void)
{
    extern cvar_t editor_show_links;
    int show_all = editor_show_links.value != 0.0f;
    int i, j, k, m;
    vec3_t a, b;

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        int src_sel, src_hidden;
        src_hidden = Editor_EntityHidden(i);
        if (!Editor_EntityAnchor(e, a)) continue;
        src_sel = entity_is_selected(i);

        for (j = 0; j < e->numkv; j++)
        {
            const char *key = e->kv[j].key;
            const char *val = e->kv[j].value;
            byte color;

            if      (!strcmp(key, "target"))     color = EDIT_COLOR_AXIS_Z;   // 244 light blue
            else if (!strcmp(key, "killtarget")) color = EDIT_COLOR_AXIS_X;   // 251 red
            else continue;
            if (!val[0]) continue;

            for (k = 0; k < edit_scene.numentities; k++)
            {
                edit_entity_t *t;
                int dst_sel, either_sel, dst_hidden;
                if (k == i) continue;
                t = &edit_scene.entities[k];
                dst_hidden = Editor_EntityHidden(k);
                // Draw if at least one end is visible — the arrow is a
                // relationship indicator, not the entity itself, and we
                // want users to see the link even when one side is in a
                // category they've hidden (e.g. trigger → door, with the
                // trigger category collapsed).
                if (src_hidden && dst_hidden) continue;
                dst_sel    = entity_is_selected(k);
                either_sel = src_sel || dst_sel;
                // Toolbar toggle gates everything except links touching
                // the selection — a clicked entity always shows its
                // outgoing AND incoming links even when "links" is off.
                if (!show_all && !either_sel) continue;
                for (m = 0; m < t->numkv; m++)
                {
                    if (!strcmp(t->kv[m].key, "targetname")
                     && !strcmp(t->kv[m].value, val))
                    {
                        if (Editor_EntityAnchor(t, b))
                            draw_link_arrow(a, b, color, either_sel);
                        break;
                    }
                }
            }
        }
    }
}

void Editor_RenderScene(void)
{
    int i, j;
    int style = (int)editor_render_style.value;
    int multi = Scene_NumSelected() > 1;
    int editor_open = Editor_IsOpen();
    extern cvar_t editor_view_mode;
    int view_map = (int)editor_view_mode.value == 1;
    int trigger_tex = (int)editor_trigger_render.value == 1;
    int clip_tex    = (int)editor_clip_render.value    == 1;

    if (!editor_open && !view_map) return;
    if (edit_scene.numentities == 0) return;

    // Pool the entities at the far end of any selected target/targetname
    // link so the subsequent wire + bbox passes know to draw them with
    // depth-bypass. Without this, a selected button's link arrow flies
    // through walls but the destination door's wireframe is still occluded.
    compute_link_partners();

    // If the engine's loaded worldmodel came from a recent editor_compile
    // (i.e. it lives in the virtual-file registry), the BSP renderer is
    // already drawing the exact same geometry we'd otherwise emit here.
    // Skip the fill pass to avoid pure double-rendering. The wireframe
    // pass below still runs so selection outlines + active-face highlight
    // stay visible. When the user `map`s an arbitrary .bsp from disk
    // without compiling, the VFS won't have an entry and we render
    // fills as before — useful as a .map preview against a stale BSP.
    int bsp_is_ours = 0;
    if (cl.worldmodel && cl.worldmodel->name[0])
    {
        extern int Editor_VFS_Find(const char *path, void **out_bytes, int *out_size);
        void *vbytes; int vsize;
        if (Editor_VFS_Find(cl.worldmodel->name, &vbytes, &vsize))
            bsp_is_ours = 1;
    }

    // Pass 1: filled faces (flat-shaded or textured).
    //
    // Trigger override: triggers always honour editor_trigger_render — bbox
    // mode (0) skips them here so the single-AABB pass below has them to
    // itself; textured mode (1) force-tex-draws them even in pure wire
    // styles, so they pop against world wireframe. BSP-populated scenes
    // have no .map brushes for triggers (start.bsp et al), so we
    // synthesise a 6-plane cube from live_ent->v.absmin/absmax and feed
    // that to the tex-draw path — same visual as bbox mode, just textured.
    {
        int do_flat = (style == EDIT_STYLE_FLAT || style == EDIT_STYLE_FLAT_WIRE);
        int do_tex  = (style == EDIT_STYLE_TEX  || style == EDIT_STYLE_TEX_WIRE);
        static edit_brush_t s_trigger_proxy;     // reused; faces alloc'd per call
        if (!bsp_is_ours)
        {
            for (i = 0; i < edit_scene.numentities; i++)
            {
                edit_entity_t *e = &edit_scene.entities[i];
                int is_trigger = (Editor_EntityCategory(e) == EDIT_CAT_TRIGGER);
                int trig_force_tex = (is_trigger && trigger_tex);
                int drew_any = 0;
                if (is_trigger && !trigger_tex) continue;  // bbox pass owns it
                if (!do_flat && !do_tex && !trig_force_tex) continue;
                // Brushes panel "Hide triggers" checkbox (and the other
                // category filters / visible-only / skill filter) — apply
                // to both real-brush and proxy-brush trigger drawing.
                if (is_trigger && Editor_EntityHiddenByCategory(i)) continue;
                for (j = 0; j < e->numbrushes; j++)
                {
                    edit_brush_t *b = &e->brushes[j];
                    int is_clip;
                    if (!b->valid) continue;
                    if (!brush_visible(b)) continue;
                    // Clip-brush override (per-brush; trigger entities don't
                    // contain clip brushes in practice, but the predicate is
                    // cheap and the rules compose). bbox mode: the clip AABB
                    // pass below owns it. tex mode: force tex-draw, falling
                    // back to the procedural grid since the BSP has no
                    // "clip" miptex.
                    is_clip = brush_is_clip(b);
                    if (is_clip && !clip_tex) continue;
                    if (trig_force_tex || do_tex || (is_clip && clip_tex))
                        Editor_TexDrawBrush(b);
                    else
                        Editor_FlatDrawBrush(b);
                    drew_any = 1;
                }
                // BSP-trigger proxy: synthesise a textured cube from the
                // live edict's bbox when no .map brushes exist.
                if (trig_force_tex && !drew_any
                    && e->live_ent && !e->live_ent->free)
                {
                    const float *amn = e->live_ent->v.absmin;
                    const float *amx = e->live_ent->v.absmax;
                    if (amx[0] > amn[0] && amx[1] > amn[1] && amx[2] > amn[2])
                    {
                        Brush_FreeFaces(&s_trigger_proxy);
                        memset(&s_trigger_proxy, 0, sizeof(s_trigger_proxy));
                        if (Editor_BuildCubeBrush(&s_trigger_proxy, amn, amx,
                                                  "trigger"))
                            Editor_TexDrawBrush(&s_trigger_proxy);
                    }
                }
            }
        }
    }

    // Pass 2: wireframe (drawn over filled faces so selection + brush
    // boundaries stay readable). For pure-fill styles only the selected
    // brush gets an outline.
    {
        int wire_all = (style == EDIT_STYLE_WIRE
                     || style == EDIT_STYLE_FLAT_WIRE
                     || style == EDIT_STYLE_TEX_WIRE);
        int af_e, af_b, af_p;
        int has_active_face = Scene_GetActiveFace(&af_e, &af_b, &af_p);
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_entity_t *e = &edit_scene.entities[i];
            int is_trigger = (Editor_EntityCategory(e) == EDIT_CAT_TRIGGER);
            // bbox mode: the single-AABB pass below draws the trigger; skip
            // the per-brush wireframe so we don't get a busy outline behind
            // it. (For face-level work, toggle editor_trigger_render → tex.)
            if (is_trigger && !trigger_tex) continue;
            int is_link = !Scene_SelectionContains(i, -1)
                          && entity_is_link_partner(i);
            for (j = 0; j < e->numbrushes; j++)
            {
                edit_brush_t *b = &e->brushes[j];
                int is_sel      = Scene_SelectionContains(i, j);
                int is_brush_ent = (i > 0);  /* non-worldspawn entity */
                if (!b->valid) continue;
                if (!brush_visible(b)) continue;
                // Per-brush clip skip in bbox mode — the dedicated AABB
                // pass below draws each clip brush in teal.
                if (!clip_tex && brush_is_clip(b)) continue;
                // Worldspawn brushes only need the wireframe in wire-all
                // mode or when selected — the BSP renderer already draws
                // them.  Non-worldspawn brush entities (func_door, etc.)
                // always get an outline so they're never invisible even
                // when the fill pass is skipped (bsp_is_ours).
                if (!wire_all && !is_sel && !is_brush_ent) continue;
                // Multi-select hides the per-brush yellow outline — we draw
                // a single union bbox below instead so the group reads as
                // one thing. In single-select the per-brush outline still
                // wins for clarity (and ignores depth so it's always seen).
                if (multi && is_sel && editor_open)
                {
                    if (wire_all || is_brush_ent)
                        draw_brush(b, is_brush_ent ? category_color(e) : EDIT_COLOR_BRUSH, 0);
                }
                else
                {
                    int show_sel = is_sel && editor_open;
                    byte bc = show_sel    ? EDIT_COLOR_SELECTED :
                              is_brush_ent ? category_color(e)  : EDIT_COLOR_BRUSH;
                    // Link partners ride the depth-bypass path alongside
                    // selected brushes so the user can see the far end of
                    // the relationship through walls.
                    draw_brush(b, bc, show_sel || is_link);
                }
                // Active-face highlight: walk the face whose plane_idx
                // matches and draw its edges in white over everything,
                // depth-bypassed. Plane-index keying survives Brush_Compile
                // reorderings — same trick Brush_TranslateFace uses.
                if (has_active_face && i == af_e && j == af_b)
                {
                    int kk;
                    for (kk = 0; kk < b->numfaces; kk++)
                    {
                        const edit_face_t *f = &b->faces[kk];
                        int v;
                        if (f->plane_idx != af_p) continue;
                        for (v = 0; v < f->numverts; v++)
                        {
                            int next = (v + 1) % f->numverts;
                            Editor_DrawLine3DOver(f->verts[v], f->verts[next],
                                                  EDIT_COLOR_AXIS_HOT);
                        }
                        break;
                    }
                }
            }
        }
    }

    // Pass 2.5: trigger-volume bbox. Only when editor_trigger_render == 0
    // (the bbox view). Single AABB per trigger entity, computed as the
    // union of its compiled brushes; falls back to live_ent->v.absmin/max
    // for BSP-populated scenes that don't have .map brushes. Depth-bypass
    // when selected so the user can always see what they picked.
    //
    // Uses HiddenByCategory (not Editor_EntityHidden) on purpose: triggers
    // are authoring-only visualisations — the spawn function zeroes
    // v.modelindex so cl_entities[N].model is NULL and the live-view
    // "engine isn't drawing this" filter would hide every trigger in a
    // BSP-populated scene (e.g. start.bsp's trigger_changelevels).
    if (!trigger_tex)
    {
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_entity_t *e = &edit_scene.entities[i];
            vec3_t tmin, tmax;
            int    have_bbox = 0;
            int    is_sel;
            byte   color;
            if (Editor_EntityCategory(e) != EDIT_CAT_TRIGGER) continue;
            if (Editor_EntityHiddenByCategory(i)) continue;
            tmin[0] = tmin[1] = tmin[2] =  1e30f;
            tmax[0] = tmax[1] = tmax[2] = -1e30f;
            for (j = 0; j < e->numbrushes; j++)
            {
                edit_brush_t *b = &e->brushes[j];
                int k;
                if (!b->valid) continue;
                for (k = 0; k < 3; k++)
                {
                    if (b->mins[k] < tmin[k]) tmin[k] = b->mins[k];
                    if (b->maxs[k] > tmax[k]) tmax[k] = b->maxs[k];
                }
                have_bbox = 1;
            }
            if (!have_bbox && e->live_ent && !e->live_ent->free)
            {
                const float *amn = e->live_ent->v.absmin;
                const float *amx = e->live_ent->v.absmax;
                if (amx[0] > amn[0] || amx[1] > amn[1] || amx[2] > amn[2])
                {
                    VectorCopy(amn, tmin);
                    VectorCopy(amx, tmax);
                    have_bbox = 1;
                }
            }
            if (!have_bbox) continue;
            is_sel = Scene_SelectionContains(i, -1);
            color  = is_sel ? EDIT_COLOR_SELECTED : EDIT_COLOR_TRIGGER;
            // Linked but unselected triggers (e.g. a trigger_relay called
            // from the selected button) get the same through-wall path so
            // they're visible alongside the link arrow.
            draw_aabb_ex(tmin, tmax, color,
                         is_sel || (!is_sel && entity_is_link_partner(i)));
        }
    }

    // Pass 2.6: per-brush AABB for clip brushes. Mirrors the trigger
    // bbox pass but keyed on the brush predicate (all faces "clip*")
    // rather than the entity classname — clip brushes typically live in
    // worldspawn alongside visible geometry. Honours per-brush selection
    // colour. No BSP-loaded fallback: qbsp strips clip faces from the
    // visible BSP, so a BSP-only scene has no clip brushes to draw.
    if (!clip_tex)
    {
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_entity_t *e = &edit_scene.entities[i];
            for (j = 0; j < e->numbrushes; j++)
            {
                edit_brush_t *b = &e->brushes[j];
                int is_sel;
                byte color;
                if (!b->valid) continue;
                if (!brush_is_clip(b)) continue;
                if (!brush_visible(b)) continue;
                is_sel = Scene_SelectionContains(i, j);
                color  = is_sel ? EDIT_COLOR_SELECTED : EDIT_COLOR_CLIP;
                draw_aabb_ex(b->mins, b->maxs, color, is_sel);
            }
        }
    }

    // Pass 3: point-entity wire AABB. Model previews themselves are
    // rendered by the engine — Editor_PushPreviewEntities (called from
    // PreRender) appends fake entities into cl_visedicts so both alias
    // (R_DrawEntitiesOnList) and brushmodel (R_DrawBEntitiesOnList)
    // dispatch happens through the standard pipeline. We only draw the
    // bbox here, gated to "selected, or no model registered" so the
    // user gets a visible marker for lights / metadata ents but doesn't
    // see a noisy box around every visible model in the world. Selected
    // stays depth-bypassed so the user can find their picked item even
    // when it's behind a wall; unselected is depth-tested so walls
    // properly occlude.
    if (Editor_IsOpen())
    {
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_entity_t *e = &edit_scene.entities[i];
            const char *cls = NULL;
            vec3_t pmin, pmax;
            int is_sel, is_link, has_model;
            byte color;
            if (!Entity_IsPoint(e)) continue;
            if (Editor_EntityHidden(i)) continue;
            is_sel  = Scene_SelectionContains(i, -1);
            is_link = !is_sel && entity_is_link_partner(i);
            if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;
            has_model = classname_to_model(cls) != NULL;
            // Linked point ents (path_corner with a model preview, a
            // targeted monster, etc.) override the "has-model → hide bbox"
            // skip so the user can pinpoint the far end of the link.
            if (!is_sel && !is_link && has_model) continue;
            Editor_PointEntityBBox(e, pmin, pmax);
            color = is_sel ? EDIT_COLOR_SELECTED : category_color(e);
            draw_aabb_ex(pmin, pmax, color, is_sel || is_link);
        }
    }

    // Combined selection bbox (only when more than one item selected).
    // Editor-only — when the user closes the editor, the selection still
    // exists internally but should be invisible in the running game.
    if (multi && Editor_IsOpen())
    {
        vec3_t bmin, bmax;
        if (Editor_SelectionBBox(bmin, bmax))
            draw_aabb_over(bmin, bmax, EDIT_COLOR_SELECTED);
    }

    // Per-entity facing / movedir arrows (orientation for monsters and
    // spawns, slide direction for func_doors). Drawn before target links
    // so a long target arrow can layer over the short angle stub when
    // they happen to overlap.
    if (Editor_IsOpen())
        draw_angle_arrows();

    // Target / killtarget links (incl. monster patrol paths via path_corner
    // chains). Draw last so they layer over the bbox + brush outlines.
    if (Editor_IsOpen())
        draw_target_links();

    if (Editor_IsOpen())
        Editor_DrawLightGizmos();

    Editor_GizmoDraw();
}

// -----------------------------------------------------------------------------
// Light entity gizmos (Stage 6 of the coloured-lighting editor work).
//
// `light*` point entities don't have a model in vanilla Quake, so the default
// editor bbox is just a 16-unit cube -- gives no sense of position priority,
// no colour cue, no indication of reach. These gizmos surface that
// information directly in the viewport: a 3D star at every light's origin,
// a reach sphere for the selected light, and a cone for spotlights.
// -----------------------------------------------------------------------------

static void draw_light_star(const vec3_t origin, byte color)
{
    /* 6-arm asterisk (each axis-aligned plus 4 diagonals on the XY plane). */
    const float r = 6.0f;
    vec3_t a, b;
    int i;
    static const float dirs[14][3] = {
        { 1, 0, 0}, {-1, 0, 0},
        { 0, 1, 0}, { 0,-1, 0},
        { 0, 0, 1}, { 0, 0,-1},
        { 0.707f,  0.707f, 0}, {-0.707f, -0.707f, 0},
        { 0.707f, -0.707f, 0}, {-0.707f,  0.707f, 0},
        { 0.707f, 0,  0.707f}, {-0.707f, 0, -0.707f},
        { 0.707f, 0, -0.707f}, {-0.707f, 0,  0.707f},
    };
    for (i = 0; i < 14; i += 2)
    {
        a[0] = origin[0] + dirs[i][0]   * r;
        a[1] = origin[1] + dirs[i][1]   * r;
        a[2] = origin[2] + dirs[i][2]   * r;
        b[0] = origin[0] + dirs[i+1][0] * r;
        b[1] = origin[1] + dirs[i+1][1] * r;
        b[2] = origin[2] + dirs[i+1][2] * r;
        Editor_DrawLine3DOver(a, b, color);
    }
}

static void draw_light_sphere(const vec3_t origin, float radius, byte color)
{
    /* Three wireframe great circles -- XY, YZ, XZ -- 16 segments each. The
     * eye fills in the rest. Cheap, ~50 lines per sphere. */
    const int N = 16;
    int axis;
    for (axis = 0; axis < 3; axis++)
    {
        vec3_t prev = {0, 0, 0};
        int i;
        for (i = 0; i <= N; i++)
        {
            float a = ((float)i / N) * 2.0f * (float)M_PI;
            float c = (float)cos(a) * radius;
            float s = (float)sin(a) * radius;
            vec3_t p;
            p[0] = origin[0]; p[1] = origin[1]; p[2] = origin[2];
            if (axis == 0)      { p[0] += c; p[1] += s; }
            else if (axis == 1) { p[1] += c; p[2] += s; }
            else                { p[0] += c; p[2] += s; }
            if (i > 0)
                Editor_DrawLine3DOver(prev, p, color);
            VectorCopy(p, prev);
        }
    }
}

void Editor_DrawLightGizmos(void)
{
    extern cvar_t editor_light_gizmos;
    int i, j;
    int sel_ent_idx = -1;
    int verbosity = (int)editor_light_gizmos.value;
    edit_entity_t *sel;

    /* 0 = off entirely. */
    if (verbosity <= 0) return;

    sel = Scene_GetSelectedEntity();
    if (sel) {
        for (i = 0; i < edit_scene.numentities; i++)
            if (&edit_scene.entities[i] == sel) { sel_ent_idx = i; break; }
    }

    /* Mode 1 (default) only draws the gizmo for the selected entity. Mode
     * 2 draws stars for every light* in the scene. Real Quake maps carry
     * hundreds of point lights (start.bsp has 267) so the default mode
     * is conservative; the cvar is exposed for the rare case the user
     * wants every light visible at once. */
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls;
        vec3_t origin;
        float light = 0;
        int is_selected = (i == sel_ent_idx);
        byte glyph_color = EDIT_COLOR_LIGHT;
        const char *target = NULL;

        if (e->classname_idx < 0 || e->classname_idx >= e->numkv) continue;
        cls = e->kv[e->classname_idx].value;
        if (strncmp(cls, "light", 5) != 0) continue;

        if (verbosity == 1 && !is_selected) continue;

        if (!Entity_GetOrigin(e, origin)) continue;

        for (j = 0; j < e->numkv; j++)
        {
            if (!strcmp(e->kv[j].key, "light"))
                light = (float)atof(e->kv[j].value);
            else if (!strcmp(e->kv[j].key, "target"))
                target = e->kv[j].value;
        }
        if (light <= 0) light = 300;

        draw_light_star(origin, is_selected ? EDIT_COLOR_SELECTED : glyph_color);

        if (is_selected)
            draw_light_sphere(origin, light, glyph_color);

        /* Spotlight indicator: line origin -> target's origin, if the
         * target name resolves to another entity. */
        if (target && target[0])
        {
            int k;
            for (k = 0; k < edit_scene.numentities; k++)
            {
                edit_entity_t *t = &edit_scene.entities[k];
                vec3_t torg;
                int found = 0;
                int kk;
                for (kk = 0; kk < t->numkv; kk++)
                {
                    if (!strcmp(t->kv[kk].key, "targetname")
                        && !strcmp(t->kv[kk].value, target))
                    {
                        found = 1;
                        break;
                    }
                }
                if (!found) continue;
                if (!Entity_GetOrigin(t, torg)) break;
                Editor_DrawLine3DOver(origin, torg,
                                      is_selected ? EDIT_COLOR_SELECTED
                                                  : glyph_color);
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Picking
// -----------------------------------------------------------------------------

// Möller-Trumbore-ish ray-vs-convex-polygon. Polygon is in `verts[0..n]`,
// assumed planar and convex. Returns 1 if hit, with t along ray.
static int ray_vs_face(const vec3_t origin, const vec3_t dir,
                       const vec3_t *verts, int n,
                       const vec3_t plane_normal, float plane_dist,
                       float *out_t)
{
    float denom, t;
    vec3_t hit;
    int i;

    denom = DotProduct(dir, plane_normal);
    if (fabsf(denom) < 1e-6f) return 0;
    t = (plane_dist - DotProduct(origin, plane_normal)) / denom;
    if (t < 0.001f) return 0;

    hit[0] = origin[0] + dir[0] * t;
    hit[1] = origin[1] + dir[1] * t;
    hit[2] = origin[2] + dir[2] * t;

    // Inside-test: for each edge, check that the hit is on the same side as
    // the polygon centre. Convex polygon — this is sufficient.
    {
        vec3_t centroid = {0,0,0};
        for (i = 0; i < n; i++)
        {
            centroid[0] += verts[i][0];
            centroid[1] += verts[i][1];
            centroid[2] += verts[i][2];
        }
        centroid[0] /= n; centroid[1] /= n; centroid[2] /= n;

        for (i = 0; i < n; i++)
        {
            const float *a = verts[i];
            const float *b = verts[(i + 1) % n];
            vec3_t edge, edge_normal, va, vc;
            float da, dc;
            VectorSubtract(b, a, edge);
            CrossProduct(edge, plane_normal, edge_normal);
            VectorSubtract(hit, a, va);
            VectorSubtract(centroid, a, vc);
            da = DotProduct(va, edge_normal);
            dc = DotProduct(vc, edge_normal);
            // hit must be on the same side of edge as the centroid.
            if (da * dc < -1e-3f) return 0;
        }
    }
    *out_t = t;
    return 1;
}

// Slab-test ray vs AABB. Returns 1 on hit; *out_t is the entry t (or the
// near-clipped t if origin is inside the box).
static int ray_vs_aabb(const vec3_t origin, const vec3_t dir,
                       const vec3_t mins, const vec3_t maxs, float *out_t)
{
    float tmin = -1e30f, tmax = 1e30f;
    int i;
    for (i = 0; i < 3; i++)
    {
        if (fabsf(dir[i]) < 1e-9f)
        {
            if (origin[i] < mins[i] || origin[i] > maxs[i]) return 0;
        }
        else
        {
            float inv = 1.0f / dir[i];
            float t1 = (mins[i] - origin[i]) * inv;
            float t2 = (maxs[i] - origin[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return 0;
        }
    }
    if (tmax < 0.001f) return 0;
    *out_t = tmin > 0.001f ? tmin : tmax;
    return 1;
}

// World occlusion for the picker. Cast a long ray against the BSP point hull
// and return the world-units distance to the first solid surface, so the
// caller can reject editor hits past it. Returns 1e30 (no occlusion) when:
//   - no worldmodel (shouldn't happen when the editor is open),
//   - the camera origin is already inside solid geometry (free-fly through
//     a wall — without this escape the user couldn't pick anything from
//     inside the void),
//   - the ray misses everything for the full trace distance.
static float world_pick_occlusion(const vec3_t origin, const vec3_t dir)
{
    extern qboolean SV_RecursiveHullCheck (hull_t *, int, float, float,
                                           vec3_t, vec3_t, trace_t *);
    const float TRACE_DIST = 10000.0f;
    trace_t trace;
    vec3_t  end;
    vec3_t  start;
    int     i;
    if (!cl.worldmodel) return 1e30f;
    for (i = 0; i < 3; i++)
    {
        start[i] = origin[i];
        end[i]   = origin[i] + dir[i] * TRACE_DIST;
    }
    memset(&trace, 0, sizeof(trace));
    trace.fraction = 1;
    VectorCopy(end, trace.endpos);
    SV_RecursiveHullCheck(cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);
    if (trace.startsolid || trace.allsolid) return 1e30f;
    if (trace.fraction >= 1.0f) return 1e30f;
    return trace.fraction * TRACE_DIST;
}

// "Visible only" filter — combines a frustum check (entity bbox projects
// into the viewport with at least one corner in-bounds) and a world
// occlusion trace (line of sight from camera to bbox center, no wall in
// between). Used by the Brushes panel "visible" toggle. Always returns 1
// (visible) when the camera is inside solid geometry — otherwise the user
// could end up with nothing on screen.
int Editor_EntityInView(int e_idx)
{
    extern qboolean SV_RecursiveHullCheck (hull_t *, int, float, float,
                                           vec3_t, vec3_t, trace_t *);
    edit_entity_t *e;
    vec3_t pmin, pmax, center, start, end, corner;
    trace_t trace;
    float sx, sy;
    int W, H, k, j, found;
    int any_in_screen;

    if (e_idx < 0 || e_idx >= edit_scene.numentities) return 1;
    e = &edit_scene.entities[e_idx];

    // Get world-space bbox.
    if (Entity_IsPoint(e))
    {
        Editor_PointEntityBBox(e, pmin, pmax);
    }
    else
    {
        // Brush entity: union of valid brushes' compiled bboxes.
        pmin[0] = pmin[1] = pmin[2] =  1e30f;
        pmax[0] = pmax[1] = pmax[2] = -1e30f;
        found = 0;
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            if (!b->valid) continue;
            for (k = 0; k < 3; k++)
            {
                if (b->mins[k] < pmin[k]) pmin[k] = b->mins[k];
                if (b->maxs[k] > pmax[k]) pmax[k] = b->maxs[k];
            }
            found = 1;
        }
        if (!found) return 1;       // worldspawn-only / metadata — never hide
    }

    for (k = 0; k < 3; k++) center[k] = (pmin[k] + pmax[k]) * 0.5f;

    // Frustum: any of the 8 bbox corners must project into the viewport.
    W = (int)vid.width;
    H = (int)vid.height;
    any_in_screen = 0;
    for (j = 0; j < 8; j++)
    {
        corner[0] = (j & 1) ? pmax[0] : pmin[0];
        corner[1] = (j & 2) ? pmax[1] : pmin[1];
        corner[2] = (j & 4) ? pmax[2] : pmin[2];
        if (Editor_ProjectWorld(corner, &sx, &sy))
        {
            if (sx >= 0 && sx < W && sy >= 0 && sy < H) { any_in_screen = 1; break; }
        }
    }
    if (!any_in_screen) return 0;

    // Occlusion: world-trace from camera to bbox center.
    if (cl.worldmodel)
    {
        VectorCopy(r_origin, start);
        VectorCopy(center,   end);
        memset(&trace, 0, sizeof(trace));
        trace.fraction = 1;
        VectorCopy(end, trace.endpos);
        SV_RecursiveHullCheck(cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);
        if (trace.startsolid || trace.allsolid) return 1;   // camera in solid
        if (trace.fraction < 0.99f) return 0;               // wall in the way
    }
    return 1;
}

// Detach e->live_ent when the underlying server slot has been ED_Free'd
// and reused for a different classname. Common path: the player picks up
// an item the .map authored (item_health, item_shells, …); ED_Free 0.5s
// later the slot is reallocated for a runtime-spawned projectile (spike /
// missile / grenade). Without this, picking against `live_ent->v.absmin/
// absmax` would let the user click "the item" but actually be selecting a
// rocket flying past. After detach, point_entity_bbox falls back to the
// .map origin path — that's what the user authored, so it's the correct
// anchor for the original item. The rocket itself is reachable via the
// runtime-edict pick pass below, which materialises a transient entry.
static void detach_stale_live_ent(edit_entity_t *e)
{
    if (!e->live_ent) return;
    if (e->live_ent->free) {
        e->live_ent = NULL;
        e->live_bound_classname[0] = '\0';
        return;
    }
    if (!e->live_ent->v.classname) return;
    // No snapshot → the binding pre-dates this field (e.g. a legacy code
    // path that didn't call through the bind helper). Skip the detach
    // rather than null a binding we can't validate.
    if (!e->live_bound_classname[0]) return;
    // Compare against the classname captured at bind time, NOT against the
    // .map kv. QuakeC spawn functions rewrite v.classname ("func_door" →
    // "door", "func_button" → "button"), so a kv-vs-live compare would
    // detach every spawn-renamed brush ent every frame — exactly the bug
    // that was causing transient stubs to shadow real entities in the
    // picker. The snapshot already reflects the post-spawn name, so a
    // mismatch here means the slot got reused by something else (the case
    // the detach was meant to catch).
    if (strcmp(e->live_ent->v.classname, e->live_bound_classname) != 0) {
        e->live_ent = NULL;
        e->live_bound_classname[0] = '\0';
    }
}

// Reap transient entries whose underlying live edict went away (rocket
// detonated, gib settled and freed, …). Without this, every picked-then-
// gone projectile leaves a permanent ghost at its last classname/origin.
// Reaped in-place; we build a compaction map and rewrite selection +
// active-face entity refs through it so a transient disappearing earlier
// in the array doesn't leave the gizmo pointing at the wrong entry. Skip
// entries that are currently selected — keeps the user's context across
// a churn frame so they don't lose a selected rocket mid-edit.
//
// `force_all`: reap every transient regardless of liveness or selection.
// Used when switching to map view, where transients shouldn't exist at all.
static void reap_dead_transients(int force_all)
{
    int i, w, k, dst;
    int old_count = edit_scene.numentities;
    int *new_idx;

    if (old_count == 0) return;

    new_idx = (int *)malloc((size_t)old_count * sizeof(int));
    if (!new_idx) return;       // OOM: skip the reap pass rather than crash

    for (i = 0, w = 0; i < old_count; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (e->transient
         && (force_all
             || ((!e->live_ent || e->live_ent->free)
                 && !Scene_SelectionContains(i, -1))))
        {
            // Free per-entity allocations; brushes never set on transients.
            free(e->kv);
            new_idx[i] = -1;
            continue;
        }
        new_idx[i] = w;
        if (w != i) edit_scene.entities[w] = *e;
        w++;
    }
    edit_scene.numentities = w;

    // Rewrite selection entity refs through the compaction map. Selected
    // transients are never reaped (the loop above's Scene_SelectionContains
    // guard), so existing entries should always map to a valid new index —
    // but drop anything that doesn't, defensively, so a future change to the
    // guard rule can't leak a dangling index into the gizmo.
    for (k = 0, dst = 0; k < edit_scene.num_selected; k++)
    {
        int e_old = edit_scene.selection[k].entity;
        int e_new;
        if (e_old < 0 || e_old >= old_count) continue;
        e_new = new_idx[e_old];
        if (e_new < 0) continue;
        if (dst != k) edit_scene.selection[dst] = edit_scene.selection[k];
        edit_scene.selection[dst].entity = e_new;
        dst++;
    }
    edit_scene.num_selected = dst;

    // Active face entity index — clear the trio if its entity got reaped,
    // shift if it just moved.
    if (edit_scene.active_face_ent >= 0 && edit_scene.active_face_ent < old_count)
    {
        int new_ent = new_idx[edit_scene.active_face_ent];
        if (new_ent < 0)
        {
            edit_scene.active_face_ent   = -1;
            edit_scene.active_face_brush = -1;
            edit_scene.active_face_plane = -1;
        }
        else
        {
            edit_scene.active_face_ent = new_ent;
        }
    }

    free(new_idx);
}

// Look up an existing transient for `ed`; if none, append one. Returns the
// index in edit_scene.entities, or -1 on allocation failure. Live state
// (classname + origin) is captured into kv just so Inspector + the brush
// list have something to display; subsequent live-mode reads pull straight
// from `live_ent` so the kv copy is purely cosmetic.
static int find_or_create_transient(edict_t *ed)
{
    int i;
    char buf[64];
    edit_entity_t *e;

    if (!ed || ed->free) return -1;

    for (i = 0; i < edit_scene.numentities; i++)
        if (edit_scene.entities[i].live_ent == ed
         && edit_scene.entities[i].transient)
            return i;

    edit_scene.entities = (edit_entity_t *)realloc(edit_scene.entities,
        (edit_scene.numentities + 1) * sizeof(edit_entity_t));
    if (!edit_scene.entities) return -1;

    i = edit_scene.numentities;
    e = &edit_scene.entities[i];
    memset(e, 0, sizeof(*e));
    e->classname_idx = -1;
    e->origin_idx    = -1;
    e->spawned       = 1;
    e->transient     = 1;
    e->live_ent      = ed;
    if (ed->v.classname)
        Q_strncpy(e->live_bound_classname, ed->v.classname, EDIT_KEY_LEN - 1);

    Entity_SetKV(e, "classname",
                 ed->v.classname ? ed->v.classname : "(runtime)");
    snprintf(buf, sizeof(buf), "%g %g %g",
             ed->v.origin[0], ed->v.origin[1], ed->v.origin[2]);
    Entity_SetKV(e, "origin", buf);

    edit_scene.numentities++;
    return i;
}

// True iff any edit_scene entry's live_ent points to `ed`. Used to skip
// edicts that already have a .map-authored handle, so the transient pass
// only fires for genuinely runtime-only ents.
static int edict_is_already_bound(const edict_t *ed)
{
    int i;
    for (i = 0; i < edit_scene.numentities; i++)
        if (edit_scene.entities[i].live_ent == ed) return 1;
    return 0;
}

// Walk sv.edicts and ensure every alive runtime edict has a transient
// edit_entity_t wrapping it. Drops dead transients first. Mirrors the
// edict-skip rules of Editor_PickAt's runtime-edict pass (degenerate
// bbox → skip; already-bound → skip).
void Editor_MaterialiseLiveTransients(void)
{
    extern cvar_t editor_view_mode;
    int n;
    int i;
    int view_live = (int)editor_view_mode.value == 0;

    // Map view: tear down any transients left over from a previous live
    // session so runtime edicts (rockets, monsters, the player) stop
    // rendering and stop being clickable. Editor_PreRender clears the
    // selection on view-mode switch, so force_all won't strand the user's
    // current focus.
    if (!view_live) { reap_dead_transients(1); return; }
    if (!sv.active) return;

    // Refresh stale live_ent links on .map-authored entries before
    // edict_is_already_bound consults them. Without this, a freed slot
    // reused by an unrelated runtime ent would block transient creation.
    for (i = 0; i < edit_scene.numentities; i++)
        detach_stale_live_ent(&edit_scene.entities[i]);

    reap_dead_transients(0);

    for (n = 1; n < sv.num_edicts; n++)
    {
        edict_t *ed = EDICT_NUM(n);
        const float *amn, *amx;
        if (ed->free) continue;
        if (edict_is_already_bound(ed)) continue;
        amn = ed->v.absmin;
        amx = ed->v.absmax;
        // Degenerate bbox = temp ent or fresh edict, not selectable.
        if (amx[0] <= amn[0] || amx[1] <= amn[1] || amx[2] <= amn[2])
            continue;
        find_or_create_transient(ed);
    }
}

int Editor_PickAt(float sx, float sy, int *out_ent, int *out_brush,
                  int *out_plane)
{
    extern cvar_t editor_view_mode;
    int view_live = (int)editor_view_mode.value == 0;
    vec3_t origin, dir;
    float best_t = 1e30f;
    float world_t;
    int best_ent = -1, best_brush = -1, best_plane = -1;
    int i, j, k;

    reap_dead_transients(0);

    Editor_ScreenToRay(sx, sy, origin, dir);
    world_t = world_pick_occlusion(origin, dir);

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (Editor_EntityHidden(i)) continue;        // category-filtered out
        detach_stale_live_ent(e);                    // rocket/spike took our slot?
        if (Entity_IsPoint(e))
        {
            vec3_t pmin, pmax;
            float t;
            int k;
            Editor_PointEntityBBox(e, pmin, pmax);
            // Ensure a minimum pick volume so thin/oriented entities
            // (e.g. rockets flying toward the camera) remain clickable
            // even though the visual box correctly collapses to a small
            // cross-section. The display box is unchanged — only the
            // slab test here gets the padded volume.
            {
                vec3_t anchor;
                static const float PICK_MIN = 10.0f;
                if (Editor_EntityAnchor(e, anchor))
                    for (k = 0; k < 3; k++)
                    {
                        if (pmin[k] > anchor[k] - PICK_MIN) pmin[k] = anchor[k] - PICK_MIN;
                        if (pmax[k] < anchor[k] + PICK_MIN) pmax[k] = anchor[k] + PICK_MIN;
                    }
            }
            if (ray_vs_aabb(origin, dir, pmin, pmax, &t))
            {
                // Reject hits behind a wall — picking should match
                // what the user can actually see. world_t is the
                // distance to the nearest BSP surface along this ray.
                if (t > world_t) { /* occluded */ }
                else if (t < best_t)
                {
                    best_t = t;
                    best_ent = i;
                    best_brush = -1;
                    best_plane = -1;
                }
            }
            continue;
        }
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            if (!b->valid) continue;
            for (k = 0; k < b->numfaces; k++)
            {
                edit_face_t *f = &b->faces[k];
                edit_plane_t *pl = &b->planes[f->plane_idx];
                float t;
                if (ray_vs_face(origin, dir,
                                (const vec3_t *)f->verts, f->numverts,
                                pl->normal, pl->dist, &t))
                {
                    if (t > world_t) { /* occluded by world */ }
                    else if (t < best_t)
                    {
                        best_t = t;
                        best_ent = i;
                        best_brush = j;
                        best_plane = f->plane_idx;
                    }
                }
            }
        }
    }

    // Second pass: runtime-spawned edicts (rockets, spikes, grenades, gibs,
    // dropped backpacks, summoned monsters, …) that have no .map source.
    // These are the things `find_or_create_transient` materialises into a
    // selectable handle. We require a non-degenerate absmin/absmax — fresh
    // edicts (or temp ents) often have collapsed bboxes and would pass the
    // ray test at the wrong place. Map view skips this pass: clicking a
    // live rocket in map view should not select it.
    if (sv.active && view_live)
    {
        int n;
        for (n = 1; n < sv.num_edicts; n++)
        {
            edict_t *ed = EDICT_NUM(n);
            const float *amn, *amx;
            float t;
            int idx;
            if (ed->free) continue;
            if (edict_is_already_bound(ed)) continue;
            amn = ed->v.absmin;
            amx = ed->v.absmax;
            if (amx[0] <= amn[0] || amx[1] <= amn[1] || amx[2] <= amn[2])
                continue;
            // Skip edicts whose bbox contains the ray origin. In FPS mode
            // the ray starts at the player's eye, which sits inside the
            // player edict's bbox — without this, every click lands on
            // the player. Generalises to "you can't aim at something
            // you're standing in" for any future free-fly case too.
            if (origin[0] >= amn[0] && origin[0] <= amx[0]
             && origin[1] >= amn[1] && origin[1] <= amx[1]
             && origin[2] >= amn[2] && origin[2] <= amx[2])
                continue;
            if (!ray_vs_aabb(origin, dir, amn, amx, &t)) continue;
            if (t > world_t) continue;       // wall in the way
            if (t >= best_t) continue;
            idx = find_or_create_transient(ed);
            if (idx < 0) continue;
            best_t = t;
            best_ent = idx;
            best_brush = -1;
            best_plane = -1;
        }
    }

    if (best_ent < 0) return 0;
    if (out_ent)   *out_ent   = best_ent;
    if (out_brush) *out_brush = best_brush;
    if (out_plane) *out_plane = best_plane;
    return 1;
}

// Cursor-placement raycast. Combines a BSP trace (so we hit floors, walls,
// and ceilings) with a face-by-face check against editor brushes (which
// aren't in the BSP yet). Returns the closer of the two hits with its
// outward normal, so the spawn dialog can offset the new entity slightly
// off the surface and avoid z-fighting / wall embedding.
int Editor_RaycastForPlacement(float sx, float sy,
                               vec3_t out_hit, vec3_t out_normal)
{
    return Editor_RaycastForPlacement_Ex(sx, sy, NULL, 0, out_hit, out_normal);
}

// Same trace, but skips any editor brush listed in `skip`. n_skip == 0
// (or skip == NULL) is identical to Editor_RaycastForPlacement.
int Editor_RaycastForPlacement_Ex(float sx, float sy,
                                  const edit_skip_pair_t *skip, int n_skip,
                                  vec3_t out_hit, vec3_t out_normal)
{
    extern qboolean SV_RecursiveHullCheck (hull_t *, int, float, float,
                                           vec3_t, vec3_t, trace_t *);
    const float TRACE_DIST = 10000.0f;
    vec3_t origin, dir;
    int    have_hit = 0;
    float  best_t = 1e30f;
    vec3_t best_n = { 0, 0, 1 };
    int    i, j, k, s;

    Editor_ScreenToRay(sx, sy, origin, dir);

    // World BSP trace.
    if (cl.worldmodel)
    {
        trace_t trace;
        vec3_t  start, end;
        for (i = 0; i < 3; i++)
        {
            start[i] = origin[i];
            end[i]   = origin[i] + dir[i] * TRACE_DIST;
        }
        memset(&trace, 0, sizeof(trace));
        trace.fraction = 1;
        VectorCopy(end, trace.endpos);
        SV_RecursiveHullCheck(cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);
        if (!trace.startsolid && !trace.allsolid && trace.fraction < 1.0f)
        {
            float t = trace.fraction * TRACE_DIST;
            if (t < best_t)
            {
                best_t = t;
                VectorCopy(trace.plane.normal, best_n);
                have_hit = 1;
            }
        }
    }

    // Editor brushes: find the nearest face hit. Skip hidden categories so
    // the user doesn't drop an entity onto a filtered-out trigger volume.
    // Also skip any pair listed in `skip` (gizmo snap excludes the dragged
    // selection so a brush doesn't snap to itself).
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (Editor_EntityHidden(i)) continue;
        if (Entity_IsPoint(e)) continue;
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            int skipped = 0;
            if (!b->valid) continue;
            for (s = 0; s < n_skip; s++)
            {
                if (skip[s].e_idx == i && skip[s].b_idx == j) { skipped = 1; break; }
            }
            if (skipped) continue;
            for (k = 0; k < b->numfaces; k++)
            {
                edit_face_t *f = &b->faces[k];
                edit_plane_t *pl = &b->planes[f->plane_idx];
                float t;
                if (!ray_vs_face(origin, dir,
                                 (const vec3_t *)f->verts, f->numverts,
                                 pl->normal, pl->dist, &t))
                    continue;
                if (t < best_t)
                {
                    best_t = t;
                    VectorCopy(pl->normal, best_n);
                    have_hit = 1;
                }
            }
        }
    }

    if (!have_hit) return 0;
    for (i = 0; i < 3; i++)
        out_hit[i] = origin[i] + dir[i] * best_t;
    VectorCopy(best_n, out_normal);
    return 1;
}

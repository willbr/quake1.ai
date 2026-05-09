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

    if (!view_to_screen(va, &sx0, &sy0)) return;
    if (!view_to_screen(vb, &sx1, &sy1)) return;

    {
        float iz0 = 1.0f / va[2];
        float iz1 = 1.0f / vb[2];
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

// Render styles: index into editor_render_style cvar.
enum {
    EDIT_STYLE_WIRE = 0,
    EDIT_STYLE_FLAT = 1,
    EDIT_STYLE_FLAT_WIRE = 2,
    EDIT_STYLE_TEX  = 3,
    EDIT_STYLE_TEX_WIRE = 4,
    EDIT_STYLE_COUNT
};

cvar_t editor_render_style = { "editor_render_style", "0" };

void Editor_RegisterCvars(void)
{
    Cvar_RegisterVariable(&editor_render_style);
}

// Draw the 12 edges of an AABB as world-space lines, depth-bypassed so it
// reads as an overlay even when geometry sits in front of it.
static void draw_aabb_over(const vec3_t mins, const vec3_t maxs, byte color)
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
        Editor_DrawLine3DOver(c[edges[i][0]], c[edges[i][1]], color);
}

// Half-extent of the wireframe AABB drawn at a point entity's origin and
// used as its pick volume. Same value for all classnames in M6a; can grow
// per-class later (player bbox, light point cube) once we have spawn data.
#define EDIT_POINT_HALF 16.0f

// Compute the world-space AABB used to display + pick a point entity.
static void point_entity_bbox(const edit_entity_t *e,
                              vec3_t out_mins, vec3_t out_maxs)
{
    vec3_t o;
    int i;
    Entity_GetOrigin(e, o);
    for (i = 0; i < 3; i++)
    {
        out_mins[i] = o[i] - EDIT_POINT_HALF;
        out_maxs[i] = o[i] + EDIT_POINT_HALF;
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
// Mod_ForName — anything not in s_model_table or that fails to load falls
// back to the plain wire AABB.

static const struct {
    const char *classname;
    const char *modelpath;
} s_model_table[] = {
    {"info_player_start",       "progs/player.mdl"},
    {"info_player_deathmatch",  "progs/player.mdl"},
    {"info_teleport_destination","progs/player.mdl"},
    {"monster_army",            "progs/soldier.mdl"},
    {"monster_dog",             "progs/dog.mdl"},
    {"monster_ogre",            "progs/ogre.mdl"},
    {"monster_demon1",          "progs/demon.mdl"},
    {"monster_shambler",        "progs/shambler.mdl"},
    {"monster_knight",          "progs/knight.mdl"},
    {"monster_wizard",          "progs/wizard.mdl"},
    {"monster_zombie",          "progs/zombie.mdl"},
    {"monster_enforcer",        "progs/enforcer.mdl"},
    {"monster_hell_knight",     "progs/hknight.mdl"},
    {"monster_fish",            "progs/fish.mdl"},
    {"monster_boss",            "progs/boss.mdl"},
    {"weapon_supershotgun",     "progs/g_shot.mdl"},
    {"weapon_nailgun",          "progs/g_nail.mdl"},
    {"weapon_supernailgun",     "progs/g_nail2.mdl"},
    {"weapon_grenadelauncher",  "progs/g_rock.mdl"},
    {"weapon_rocketlauncher",   "progs/g_rock2.mdl"},
    {"weapon_lightning",        "progs/g_light.mdl"},
    {"item_armor1",             "progs/armor.mdl"},
    {"item_armor2",             "progs/armor.mdl"},
    {"item_armorInv",           "progs/armor.mdl"},
    {"item_artifact_super_damage",      "progs/quaddama.mdl"},
    {"item_artifact_invisibility",      "progs/invisibl.mdl"},
    {"item_artifact_invulnerability",   "progs/invulner.mdl"},
    {"item_artifact_envirosuit",        "progs/suit.mdl"},
    {"item_torch_small_walltorch",      "progs/flame.mdl"},
    // .bsp ammo boxes (item_health/shells/spikes/rockets/cells) are brush
    // models, not alias — they fall back to the wire AABB for now.
};

static const char *classname_to_model(const char *classname)
{
    int i;
    if (!classname) return NULL;
    for (i = 0; i < (int)(sizeof(s_model_table) / sizeof(s_model_table[0])); i++)
        if (!strcmp(s_model_table[i].classname, classname))
            return s_model_table[i].modelpath;
    return NULL;
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

// Render a single alias model at the entity's origin. Mirrors the software
// renderer's case mod_alias path in R_DrawEntitiesOnList.
static void draw_point_entity_model(const edit_entity_t *e, const char *modelpath)
{
    static entity_t fake_ent;       // reused across the per-frame loop
    model_t *m;
    alight_t lighting;
    float    lightvec[3] = { -1, 0, 0 };
    int      j;

    m = Mod_ForName((char *)modelpath, false);
    if (!m || m->type != mod_alias) return;

    memset(&fake_ent, 0, sizeof(fake_ent));
    Entity_GetOrigin(e, fake_ent.origin);
    parse_entity_angles(e, fake_ent.angles);
    fake_ent.model    = m;
    fake_ent.frame    = 0;
    fake_ent.skinnum  = 0;
    fake_ent.colormap = vid.colormap;
    fake_ent.trivial_accept = 0;

    currententity = &fake_ent;
    VectorCopy(fake_ent.origin, r_entorigin);
    VectorSubtract(r_origin, r_entorigin, modelorg);

    if (!R_AliasCheckBBox()) return;

    j = R_LightPoint(fake_ent.origin);
    lighting.ambientlight = j;
    lighting.shadelight   = j;
    lighting.plightvec    = lightvec;
    if (lighting.ambientlight > 128) lighting.ambientlight = 128;
    if (lighting.ambientlight + lighting.shadelight > 192)
        lighting.shadelight = 192 - lighting.ambientlight;

    R_AliasDrawModel(&lighting);
}

// Union bbox of every selected brush + point entity. Returns 1 if at least
// one valid item contributed.
static int selection_bbox(vec3_t out_mins, vec3_t out_maxs)
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
            // Point entity ref.
            point_entity_bbox(e, pmin, pmax);
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

void Editor_RenderScene(void)
{
    int i, j;
    int style = (int)editor_render_style.value;
    int multi = Scene_NumSelected() > 1;

    if (edit_scene.numentities == 0) return;

    // Pass 1: filled faces (flat-shaded or textured).
    {
        int do_flat = (style == EDIT_STYLE_FLAT || style == EDIT_STYLE_FLAT_WIRE);
        int do_tex  = (style == EDIT_STYLE_TEX  || style == EDIT_STYLE_TEX_WIRE);
        if (do_flat || do_tex)
        {
            for (i = 0; i < edit_scene.numentities; i++)
            {
                edit_entity_t *e = &edit_scene.entities[i];
                for (j = 0; j < e->numbrushes; j++)
                {
                    edit_brush_t *b = &e->brushes[j];
                    if (!b->valid) continue;
                    if (!brush_visible(b)) continue;
                    if (do_tex)  Editor_TexDrawBrush(b);
                    else         Editor_FlatDrawBrush(b);
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
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_entity_t *e = &edit_scene.entities[i];
            for (j = 0; j < e->numbrushes; j++)
            {
                edit_brush_t *b = &e->brushes[j];
                int is_sel = Scene_SelectionContains(i, j);
                if (!b->valid) continue;
                if (!brush_visible(b)) continue;
                if (!wire_all && !is_sel) continue;
                // Multi-select hides the per-brush yellow outline — we draw
                // a single union bbox below instead so the group reads as
                // one thing. In single-select the per-brush outline still
                // wins for clarity (and ignores depth so it's always seen).
                if (multi && is_sel)
                {
                    if (wire_all)
                        draw_brush(b, EDIT_COLOR_BRUSH, 0);
                }
                else
                {
                    draw_brush(b,
                               is_sel ? EDIT_COLOR_SELECTED : EDIT_COLOR_BRUSH,
                               is_sel);
                }
            }
        }
    }

    // Pass 3a: point entity models. When we have a model for the classname
    // we render it through the alias path so the user sees the actual
    // monster / weapon / item shape they placed; otherwise we'll fall
    // through to the wire-AABB pass below.
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls, *mpath;
        if (!Entity_IsPoint(e)) continue;
        if (e->classname_idx < 0) continue;
        cls = e->kv[e->classname_idx].value;
        mpath = classname_to_model(cls);
        if (!mpath) continue;
        draw_point_entity_model(e, mpath);
    }

    // Pass 3b: wire AABB for point entities. Selected items always draw
    // (so the user has a visible selection marker even on top of the
    // model); unselected items only draw when no model rendered, since
    // the box would otherwise duplicate the model's silhouette.
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        const char *cls = NULL;
        vec3_t pmin, pmax;
        int is_sel, has_model;
        if (!Entity_IsPoint(e)) continue;
        is_sel = Scene_SelectionContains(i, -1);
        if (e->classname_idx >= 0) cls = e->kv[e->classname_idx].value;
        has_model = classname_to_model(cls) != NULL;
        if (has_model && !is_sel) continue;
        point_entity_bbox(e, pmin, pmax);
        draw_aabb_over(pmin, pmax,
                       is_sel ? EDIT_COLOR_SELECTED : EDIT_COLOR_BRUSH);
    }

    // Combined selection bbox (only when more than one item selected).
    if (multi)
    {
        vec3_t bmin, bmax;
        if (selection_bbox(bmin, bmax))
            draw_aabb_over(bmin, bmax, EDIT_COLOR_SELECTED);
    }

    Editor_GizmoDraw();
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

int Editor_PickAt(float sx, float sy, int *out_ent, int *out_brush)
{
    vec3_t origin, dir;
    float best_t = 1e30f;
    int best_ent = -1, best_brush = -1;
    int i, j, k;

    Editor_ScreenToRay(sx, sy, origin, dir);

    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        if (Entity_IsPoint(e))
        {
            vec3_t pmin, pmax;
            float t;
            point_entity_bbox(e, pmin, pmax);
            if (ray_vs_aabb(origin, dir, pmin, pmax, &t))
            {
                if (t < best_t)
                {
                    best_t = t;
                    best_ent = i;
                    best_brush = -1;
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
                    if (t < best_t)
                    {
                        best_t = t;
                        best_ent = i;
                        best_brush = j;
                    }
                }
            }
        }
    }
    if (best_ent < 0) return 0;
    if (out_ent)   *out_ent   = best_ent;
    if (out_brush) *out_brush = best_brush;
    return 1;
}

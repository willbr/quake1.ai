/*
 * r_livelight.c — Live RGB lightmap delta state machine.
 *
 * Replaces the per-frame dlight stuffing that paint_light_preview used
 * to do (and provides the visual side of sim_light.c's Light_AddOverride
 * for Gust-extinguished torches).
 *
 * Storage: model_t.live_rgblightdata is allocated + memcpy'd from
 *          rgblightdata in Mod_LoadLITFile. surf->rgb_samples points at
 *          the live buffer. We mutate it in place; rgblightdata stays
 *          canonical and lets us restore baseline on demand.
 *
 * Mutation: Lightmap_AddDelta does a BSP-cull walk (R_MarkLights-shape)
 *           and applies a per-texel additive falloff using the same
 *           math as R_AddDynamicLights_RGB in r_surf.c. Result clamped
 *           to byte range.
 *
 * Invalidation: each touched surface gets surf->dlightframe bumped so
 *               the surface cache rebuilds (per
 *               project_surface_cache_invalidation.md). D_FlushCaches
 *               at the end of every mutation flushes any stale chunks.
 *
 * Falloff math: (rad - d) * color[ch] approximates the existing per-frame
 *               preview output. The exact equivalence to R_AddDynamicLights_RGB
 *               would be (rad - d) * 256 * color[ch] / lightadj_style_0,
 *               where lightadj_style_0 = d_lightstylevalue[0] = 264 in
 *               normal play. 256/264 ≈ 0.97, so we drop the constant.
 *               Iterate if A/B drifts.
 */

#include "quakedef.h"
#include "r_livelight.h"

typedef struct {
	vec3_t pos;
	float  radius;
	vec3_t color;
	int    owner;
} livelight_override_t;

static livelight_override_t s_overrides[LIGHTMAP_MAX_OVERRIDES];
static int                  s_override_count;

static void Lightmap_TestCmd_f(void);
static void Lightmap_RestoreCmd_f(void);
static void Lightmap_DumpCmd_f(void);

void Lightmap_Init(void)
{
	s_override_count = 0;
	Cmd_AddCommand("r_livelight_test",    Lightmap_TestCmd_f);
	Cmd_AddCommand("r_livelight_restore", Lightmap_RestoreCmd_f);
	Cmd_AddCommand("r_livelight_dump",    Lightmap_DumpCmd_f);
}

void Lightmap_Shutdown(void)
{
	s_override_count = 0;
}

void Lightmap_ClearAll(void)
{
	s_override_count = 0;
	/* Buffer reseeding happens implicitly: a fresh map calls
	 * Mod_LoadLighting -> Mod_LoadLITFile which Hunk_AllocName's a
	 * fresh live buffer pre-seeded from rgblightdata. Nothing to
	 * copy here. */
}

/*
 * Walk a single surface within the light's BSP cull window and
 * accumulate per-texel falloff into surf->rgb_samples (which points at
 * live_rgblightdata). Math mirrors R_AddDynamicLights_RGB in r_surf.c.
 *
 * sign: +1 to add (paint_light_preview light entry), -1 to subtract
 *       (gust extinguish — currently not used; Light_AddOverride sends
 *       a negative color and uses sign=+1, which is mathematically
 *       equivalent).
 */
static void apply_to_surface(msurface_t *surf,
                             const vec3_t pos, float radius,
                             const vec3_t color, int sign)
{
	int        smax, tmax;
	float      dist, rad, minlight;
	vec3_t     impact, local;
	int        s, t, sd, td;
	int        i;
	mtexinfo_t *tex;
	byte       *lm;

	if (!surf->rgb_samples) return;
	if (surf->flags & SURF_DRAWTILED) return;

	rad  = radius;
	dist = DotProduct(pos, surf->plane->normal) - surf->plane->dist;
	rad -= (float)fabs(dist);
	if (rad <= 0) return;
	minlight = rad;

	tex  = surf->texinfo;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	for (i = 0; i < 3; i++)
		impact[i] = pos[i] - surf->plane->normal[i] * dist;

	local[0] = DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3];
	local[1] = DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3];
	local[0] -= surf->texturemins[0];
	local[1] -= surf->texturemins[1];

	/* Pick the static lightmap chunk to write into. surf->rgb_samples
	 * is the first chunk; the chunks are concatenated by style index,
	 * each `size = smax*tmax` texels. R_BuildLightMap_RGB scales
	 * chunk[m] by lightadj[m] = d_lightstylevalue[styles[m]] — for
	 * animated styles (1..11) this flickers per frame. Writing the
	 * preview into chunk 0 blindly causes "blank bands" wherever the
	 * surface's first style is animated and currently dim. Static
	 * styles are 0 ("normal", always 264) and 32 ("always on", also
	 * constant). Find the first one; skip the surface if none exists. */
	{
		int chunk = -1;
		int m;
		for (m = 0; m < MAXLIGHTMAPS && surf->styles[m] != 255; m++) {
			if (surf->styles[m] == 0 || surf->styles[m] == 32) {
				chunk = m;
				break;
			}
		}
		if (chunk < 0) return;
		lm = surf->rgb_samples + chunk * smax * tmax * 3;
	}
	for (t = 0; t < tmax; t++) {
		td = (int)(local[1] - t*16);
		if (td < 0) td = -td;
		for (s = 0; s < smax; s++) {
			int   idx = (t*smax + s) * 3;
			float d, add;
			int   ch;
			sd = (int)(local[0] - s*16);
			if (sd < 0) sd = -sd;
			d = (sd > td) ? (float)sd + (td>>1) : (float)td + (sd>>1);
			if (d >= minlight) continue;
			add = (rad - d);
			for (ch = 0; ch < 3; ch++) {
				int v = (int)lm[idx + ch] + sign * (int)(add * color[ch]);
				if (v < 0)   v = 0;
				if (v > 255) v = 255;
				lm[idx + ch] = (byte)v;
			}
		}
	}

	/* Force surface cache rebuild for this surf next frame. dlightbits
	 * is logically empty for this kind of contribution — the dynamic-
	 * light pipeline never runs for it. */
	if (surf->dlightframe != r_framecount) {
		DLIGHTBITS_CLEAR(&surf->dlightbits);
		surf->dlightframe = r_framecount;
	}
}

/* Recursive BSP walk gated on the light's plane distance. Same shape
 * as R_MarkLights but we don't mark dlightbits — the renderer's
 * dynamic-light path is bypassed entirely for this contribution. */
static void apply_recursive(mnode_t *node,
                            const vec3_t pos, float radius,
                            const vec3_t color, int sign)
{
	float       dist;
	msurface_t *surf;
	int         i;

	if (node->contents < 0) return;
	dist = DotProduct(pos, node->plane->normal) - node->plane->dist;
	if (dist > radius) {
		apply_recursive(node->children[0], pos, radius, color, sign);
		return;
	}
	if (dist < -radius) {
		apply_recursive(node->children[1], pos, radius, color, sign);
		return;
	}

	surf = cl.worldmodel->surfaces + node->firstsurface;
	for (i = 0; i < node->numsurfaces; i++, surf++)
		apply_to_surface(surf, pos, radius, color, sign);

	apply_recursive(node->children[0], pos, radius, color, sign);
	apply_recursive(node->children[1], pos, radius, color, sign);
}

void Lightmap_AddDelta(const vec3_t pos, float radius,
                       const vec3_t color, int owner)
{
	livelight_override_t *o;
	if (!cl.worldmodel || !cl.worldmodel->live_rgblightdata) return;
	if (s_override_count >= LIGHTMAP_MAX_OVERRIDES) return;
	o = &s_overrides[s_override_count++];
	VectorCopy(pos,   o->pos);
	VectorCopy(color, o->color);
	o->radius = radius;
	o->owner  = owner;
	apply_recursive(cl.worldmodel->nodes, pos, radius, color, +1);
	D_FlushCaches();
}

/* live_rgblightdata = rgblightdata (the baked baseline), over the whole
 * lightmap region. Also bumps dlightframe on every map surface so the
 * surface cache rebuilds. */
static void restore_baseline(void)
{
	model_t    *m = cl.worldmodel;
	msurface_t *surf;
	int         i;

	if (!m || !m->live_rgblightdata || !m->rgblightdata) return;
	if (m->live_rgblightdata_size > 0)
		memcpy(m->live_rgblightdata, m->rgblightdata,
		       m->live_rgblightdata_size);

	surf = m->surfaces;
	for (i = 0; i < m->numsurfaces; i++, surf++) {
		if (surf->dlightframe != r_framecount) {
			DLIGHTBITS_CLEAR(&surf->dlightbits);
			surf->dlightframe = r_framecount;
		}
	}
}

void Lightmap_BaselineChanged(void)
{
	model_t    *m = cl.worldmodel;
	msurface_t *surf;
	int         i;
	if (!m || !m->live_rgblightdata || !m->rgblightdata) return;
	if (m->live_rgblightdata_size <= 0) return;

	/* Refresh live = baseline. */
	memcpy(m->live_rgblightdata, m->rgblightdata,
	       m->live_rgblightdata_size);

	/* Replay every outstanding override on top. */
	for (i = 0; i < s_override_count; i++) {
		livelight_override_t *o = &s_overrides[i];
		apply_recursive(m->nodes, o->pos, o->radius, o->color, +1);
	}

	/* Bump dlightframe on every surface so the cache rebuilds. */
	surf = m->surfaces;
	for (i = 0; i < m->numsurfaces; i++, surf++) {
		if (surf->dlightframe != r_framecount) {
			DLIGHTBITS_CLEAR(&surf->dlightbits);
			surf->dlightframe = r_framecount;
		}
	}
	D_FlushCaches();
}

void Lightmap_ClearOwner(int owner)
{
	int i, w;
	if (!cl.worldmodel || !cl.worldmodel->live_rgblightdata) return;
	w = 0;
	for (i = 0; i < s_override_count; i++) {
		if (s_overrides[i].owner == owner) continue;
		if (w != i) s_overrides[w] = s_overrides[i];
		w++;
	}
	s_override_count = w;
	restore_baseline();
	for (i = 0; i < s_override_count; i++) {
		livelight_override_t *o = &s_overrides[i];
		apply_recursive(cl.worldmodel->nodes,
		                o->pos, o->radius, o->color, +1);
	}
	D_FlushCaches();
}

/* --- Console commands for manual A/B ---------------------------------- */

static void Lightmap_TestCmd_f(void)
{
	vec3_t pos, color;
	float  radius;
	if (Cmd_Argc() < 8) {
		Con_Printf("usage: r_livelight_test x y z radius r g b\n");
		return;
	}
	pos[0]   = (float)atof(Cmd_Argv(1));
	pos[1]   = (float)atof(Cmd_Argv(2));
	pos[2]   = (float)atof(Cmd_Argv(3));
	radius   = (float)atof(Cmd_Argv(4));
	color[0] = (float)atof(Cmd_Argv(5));
	color[1] = (float)atof(Cmd_Argv(6));
	color[2] = (float)atof(Cmd_Argv(7));
	Lightmap_AddDelta(pos, radius, color, LIGHTMAP_OWNER_EDITOR);
	Con_Printf("r_livelight_test: applied (%g %g %g) r=%g c=(%g %g %g)\n",
	           pos[0], pos[1], pos[2], radius,
	           color[0], color[1], color[2]);
}

static void Lightmap_RestoreCmd_f(void)
{
	Lightmap_ClearOwner(LIGHTMAP_OWNER_EDITOR);
	Lightmap_ClearOwner(LIGHTMAP_OWNER_GAMEPLAY);
	Con_Printf("r_livelight_restore: cleared all overrides\n");
}

static void Lightmap_DumpCmd_f(void)
{
	int i;
	Con_Printf("livelight: %d overrides\n", s_override_count);
	for (i = 0; i < s_override_count; i++) {
		livelight_override_t *o = &s_overrides[i];
		Con_Printf("  [%d] owner=%d pos=(%g %g %g) r=%g c=(%g %g %g)\n",
		           i, o->owner, o->pos[0], o->pos[1], o->pos[2],
		           o->radius, o->color[0], o->color[1], o->color[2]);
	}
}

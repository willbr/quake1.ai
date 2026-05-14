/*
r_decals.c — per-surface stain decals (bullet holes, blood, scorch, blood pools).
See docs/superpowers/specs/2026-05-13-decals-design.md.
*/
#include "quakedef.h"
#include "r_local.h"

static void R_DecalsTest_f (void);       /* forward decl — defined later in this file */
static void R_DecalsTestGrid_f (void);   /* forward decl — defined later in this file */

// Cvars (registered in R_DecalsInit).
cvar_t r_decals                    = { "r_decals",                    "1", true };
cvar_t r_decals_max                = { "r_decals_max",                "512", true };
cvar_t r_decals_intensity          = { "r_decals_intensity",          "1.0", true };
cvar_t r_decals_bloodpool          = { "r_decals_bloodpool",          "1", true };
cvar_t r_decals_bloodpool_radius   = { "r_decals_bloodpool_radius",   "24", true };
cvar_t r_decals_bloodpool_growtime = { "r_decals_bloodpool_growtime", "3.0", true };
cvar_t r_decals_debug              = { "r_decals_debug",              "0", false };

void R_DecalsInit (void)
{
	Cvar_RegisterVariable (&r_decals);
	Cvar_RegisterVariable (&r_decals_max);
	Cvar_RegisterVariable (&r_decals_intensity);
	Cvar_RegisterVariable (&r_decals_bloodpool);
	Cvar_RegisterVariable (&r_decals_bloodpool_radius);
	Cvar_RegisterVariable (&r_decals_bloodpool_growtime);
	Cvar_RegisterVariable (&r_decals_debug);
	Cmd_AddCommand ("r_decals_test", R_DecalsTest_f);
	Cmd_AddCommand ("r_decals_test_grid", R_DecalsTestGrid_f);
}

// ---------------------------------------------------------------------------
// Stain pool: fixed-size slots allocated at map load, LRU-evicted on overflow.
// Each slot holds a stain_t header plus an int16_t rgb[18*18*3] payload.
// ---------------------------------------------------------------------------

#define STAIN_MAX_LUXELS_DIM 18  // matches blocklights[18*18] cap in r_surf.c
#define STAIN_PAYLOAD_INT16  (STAIN_MAX_LUXELS_DIM * STAIN_MAX_LUXELS_DIM * 3)

typedef struct stain_slot_s {
	stain_t          header;
	short            payload[STAIN_PAYLOAD_INT16];
	struct stain_slot_s *free_next;  // free-list link (NULL when in use)
} stain_slot_t;

static stain_slot_t *r_stain_slots    = NULL;
static stain_slot_t *r_stain_freelist = NULL;
static int           r_stain_capacity = 0;
static int           r_stain_count    = 0;

// LRU list, head = most-recently-touched, tail = oldest.
static stain_t *r_stain_lru_head = NULL;
static stain_t *r_stain_lru_tail = NULL;

static void Stain_LRU_Unlink (stain_t *s)
{
	if (s->lru_prev) s->lru_prev->lru_next = s->lru_next;
	else             r_stain_lru_head = s->lru_next;
	if (s->lru_next) s->lru_next->lru_prev = s->lru_prev;
	else             r_stain_lru_tail = s->lru_prev;
	s->lru_prev = s->lru_next = NULL;
}

static void Stain_LRU_PushHead (stain_t *s)
{
	s->lru_prev = NULL;
	s->lru_next = r_stain_lru_head;
	if (r_stain_lru_head) r_stain_lru_head->lru_prev = s;
	r_stain_lru_head = s;
	if (!r_stain_lru_tail) r_stain_lru_tail = s;
}

static void Stain_LRU_Touch (stain_t *s)
{
	if (r_stain_lru_head == s) return;
	Stain_LRU_Unlink (s);
	Stain_LRU_PushHead (s);
}

static void Stain_FreeSlot (stain_slot_t *slot)
{
	if (slot->header.surf) {
		// Setting stain to NULL forces a cache mismatch (cache->stain_gen != 0)
		// on next render, so the surface rebuilds without the stain.
		slot->header.surf->stain = NULL;
	}
	Stain_LRU_Unlink (&slot->header);
	memset (&slot->header, 0, sizeof(slot->header));
	memset (slot->payload, 0, sizeof(slot->payload));
	slot->free_next = r_stain_freelist;
	r_stain_freelist = slot;
	r_stain_count--;
}

static stain_slot_t *Stain_AllocSlot (msurface_t *surf)
{
	stain_slot_t *slot;
	stain_slot_t *victim;

	if (!r_stain_freelist) {
		// Evict tail (oldest).
		if (!r_stain_lru_tail) return NULL;
		victim = (stain_slot_t *)
			((byte *)r_stain_lru_tail - offsetof(stain_slot_t, header));
		Stain_FreeSlot (victim);
	}

	slot = r_stain_freelist;
	r_stain_freelist = slot->free_next;
	slot->free_next = NULL;
	r_stain_count++;

	{
		int smax = (surf->extents[0] >> 4) + 1;
		int tmax = (surf->extents[1] >> 4) + 1;
		if (smax > STAIN_MAX_LUXELS_DIM || tmax > STAIN_MAX_LUXELS_DIM) {
			// Surface lightmap exceeds the engine's blocklights cap — already
			// undefined behaviour upstream. Refuse to stain rather than overflow
			// our payload.
			slot->free_next = r_stain_freelist;
			r_stain_freelist = slot;
			r_stain_count--;
			return NULL;
		}
		slot->header.rgb = slot->payload;
		slot->header.smax = smax;
		slot->header.tmax = tmax;
	}
	slot->header.generation = 1;
	slot->header.last_touched_frame = r_framecount;
	slot->header.surf = surf;
	Stain_LRU_PushHead (&slot->header);

	surf->stain = &slot->header;
	return slot;
}

// Returns the stain for surf, allocating one if needed.
static stain_t *Stain_GetOrAlloc (msurface_t *surf)
{
	stain_slot_t *slot;
	if (surf->stain) {
		Stain_LRU_Touch (surf->stain);
		return surf->stain;
	}
	slot = Stain_AllocSlot (surf);
	return slot ? &slot->header : NULL;
}

// ---------------------------------------------------------------------------
// Blood pools — growing radial stains on the floor under a dying monster.
// ---------------------------------------------------------------------------

#define MAX_ACTIVE_BLOODPOOLS 32

typedef struct {
	vec3_t       origin;          // floor hit point (centre of pool)
	msurface_t  *surf;            // target floor surface
	float        spawn_time;      // cl.time at spawn
	float        radius_max;      // game units
	float        radius_painted;  // last radius painted
	qboolean     alive;
} bloodpool_t;

static bloodpool_t r_bloodpools[MAX_ACTIVE_BLOODPOOLS];

void R_DecalsClear (void)
{
	int i, cap;

	cap = (int)r_decals_max.value;
	if (cap < 16) cap = 16;
	if (cap > 4096) cap = 4096;

	// (Re)allocate the pool on every map load — hunk reset by Host_ClearMemory
	// frees the previous block automatically.
	r_stain_slots    = Hunk_AllocName (cap * sizeof(stain_slot_t), "stainpool");
	r_stain_capacity = cap;
	r_stain_count    = 0;
	r_stain_freelist = NULL;
	r_stain_lru_head = r_stain_lru_tail = NULL;

	for (i = cap - 1; i >= 0; i--) {
		r_stain_slots[i].free_next = r_stain_freelist;
		r_stain_freelist = &r_stain_slots[i];
	}

	memset (r_bloodpools, 0, sizeof(r_bloodpools));
}

// Per-frame growth tick for active blood pools.
void R_DecalsFrame (void)
{
	int          i;
	float        dur;
	float        t, target, r_inner_sq, r_outer_sq;
	int          luxel_radius, dx, dy, u, v, idx;
	float        gx, gy, dsq, ou, ov;
	int          olu, olv, smax, tmax;
	int          nr, ng, nb;
	bloodpool_t *bp;
	msurface_t  *surf;
	mtexinfo_t  *tex;
	stain_t     *st;

	dur = r_decals_bloodpool_growtime.value;
	if (dur <= 0.001f) dur = 0.001f;

	for (i = 0; i < MAX_ACTIVE_BLOODPOOLS; i++) {
		bp = &r_bloodpools[i];
		if (!bp->alive) continue;

		t = (cl.time - bp->spawn_time) / dur;
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;
		target = bp->radius_max * t;
		if (target <= bp->radius_painted) {
			if (t >= 1.0f) bp->alive = false;
			continue;
		}

		surf = bp->surf;
		if (!surf) { bp->alive = false; continue; }

		tex = surf->texinfo;
		ou  = DotProduct(bp->origin, tex->vecs[0]) + tex->vecs[0][3];
		ov  = DotProduct(bp->origin, tex->vecs[1]) + tex->vecs[1][3];
		olu = ((int)floor(ou) - surf->texturemins[0]) >> 4;
		olv = ((int)floor(ov) - surf->texturemins[1]) >> 4;
		smax = (surf->extents[0] >> 4) + 1;
		tmax = (surf->extents[1] >> 4) + 1;

		luxel_radius = (int)((target / 16.0f) + 1.0f);
		st = Stain_GetOrAlloc (surf);
		if (!st) { bp->alive = false; continue; }

		r_inner_sq = bp->radius_painted * bp->radius_painted;
		r_outer_sq = target * target;

		for (dy = -luxel_radius; dy <= luxel_radius; dy++) {
			v = olv + dy;
			if (v < 0 || v >= tmax) continue;
			for (dx = -luxel_radius; dx <= luxel_radius; dx++) {
				u = olu + dx;
				if (u < 0 || u >= smax) continue;
				gx  = dx * 16.0f;
				gy  = dy * 16.0f;
				dsq = gx*gx + gy*gy;
				if (dsq < r_inner_sq || dsq > r_outer_sq) continue;
				idx = (v * smax + u) * 3;
				// Blood guarantees a red tint regardless of previous stains:
				// floor R at +200 (positive boost), ceiling G/B at −100 (dim).
				// Pure accumulation would let a prior scorch's negative R
				// dominate and turn the pool black.
				nr  = st->rgb[idx + 0] + 200;
				if (nr < 200)   nr = 200;
				if (nr > 4096)  nr = 4096;
				ng  = st->rgb[idx + 1] - 100;
				if (ng > -100)  ng = -100;
				if (ng < -4096) ng = -4096;
				nb  = st->rgb[idx + 2] - 100;
				if (nb > -100)  nb = -100;
				if (nb < -4096) nb = -4096;
				st->rgb[idx + 0] = (short)nr;
				st->rgb[idx + 1] = (short)ng;
				st->rgb[idx + 2] = (short)nb;
			}
		}
		st->generation++;
		st->last_touched_frame = r_framecount;
		bp->radius_painted = target;
		if (t >= 1.0f) bp->alive = false;
	}
}

// ---------------------------------------------------------------------------
// Per-decal-type kernels and centre deltas. Indexed by decal_type_t.
// 3x3 falloff [1 2 1 / 2 4 2 / 1 2 1], norm 16.
// 5x5 wider falloff for scorch, norm 256.
// ---------------------------------------------------------------------------

static const int K3x3[9]  = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
static const int K5x5[25] = {
	 1,  4,  6,  4,  1,
	 4, 16, 24, 16,  4,
	 6, 24, 36, 24,  6,
	 4, 16, 24, 16,  4,
	 1,  4,  6,  4,  1,
};

/* Single-luxel solid used by r_decals_test to show the luxel grid resolution. */
static const int K1x1_solid[1] = { 1 };

typedef struct {
	const int *k;
	int        ksize;
	int        knorm;
	int        dr, dg, db;
} decal_kernel_t;

static const decal_kernel_t decal_kernels[DECAL_NUM_TYPES] = {
	/* DECAL_BULLET      */ { K1x1_solid, 1,  1, -150, -150, -150 },
	/* DECAL_SPIKE       */ { K1x1_solid, 1,  1, -150, -150, -150 },
	/* DECAL_BLOOD_SPLAT */ { K3x3, 3,  16, +60, -40, -40 },
	/* DECAL_SCORCH      */ { K5x5, 5, 36, -200, -200, -200 },
	/* DECAL_LIGHTNING   */ { K3x3, 3,  16, -50, -60, -40 },
};

// ---------------------------------------------------------------------------
// Internal: paint a single luxel kernel into a surface's stain buffer.
// kernel is a 3x3 or 5x5 (ksize) weight grid summing to ~16 (3x3) or ~256 (5x5).
// dr/dg/db are the centre-delta values; per-luxel delta = (centre * weight) / norm.
// ---------------------------------------------------------------------------
static void Stain_PaintKernel (msurface_t *surf, int lu, int lv,
                               int dr, int dg, int db,
                               const int *kernel, int ksize, int knorm)
{
	stain_t *st;
	int      half, kx, ky, u, v, w, idx, nr, ng, nb;

	st = Stain_GetOrAlloc (surf);
	if (!st) return;

	half = ksize / 2;
	for (ky = 0; ky < ksize; ky++) {
		v = lv + ky - half;
		if (v < 0 || v >= st->tmax) continue;
		for (kx = 0; kx < ksize; kx++) {
			u = lu + kx - half;
			if (u < 0 || u >= st->smax) continue;
			w = kernel[ky * ksize + kx];
			if (!w) continue;
			idx = (v * st->smax + u) * 3;
			nr = st->rgb[idx + 0] + (dr * w) / knorm;
			ng = st->rgb[idx + 1] + (dg * w) / knorm;
			nb = st->rgb[idx + 2] + (db * w) / knorm;
			if (nr < -4096) nr = -4096; else if (nr > 4096) nr = 4096;
			if (ng < -4096) ng = -4096; else if (ng > 4096) ng = 4096;
			if (nb < -4096) nb = -4096; else if (nb > 4096) nb = 4096;
			st->rgb[idx + 0] = (short)nr;
			st->rgb[idx + 1] = (short)ng;
			st->rgb[idx + 2] = (short)nb;
		}
	}
	st->generation++;
	st->last_touched_frame = r_framecount;
}

// Forward decl: definition lives below.
static msurface_t *R_PointOnSurface_World (vec3_t p, vec3_t normal, float max_plane_dist);

// Internal: project cell_world into `target`'s lightmap UV and apply one kernel
// cell. Skips silently if the cell falls outside target's lightmap rectangle.
static void Stain_AddCell (msurface_t *target, vec3_t cell_world,
                            int w, int dr, int dg, int db, int knorm)
{
	mtexinfo_t *ttex;
	stain_t    *st;
	float       u, v;
	int         tlu, tlv, tsmax, ttmax, idx, nr, ng, nb;

	ttex = target->texinfo;
	u = DotProduct(cell_world, ttex->vecs[0]) + ttex->vecs[0][3];
	v = DotProduct(cell_world, ttex->vecs[1]) + ttex->vecs[1][3];
	tlu = ((int)floor(u) - target->texturemins[0]) >> 4;
	tlv = ((int)floor(v) - target->texturemins[1]) >> 4;
	tsmax = (target->extents[0] >> 4) + 1;
	ttmax = (target->extents[1] >> 4) + 1;
	if (r_decals_debug.value >= 2) {
		Con_Printf ("    -> surf=%p uv=%.1f %.1f tlu=%d tlv=%d sz=%dx%d tmins=%d %d ext=%d %d\n",
			(void*)target, u, v, tlu, tlv, tsmax, ttmax,
			(int)target->texturemins[0], (int)target->texturemins[1],
			(int)target->extents[0], (int)target->extents[1]);
	}
	if (tlu < 0 || tlu >= tsmax || tlv < 0 || tlv >= ttmax) return;

	st = Stain_GetOrAlloc (target);
	if (!st) return;

	idx = (tlv * st->smax + tlu) * 3;
	nr = st->rgb[idx + 0] + (dr * w) / knorm;
	ng = st->rgb[idx + 1] + (dg * w) / knorm;
	nb = st->rgb[idx + 2] + (db * w) / knorm;
	if (nr < -4096) nr = -4096; else if (nr > 4096) nr = 4096;
	if (ng < -4096) ng = -4096; else if (ng > 4096) ng = 4096;
	if (nb < -4096) nb = -4096; else if (nb > 4096) nb = 4096;
	st->rgb[idx + 0] = (short)nr;
	st->rgb[idx + 1] = (short)ng;
	st->rgb[idx + 2] = (short)nb;
	st->generation++;
	st->last_touched_frame = r_framecount;
}

// Paint a kernel whose centre is at world-space point `center` and whose UV
// basis comes from `primary`. Each cell is converted to world coordinates by
// stepping along the primary surface's texture axes, then R_PointOnSurface_World
// dispatches the paint to whichever coplanar world surface actually contains
// that point. Cells over no surface (an open void past a face edge) drop.
// This is what lets a scorch bleed across coplanar face boundaries.
static void Stain_PaintKernel_World (vec3_t center, msurface_t *primary,
                                     int dr, int dg, int db,
                                     const int *kernel, int ksize, int knorm)
{
	mtexinfo_t *tex = primary->texinfo;
	vec3_t      step_u, step_v;
	float       ulen2, vlen2;
	int         half, kx, ky, w;
	int         sx, sy;
	int         i;

	ulen2 = tex->vecs[0][0]*tex->vecs[0][0]
	      + tex->vecs[0][1]*tex->vecs[0][1]
	      + tex->vecs[0][2]*tex->vecs[0][2];
	vlen2 = tex->vecs[1][0]*tex->vecs[1][0]
	      + tex->vecs[1][1]*tex->vecs[1][1]
	      + tex->vecs[1][2]*tex->vecs[1][2];
	if (ulen2 < 1e-6f || vlen2 < 1e-6f) return;

	// Inverse of the world→UV projection: world step per 16-UV-unit (luxel).
	for (i = 0; i < 3; i++) {
		step_u[i] = tex->vecs[0][i] * (16.0f / ulen2);
		step_v[i] = tex->vecs[1][i] * (16.0f / vlen2);
	}

	{
		model_t  *world = cl.worldmodel;
		mplane_t *primary_plane = primary->plane;

		half = ksize / 2;
		for (ky = 0; ky < ksize; ky++) {
			sy = ky - half;
			for (kx = 0; kx < ksize; kx++) {
				vec3_t cell_world;
				int    si;
				int    painted = 0;

				w = kernel[ky * ksize + kx];
				if (!w) continue;
				sx = kx - half;

				cell_world[0] = center[0] + sx * step_u[0] + sy * step_v[0];
				cell_world[1] = center[1] + sx * step_u[1] + sy * step_v[1];
				cell_world[2] = center[2] + sx * step_u[2] + sy * step_v[2];

				// Paint EVERY coplanar world face whose UV bounds contain
				// this cell. Returning a single "best" match leaves edge
				// luxels on one side of a BSP boundary unpainted whenever
				// iteration order picks the wrong tie-breaker.
				for (si = 0; si < world->numsurfaces; si++) {
					msurface_t *s = &world->surfaces[si];
					mplane_t   *pl = s->plane;
					mtexinfo_t *stex;
					float       d, ad, u, v;

					if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED)) continue;
					// Tight 4-unit plane tolerance: only strict coplanar
					// faces match. Looser tolerances paint on recessed /
					// offset walls that are visually hidden behind the
					// impact wall's edge, creating apparent gaps.
					d  = DotProduct(cell_world, pl->normal) - pl->dist;
					if (s->flags & SURF_PLANEBACK) d = -d;
					ad = d < 0 ? -d : d;
					if (ad > 4.0f) continue;
					// Always check outward facing — a face sharing the same
					// mplane_t pointer can still be the back-side of the wall
					// (opposite SURF_PLANEBACK). Painting the back-side stains
					// a hidden surface and looks like a gap on the visible one.
					{
						float nd = DotProduct(pl->normal, primary_plane->normal);
						if (s->flags & SURF_PLANEBACK)        nd = -nd;
						if (primary->flags & SURF_PLANEBACK)  nd = -nd;
						if (nd < 0.5f) continue;
					}
					// UV bounds contain the cell
					stex = s->texinfo;
					u = DotProduct(cell_world, stex->vecs[0]) + stex->vecs[0][3];
					v = DotProduct(cell_world, stex->vecs[1]) + stex->vecs[1][3];
					if (u < s->texturemins[0] || u > s->texturemins[0] + s->extents[0]) continue;
					if (v < s->texturemins[1] || v > s->texturemins[1] + s->extents[1]) continue;

					Stain_AddCell (s, cell_world, w, dr, dg, db, knorm);
					painted++;
				}

				if (r_decals_debug.value >= 2) {
					Con_Printf ("  cell sx=%d sy=%d w=%d painted=%d at %.1f %.1f %.1f\n",
						sx, sy, w, painted,
						cell_world[0], cell_world[1], cell_world[2]);
				}
			}
		}
	}
}

// Walk the world BSP to find the surface that point P lies on (or near).
// Uses the surface plane and a caller-supplied tolerance. Returns NULL if no match.
static msurface_t *R_PointOnSurface_World (vec3_t p, vec3_t normal, float max_plane_dist)
{
	model_t    *world;
	msurface_t *best, *s;
	mplane_t   *pl;
	mtexinfo_t *tex;
	float       best_d, d, ad, nd, u, v;
	int         i;

	world = cl.worldmodel;
	if (!world) return NULL;

	best   = NULL;
	best_d = max_plane_dist;

	for (i = 0; i < world->numsurfaces; i++) {
		s  = &world->surfaces[i];
		pl = s->plane;
		d  = DotProduct(p, pl->normal) - pl->dist;
		if (s->flags & SURF_PLANEBACK) d = -d;
		ad = d < 0 ? -d : d;
		if (ad > best_d) continue;
		if (normal) {
			nd = DotProduct(normal, pl->normal);
			if (s->flags & SURF_PLANEBACK) nd = -nd;
			if (nd < 0.5f) continue;
		}
		if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED)) continue;

		tex = s->texinfo;
		u = DotProduct(p, tex->vecs[0]) + tex->vecs[0][3];
		v = DotProduct(p, tex->vecs[1]) + tex->vecs[1][3];
		if (u < s->texturemins[0] || u > s->texturemins[0] + s->extents[0]) continue;
		if (v < s->texturemins[1] || v > s->texturemins[1] + s->extents[1]) continue;

		best   = s;
		best_d = ad;
	}
	return best;
}

// Dev command: paint a solid black 5x5 decal at the spot the player is looking at.
// Usage: r_decals_test
static void R_DecalsTest_f (void)
{
	vec3_t      forward, right, up, end;
	trace_t     tr;
	msurface_t *surf;
	mtexinfo_t *tex;
	float       u, v;
	int         lu, lv, smax, tmax;

	AngleVectors (r_refdef.viewangles, forward, right, up);
	(void)right; (void)up;
	VectorMA (r_refdef.vieworg, 1024.0f, forward, end);

	tr = SV_Move (r_refdef.vieworg, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) {
		Con_Printf ("r_decals_test: nothing in front\n");
		return;
	}

	surf = R_PointOnSurface_World (tr.endpos, NULL, 4.0f);
	if (!surf) {
		Con_Printf ("r_decals_test: no world surface at hit\n");
		return;
	}

	tex = surf->texinfo;
	u = DotProduct(tr.endpos, tex->vecs[0]) + tex->vecs[0][3];
	v = DotProduct(tr.endpos, tex->vecs[1]) + tex->vecs[1][3];
	lu = ((int)floor(u) - surf->texturemins[0]) >> 4;
	lv = ((int)floor(v) - surf->texturemins[1]) >> 4;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	if (lu < 0 || lu >= smax || lv < 0 || lv >= tmax) {
		Con_Printf ("r_decals_test: luxel out of bounds (%d,%d) of %dx%d\n",
			lu, lv, smax, tmax);
		return;
	}

	Stain_PaintKernel (surf, lu, lv, -4096, -4096, -4096, K1x1_solid, 1, 1);
	Con_Printf ("r_decals_test: single luxel black at (%d,%d) of surf %p at %.1f %.1f %.1f\n",
		lu, lv, (void*)surf, tr.endpos[0], tr.endpos[1], tr.endpos[2]);
}

// Dev command: paint a 5x5 grid where every cell has a unique RGB delta.
// Painted luxels show as colors that vary across the grid; any luxel inside
// the grid footprint that renders as the gray base instead is a gap.
// Requires r_lightmap 1 and r_coloredlight 1 for clean visualization.
// Usage: r_decals_test_grid
static void R_DecalsTestGrid_f (void)
{
	vec3_t      forward, right, up, end;
	trace_t     tr;
	msurface_t *primary;
	mtexinfo_t *tex;
	float       ulen2, vlen2;
	vec3_t      step_u, step_v;
	int         sx, sy, i;

	AngleVectors (r_refdef.viewangles, forward, right, up);
	(void)right; (void)up;
	VectorMA (r_refdef.vieworg, 1024.0f, forward, end);

	tr = SV_Move (r_refdef.vieworg, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) {
		Con_Printf ("r_decals_test_grid: nothing in front\n");
		return;
	}

	primary = R_PointOnSurface_World (tr.endpos, NULL, 4.0f);
	if (!primary) {
		Con_Printf ("r_decals_test_grid: no world surface at hit\n");
		return;
	}

	tex = primary->texinfo;
	ulen2 = tex->vecs[0][0]*tex->vecs[0][0] + tex->vecs[0][1]*tex->vecs[0][1] + tex->vecs[0][2]*tex->vecs[0][2];
	vlen2 = tex->vecs[1][0]*tex->vecs[1][0] + tex->vecs[1][1]*tex->vecs[1][1] + tex->vecs[1][2]*tex->vecs[1][2];
	if (ulen2 < 1e-6f || vlen2 < 1e-6f) return;
	for (i = 0; i < 3; i++) {
		step_u[i] = tex->vecs[0][i] * (16.0f / ulen2);
		step_v[i] = tex->vecs[1][i] * (16.0f / vlen2);
	}

	Con_Printf ("r_decals_test_grid: primary surf=%p rgb_samples=%s\n",
		(void*)primary, primary->rgb_samples ? "yes (RGB path)" : "NO (mono path)");

	// One 1x1 paint per cell with a unique (dr,dg,db) so each painted luxel
	// has a distinguishable colour. Requires the impacted surface to have
	// rgb_samples (.lit data) for the colour variation to be visible; the
	// mono lightmap path collapses dr/dg/db into a luminance step.
	for (sy = -2; sy <= 2; sy++) {
		for (sx = -2; sx <= 2; sx++) {
			vec3_t cell_world;
			int    dr, dg, db;
			cell_world[0] = tr.endpos[0] + sx * step_u[0] + sy * step_v[0];
			cell_world[1] = tr.endpos[1] + sx * step_u[1] + sy * step_v[1];
			cell_world[2] = tr.endpos[2] + sx * step_u[2] + sy * step_v[2];

			dr = -800 + (sx + 2) * 400;  // -800, -400, 0, +400, +800
			dg = -800 + (sy + 2) * 400;
			db = -400 + ((sx + sy) & 1) * 800;

			Stain_PaintKernel_World (cell_world, primary, dr, dg, db, K1x1_solid, 1, 1);
		}
	}
	Con_Printf ("r_decals_test_grid: painted 5x5 rainbow at %.1f %.1f %.1f\n",
		tr.endpos[0], tr.endpos[1], tr.endpos[2]);
}

void R_SpawnDecal (vec3_t pos, decal_type_t type)
{
	msurface_t *surf;
	const decal_kernel_t *dk;
	vec3_t      hit_buf;  // holds a swept hit point so pos can be re-aimed

	if (!r_decals.value) return;
	if (type < 0 || type >= DECAL_NUM_TYPES) return;

	// Bullets / spikes: pos is the server trace endpoint, already on the
	// surface plane — 4-unit tolerance handles float-precision slop.
	surf = R_PointOnSurface_World (pos, NULL, 4.0f);

	// Explosions: pos is the missile origin at detonation time, which sits
	// ~16-32 units off any wall because the missile has a bbox. If the direct
	// surface lookup whiffs, sweep outward in 6 axis directions up to 64
	// units to find the nearest world surface, then stamp on it.
	if (!surf) {
		static const float dirs[6][3] = {
			{  1, 0, 0 }, { -1, 0, 0 },
			{  0, 1, 0 }, {  0,-1, 0 },
			{  0, 0, 1 }, {  0, 0,-1 },
		};
		float       best_len = 64.0f + 1.0f;
		msurface_t *best     = NULL;
		int         d;
		for (d = 0; d < 6; d++) {
			vec3_t      end;
			trace_t     tr;
			msurface_t *cand;
			float       len;
			end[0] = pos[0] + 64.0f * dirs[d][0];
			end[1] = pos[1] + 64.0f * dirs[d][1];
			end[2] = pos[2] + 64.0f * dirs[d][2];
			tr = SV_Move (pos, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
			if (tr.fraction >= 1.0f || tr.allsolid) continue;
			len = tr.fraction * 64.0f;
			if (len >= best_len) continue;
			cand = R_PointOnSurface_World (tr.endpos, NULL, 4.0f);
			if (!cand) continue;
			best     = cand;
			best_len = len;
			hit_buf[0] = tr.endpos[0];
			hit_buf[1] = tr.endpos[1];
			hit_buf[2] = tr.endpos[2];
		}
		if (best) {
			surf = best;
			pos  = hit_buf;  // function-scope buffer keeps the hit alive
		}
	}

	if (r_decals_debug.value) {
		Con_Printf ("R_SpawnDecal: pos=%.2f %.2f %.2f type=%d surf=%p\n",
			pos[0], pos[1], pos[2], (int)type, (void*)surf);
	}
	if (!surf) return;

	dk = &decal_kernels[type];
	Stain_PaintKernel_World (pos, surf, dk->dr, dk->dg, dk->db, dk->k, dk->ksize, dk->knorm);
	if (r_decals_debug.value) {
		Con_Printf ("  painted kernel %d (dr=%d dg=%d db=%d)\n",
			dk->ksize, dk->dr, dk->dg, dk->db);
	}
}

void R_SpawnBloodPool (vec3_t origin)
{
	vec3_t   end;
	trace_t  tr;
	msurface_t *surf;
	int      i, slot;
	float    oldest_time;
	int      oldest_slot;
	bloodpool_t *bp;

	if (!r_decals.value || !r_decals_bloodpool.value) return;

	// Trace straight down to find the floor.
	end[0] = origin[0];
	end[1] = origin[1];
	end[2] = origin[2] - 64.0f;
	tr = SV_Move (origin, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) return;
	if (tr.plane.normal[2] < 0.7f) return;  // too steep

	surf = R_PointOnSurface_World (tr.endpos, tr.plane.normal, 4.0f);
	if (!surf) return;

	// Find an empty slot or recycle the oldest.
	slot         = -1;
	oldest_time  = 1e9f;
	oldest_slot  = 0;
	for (i = 0; i < MAX_ACTIVE_BLOODPOOLS; i++) {
		if (!r_bloodpools[i].alive) { slot = i; break; }
		if (r_bloodpools[i].spawn_time < oldest_time) {
			oldest_time = r_bloodpools[i].spawn_time;
			oldest_slot = i;
		}
	}
	if (slot < 0) {
		// Force the oldest to finish instantly; its luxels stay permanent.
		bp = &r_bloodpools[oldest_slot];
		bp->radius_painted = bp->radius_max;
		bp->alive          = false;
		slot               = oldest_slot;
	}

	bp = &r_bloodpools[slot];
	bp->origin[0]      = tr.endpos[0];
	bp->origin[1]      = tr.endpos[1];
	bp->origin[2]      = tr.endpos[2];
	bp->surf           = surf;
	bp->spawn_time     = cl.time;
	bp->radius_max     = r_decals_bloodpool_radius.value;
	bp->radius_painted = 0.0f;
	bp->alive          = true;
}

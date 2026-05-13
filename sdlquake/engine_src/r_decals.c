/*
r_decals.c — per-surface stain decals (bullet holes, blood, scorch, blood pools).
See docs/superpowers/specs/2026-05-13-decals-design.md.
*/
#include "quakedef.h"
#include "r_local.h"

static void R_DecalsTest_f (void);  /* forward decl — defined later in this file */

// Cvars (registered in R_DecalsInit).
cvar_t r_decals                    = { "r_decals",                    "1", true };
cvar_t r_decals_max                = { "r_decals_max",                "512", true };
cvar_t r_decals_intensity          = { "r_decals_intensity",          "1.0", true };
cvar_t r_decals_bloodpool          = { "r_decals_bloodpool",          "1", true };
cvar_t r_decals_bloodpool_radius   = { "r_decals_bloodpool_radius",   "24", true };
cvar_t r_decals_bloodpool_growtime = { "r_decals_bloodpool_growtime", "3.0", true };

void R_DecalsInit (void)
{
	Cvar_RegisterVariable (&r_decals);
	Cvar_RegisterVariable (&r_decals_max);
	Cvar_RegisterVariable (&r_decals_intensity);
	Cvar_RegisterVariable (&r_decals_bloodpool);
	Cvar_RegisterVariable (&r_decals_bloodpool_radius);
	Cvar_RegisterVariable (&r_decals_bloodpool_growtime);
	Cmd_AddCommand ("r_decals_test", R_DecalsTest_f);
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
		// Bump generation on the orphaned surface so the cache rebuilds without the stain.
		slot->header.surf->cached_stain_gen = -1;  // force mismatch on next check
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

	slot->header.rgb = slot->payload;
	slot->header.smax = (surf->extents[0] >> 4) + 1;
	slot->header.tmax = (surf->extents[1] >> 4) + 1;
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
				nr  = st->rgb[idx + 0] + 80;
				ng  = st->rgb[idx + 1] - 60;
				nb  = st->rgb[idx + 2] - 60;
				if (nr > 4096)  nr = 4096;
				if (ng < -4096) ng = -4096;
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

typedef struct {
	const int *k;
	int        ksize;
	int        knorm;
	int        dr, dg, db;
} decal_kernel_t;

static const decal_kernel_t decal_kernels[DECAL_NUM_TYPES] = {
	/* DECAL_BULLET      */ { K3x3, 3,  16, -40, -40, -40 },
	/* DECAL_SPIKE       */ { K3x3, 3,  16, -40, -40, -40 },
	/* DECAL_BLOOD_SPLAT */ { K3x3, 3,  16, +60, -40, -40 },
	/* DECAL_SCORCH      */ { K5x5, 5, 256, -80, -80, -80 },
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

// Walk the world BSP to find the surface that point P lies on (or near).
// Uses the surface plane and a small tolerance. Returns NULL if no match.
static msurface_t *R_PointOnSurface_World (vec3_t p, vec3_t normal)
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
	best_d = 4.0f;  // max plane-distance tolerance (game units)

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

// Dev command: spawn a decal at the spot the player is looking at.
// Usage: r_decals_test [bullet|spike|blood|scorch|lightning]
static void R_DecalsTest_f (void)
{
	vec3_t  forward, right, up, end;
	trace_t tr;
	decal_type_t type = DECAL_BULLET;

	AngleVectors (r_refdef.viewangles, forward, right, up);
	(void)right; (void)up;
	VectorMA (r_refdef.vieworg, 1024.0f, forward, end);

	memset (&tr, 0, sizeof(tr));
	tr = SV_Move (r_refdef.vieworg, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) {
		Con_Printf ("r_decals_test: nothing in front\n");
		return;
	}

	if (Cmd_Argc() > 1) {
		char *t = Cmd_Argv(1);
		if      (!Q_strcasecmp(t, "bullet"))    type = DECAL_BULLET;
		else if (!Q_strcasecmp(t, "spike"))     type = DECAL_SPIKE;
		else if (!Q_strcasecmp(t, "blood"))     type = DECAL_BLOOD_SPLAT;
		else if (!Q_strcasecmp(t, "scorch"))    type = DECAL_SCORCH;
		else if (!Q_strcasecmp(t, "lightning")) type = DECAL_LIGHTNING;
	}

	R_SpawnDecal (tr.endpos, type);
	Con_Printf ("r_decals_test: spawned type %d at %.1f %.1f %.1f\n",
		(int)type, tr.endpos[0], tr.endpos[1], tr.endpos[2]);
}

// Fire a short trace from `pos` along several directions; return the closest
// world surface hit within 8 game units. Returns NULL if none.
// Also writes the hit point and surface normal back out.
static msurface_t *Retrace_ForDecal (vec3_t pos, vec3_t out_hit, vec3_t out_normal)
{
	vec3_t dirs[7];
	int    ndirs = 7;
	int    i;
	msurface_t *best;
	float       best_len;
	vec3_t      best_hit, best_nrm;
	trace_t     tr;
	vec3_t      end;
	float       len;
	float       elen;
	extern server_t sv;

	/* Axis-aligned probe directions */
	dirs[1][0] =  1; dirs[1][1] =  0; dirs[1][2] =  0;
	dirs[2][0] = -1; dirs[2][1] =  0; dirs[2][2] =  0;
	dirs[3][0] =  0; dirs[3][1] =  1; dirs[3][2] =  0;
	dirs[4][0] =  0; dirs[4][1] = -1; dirs[4][2] =  0;
	dirs[5][0] =  0; dirs[5][1] =  0; dirs[5][2] =  1;
	dirs[6][0] =  0; dirs[6][1] =  0; dirs[6][2] = -1;

	/* First direction: eye -> pos (back toward the surface that was hit) */
	VectorSubtract (r_refdef.vieworg, pos, dirs[0]);
	elen = VectorLength (dirs[0]);
	if (elen < 0.001f) {
		dirs[0][0] = 1; dirs[0][1] = 0; dirs[0][2] = 0;
	} else {
		dirs[0][0] /= elen;
		dirs[0][1] /= elen;
		dirs[0][2] /= elen;
	}

	best     = NULL;
	best_len = 8.0f;

	for (i = 0; i < ndirs; i++) {
		VectorMA (pos, 8.0f, dirs[i], end);
		tr = SV_Move (pos, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
		if (tr.fraction >= 1.0f || tr.allsolid) continue;

		len = tr.fraction * 8.0f;
		if (len >= best_len) continue;

		/* Only stamp world geometry, not brush entities */
		if (tr.ent != NULL && tr.ent != sv.edicts) continue;

		{
			msurface_t *s = R_PointOnSurface_World (tr.endpos, tr.plane.normal);
			if (!s) continue;

			best = s;
			best_len = len;
			VectorCopy (tr.endpos,       best_hit);
			VectorCopy (tr.plane.normal, best_nrm);
		}
	}

	if (best) {
		if (out_hit)    VectorCopy (best_hit, out_hit);
		if (out_normal) VectorCopy (best_nrm, out_normal);
	}
	return best;
}

void R_SpawnDecal (vec3_t pos, decal_type_t type)
{
	vec3_t      hit;
	msurface_t *surf;
	mtexinfo_t *tex;
	float       u, v;
	int         lu, lv, smax, tmax;
	const decal_kernel_t *dk;

	if (!r_decals.value) return;
	if (type < 0 || type >= DECAL_NUM_TYPES) return;

	surf = Retrace_ForDecal (pos, hit, NULL);
	if (!surf) return;

	tex = surf->texinfo;
	u = DotProduct(hit, tex->vecs[0]) + tex->vecs[0][3];
	v = DotProduct(hit, tex->vecs[1]) + tex->vecs[1][3];
	lu = ((int)floor(u) - surf->texturemins[0]) >> 4;
	lv = ((int)floor(v) - surf->texturemins[1]) >> 4;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	if (lu < 0 || lu >= smax || lv < 0 || lv >= tmax) return;

	dk = &decal_kernels[type];
	Stain_PaintKernel (surf, lu, lv, dk->dr, dk->dg, dk->db, dk->k, dk->ksize, dk->knorm);
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

	surf = R_PointOnSurface_World (tr.endpos, tr.plane.normal);
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

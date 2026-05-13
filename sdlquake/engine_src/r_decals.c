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
}

void R_DecalsFrame (void)
{
	// Filled in Task 11.
}

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

// Dev command: spawn a bullet-style decal at the spot the player is looking at.
// Usage: r_decals_test (defaults to bullet).
// Walks 1024 units forward and slaps a stain on whatever's there.
static void R_DecalsTest_f (void)
{
	vec3_t  forward, right, up, end, spawn_pos;
	trace_t tr;
	msurface_t *surf;
	mtexinfo_t *tex;
	float   u, vv;
	int     lu, lv, smax, tmax;
	static const int K3x3[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };

	AngleVectors (r_refdef.viewangles, forward, right, up);
	(void)right; (void)up;
	VectorMA (r_refdef.vieworg, 1024.0f, forward, end);

	memset (&tr, 0, sizeof(tr));
	tr = SV_Move (r_refdef.vieworg, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) {
		Con_Printf ("r_decals_test: nothing in front\n");
		return;
	}

	// Step 1 unit back along the trace so the projection lands on the surface side.
	VectorMA (tr.endpos, -1.0f, forward, spawn_pos);

	surf = R_PointOnSurface_World (tr.endpos, tr.plane.normal);
	if (!surf) {
		Con_Printf ("r_decals_test: no world surface at hit point\n");
		return;
	}

	tex = surf->texinfo;
	u  = DotProduct(tr.endpos, tex->vecs[0]) + tex->vecs[0][3];
	vv = DotProduct(tr.endpos, tex->vecs[1]) + tex->vecs[1][3];
	lu = ((int)floor(u)  - surf->texturemins[0]) >> 4;
	lv = ((int)floor(vv) - surf->texturemins[1]) >> 4;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;
	if (lu < 0 || lu >= smax || lv < 0 || lv >= tmax) {
		Con_Printf ("r_decals_test: luxel out of bounds (%d,%d) in %dx%d\n",
			lu, lv, smax, tmax);
		return;
	}

	Stain_PaintKernel (surf, lu, lv, -120, -120, -120, K3x3, 3, 16);
	Con_Printf ("r_decals_test: stained luxel (%d,%d) of surface %p\n",
		lu, lv, (void*)surf);
	(void)spawn_pos;
}

void R_SpawnDecal (vec3_t pos, decal_type_t type)
{
	(void)pos; (void)type;
	// Filled in Task 8.
}

void R_SpawnBloodPool (vec3_t origin)
{
	(void)origin;
	// Filled in Task 11.
}

/*
r_decals.c — per-surface stain decals (bullet holes, blood, scorch, blood pools).
See docs/superpowers/specs/2026-05-13-decals-design.md.
*/
#include "quakedef.h"
#include "r_local.h"

static void R_DecalsTest_f (void);       /* forward decl — defined later in this file */
static void R_DecalsTestGrid_f (void);   /* forward decl — defined later in this file */
static void R_DecalsTestPool_f (void);   /* forward decl — defined later in this file */
/* Forward decl: defined later. The drip tick in R_DecalsFrame paints
   via this, so it appears before its definition. */
static void Stain_AddCell (msurface_t *target, vec3_t cell_world,
                            int w, int dr, int dg, int db, int knorm);

// Cvars (registered in R_DecalsInit).
cvar_t r_decals                    = { "r_decals",                    "1", true };
cvar_t r_decals_max                = { "r_decals_max",                "512", true };
cvar_t r_decals_intensity          = { "r_decals_intensity",          "1.0", true };
cvar_t r_decals_bloodpool             = { "r_decals_bloodpool",             "1", true };
cvar_t r_decals_bloodpool_radius      = { "r_decals_bloodpool_radius",      "64", true };
// Growth rate in world units per second. A corpse pool of radius 64 finishes
// in ~10s; a gib pool of radius 12 finishes in ~1.8s. This scales the freeze
// window with pool size — big pools give the player time to disturb them,
// small pools resolve quickly so the world doesn't feel half-finished.
cvar_t r_decals_bloodpool_growrate    = { "r_decals_bloodpool_growrate",    "6.5", true };
cvar_t r_decals_bloodpool_owner_drift = { "r_decals_bloodpool_owner_drift", "4", true };
cvar_t r_decals_gibbounce          = { "r_decals_gibbounce",          "1", true };
cvar_t r_decals_blooddrip          = { "r_decals_blooddrip",          "1", true };
cvar_t r_decals_blooddrip_length   = { "r_decals_blooddrip_length",   "48", true };
cvar_t r_decals_blooddrip_growtime = { "r_decals_blooddrip_growtime", "1.5", true };
cvar_t r_decals_debug              = { "r_decals_debug",              "0", false };
cvar_t r_decals_blood_spatter      = { "r_decals_blood_spatter",      "1", true };
cvar_t r_decals_blood_spatter_size = { "r_decals_blood_spatter_size", "3", true };

/* Forward decl: defined in the kernel section below. */
static void DecalKernels_Init (void);

void R_DecalsInit (void)
{
	DecalKernels_Init ();
	Cvar_RegisterVariable (&r_decals);
	Cvar_RegisterVariable (&r_decals_max);
	Cvar_RegisterVariable (&r_decals_intensity);
	Cvar_RegisterVariable (&r_decals_bloodpool);
	Cvar_RegisterVariable (&r_decals_bloodpool_radius);
	Cvar_RegisterVariable (&r_decals_bloodpool_growrate);
	Cvar_RegisterVariable (&r_decals_bloodpool_owner_drift);
	Cvar_RegisterVariable (&r_decals_gibbounce);
	Cvar_RegisterVariable (&r_decals_blooddrip);
	Cvar_RegisterVariable (&r_decals_blooddrip_length);
	Cvar_RegisterVariable (&r_decals_blooddrip_growtime);
	Cvar_RegisterVariable (&r_decals_debug);
	Cvar_RegisterVariable (&r_decals_blood_spatter);
	Cvar_RegisterVariable (&r_decals_blood_spatter_size);
	Cmd_AddCommand ("r_decals_test", R_DecalsTest_f);
	Cmd_AddCommand ("r_decals_test_grid", R_DecalsTestGrid_f);
	Cmd_AddCommand ("r_decals_test_pool", R_DecalsTestPool_f);
}

/* Dev: spawn a blood pool at the player's view origin (isolates the pool
   spawn+paint chain from the QC kill chain). */
static void R_DecalsTestPool_f (void)
{
	Con_Printf ("r_decals_test_pool: spawning at %.1f %.1f %.1f\n",
		r_refdef.vieworg[0], r_refdef.vieworg[1], r_refdef.vieworg[2]);
	R_SpawnBloodPool (r_refdef.vieworg, 0.0f, 0);
}

// ---------------------------------------------------------------------------
// Stain pool: fixed-size slots allocated at map load, LRU-evicted on overflow.
// Each slot holds a stain_t header plus an int16_t rgb[STAIN_PAYLOAD_INT16] payload.
// ---------------------------------------------------------------------------

#define STAIN_CELL_SIZE      (1 << STAIN_CELL_SHIFT)   // game units per cell side
// Max cells per surface side. Derived from the upstream lightmap cap
// (blocklights[18*18], i.e. 18 luxels per side × 16 game units = 288 units)
// divided by the current cell size — auto-scales when STAIN_CELL_SHIFT changes.
#define STAIN_MAX_CELLS_DIM  (18 << (4 - STAIN_CELL_SHIFT))
#define STAIN_PAYLOAD_INT16  (STAIN_MAX_CELLS_DIM * STAIN_MAX_CELLS_DIM * 3)

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
		int smax = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
		int tmax = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;
		if (smax > STAIN_MAX_CELLS_DIM || tmax > STAIN_MAX_CELLS_DIM) {
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
	// Owner tracking — pool freezes if its source edict goes away or moves.
	// owner_edict == 0 means "unowned" (dev cmd, no auto-freeze).
	int          owner_edict;
	const char  *owner_classname; // pointer snapshot for fast identity check
	vec3_t       owner_origin;    // origin snapshot at spawn time
} bloodpool_t;

static bloodpool_t r_bloodpools[MAX_ACTIVE_BLOODPOOLS];

// ---------------------------------------------------------------------------
// Blood drips — vertical streaks that grow downward along a wall over time.
// Fired by R_SpawnBloodDrip from SV_Physics_Toss when a gib bounces off a
// non-floor surface.
// ---------------------------------------------------------------------------

#define MAX_ACTIVE_BLOODDRIPS 32

typedef struct {
	vec3_t       origin;          // wall impact point
	vec3_t       down_dir;        // unit vector along wall, pointing down in world
	vec3_t       right_dir;       // unit vector along wall, perpendicular to down (for streak width)
	msurface_t  *surf;            // target wall surface (for surface validity check)
	float        spawn_time;
	float        length_max;      // game units
	float        length_painted;
	qboolean     alive;
} blooddrip_t;

static blooddrip_t r_blooddrips[MAX_ACTIVE_BLOODDRIPS];

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
	memset (r_blooddrips, 0, sizeof(r_blooddrips));
}

// Per-frame growth tick for active blood pools.
void R_DecalsFrame (void)
{
	int          i;
	float        rate;
	float        target, r_inner_sq, r_outer_sq;
	int          luxel_radius, dx, dy, u, v, idx;
	float        gx, gy, dsq, ou, ov;
	int          olu, olv, smax, tmax;
	int          nr, ng, nb;
	bloodpool_t *bp;
	msurface_t  *surf;
	mtexinfo_t  *tex;
	stain_t     *st;

	rate = r_decals_bloodpool_growrate.value;
	if (rate <= 0.001f) rate = 0.001f;

	for (i = 0; i < MAX_ACTIVE_BLOODPOOLS; i++) {
		bp = &r_bloodpools[i];
		if (!bp->alive) continue;

		// Owner-freeze: if the source entity is gone (freed, replaced by an
		// edict with a different classname pointer) or has moved more than
		// `owner_drift` units from where the pool spawned, stop growing.
		// Already-painted luxels stay (no fade — pools are permanent stains).
		if (bp->owner_edict > 0) {
			edict_t *owner = EDICT_NUM(bp->owner_edict);
			float drift_sq = r_decals_bloodpool_owner_drift.value;
			drift_sq = drift_sq * drift_sq;
			if (!owner || owner->free
			 || owner->v.classname != bp->owner_classname) {
				bp->alive = false;
				continue;
			}
			{
				float ox = owner->v.origin[0] - bp->owner_origin[0];
				float oy = owner->v.origin[1] - bp->owner_origin[1];
				float oz = owner->v.origin[2] - bp->owner_origin[2];
				if (ox*ox + oy*oy + oz*oz > drift_sq) {
					bp->alive = false;
					continue;
				}
			}
		}

		// Constant growth rate: radius grows by `rate` units per second
		// until it reaches radius_max. So a 64-unit pool at rate 6.5
		// takes ~10s, a 12-unit gib pool ~1.8s — the freeze window
		// scales naturally with how big the spill is.
		target = (cl.time - bp->spawn_time) * rate;
		if (target < 0.0f) target = 0.0f;
		if (target > bp->radius_max) target = bp->radius_max;
		if (target <= bp->radius_painted) {
			if (target >= bp->radius_max) bp->alive = false;
			continue;
		}

		surf = bp->surf;
		if (!surf) { bp->alive = false; continue; }

		if (r_decals_debug.value) {
			Con_Printf ("BloodPool[%d]: target=%.1f radius_painted=%.1f surf=%p\n",
				i, target, bp->radius_painted, (void*)surf);
		}

		tex = surf->texinfo;
		ou  = DotProduct(bp->origin, tex->vecs[0]) + tex->vecs[0][3];
		ov  = DotProduct(bp->origin, tex->vecs[1]) + tex->vecs[1][3];
		olu = ((int)floor(ou) - surf->texturemins[0]) >> STAIN_CELL_SHIFT;
		olv = ((int)floor(ov) - surf->texturemins[1]) >> STAIN_CELL_SHIFT;
		smax = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
		tmax = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;

		luxel_radius = (int)((target / (float)STAIN_CELL_SIZE) + 1.0f);
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
				gx  = dx * (float)STAIN_CELL_SIZE;
				gy  = dy * (float)STAIN_CELL_SIZE;
				dsq = gx*gx + gy*gy;
				if (dsq < r_inner_sq || dsq > r_outer_sq) continue;
				idx = (v * smax + u) * 3;
				// Blood guarantees a dark-red tint regardless of previous stains:
				// R darkened slightly, G/B suppressed hard. Each channel is
				// clamped to never read brighter than its blood-tint ceiling,
				// so prior bright stains (e.g. a fresh splat) are pushed down
				// into the same dark-red pool colour rather than blending out.
				nr  = st->rgb[idx + 0] - 50;
				if (nr > -50)   nr = -50;
				if (nr < -4096) nr = -4096;
				ng  = st->rgb[idx + 1] - 250;
				if (ng > -250)  ng = -250;
				if (ng < -4096) ng = -4096;
				nb  = st->rgb[idx + 2] - 250;
				if (nb > -250)  nb = -250;
				if (nb < -4096) nb = -4096;
				st->rgb[idx + 0] = (short)nr;
				st->rgb[idx + 1] = (short)ng;
				st->rgb[idx + 2] = (short)nb;
			}
		}
		st->generation++;
		st->last_touched_frame = r_framecount;
		bp->radius_painted = target;
		if (target >= bp->radius_max) bp->alive = false;
	}

	// Drip tick — extend each active drip downward along the wall.
	{
		float        drip_dur, drip_t, drip_target;
		float        step_len, dx_off;
		int          si;
		vec3_t       cell;
		blooddrip_t *bd;

		drip_dur = r_decals_blooddrip_growtime.value;
		if (drip_dur <= 0.001f) drip_dur = 0.001f;

		for (i = 0; i < MAX_ACTIVE_BLOODDRIPS; i++) {
			bd = &r_blooddrips[i];
			if (!bd->alive) continue;

			drip_t = (cl.time - bd->spawn_time) / drip_dur;
			if (drip_t < 0.0f) drip_t = 0.0f;
			if (drip_t > 1.0f) drip_t = 1.0f;
			drip_target = bd->length_max * drip_t;
			if (drip_target <= bd->length_painted) {
				if (drip_t >= 1.0f) bd->alive = false;
				continue;
			}

			if (!bd->surf) { bd->alive = false; continue; }

			// Paint each crossed cell boundary (STAIN_CELL_SIZE units) exactly once.
			// length_painted advances continuously per frame (sub-cell);
			// we paint the integer cell range we've newly crossed since
			// last frame. Without this quantization a 60fps tick would
			// paint the same cell ~30 times per drip and saturate the
			// lightmap to flat red.
			{
				int   from_luxel = (int)(bd->length_painted / (float)STAIN_CELL_SIZE);
				int   to_luxel   = (int)(drip_target          / (float)STAIN_CELL_SIZE);
				int   max_luxel  = (int)(bd->length_max       / (float)STAIN_CELL_SIZE);
				int   k;
				if (max_luxel < 1) max_luxel = 1;
				for (k = from_luxel + 1; k <= to_luxel; k++) {
					// Linear fade with distance from impact so the splat
					// at the bounce point reads as the most intense element
					// and the streak tapers off.
					float fall = 1.0f - (float)(k - 1) / (float)max_luxel;
					// Per-cell delta scaled by (reference cell size) / (current cell size)
					// so the drip's overall darkness stays the same even though we now
					// paint more cells per unit length. The deltas below (-40, -100, …)
					// were calibrated against a 16-unit luxel (1 << 4); divide by 16 to
					// renormalise to the current STAIN_CELL_SIZE. At shift==2 this is 0.25.
					float drip_scale = (float)STAIN_CELL_SIZE / 16.0f;
					int   dr_c = (int)(-40.0f  * fall * drip_scale);
					int   dg_c = (int)(-100.0f * fall * drip_scale);
					int   db_c = (int)(-100.0f * fall * drip_scale);
					int   dr_s = (int)(-20.0f  * fall * drip_scale);
					int   dg_s = (int)(-50.0f  * fall * drip_scale);
					int   db_s = (int)(-50.0f  * fall * drip_scale);
					step_len = (float)k * (float)STAIN_CELL_SIZE;
					for (si = -1; si <= 1; si++) {
						dx_off = (float)si * (float)STAIN_CELL_SIZE;
						cell[0] = bd->origin[0]
						        + step_len * bd->down_dir[0]
						        + dx_off   * bd->right_dir[0];
						cell[1] = bd->origin[1]
						        + step_len * bd->down_dir[1]
						        + dx_off   * bd->right_dir[1];
						cell[2] = bd->origin[2]
						        + step_len * bd->down_dir[2]
						        + dx_off   * bd->right_dir[2];
						if (si == 0) {
							Stain_AddCell (bd->surf, cell, 1, dr_c, dg_c, db_c, 1);
						} else {
							Stain_AddCell (bd->surf, cell, 1, dr_s, dg_s, db_s, 1);
						}
					}
				}
			}
			bd->length_painted = drip_target;
			if (drip_t >= 1.0f) bd->alive = false;
		}
	}
}

// ---------------------------------------------------------------------------
// Per-decal-type kernels and centre deltas. Indexed by decal_type_t.
// Kernel sizes are now in stain cells (= game units at STAIN_CELL_SHIFT==0).
// Footprints: BULLET 4u solid, SPIKE 1u pinprick, SPATTER 1u (matches
// particle), BLOOD_SPLAT/LIGHTNING 28u Gaussian, SCORCH 52u Gaussian.
// ---------------------------------------------------------------------------

/* Single-cell solid used by SPIKE, SPATTER, and the r_decals_test dev cmds. */
static const int K1x1_solid[1] = { 1 };

/* Runtime-filled kernel arrays. Sized for the maximum footprint at 1u cells. */
#define K_BULLET_DIM   4
#define K_GAUSS28_DIM  28
#define K_GAUSS52_DIM  52

static int K_bullet      [K_BULLET_DIM  * K_BULLET_DIM ];  /* 4×4 solid square */
static int K_blood_splat [K_GAUSS28_DIM * K_GAUSS28_DIM];  /* 28u Gaussian */
static int K_scorch      [K_GAUSS52_DIM * K_GAUSS52_DIM];  /* 52u Gaussian */
static int K_lightning   [K_GAUSS28_DIM * K_GAUSS28_DIM];  /* 28u Gaussian (same shape as splat) */

static int K_bullet_norm;
static int K_blood_splat_norm;
static int K_scorch_norm;
static int K_lightning_norm;

typedef struct {
	const int *k;
	int        ksize;
	int        knorm;
	int        dr, dg, db;
} decal_kernel_t;

/* Non-const: populated at startup by DecalKernels_Init via R_DecalsInit. */
static decal_kernel_t decal_kernels[DECAL_NUM_TYPES];

static void Kernel_Init_Solid (int *k, int n)
{
	int i;
	for (i = 0; i < n * n; i++) k[i] = 1;
}

/* Fill an n×n Gaussian with peak value ~256 at the centre. */
static void Kernel_Init_Gauss (int *k, int n, float sigma)
{
	int   i, j;
	float c = (n - 1) * 0.5f;
	float two_s2 = 2.0f * sigma * sigma;
	for (j = 0; j < n; j++) {
		for (i = 0; i < n; i++) {
			float dx = (float)i - c;
			float dy = (float)j - c;
			float w  = expf (-(dx*dx + dy*dy) / two_s2);
			int   wi = (int)(w * 256.0f + 0.5f);
			/* Tiny cells (corner of a wide kernel) round to 0; force ≥1 so
			   they still contribute a faint edge instead of disappearing. */
			if (wi < 1 && w > 0.002f) wi = 1;
			k[j * n + i] = wi;
		}
	}
}

static int Kernel_Sum (const int *k, int n)
{
	int sum = 0, i;
	for (i = 0; i < n * n; i++) sum += k[i];
	return sum;
}

static void DecalKernels_Init (void)
{
	Kernel_Init_Solid (K_bullet, K_BULLET_DIM);
	K_bullet_norm = Kernel_Sum (K_bullet, K_BULLET_DIM);

	/* sigma ≈ 6 for a 28-cell kernel: peak holds, edges fade smoothly. */
	Kernel_Init_Gauss (K_blood_splat, K_GAUSS28_DIM, 6.0f);
	K_blood_splat_norm = Kernel_Sum (K_blood_splat, K_GAUSS28_DIM);

	/* sigma ≈ 11 for the 52-cell scorch — wider falloff matching the explosion radius. */
	Kernel_Init_Gauss (K_scorch, K_GAUSS52_DIM, 11.0f);
	K_scorch_norm = Kernel_Sum (K_scorch, K_GAUSS52_DIM);

	Kernel_Init_Gauss (K_lightning, K_GAUSS28_DIM, 6.0f);
	K_lightning_norm = Kernel_Sum (K_lightning, K_GAUSS28_DIM);

	/* knorm == sum gives total-ink ≈ centre delta (the centre cell receives
	   delta × peak / sum, much less than the centre delta itself; the kernel
	   spreads ink over many cells preserving aggregate darkness). */
	decal_kernels[DECAL_BULLET]        = (decal_kernel_t){ K_bullet,      K_BULLET_DIM,  K_bullet_norm,      -150, -150, -150 };
	decal_kernels[DECAL_SPIKE]         = (decal_kernel_t){ K1x1_solid,    1,             1,                  -150, -150, -150 };
	decal_kernels[DECAL_BLOOD_SPLAT]   = (decal_kernel_t){ K_blood_splat, K_GAUSS28_DIM, K_blood_splat_norm, -200, -500, -500 };
	decal_kernels[DECAL_SCORCH]        = (decal_kernel_t){ K_scorch,      K_GAUSS52_DIM, K_scorch_norm,      -200, -200, -200 };
	decal_kernels[DECAL_LIGHTNING]     = (decal_kernel_t){ K_lightning,   K_GAUSS28_DIM, K_lightning_norm,    -50,  -60,  -40 };
	/* Spatter delta is mixed-sign: ADDITIVE on red, subtractive on G/B.
	   This forces the cell toward pure-red regardless of the base texture
	   colour. Pure subtractive deltas clamped to black on dark textures
	   (e.g. base r=20 minus delta -50 = 0). With +50 to R the cell stays
	   visibly red even on the darkest wall pixels. */
	decal_kernels[DECAL_BLOOD_SPATTER] = (decal_kernel_t){ K1x1_solid,    1,             1,                   +50, -100, -100 };
	/* Oil (M8 F2): a wide dark stain, faintly cool/blue (B darkened least), so a
	   poured patch reads as a slick on the floor and a poured trail overlaps into
	   a line. Reuses the 52-cell scorch footprint; a burning patch then stacks a
	   DECAL_SCORCH on top (additive -> burnt-oil look). */
	decal_kernels[DECAL_OIL]           = (decal_kernel_t){ K_scorch,      K_GAUSS52_DIM, K_scorch_norm,      -340, -340, -310 };
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

// Forward decl: definition lives below. Exposed via r_local.h so r_part.c
// can reuse it for nail/bullet spark texture sampling.
msurface_t *R_PointOnSurface_World (vec3_t p, vec3_t normal, float max_plane_dist);

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
	tlu = ((int)floor(u) - target->texturemins[0]) >> STAIN_CELL_SHIFT;
	tlv = ((int)floor(v) - target->texturemins[1]) >> STAIN_CELL_SHIFT;
	tsmax = (target->extents[0] >> STAIN_CELL_SHIFT) + 1;
	ttmax = (target->extents[1] >> STAIN_CELL_SHIFT) + 1;
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

	// Inverse of the world→UV projection: world step per cell (STAIN_CELL_SIZE UV units).
	for (i = 0; i < 3; i++) {
		step_u[i] = tex->vecs[0][i] * ((float)STAIN_CELL_SIZE / ulen2);
		step_v[i] = tex->vecs[1][i] * ((float)STAIN_CELL_SIZE / vlen2);
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
msurface_t *R_PointOnSurface_World (vec3_t p, vec3_t normal, float max_plane_dist)
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
	lu = ((int)floor(u) - surf->texturemins[0]) >> STAIN_CELL_SHIFT;
	lv = ((int)floor(v) - surf->texturemins[1]) >> STAIN_CELL_SHIFT;
	smax = (surf->extents[0] >> STAIN_CELL_SHIFT) + 1;
	tmax = (surf->extents[1] >> STAIN_CELL_SHIFT) + 1;
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
		step_u[i] = tex->vecs[0][i] * ((float)STAIN_CELL_SIZE / ulen2);
		step_v[i] = tex->vecs[1][i] * ((float)STAIN_CELL_SIZE / vlen2);
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
	extern server_t sv;

	if (!r_decals.value) return;
	if (type < 0 || type >= DECAL_NUM_TYPES) return;
	// Demo playback path (CL_ParseTEnt -> R_SpawnDecal) fires before
	// sv.active is set; calling SV_Move into uninitialised sv.edicts
	// dereferences a NULL hull pointer in SV_HullForEntity. Bail out --
	// missing decals during demo playback are cosmetic.
	if (!sv.active) return;

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

/* Paint a small blood dot at `pos` on the surface near `normal`.
   Called from r_part.c on PARTFL_STICK_ON_HIT, from R_SlideRelease's
   floor-snap, and from the wall-slide drag trail. Bails silently if
   decals are disabled, the spatter cvar is off, the server is inactive
   (demo playback), or no nearby world surface is found.

   Footprint is an NxN square in stain cells (N = r_decals_blood_spatter_size,
   default 3). With STAIN_CELL_SIZE 1 game unit this gives a ~3u blot per
   droplet — large enough to read on-screen, small enough that the slide
   trail still looks like a streak rather than a band.

   Saturating: each channel takes max-magnitude delta rather than accumulating.
   N stuck droplets sliding through the same column would otherwise add their
   negative deltas (e.g. 4 × -125 on green/blue → clamp to 0 → pure black).
   With saturation, repeats can't darken further than one spatter's worth,
   so the trail stays dark-red regardless of overlap. The trade-off vs the
   regular additive painter: no kernel falloff, no BSP coplanar walk.
   Both are negligible at the small footprint size. */
void R_SpawnBloodSpatter (vec3_t pos, vec3_t normal)
{
	const decal_kernel_t *dk;
	msurface_t           *surf;
	stain_t              *st;
	mtexinfo_t           *tex;
	float                 u, v;
	int                   lu, lv, idx;
	int                   size, half, dx, dy, cu, cv;
	extern server_t       sv;

	if (!r_decals.value)               return;
	if (!r_decals_blood_spatter.value) return;
	if (!sv.active)                    return;  /* demo playback safety */

	surf = R_PointOnSurface_World (pos, normal, 4.0f);
	if (!surf) return;

	st = Stain_GetOrAlloc (surf);
	if (!st) return;

	tex = surf->texinfo;
	u   = DotProduct(pos, tex->vecs[0]) + tex->vecs[0][3];
	v   = DotProduct(pos, tex->vecs[1]) + tex->vecs[1][3];
	lu  = ((int)floor(u) - surf->texturemins[0]) >> STAIN_CELL_SHIFT;
	lv  = ((int)floor(v) - surf->texturemins[1]) >> STAIN_CELL_SHIFT;
	if (lu < 0 || lu >= st->smax || lv < 0 || lv >= st->tmax) return;

	size = (int)r_decals_blood_spatter_size.value;
	if (size < 1) size = 1;
	if (size > 9) size = 9;       /* cap so a stray cvar can't blanket a wall */
	half = size / 2;              /* centered footprint: size==3 → [-1..+1] */

	dk  = &decal_kernels[DECAL_BLOOD_SPATTER];
	/* Saturating per channel: a brighten-delta (>0) takes the max value,
	   a darken-delta (<0) takes the min. Spatter pushes red UP (+50) and
	   G/B DOWN (-100 each); each cell saturates at one spatter's worth
	   so repeated paints into the same cell don't compound. Existing
	   stronger stains (splat, scorch) win in the direction they already lead. */
	#define SAT_BRIGHTEN(channel, delta)  \
		do { if (st->rgb[idx + (channel)] < (delta)) st->rgb[idx + (channel)] = (short)(delta); } while (0)
	#define SAT_DARKEN(channel, delta)    \
		do { if (st->rgb[idx + (channel)] > (delta)) st->rgb[idx + (channel)] = (short)(delta); } while (0)
	for (dy = -half; dy <= half; dy++) {
		cv = lv + dy;
		if (cv < 0 || cv >= st->tmax) continue;
		for (dx = -half; dx <= half; dx++) {
			cu = lu + dx;
			if (cu < 0 || cu >= st->smax) continue;
			idx = (cv * st->smax + cu) * 3;
			if (dk->dr >= 0) SAT_BRIGHTEN (0, dk->dr); else SAT_DARKEN (0, dk->dr);
			if (dk->dg >= 0) SAT_BRIGHTEN (1, dk->dg); else SAT_DARKEN (1, dk->dg);
			if (dk->db >= 0) SAT_BRIGHTEN (2, dk->db); else SAT_DARKEN (2, dk->db);
		}
	}
	#undef SAT_BRIGHTEN
	#undef SAT_DARKEN
	st->generation++;
	st->last_touched_frame = r_framecount;
}

void R_SpawnBloodPool (vec3_t origin, float radius_max, int owner_edict)
{
	vec3_t   end;
	trace_t  tr;
	msurface_t *surf;
	int      i, slot;
	float    oldest_time;
	int      oldest_slot;
	bloodpool_t *bp;
	float    use_radius;

	if (r_decals_debug.value) {
		Con_Printf ("R_SpawnBloodPool: origin=%.1f %.1f %.1f rdecals=%d rbloodpool=%d\n",
			origin[0], origin[1], origin[2],
			(int)r_decals.value, (int)r_decals_bloodpool.value);
	}
	if (!r_decals.value || !r_decals_bloodpool.value) return;

	// Trace straight down to find the floor.
	end[0] = origin[0];
	end[1] = origin[1];
	end[2] = origin[2] - 64.0f;
	tr = SV_Move (origin, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (r_decals_debug.value) {
		Con_Printf ("  trace: frac=%.2f endpos=%.1f %.1f %.1f normal_z=%.2f\n",
			tr.fraction, tr.endpos[0], tr.endpos[1], tr.endpos[2], tr.plane.normal[2]);
	}
	if (tr.fraction >= 1.0f) { if (r_decals_debug.value) Con_Printf ("  rejected: no floor within 64 units\n"); return; }
	if (tr.plane.normal[2] < 0.7f) { if (r_decals_debug.value) Con_Printf ("  rejected: floor too steep\n"); return; }

	surf = R_PointOnSurface_World (tr.endpos, tr.plane.normal, 4.0f);
	if (r_decals_debug.value) {
		Con_Printf ("  surf=%p rgb_samples=%s\n",
			(void*)surf, surf && surf->rgb_samples ? "yes" : "NO");
	}
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

	use_radius = (radius_max > 0.0f) ? radius_max : r_decals_bloodpool_radius.value;

	bp = &r_bloodpools[slot];
	bp->origin[0]      = tr.endpos[0];
	bp->origin[1]      = tr.endpos[1];
	bp->origin[2]      = tr.endpos[2];
	bp->surf           = surf;
	bp->spawn_time     = cl.time;
	bp->radius_max     = use_radius;
	bp->radius_painted = 0.0f;
	bp->alive          = true;
	bp->owner_edict    = owner_edict;
	if (owner_edict > 0) {
		edict_t *owner = EDICT_NUM(owner_edict);
		if (owner && !owner->free) {
			bp->owner_classname  = owner->v.classname;
			bp->owner_origin[0]  = owner->v.origin[0];
			bp->owner_origin[1]  = owner->v.origin[1];
			bp->owner_origin[2]  = owner->v.origin[2];
		} else {
			bp->owner_edict      = 0;  // owner already gone — treat as unowned
			bp->owner_classname  = NULL;
		}
	} else {
		bp->owner_classname  = NULL;
	}
}

// Spawn a wall-drip. Returns 1 on success (caller may skip splat fallback),
// 0 if the surface is too horizontal for a meaningful drip (ceiling) — caller
// should fall back to R_SpawnDecal(pos, DECAL_BLOOD_SPLAT).
int R_SpawnBloodDrip (vec3_t origin, vec3_t normal)
{
	msurface_t  *surf;
	vec3_t       down_world = { 0.0f, 0.0f, -1.0f };
	vec3_t       down_proj, right;
	float        dn, dlen, rlen;
	int          i, slot, oldest_slot;
	float        oldest_time;
	blooddrip_t *bd;
	extern server_t sv;

	if (!r_decals.value || !r_decals_blooddrip.value) return 0;
	if (!sv.active) return 0;

	// Project world-down onto the wall plane: down_proj = down - (down·n) * n.
	dn = -normal[2];   // down·n = (0,0,-1)·n = -n[2]
	down_proj[0] = down_world[0] - dn * normal[0];
	down_proj[1] = down_world[1] - dn * normal[1];
	down_proj[2] = down_world[2] - dn * normal[2];
	dlen = sqrt (down_proj[0]*down_proj[0] + down_proj[1]*down_proj[1] + down_proj[2]*down_proj[2]);
	// Below ~0.3 the surface is nearly horizontal (floor or ceiling) — no drip.
	if (dlen < 0.3f) return 0;
	down_proj[0] /= dlen; down_proj[1] /= dlen; down_proj[2] /= dlen;

	// right = down_proj × normal (any consistent orthogonal in the wall plane).
	right[0] = down_proj[1]*normal[2] - down_proj[2]*normal[1];
	right[1] = down_proj[2]*normal[0] - down_proj[0]*normal[2];
	right[2] = down_proj[0]*normal[1] - down_proj[1]*normal[0];
	rlen = sqrt (right[0]*right[0] + right[1]*right[1] + right[2]*right[2]);
	if (rlen < 0.01f) return 0;
	right[0] /= rlen; right[1] /= rlen; right[2] /= rlen;

	surf = R_PointOnSurface_World (origin, normal, 4.0f);
	if (!surf) return 0;

	// LRU-pick a slot.
	slot        = -1;
	oldest_time = 1e9f;
	oldest_slot = 0;
	for (i = 0; i < MAX_ACTIVE_BLOODDRIPS; i++) {
		if (!r_blooddrips[i].alive) { slot = i; break; }
		if (r_blooddrips[i].spawn_time < oldest_time) {
			oldest_time = r_blooddrips[i].spawn_time;
			oldest_slot = i;
		}
	}
	if (slot < 0) {
		bd = &r_blooddrips[oldest_slot];
		bd->alive          = false;
		bd->length_painted = bd->length_max;
		slot               = oldest_slot;
	}

	bd = &r_blooddrips[slot];
	bd->origin[0]    = origin[0];
	bd->origin[1]    = origin[1];
	bd->origin[2]    = origin[2];
	bd->down_dir[0]  = down_proj[0];
	bd->down_dir[1]  = down_proj[1];
	bd->down_dir[2]  = down_proj[2];
	bd->right_dir[0] = right[0];
	bd->right_dir[1] = right[1];
	bd->right_dir[2] = right[2];
	bd->surf         = surf;
	bd->spawn_time   = cl.time;
	bd->length_max   = r_decals_blooddrip_length.value;
	bd->length_painted = 0.0f;
	bd->alive        = true;
	return 1;
}

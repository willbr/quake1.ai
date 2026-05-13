# Decals Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Permanent per-surface stain decals (bullet holes, blood splats, scorch marks, lightning burns, growing blood pools on death) painted into the lightmap and clamped during the surface-cache rebuild.

**Architecture:** Each `msurface_t` gains an optional `stain_t *stain` whose RGB-delta luxel buffer is added to `blocklights_rgb` (and luminance-added to mono `blocklights`) after the static+dynamic light sum, then clamped. Slot storage is a fixed-size pool allocated at map load and reused via LRU eviction. Impact decals fire from `CL_ParseTEnt` after each impact-style TE_ event via a short retrace; blood pools fire from game.dll `Killed()` via a new `engine_api->spawn_blood_pool` and grow over time via `R_DecalsFrame`.

**Tech Stack:** C (gnu89), Zig 0.16 build, software renderer, hot-reload game.dll (Phase 5 ABI).

**Spec:** `docs/superpowers/specs/2026-05-13-decals-design.md`

**Verification:** No unit test suite exists in this project. Each task's verification is a `zig build` + (where applicable) `zig build run -- +map e1m1` + in-game smoke test. Per `feedback_smoke_test.md`: compile-clean is not enough; exercise the code path.

**Commit policy:** Each task ends with `git add ... && git commit && git push` per the `no-pr` memory.

---

## File map

| File | Touch type | Responsibility |
|---|---|---|
| `sdlquake/engine_src/model.h` | modify | Two new fields on `msurface_t`: `stain_t *stain`, `int cached_stain_gen`. Forward-declare `stain_t`. |
| `sdlquake/engine_src/d_local.h` | modify | One new field on `surfcache_t`: `int stain_gen`. |
| `sdlquake/engine_src/r_local.h` | modify | Declarations for `stain_t`, `R_DecalsInit/Clear/Frame`, `R_SpawnDecal`, `R_SpawnBloodPool`, decal-type enum, `r_decals*` cvars. |
| `sdlquake/engine_src/r_decals.c` | create | All decal logic: pool allocator, LRU, kernel tables, projection math, retrace, `R_SpawnDecal`, `R_SpawnBloodPool`, `R_DecalsFrame`, `r_decals_test_f` console command. |
| `sdlquake/engine_src/r_surf.c` | modify | Stain-apply pass added to `R_BuildLightMap` (mono) and `R_BuildLightMap_RGB`. |
| `sdlquake/engine_src/d_surf.c` | modify | Cache validity check at `D_CacheSurface:283-288` extended to include `cache->stain_gen`. |
| `sdlquake/engine_src/r_main.c` | modify | `R_NewMap` calls `R_DecalsClear`; `R_Init` calls `R_DecalsInit`; `R_RenderView` calls `R_DecalsFrame` once per frame. |
| `sdlquake/engine_src/cl_tent.c` | modify | After each impact-style TE_ case in `CL_ParseTEnt`, call `R_SpawnDecal(pos, type)`. Lightning beam end-point dispatch via `CL_ParseBeam` (or post-parse hook). |
| `sdlquake/game/game_api.h` | modify | Bump `GAME_API_VERSION` 15 → 16. Add `void (*spawn_blood_pool)(const vec3_t origin)` to `engine_api_t`. |
| `sdlquake/engine/hotreload.c` | modify | New static wrapper `engine_spawn_blood_pool`. Add entry to the `engine_funcs` table initializer. |
| `sdlquake/game/combat.c` | modify | `Killed()` calls `eng->spawn_blood_pool(targ->v.origin)` for FL_MONSTER deaths. |
| `build.zig` | modify | Add `"r_decals.c"` to `engine_files`. |
| `ideas.md` | modify | Drop "decals" from the "Next" list. |

---

## Task 1: Add data fields + cvars + r_decals.c skeleton

**Goal:** Plumbing in place. No behaviour yet. Build must succeed.

**Files:**
- Modify: `sdlquake/engine_src/model.h`
- Modify: `sdlquake/engine_src/d_local.h`
- Modify: `sdlquake/engine_src/r_local.h`
- Create: `sdlquake/engine_src/r_decals.c`
- Modify: `build.zig`

- [ ] **Step 1.1: Forward-declare `stain_t` and add fields to `msurface_t`**

Edit `sdlquake/engine_src/model.h`. Find the line `typedef struct msurface_s` (around line 102). Just above it, add the forward-declaration:

```c
struct stain_s;
```

Inside `msurface_t`, just before the closing `} msurface_t;` (around line 127), add:

```c
// decal stain layer (NULL until first impact on this surface)
	struct stain_s	*stain;
	int				cached_stain_gen;
```

- [ ] **Step 1.2: Add cache-validity field to `surfcache_t`**

Edit `sdlquake/engine_src/d_local.h`. Find `typedef struct surfcache_s` (line 37). Add a new field just after `int dlight;` (line 42):

```c
	int					stain_gen;  // decal generation seen when cache built
```

- [ ] **Step 1.3: Declare the public decal API in `r_local.h`**

Edit `sdlquake/engine_src/r_local.h`. Find a sensible location near other forward declarations (after the cvars block or near `R_ClearParticles` declaration). Add:

```c
// -- decals ---------------------------------------------------------------

typedef enum {
	DECAL_BULLET,
	DECAL_SPIKE,
	DECAL_BLOOD_SPLAT,
	DECAL_SCORCH,
	DECAL_LIGHTNING,
	DECAL_NUM_TYPES
} decal_type_t;

typedef struct stain_s {
	short		*rgb;             // [smax*tmax*3], signed luxel deltas
	int			smax, tmax;
	int			generation;
	int			last_touched_frame;
	struct msurface_s *surf;
	struct stain_s *lru_prev, *lru_next;
} stain_t;

extern cvar_t r_decals;
extern cvar_t r_decals_max;
extern cvar_t r_decals_intensity;
extern cvar_t r_decals_bloodpool;
extern cvar_t r_decals_bloodpool_radius;
extern cvar_t r_decals_bloodpool_growtime;

void R_DecalsInit (void);          // called once from R_Init (registers cvars + commands)
void R_DecalsClear (void);         // called from R_NewMap (zero stain pool, reset bloodpools)
void R_DecalsFrame (void);         // called per-frame from R_RenderView (advances bloodpools)
void R_SpawnDecal (vec3_t pos, decal_type_t type);
void R_SpawnBloodPool (vec3_t origin);
```

- [ ] **Step 1.4: Create the r_decals.c skeleton**

Create `sdlquake/engine_src/r_decals.c` with this initial content:

```c
/*
r_decals.c — per-surface stain decals (bullet holes, blood, scorch, blood pools).
See docs/superpowers/specs/2026-05-13-decals-design.md.
*/
#include "quakedef.h"
#include "r_local.h"

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
}

void R_DecalsClear (void)
{
	// Filled in Task 2.
}

void R_DecalsFrame (void)
{
	// Filled in Task 11.
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
```

- [ ] **Step 1.5: Wire `R_DecalsInit` into `R_Init`**

Edit `sdlquake/engine_src/r_main.c`. Find `R_Init` (search for `void R_Init (void)`). Just before its closing brace, add:

```c
	R_DecalsInit ();
```

- [ ] **Step 1.6: Wire `R_DecalsClear` into `R_NewMap`**

Edit `sdlquake/engine_src/r_main.c`. Find `R_NewMap` (around line 278). Add as the first statement inside the function body:

```c
	R_DecalsClear ();
```

- [ ] **Step 1.7: Add r_decals.c to the build**

Edit `build.zig`. Find the engine file list at line ~52 (`"r_sky.c", "r_sprite.c", "r_surf.c", "r_surf_rgb.c", "r_vars.c"`). Insert `"r_decals.c"` immediately after `"r_part.c"`. The line should look like:

```zig
        "r_efrag.c", "r_light.c", "r_lut.c", "r_main.c", "r_misc.c", "r_part.c",
        "r_decals.c",
        "r_sky.c", "r_sprite.c", "r_surf.c", "r_surf_rgb.c", "r_vars.c",
```

- [ ] **Step 1.8: Verify build**

Run: `zig build`
Expected: succeeds, no new warnings beyond baseline.

- [ ] **Step 1.9: Commit**

```sh
git add sdlquake/engine_src/model.h sdlquake/engine_src/d_local.h sdlquake/engine_src/r_local.h sdlquake/engine_src/r_decals.c sdlquake/engine_src/r_main.c build.zig
git commit -m "feat(decals): scaffold stain types, cvars, and r_decals.c"
git push
```

---

## Task 2: Stain pool allocator + LRU + R_DecalsClear

**Goal:** A fixed-size slot pool is allocated at map load; stains are taken from a free list, returned on eviction, intrusively linked into an LRU. No render integration yet.

**Files:**
- Modify: `sdlquake/engine_src/r_decals.c`

- [ ] **Step 2.1: Add pool state and helpers to r_decals.c**

Edit `sdlquake/engine_src/r_decals.c`. Append (after `R_DecalsInit`):

```c
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

	if (!r_stain_freelist) {
		// Evict tail (oldest).
		if (!r_stain_lru_tail) return NULL;
		stain_slot_t *victim = (stain_slot_t *)
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
	if (surf->stain) {
		Stain_LRU_Touch (surf->stain);
		return surf->stain;
	}
	stain_slot_t *slot = Stain_AllocSlot (surf);
	return slot ? &slot->header : NULL;
}
```

- [ ] **Step 2.2: Implement R_DecalsClear**

Replace the empty `R_DecalsClear` body with:

```c
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
```

`Hunk_AllocName` already zeroes its memory, so payloads start clean.

- [ ] **Step 2.3: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 2.4: Verify smoke (no behaviour yet)**

Run: `zig build run -- +map e1m1`
Expected: game starts, plays normally, no crashes. The pool exists in memory but nothing writes to it yet.

- [ ] **Step 2.5: Commit**

```sh
git add sdlquake/engine_src/r_decals.c
git commit -m "feat(decals): stain pool allocator with LRU eviction"
git push
```

---

## Task 3: Stain-apply pass in R_BuildLightMap_RGB

**Goal:** When `surf->stain` is non-NULL, its luxel deltas are added to `blocklights_rgb` and clamped during the surface-cache rebuild. Visible if something writes to a stain (still not possible yet — coming in Task 6).

**Files:**
- Modify: `sdlquake/engine_src/r_surf.c`

- [ ] **Step 3.1: Add stain pass to R_BuildLightMap_RGB**

Edit `sdlquake/engine_src/r_surf.c`. Find `R_BuildLightMap_RGB` (line ~292). After the existing dynamic-light addition (the `if (surf->dlightframe == r_framecount) R_AddDynamicLights_RGB ();` block, around line 332) and *before* the final clamp loop (`for (i = 0; i < size * 3; i++) { unsigned v = blocklights_rgb[i] >> ...; }`), insert:

```c
	// Stain layer (decals). Signed int16 deltas in luxel space; applied in 8.8 fixed-point.
	if (surf->stain) {
		short *s = surf->stain->rgb;
		float kscale = r_decals_intensity.value;
		int   ks    = (int)(kscale * 256.0f);  // pre-scale into 8.8
		for (i = 0; i < size; i++) {
			int br = (int)blocklights_rgb[i*3 + 0] + ((int)s[i*3 + 0] * ks);
			int bg = (int)blocklights_rgb[i*3 + 1] + ((int)s[i*3 + 1] * ks);
			int bb = (int)blocklights_rgb[i*3 + 2] + ((int)s[i*3 + 2] * ks);
			if (br < 0) br = 0;
			if (bg < 0) bg = 0;
			if (bb < 0) bb = 0;
			blocklights_rgb[i*3 + 0] = (unsigned)br;
			blocklights_rgb[i*3 + 1] = (unsigned)bg;
			blocklights_rgb[i*3 + 2] = (unsigned)bb;
		}
		surf->stain->last_touched_frame = r_framecount;
	}
```

The upper-clamp is handled by the existing final clamp loop two lines below.

- [ ] **Step 3.2: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 3.3: Smoke test**

Run: `zig build run -- +map e1m1`
Expected: game runs unchanged (no stains exist yet).

- [ ] **Step 3.4: Commit**

```sh
git add sdlquake/engine_src/r_surf.c
git commit -m "feat(decals): apply stain deltas in R_BuildLightMap_RGB"
git push
```

---

## Task 4: Stain-apply pass in R_BuildLightMap (mono fallback)

**Goal:** When `r_coloredlight 0` (or the map has no `.lit` file), stains still appear in the mono luminance path.

**Files:**
- Modify: `sdlquake/engine_src/r_surf.c`

- [ ] **Step 4.1: Add stain pass to R_BuildLightMap**

Edit `sdlquake/engine_src/r_surf.c`. Find `R_BuildLightMap` (line ~224). After the `if (surf->dlightframe == r_framecount) R_AddDynamicLights ();` line (around line 266) and *before* the final `// bound, invert, and shift` loop (line ~268), insert:

```c
	// Stain layer (decals). Mono path uses Rec.601 luminance of the RGB delta.
	if (surf->stain) {
		short *s = surf->stain->rgb;
		float kscale = r_decals_intensity.value;
		int   ks    = (int)(kscale * 256.0f);  // pre-scale into 8.8
		for (i = 0; i < size; i++) {
			int dy = (3 * s[i*3 + 0] + 6 * s[i*3 + 1] + 1 * s[i*3 + 2]) / 10;
			int b  = (int)blocklights[i] + dy * ks;
			if (b < 0) b = 0;
			blocklights[i] = (unsigned)b;
		}
		surf->stain->last_touched_frame = r_framecount;
	}
```

- [ ] **Step 4.2: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 4.3: Commit**

```sh
git add sdlquake/engine_src/r_surf.c
git commit -m "feat(decals): apply stain deltas in mono R_BuildLightMap"
git push
```

---

## Task 5: Cache invalidation via cached_stain_gen

**Goal:** When a stain is added/modified, the surface's cached lightmap is rebuilt on next render. Without this, post-spawn decals would only appear when a dlight happens to touch the face.

**Files:**
- Modify: `sdlquake/engine_src/d_surf.c`

- [ ] **Step 5.1: Extend D_CacheSurface validity check**

Edit `sdlquake/engine_src/d_surf.c`. Find `D_CacheSurface` (line ~265). The validity check at line 283-288 looks like:

```c
	if (cache && !cache->dlight && surface->dlightframe != r_framecount
			&& cache->texture == r_drawsurf.texture
			&& cache->lightadj[0] == r_drawsurf.lightadj[0]
			&& cache->lightadj[1] == r_drawsurf.lightadj[1]
			&& cache->lightadj[2] == r_drawsurf.lightadj[2]
			&& cache->lightadj[3] == r_drawsurf.lightadj[3] )
		return cache;
```

Replace it with:

```c
	{
		int cur_stain_gen = surface->stain ? surface->stain->generation : 0;
		if (cache && !cache->dlight && surface->dlightframe != r_framecount
				&& cache->texture == r_drawsurf.texture
				&& cache->lightadj[0] == r_drawsurf.lightadj[0]
				&& cache->lightadj[1] == r_drawsurf.lightadj[1]
				&& cache->lightadj[2] == r_drawsurf.lightadj[2]
				&& cache->lightadj[3] == r_drawsurf.lightadj[3]
				&& cache->stain_gen == cur_stain_gen )
			return cache;
	}
```

- [ ] **Step 5.2: Copy generation into cache on rebuild**

Still in `D_CacheSurface`, find the block where cache state is updated (around line 312-323, after the cache is (re)allocated):

```c
	if (surface->dlightframe == r_framecount)
		cache->dlight = 1;
	else
		cache->dlight = 0;

	r_drawsurf.surfdat = (pixel_t *)cache->data;
	
	cache->texture = r_drawsurf.texture;
	cache->lightadj[0] = r_drawsurf.lightadj[0];
	cache->lightadj[1] = r_drawsurf.lightadj[1];
	cache->lightadj[2] = r_drawsurf.lightadj[2];
	cache->lightadj[3] = r_drawsurf.lightadj[3];
```

Add this line immediately after the `cache->lightadj[3] = ...` line:

```c
	cache->stain_gen = surface->stain ? surface->stain->generation : 0;
	if (surface->stain) surface->cached_stain_gen = surface->stain->generation;
	else                surface->cached_stain_gen = 0;
```

- [ ] **Step 5.3: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 5.4: Smoke**

Run: `zig build run -- +map e1m1`
Expected: game runs normally. Cache invalidation logic added but no stains yet to trigger it.

- [ ] **Step 5.5: Commit**

```sh
git add sdlquake/engine_src/d_surf.c
git commit -m "feat(decals): invalidate surface cache on stain generation change"
git push
```

---

## Task 6: Test command `r_decals_test`

**Goal:** First visible decal. A console command paints a known dark stain pattern into the surface the player is looking at, so we can verify all of Tasks 1–5 work end-to-end before wiring up real spawns. The command stays in the codebase as a permanent dev tool.

**Files:**
- Modify: `sdlquake/engine_src/r_decals.c`

- [ ] **Step 6.1: Add a forward-cast helper and the test command**

Edit `sdlquake/engine_src/r_decals.c`. Append at the end of the file:

```c
// ---------------------------------------------------------------------------
// Internal: paint a single luxel kernel into a surface's stain buffer.
// kernel is a 3x3 or 5x5 (ksize) weight grid summing to ~16 (3x3) or ~256 (5x5).
// dr/dg/db are the centre-delta values; per-luxel delta = (centre * weight) / norm.
// ---------------------------------------------------------------------------
static void Stain_PaintKernel (msurface_t *surf, int lu, int lv,
                               int dr, int dg, int db,
                               const int *kernel, int ksize, int knorm)
{
	stain_t *st = Stain_GetOrAlloc (surf);
	if (!st) return;

	int half = ksize / 2;
	for (int ky = 0; ky < ksize; ky++) {
		int v = lv + ky - half;
		if (v < 0 || v >= st->tmax) continue;
		for (int kx = 0; kx < ksize; kx++) {
			int u = lu + kx - half;
			if (u < 0 || u >= st->smax) continue;
			int w = kernel[ky * ksize + kx];
			if (!w) continue;
			int idx = (v * st->smax + u) * 3;
			int nr = st->rgb[idx + 0] + (dr * w) / knorm;
			int ng = st->rgb[idx + 1] + (dg * w) / knorm;
			int nb = st->rgb[idx + 2] + (db * w) / knorm;
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

// Dev command: paint a dark blot at the centre of the first non-sky world surface
// in front of the player. Walks 64 units forward and slaps a stain on whatever's there.
static void R_DecalsTest_f (void)
{
	vec3_t forward, right, up, start, end;
	AngleVectors (r_refdef.viewangles, forward, right, up);
	VectorCopy (r_refdef.vieworg, start);
	VectorMA (start, 1024, forward, end);

	trace_t tr;
	memset (&tr, 0, sizeof(tr));
	tr = SV_Move (start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) {
		Con_Printf ("r_decals_test: nothing in front\n");
		return;
	}

	// Find the surface by re-tracing on the world model.
	msurface_t *surf = R_PointOnSurface_World (tr.endpos, tr.plane.normal);
	if (!surf) {
		Con_Printf ("r_decals_test: no world surface at hit point\n");
		return;
	}

	// Project tr.endpos into surface UV/luxel coords.
	mtexinfo_t *tex = surf->texinfo;
	float u = DotProduct(tr.endpos, tex->vecs[0]) + tex->vecs[0][3];
	float v = DotProduct(tr.endpos, tex->vecs[1]) + tex->vecs[1][3];
	int   lu = ((int)floor(u) - surf->texturemins[0]) >> 4;
	int   lv = ((int)floor(v) - surf->texturemins[1]) >> 4;
	int   smax = (surf->extents[0] >> 4) + 1;
	int   tmax = (surf->extents[1] >> 4) + 1;
	if (lu < 0 || lu >= smax || lv < 0 || lv >= tmax) {
		Con_Printf ("r_decals_test: luxel out of bounds (%d,%d) in %dx%d\n", lu, lv, smax, tmax);
		return;
	}

	static const int K3x3[9] = { 1, 2, 1, 2, 4, 2, 1, 2, 1 };
	Stain_PaintKernel (surf, lu, lv, -120, -120, -120, K3x3, 3, 16);
	Con_Printf ("r_decals_test: stained luxel (%d,%d) of surface %p\n", lu, lv, (void*)surf);
}
```

- [ ] **Step 6.2: Add R_PointOnSurface_World helper**

The test command above calls `R_PointOnSurface_World`. This helper is needed by every decal spawn path too; implement it once here.

Still in `r_decals.c`, add (above `R_DecalsTest_f`):

```c
// Walk the world BSP to find the surface that point P lies on (or near).
// Uses the surface plane and a small tolerance. Returns NULL if no match.
static msurface_t *R_PointOnSurface_World (vec3_t p, vec3_t normal)
{
	model_t *world = cl.worldmodel;
	if (!world) return NULL;

	msurface_t *best = NULL;
	float       best_d = 4.0f;  // max plane-distance tolerance (game units)

	for (int i = 0; i < world->numsurfaces; i++) {
		msurface_t *s = &world->surfaces[i];
		mplane_t   *pl = s->plane;
		float       d = DotProduct(p, pl->normal) - pl->dist;
		if (s->flags & SURF_PLANEBACK) d = -d;
		float       ad = d < 0 ? -d : d;
		if (ad > best_d) continue;
		// Optional: reject if surface normal disagrees with the hit normal
		if (normal) {
			float nd = DotProduct(normal, pl->normal);
			if (s->flags & SURF_PLANEBACK) nd = -nd;
			if (nd < 0.5f) continue;  // surface faces away from the impact normal
		}
		// Reject sky / water / non-lightmapped surfaces.
		if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB | SURF_DRAWTILED)) continue;

		// Check UV bounds (avoid hitting an unrelated coplanar surface).
		mtexinfo_t *tex = s->texinfo;
		float u = DotProduct(p, tex->vecs[0]) + tex->vecs[0][3];
		float v = DotProduct(p, tex->vecs[1]) + tex->vecs[1][3];
		if (u < s->texturemins[0] || u > s->texturemins[0] + s->extents[0]) continue;
		if (v < s->texturemins[1] || v > s->texturemins[1] + s->extents[1]) continue;

		best   = s;
		best_d = ad;
	}
	return best;
}
```

- [ ] **Step 6.3: Register the command in R_DecalsInit**

Edit `R_DecalsInit` in `r_decals.c`. After the existing `Cvar_RegisterVariable` calls, add:

```c
	Cmd_AddCommand ("r_decals_test", R_DecalsTest_f);
```

- [ ] **Step 6.4: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 6.5: Smoke test the test command**

Run: `zig build run -- +map e1m1`
At the start of the map, open the console and type:

```
r_decals_test
```

Walk back a step and look at the wall you were facing. Expected: a dark blotch appears on the wall, ~16 game units wide. The blotch persists as you move around (cache rebuild propagates it). Type `r_decals_test` again at another wall, see another dark blotch.

If nothing visible:
- `r_decals_intensity 4` and try again — increase the delta scale.
- Console should print `r_decals_test: stained luxel (X,Y) of surface 0x...` — if it doesn't, the retrace whiffed.
- If you see the print but no visible stain, the lightmap-apply path isn't running (check Task 3/4 was committed).

- [ ] **Step 6.6: Commit**

```sh
git add sdlquake/engine_src/r_decals.c
git commit -m "feat(decals): add r_decals_test command to verify stain plumbing"
git push
```

---

## Task 7: Real impact decals — retrace + R_SpawnDecal + kernels

**Goal:** Replace the test command's hardcoded path with a real `R_SpawnDecal(pos, type)` that fires a short retrace from the impact position and stamps the per-type kernel. Not yet wired to TE_ events — that's Task 9.

**Files:**
- Modify: `sdlquake/engine_src/r_decals.c`

- [ ] **Step 7.1: Add the kernel + delta tables**

Edit `sdlquake/engine_src/r_decals.c`. Above `Stain_PaintKernel`, add:

```c
// Per-decal-type kernels and centre deltas. Indexed by decal_type_t.
// 3x3 falloff [1 2 1 / 2 4 2 / 1 2 1], norm 16.
// 5x5 wider falloff for scorch, norm 256.

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
	[DECAL_BULLET]      = { K3x3, 3,  16, -40, -40, -40 },
	[DECAL_SPIKE]       = { K3x3, 3,  16, -40, -40, -40 },
	[DECAL_BLOOD_SPLAT] = { K3x3, 3,  16, +60, -40, -40 },
	[DECAL_SCORCH]      = { K5x5, 5, 256, -80, -80, -80 },
	[DECAL_LIGHTNING]   = { K3x3, 3,  16, -50, -60, -40 },
};
```

- [ ] **Step 7.2: Update R_DecalsTest_f to use the kernels (and shrink it)**

Replace the existing `Stain_PaintKernel (surf, lu, lv, -120, -120, -120, K3x3, 3, 16);` line in `R_DecalsTest_f` with:

```c
	const decal_kernel_t *dk = &decal_kernels[DECAL_BULLET];
	Stain_PaintKernel (surf, lu, lv, dk->dr, dk->dg, dk->db, dk->k, dk->ksize, dk->knorm);
```

And remove the now-unused `static const int K3x3[9]` declaration *inside* `R_DecalsTest_f` (the file-scope one above replaces it).

- [ ] **Step 7.3: Implement the retrace helper**

Add to `r_decals.c` (above `R_SpawnDecal`):

```c
// Fire a short trace from `pos` along several directions; return the closest
// world surface hit within 8 game units. Returns NULL if none.
// Also writes the hit point and surface normal back out.
static msurface_t *Retrace_ForDecal (vec3_t pos, vec3_t out_hit, vec3_t out_normal)
{
	static const vec3_t dirs[7] = {
		{  0,  0,  0 },  // first slot replaced with (eye - pos) at runtime
		{  1,  0,  0 }, { -1,  0,  0 },
		{  0,  1,  0 }, { 0, -1,  0 },
		{  0,  0,  1 }, { 0,  0, -1 },
	};
	vec3_t local_dirs[7];
	int    ndirs = 7;

	memcpy (local_dirs, dirs, sizeof(local_dirs));
	VectorSubtract (r_refdef.vieworg, pos, local_dirs[0]);
	if (VectorLength (local_dirs[0]) < 0.001f) {
		// player is at the impact point; skip the eye direction
		local_dirs[0][0] = 1; local_dirs[0][1] = 0; local_dirs[0][2] = 0;
	}
	VectorNormalize (local_dirs[0]);

	msurface_t *best     = NULL;
	float       best_len = 8.0f;
	vec3_t      best_hit = { 0, 0, 0 };
	vec3_t      best_nrm = { 0, 0, 0 };

	for (int i = 0; i < ndirs; i++) {
		vec3_t end;
		VectorMA (pos, 8.0f, local_dirs[i], end);

		// Use the world hull trace; we want surfaces only, so MOVE_NOMONSTERS.
		trace_t tr = SV_Move (pos, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
		if (tr.fraction >= 1.0f || tr.allsolid) continue;

		float len = tr.fraction * 8.0f;
		if (len >= best_len) continue;

		// Confirm the hit is on a world surface (not a brush model door / lift / train).
		// trace_t.ent comes from SV_Move; world is sv.edicts[0].
		extern server_t sv;
		if (tr.ent != NULL && tr.ent != sv.edicts) continue;

		msurface_t *s = R_PointOnSurface_World (tr.endpos, tr.plane.normal);
		if (!s) continue;

		best     = s;
		best_len = len;
		VectorCopy (tr.endpos,        best_hit);
		VectorCopy (tr.plane.normal,  best_nrm);
	}

	if (best) {
		if (out_hit)    VectorCopy (best_hit, out_hit);
		if (out_normal) VectorCopy (best_nrm, out_normal);
	}
	return best;
}
```

- [ ] **Step 7.4: Implement R_SpawnDecal**

Replace the stub `R_SpawnDecal` body with:

```c
void R_SpawnDecal (vec3_t pos, decal_type_t type)
{
	if (!r_decals.value) return;
	if (type < 0 || type >= DECAL_NUM_TYPES) return;

	vec3_t hit, normal;
	msurface_t *surf = Retrace_ForDecal (pos, hit, normal);
	if (!surf) return;

	// Project onto luxel grid.
	mtexinfo_t *tex = surf->texinfo;
	float u = DotProduct(hit, tex->vecs[0]) + tex->vecs[0][3];
	float v = DotProduct(hit, tex->vecs[1]) + tex->vecs[1][3];
	int   lu = ((int)floor(u) - surf->texturemins[0]) >> 4;
	int   lv = ((int)floor(v) - surf->texturemins[1]) >> 4;
	int   smax = (surf->extents[0] >> 4) + 1;
	int   tmax = (surf->extents[1] >> 4) + 1;
	if (lu < 0 || lu >= smax || lv < 0 || lv >= tmax) return;

	const decal_kernel_t *dk = &decal_kernels[type];
	Stain_PaintKernel (surf, lu, lv, dk->dr, dk->dg, dk->db, dk->k, dk->ksize, dk->knorm);
}
```

- [ ] **Step 7.5: Update R_DecalsTest_f to call R_SpawnDecal**

Now that we have the real spawn function, simplify `R_DecalsTest_f` to use it:

```c
static void R_DecalsTest_f (void)
{
	vec3_t forward, end;
	AngleVectors (r_refdef.viewangles, forward, NULL, NULL);
	VectorMA (r_refdef.vieworg, 1024, forward, end);

	trace_t tr = SV_Move (r_refdef.vieworg, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f) {
		Con_Printf ("r_decals_test: nothing in front\n");
		return;
	}
	// Step 1 unit back along the trace so the retrace has somewhere to go.
	vec3_t spawn_pos;
	VectorMA (tr.endpos, -1.0f, forward, spawn_pos);

	decal_type_t type = DECAL_BULLET;
	if (Cmd_Argc() > 1) {
		const char *t = Cmd_Argv (1);
		if      (!Q_strcasecmp(t, "bullet"))   type = DECAL_BULLET;
		else if (!Q_strcasecmp(t, "spike"))    type = DECAL_SPIKE;
		else if (!Q_strcasecmp(t, "blood"))    type = DECAL_BLOOD_SPLAT;
		else if (!Q_strcasecmp(t, "scorch"))   type = DECAL_SCORCH;
		else if (!Q_strcasecmp(t, "lightning"))type = DECAL_LIGHTNING;
	}
	R_SpawnDecal (spawn_pos, type);
	Con_Printf ("r_decals_test: spawned type %d at %.1f %.1f %.1f\n",
		(int)type, spawn_pos[0], spawn_pos[1], spawn_pos[2]);
}
```

`AngleVectors` is called with NULL for the unused output vectors — verify that's valid here by checking the existing callers. (If it is not, pass dummy locals.)

- [ ] **Step 7.6: Verify build**

Run: `zig build`
Expected: succeeds. If `AngleVectors` complains about NULL, replace with `vec3_t right, up; AngleVectors (r_refdef.viewangles, forward, right, up); (void)right; (void)up;`.

- [ ] **Step 7.7: Smoke test the type variants**

Run: `zig build run -- +map e1m1`
In console:

```
r_decals_test bullet
r_decals_test scorch
r_decals_test blood
```

Walk around: bullet & scorch should darken the wall; blood should redden it. Scorch should be a wider footprint than bullet.

- [ ] **Step 7.8: Commit**

```sh
git add sdlquake/engine_src/r_decals.c
git commit -m "feat(decals): R_SpawnDecal with retrace and per-type kernels"
git push
```

---

## Task 8: Wire R_SpawnDecal into CL_ParseTEnt impact events

**Goal:** Real weapon impacts leave decals — no console command needed.

**Files:**
- Modify: `sdlquake/engine_src/cl_tent.c`

- [ ] **Step 8.1: Hook each impact-style TE_ case**

Edit `sdlquake/engine_src/cl_tent.c`. For each of the following cases in `CL_ParseTEnt`, add an `R_SpawnDecal` call immediately after the existing `R_RunParticleEffect` / `R_ParticleExplosion` call and before the `S_StartSound` (or break, if no sound):

`TE_WIZSPIKE` (line ~130):
```c
		R_RunParticleEffect (pos, vec3_origin, 20, 30);
		R_SpawnDecal (pos, DECAL_SPIKE);
		S_StartSound (-1, 0, cl_sfx_wizhit, pos, 1, 1);
```

`TE_KNIGHTSPIKE` (line ~138):
```c
		R_RunParticleEffect (pos, vec3_origin, 226, 20);
		R_SpawnDecal (pos, DECAL_SPIKE);
		S_StartSound (-1, 0, cl_sfx_knighthit, pos, 1, 1);
```

`TE_SPIKE` (line ~146):
```c
		R_RunParticleEffect (pos, vec3_origin, 0, 10);
		R_SpawnDecal (pos, DECAL_SPIKE);
```
(Note: TE_SPIKE has a `#ifdef GLTEST` branch above the `R_RunParticleEffect` call — put `R_SpawnDecal` *after* the entire `#ifdef`/`#else`/`#endif` block, before `if (rand() % 5)`.)

`TE_SUPERSPIKE` (line ~168):
```c
		R_RunParticleEffect (pos, vec3_origin, 0, 20);
		R_SpawnDecal (pos, DECAL_SPIKE);
```

`TE_GUNSHOT` (line ~188):
```c
		R_RunParticleEffect (pos, vec3_origin, 0, 20);
		R_SpawnDecal (pos, DECAL_BULLET);
		break;
```

`TE_EXPLOSION` (line ~195):
```c
		R_ParticleExplosion (pos);
		R_SpawnDecal (pos, DECAL_SCORCH);
		dl = CL_AllocDlight (0);
		...
```

`TE_TAREXPLOSION` (line ~209):
```c
		R_BlobExplosion (pos);
		R_SpawnDecal (pos, DECAL_SCORCH);
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
```

`TE_EXPLOSION2` (line ~250):
```c
		R_ParticleExplosion2 (pos, colorStart, colorLength);
		R_SpawnDecal (pos, DECAL_SCORCH);
		dl = CL_AllocDlight (0);
		...
```

`TE_LIGHTNINGBLOOD` (search for "TE_LIGHTNINGBLOOD" — the case body): after the existing particle call, add `R_SpawnDecal (pos, DECAL_BLOOD_SPLAT);`.

`TE_BLOOD` (search for "TE_BLOOD" — the case body): after the existing particle call, add `R_SpawnDecal (pos, DECAL_BLOOD_SPLAT);`. Note that TE_BLOOD reads a `count` byte before the position; this doesn't affect the decal call.

- [ ] **Step 8.2: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 8.3: Smoke test in-game**

Run: `zig build run -- +map e1m1`

Pick up the shotgun in start hall. Fire at a brick wall close-up — dark stain appears. Switch to super shotgun and rocket launcher (`impulse 7`) — fire a rocket at a wall, a wider darker scorch appears. Walk into the next room where a grunt is; the grunt's bullets hitting walls leave decals too. Kill the grunt at point-blank against a wall — see red blood splat on the wall behind it (sometimes — retrace may whiff if the grunt isn't right against the wall).

- [ ] **Step 8.4: Commit**

```sh
git add sdlquake/engine_src/cl_tent.c
git commit -m "feat(decals): spawn decals from CL_ParseTEnt impact events"
git push
```

---

## Task 9: Lightning beam endpoint → DECAL_LIGHTNING

**Goal:** Firing the lightning gun (or a shambler bolting the wall) leaves a cool-tinted scorch at the beam's endpoint.

**Files:**
- Modify: `sdlquake/engine_src/cl_tent.c`

- [ ] **Step 9.1: Locate CL_ParseBeam**

Find `CL_ParseBeam` in `cl_tent.c` (around line 70-110 — search for `void CL_ParseBeam`). It reads the entity index, start, and end coordinates and stores them in a `beam_t` slot.

Look at the *callers* (`CL_ParseTEnt` cases `TE_LIGHTNING1`, `TE_LIGHTNING2`, `TE_LIGHTNING3`, `TE_BEAM`). After `CL_ParseBeam` returns, the most-recently-written beam slot has the start/end. Capture the endpoint and call `R_SpawnDecal`.

- [ ] **Step 9.2: Add a CL_ParseBeam wrapper that returns endpoint**

The cleanest option is to extract the last-written beam's end position. Add a small helper in `cl_tent.c`. Above `CL_ParseTEnt`, add:

```c
// Read the most recent beam record's end position, or zero if none exists.
// Call immediately after CL_ParseBeam.
static void CL_LastBeamEnd (vec3_t out_end)
{
	int i;
	beam_t *latest = NULL;
	float   latest_time = -1.0f;
	for (i = 0; i < MAX_BEAMS; i++) {
		beam_t *b = &cl_beams[i];
		if (!b->model) continue;
		if (b->endtime > latest_time) {
			latest_time = b->endtime;
			latest = b;
		}
	}
	if (latest) VectorCopy (latest->end, out_end);
	else        VectorClear (out_end);
}
```

(If `VectorClear` isn't available, use `out_end[0] = out_end[1] = out_end[2] = 0;`.)

- [ ] **Step 9.3: Hook the lightning cases**

In `CL_ParseTEnt`, modify each of the three lightning cases:

```c
	case TE_LIGHTNING1:				// lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt.mdl", true));
		{ vec3_t end; CL_LastBeamEnd (end); R_SpawnDecal (end, DECAL_LIGHTNING); }
		break;
	
	case TE_LIGHTNING2:				// lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt2.mdl", true));
		{ vec3_t end; CL_LastBeamEnd (end); R_SpawnDecal (end, DECAL_LIGHTNING); }
		break;
	
	case TE_LIGHTNING3:				// lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt3.mdl", true));
		{ vec3_t end; CL_LastBeamEnd (end); R_SpawnDecal (end, DECAL_LIGHTNING); }
		break;
```

`TE_BEAM` (grappling hook) is left alone — it doesn't represent a "hit".

- [ ] **Step 9.4: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 9.5: Smoke test**

Run: `zig build run -- +map e1m1`
In console: `impulse 8` to switch to the lightning gun (if cheats allow; or use the map e2m4 which has the lightning gun pickup). Fire at a wall — cool-tinted dark stains appear along the beam endpoint trail. Walk into a shambler in a later map and have it lightning-bolt a wall behind you.

If `impulse 8` doesn't work, use `give` cheat: `give 8 200` (lightning gun + cells) — adjust per your cheat setup.

- [ ] **Step 9.6: Commit**

```sh
git add sdlquake/engine_src/cl_tent.c
git commit -m "feat(decals): spawn lightning decals at beam endpoints"
git push
```

---

## Task 10: Blood pools — bloodpool_t state + R_SpawnBloodPool + R_DecalsFrame

**Goal:** Per-frame state machine that grows a pool of blood from a spawn origin over `r_decals_bloodpool_growtime` seconds. Not yet triggered — Task 11 wires the spawn call.

**Files:**
- Modify: `sdlquake/engine_src/r_decals.c`
- Modify: `sdlquake/engine_src/r_main.c`

- [ ] **Step 10.1: Add bloodpool state**

Edit `sdlquake/engine_src/r_decals.c`. Above `R_DecalsClear`, add:

```c
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
```

- [ ] **Step 10.2: Extend R_DecalsClear to zero bloodpools**

In `R_DecalsClear`, after the existing stain-pool reset, add:

```c
	memset (r_bloodpools, 0, sizeof(r_bloodpools));
```

- [ ] **Step 10.3: Implement R_SpawnBloodPool**

Replace the stub `R_SpawnBloodPool` body with:

```c
void R_SpawnBloodPool (vec3_t origin)
{
	int i;
	if (!r_decals.value || !r_decals_bloodpool.value) return;

	// Trace straight down to find the floor.
	vec3_t end;
	end[0] = origin[0];
	end[1] = origin[1];
	end[2] = origin[2] - 64.0f;
	trace_t tr = SV_Move (origin, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, NULL);
	if (tr.fraction >= 1.0f || tr.plane.normal[2] < 0.7f) return;  // no floor / too steep

	msurface_t *surf = R_PointOnSurface_World (tr.endpos, tr.plane.normal);
	if (!surf) return;

	// Find an empty slot or recycle the oldest.
	int   slot = -1;
	float oldest_time = 1e9f;
	int   oldest_slot = 0;
	for (i = 0; i < MAX_ACTIVE_BLOODPOOLS; i++) {
		if (!r_bloodpools[i].alive) { slot = i; break; }
		if (r_bloodpools[i].spawn_time < oldest_time) {
			oldest_time = r_bloodpools[i].spawn_time;
			oldest_slot = i;
		}
	}
	if (slot < 0) {
		// Force the oldest to finish instantly so its luxels stay permanent.
		bloodpool_t *p = &r_bloodpools[oldest_slot];
		p->radius_painted = p->radius_max;  // will skip painting on next frame
		p->alive          = false;
		slot              = oldest_slot;
	}

	bloodpool_t *bp = &r_bloodpools[slot];
	VectorCopy (tr.endpos, bp->origin);
	bp->surf           = surf;
	bp->spawn_time     = cl.time;
	bp->radius_max     = r_decals_bloodpool_radius.value;
	bp->radius_painted = 0.0f;
	bp->alive          = true;
}
```

- [ ] **Step 10.4: Implement R_DecalsFrame**

Replace the stub `R_DecalsFrame` body with:

```c
// Per-frame growth tick for active blood pools.
void R_DecalsFrame (void)
{
	int   i;
	float dur = r_decals_bloodpool_growtime.value;
	if (dur <= 0.001f) dur = 0.001f;

	for (i = 0; i < MAX_ACTIVE_BLOODPOOLS; i++) {
		bloodpool_t *bp = &r_bloodpools[i];
		if (!bp->alive) continue;

		float t        = (cl.time - bp->spawn_time) / dur;
		if (t < 0.0f)  t = 0.0f;
		if (t > 1.0f)  t = 1.0f;
		float target  = bp->radius_max * t;
		if (target <= bp->radius_painted) {
			if (t >= 1.0f) bp->alive = false;
			continue;
		}

		// Paint the annulus [radius_painted, target] into the floor surface
		// at this pool's origin. We iterate over a bounded luxel box and add
		// blood-color deltas to those whose game-space distance falls in the
		// annulus.
		msurface_t *surf = bp->surf;
		if (!surf) { bp->alive = false; continue; }

		mtexinfo_t *tex = surf->texinfo;
		float ou = DotProduct(bp->origin, tex->vecs[0]) + tex->vecs[0][3];
		float ov = DotProduct(bp->origin, tex->vecs[1]) + tex->vecs[1][3];
		int   olu = ((int)floor(ou) - surf->texturemins[0]) >> 4;
		int   olv = ((int)floor(ov) - surf->texturemins[1]) >> 4;
		int   smax = (surf->extents[0] >> 4) + 1;
		int   tmax = (surf->extents[1] >> 4) + 1;

		int luxel_radius = (int)((target / 16.0f) + 1.0f);
		stain_t *st = Stain_GetOrAlloc (surf);
		if (!st) { bp->alive = false; continue; }

		float r_inner_sq = bp->radius_painted * bp->radius_painted;
		float r_outer_sq = target * target;

		for (int dy = -luxel_radius; dy <= luxel_radius; dy++) {
			int v = olv + dy;
			if (v < 0 || v >= tmax) continue;
			for (int dx = -luxel_radius; dx <= luxel_radius; dx++) {
				int u = olu + dx;
				if (u < 0 || u >= smax) continue;
				float gx = dx * 16.0f;
				float gy = dy * 16.0f;
				float dsq = gx*gx + gy*gy;
				if (dsq < r_inner_sq || dsq > r_outer_sq) continue;
				int idx = (v * smax + u) * 3;
				int nr = st->rgb[idx + 0] + 80;
				int ng = st->rgb[idx + 1] - 60;
				int nb = st->rgb[idx + 2] - 60;
				if (nr > 4096) nr = 4096;
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
```

- [ ] **Step 10.5: Wire R_DecalsFrame into R_RenderView**

Edit `sdlquake/engine_src/r_main.c`. Find `R_RenderView` (search for `void R_RenderView`). At the top of the function body, after the time guard if present, add:

```c
	R_DecalsFrame ();
```

A reasonable location is right after the existing `R_AnimateLight ();` call or near the top of the function — just ensure it runs once per frame and before surfaces are drawn.

- [ ] **Step 10.6: Verify build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 10.7: Smoke (no triggers yet)**

Run: `zig build run -- +map e1m1`
Game runs normally; pools can't spawn yet because no caller exists. Still confirm: no crash, no perf hit.

- [ ] **Step 10.8: Commit**

```sh
git add sdlquake/engine_src/r_decals.c sdlquake/engine_src/r_main.c
git commit -m "feat(decals): blood pool active list and per-frame growth"
git push
```

---

## Task 11: Add engine_api->spawn_blood_pool and bump GAME_API_VERSION

**Goal:** Cross-DLL ABI extended with one new entry. game.dll won't compile until the next task wires it; but the engine side is done.

**Files:**
- Modify: `sdlquake/game/game_api.h`
- Modify: `sdlquake/engine/hotreload.c`

- [ ] **Step 11.1: Bump version + add entry to game_api.h**

Edit `sdlquake/game/game_api.h`. Change `#define GAME_API_VERSION 15` to `16`.

Find the `engine_api_t` struct. After the `SV_TraceMove` entry (line ~174):

```c
    void  (*SV_TraceMove)(vec3_t start, vec3_t mins, vec3_t maxs,
                          vec3_t end, int nomonsters, edict_t *skip);
```

Add immediately after (before the closing `} engine_api_t;`):

```c
    // Spawn a growing blood pool on the floor beneath origin. Called from
    // game.dll's Killed() on monster death. No-op if no floor within 64
    // units, or if r_decals/r_decals_bloodpool is 0.
    void  (*spawn_blood_pool)(const vec3_t origin);
```

- [ ] **Step 11.2: Add wrapper in hotreload.c**

Edit `sdlquake/engine/hotreload.c`. Search for `engine_sv_tracemove` to find the section with other engine_ wrappers. Add this new wrapper near it:

```c
static void engine_spawn_blood_pool (const vec3_t origin)
{
    vec3_t o;
    o[0] = origin[0]; o[1] = origin[1]; o[2] = origin[2];
    R_SpawnBloodPool (o);
}
```

`R_SpawnBloodPool` is declared in `r_local.h`; ensure hotreload.c either includes `r_local.h` or forward-declares the function. The cleanest fix is the forward declaration if `r_local.h` would pull in too much; check the existing includes at the top of `hotreload.c`. If the file already includes `quakedef.h`, then `r_local.h` may be reachable via a single `#include "r_local.h"`.

If adding the include is messy, just declare locally above `engine_spawn_blood_pool`:

```c
extern void R_SpawnBloodPool (vec3_t origin);
```

- [ ] **Step 11.3: Add entry to engine_funcs table**

Still in `sdlquake/engine/hotreload.c`, find the `static engine_api_t engine_funcs = { ... };` initializer (line ~859). The last entry is `engine_sv_tracemove,` (line ~935). Add the new entry on the next line:

```c
    engine_sv_tracemove,
    engine_spawn_blood_pool,
};
```

- [ ] **Step 11.4: Verify build**

Run: `zig build`
Expected: engine build succeeds. game.dll build will FAIL because `GAME_API_VERSION` is now 16 but the game.dll's compile-time check expects 16 only after Task 12 implements the death-site call (the version constant itself comes from the same header, so this should actually still compile — but the runtime version check may now reject the loaded DLL).

If the runtime rejects the DLL, the next task (12) is needed before the game launches.

If `zig build` fails for the game.dll, that's expected — proceed to Task 12.

- [ ] **Step 11.5: Commit**

```sh
git add sdlquake/game/game_api.h sdlquake/engine/hotreload.c
git commit -m "feat(decals): add engine_api->spawn_blood_pool, bump ABI to 16"
git push
```

---

## Task 12: game.dll Killed() calls spawn_blood_pool

**Goal:** Monster deaths actually spawn the pool.

**Files:**
- Modify: `sdlquake/game/combat.c`

- [ ] **Step 12.1: Hook Killed**

Edit `sdlquake/game/combat.c`. Find the `Killed` function (line ~57). Find the section where `FL_MONSTER` kills increment counters:

```c
    if ((int)g->self->v.flags & FL_MONSTER) {
        g->killed_monsters++;
        eng->MSG_WriteByte(MSG_ALL, SVC_KILLEDMONSTER);
    }
```

Add the blood-pool call immediately inside that `if`:

```c
    if ((int)g->self->v.flags & FL_MONSTER) {
        g->killed_monsters++;
        eng->MSG_WriteByte(MSG_ALL, SVC_KILLEDMONSTER);
        eng->spawn_blood_pool(g->self->v.origin);
    }
```

- [ ] **Step 12.2: Verify build (game.dll only)**

Run: `zig build game`
Expected: succeeds.

- [ ] **Step 12.3: Verify full build**

Run: `zig build`
Expected: succeeds.

- [ ] **Step 12.4: Smoke test**

Run: `zig build run -- +map e1m1`
Walk into the first room past a grunt and shoot it on a flat tiled floor. Expect: as the grunt dies, a red blob grows out from under it over ~3 seconds, settling at ~24-unit radius. Walk back to it — pool persists.

Kill 5 grunts in a row in the start hall — each leaves a pool. Floor surfaces near `start` are large enough that pools fit comfortably.

Edge: kill a monster mid-air (the next room has a vore that floats? if not, push a grunt off a ledge with the shotgun knockback). Expect: no pool (downward trace whiffs).

- [ ] **Step 12.5: Commit**

```sh
git add sdlquake/game/combat.c
git commit -m "feat(decals): spawn blood pool on monster death"
git push
```

---

## Task 13: Wrap-up — ideas.md + full smoke pass

**Goal:** Drop "decals" from the wishlist and confirm the whole system still works end-to-end after all integrations.

**Files:**
- Modify: `ideas.md`

- [ ] **Step 13.1: Update ideas.md**

Edit `ideas.md`. In the `## Next` section, remove the line:

```
1. **decals** (bullet holes, blood) — combat feedback for the Phase 8 immersive-sim work; contained scope
```

Renumber the following item if needed. In the `## Rendering & visuals` section, also remove the line `* decals — bullet holes, blood, dirt & grime, scratches` (it's now shipped).

- [ ] **Step 13.2: Full smoke pass**

Run: `zig build run -- +map e1m1`

Walk through this checklist in-game:
1. Shotgun a brick wall close-up → dark stain.
2. Nailgun (`impulse 4`) → small dark stains. (Pickup is in room past the start hall.)
3. Rocket (`impulse 7`) at wall → wider scorch.
4. Kill a grunt at close range → blood splat behind it (if it was near the wall).
5. Kill a grunt on a flat floor → blood pool grows.
6. Try `r_decals_test scorch` and `r_decals_test bullet` from the console for direct verification.
7. Fire shotgun at the sky in start → no decal (sky filter).
8. Step into water at the end → no decal there.
9. Shoot the start-hall lift while it's moving → no decal on the lift (brush model filter).
10. `restart` → all decals gone.
11. `r_coloredlight 0` → fire shotgun → stains still appear, in luminance only.
12. `r_decals 0` → fire shotgun → no new stains. Existing ones stay until restart.

- [ ] **Step 13.3: Commit**

```sh
git add ideas.md
git commit -m "docs(ideas): drop shipped decals entry"
git push
```

---

## Memory update (post-merge follow-up, not a task)

After shipping, add a `project_decals.md` memory note if any non-obvious surprises came up during smoke testing — e.g., a particular surface flag combination that defeats the retrace, or a tuning value that needed adjustment from spec defaults. Include the corrected value and the reason. Index it from MEMORY.md.

---

## Self-review

**Spec coverage:** Walking each section of the spec:
- "Stain layer in the lightmap" — Tasks 3, 4, 5 ✓
- "Impact spawn pipeline" with all 8 TE_ events listed in spec — Task 8 ✓ (verify all 8 mappings in code)
- "Lightning beam endpoint at TE_LIGHTNING1/2/3" — Task 9 ✓
- "Blood pools on monster death" — Tasks 10, 11, 12 ✓
- "Per-surface stain buffer (lazy alloc, hunk-owned)" — Task 1 + 2 ✓
- "Fixed-size pool sized by r_decals_max at map load" — Task 2 ✓
- "LRU eviction" — Task 2 ✓
- "Cvars: r_decals/_max/_intensity/_bloodpool/_bloodpool_radius/_bloodpool_growtime" — Task 1 ✓
- "Cache invalidation via cached_stain_gen" — Task 5 ✓
- "ABI bump GAME_API_VERSION + engine_api->spawn_blood_pool" — Task 11 ✓
- "Surface filters: sky/water/drawtiled/brush models" — Task 7 (in `R_PointOnSurface_World` + retrace world-only check) ✓
- "Mono lightmap path luminance fallback" — Task 4 ✓
- "Kernel tables per type" — Task 7 ✓

**Placeholder scan:** No "TBD" / "TODO" / "fill in details" / "similar to Task N". Each step shows the code that goes in. The one "if AngleVectors doesn't accept NULL" branch in Step 7.5 is a conditional fallback, not a placeholder — both alternatives are spelled out.

**Type consistency:** `stain_t` fields used in Task 6 (`smax`, `tmax`, `rgb`, `generation`, `last_touched_frame`) match Task 1 declaration. `decal_kernel_t` introduced in Task 7. `bloodpool_t` introduced in Task 10. `engine_api_t.spawn_blood_pool` signature `void(*)(const vec3_t)` matches `engine_spawn_blood_pool` wrapper and the call site `eng->spawn_blood_pool(g->self->v.origin)`.

**Open risks the implementer should know:**
1. `AngleVectors` may not accept NULL for unused outputs (Step 7.5) — fallback spelled out.
2. `Q_strcasecmp` may be named differently in this codebase — likely it's actually `Q_strcasecmp` per the include chain, but the implementer should grep if Step 7.5 fails to link.
3. `SV_Move` and `trace_t` are defined in `world.h` / `server.h` — `r_decals.c` may need the include. Add `#include "server.h"` near the top if `SV_Move` is unresolved.
4. The retrace happens at decal-spawn time, which is client-side; calling `SV_Move` from the client thread is acceptable in single-player loopback because client and server share an address space — but document this caveat in `r_decals.c`'s top comment.
5. `r_decals_max` is read once in `R_DecalsClear` (at map load). Changing it mid-game doesn't take effect until the next map load — documented in the cvar table of the spec but worth mentioning in `R_DecalsClear`'s comment.

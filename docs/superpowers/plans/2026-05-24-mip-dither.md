# Mip-dither blend implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the visible per-surface mip pop with a Bayer-dithered blend baked into the surface cache, so transitions between mips appear as a brief noise band instead of a hard snap.

**Architecture:** Per-surface mip selection in `d_edge.c` produces both a `miplevel` (the sharper of the two mips being blended) and a `bucket` (the blend weight). The surface cache grows a bucket dimension (`cachespots[mip][bucket]`). For `bucket > 0`, `R_DrawSurface` pre-blends mip-N and mip-N+1 raw texels into a scratch buffer using the existing `r_bayer4x4` table, then substitutes that scratch for `r_source`. The existing per-mip block writers (`surfmiptable[]` and `surfmiptable_rgb[]`) are unchanged — they apply the lightmap to the dithered palette indices as if it were a normal texture.

**Tech Stack:** C (gnu89), Zig build (`zig build`), SDL3 platform. No unit-test suite — verification is build success + manual in-game observation per CLAUDE.md.

**Source of truth:** `docs/superpowers/specs/2026-05-24-mip-dither-design.md`

---

## File map

| File | Action | Responsibility |
|---|---|---|
| `sdlquake/engine_src/d_local.h` | Modify | Add `NUM_DITHER_BUCKETS`; bump `D_CacheSurface` signature; expose new cvars |
| `sdlquake/engine_src/d_init.c` | Modify | Register `r_mipdither` and `r_mipdither_band` cvars |
| `sdlquake/engine_src/d_iface.h` | Modify | Add `surf_bucket` to `drawsurf_t` |
| `sdlquake/engine_src/model.h` | Modify | Expand `cachespots[MIPLEVELS]` → `[MIPLEVELS][NUM_DITHER_BUCKETS]`; add `last_bucket` to `msurface_t` |
| `sdlquake/engine_src/d_surf.c` | Modify | `D_CacheSurface` takes bucket; owner backpointer; cvar-generation invalidation |
| `sdlquake/engine_src/d_edge.c` | Modify | Mip-and-bucket selection with hysteresis; dlight skip |
| `sdlquake/engine_src/r_surf.c` | Modify | Dither scratch + `r_source` substitution in `R_DrawSurface` |

No new files.

---

### Task 1: Add cvars and grow `cachespots` to `[mip][bucket]` (no behavior change)

This is a mechanical refactor. All call sites pass `bucket = 0`, so the engine should behave bit-identically to baseline. The point of doing it first is to land the type-system / call-signature changes in isolation so any compile breakage is easy to triage.

**Files:**
- Modify: `sdlquake/engine_src/d_local.h`
- Modify: `sdlquake/engine_src/d_init.c`
- Modify: `sdlquake/engine_src/d_iface.h`
- Modify: `sdlquake/engine_src/model.h`
- Modify: `sdlquake/engine_src/d_surf.c`
- Modify: `sdlquake/engine_src/d_edge.c`

- [ ] **Step 1.1: Add `NUM_DITHER_BUCKETS`, cvar externs, and updated signature to `d_local.h`**

Find the existing `D_CacheSurface` declaration at line 85 and replace. Also add the constant and cvar externs near the other top-level externs.

Old (line 85):
```c
surfcache_t	*D_CacheSurface (msurface_t *surface, int miplevel);
```

New:
```c
#define NUM_DITHER_BUCKETS 8

extern cvar_t	r_mipdither;
extern cvar_t	r_mipdither_band;

surfcache_t	*D_CacheSurface (msurface_t *surface, int miplevel, int bucket);
```

- [ ] **Step 1.2: Register cvars in `d_init.c`**

In `d_init.c` near the existing `d_mipscale` cvar (around line 36), add:

```c
cvar_t	r_mipdither      = {"r_mipdither", "1"};
cvar_t	r_mipdither_band = {"r_mipdither_band", "0.3"};
```

In `D_Init()` near the existing `Cvar_RegisterVariable (&d_mipscale);` (line 62), add:

```c
Cvar_RegisterVariable (&r_mipdither);
Cvar_RegisterVariable (&r_mipdither_band);
```

- [ ] **Step 1.3: Add `surf_bucket` to `drawsurf_t` in `d_iface.h`**

Find the `drawsurf_t` struct (around line 215). Add a field after `surfheight`:

```c
typedef struct
{
	pixel_t		*surfdat;
	int			rowbytes;
	msurface_t	*surf;
	fixed8_t	lightadj[MAXLIGHTMAPS];
	texture_t	*texture;
	int			surfmip;
	int			surfwidth;
	int			surfheight;
	int			surf_bucket;	// 0 = pure mip surfmip; >0 = dithered blend toward surfmip+1
} drawsurf_t;
```

- [ ] **Step 1.4: Grow `cachespots` and add `last_bucket` in `model.h`**

Find the `msurface_t` struct (line 118 area). Replace the existing `cachespots` field and add `last_bucket` near `last_miplevel`:

Old:
```c
struct surfcache_s	*cachespots[MIPLEVELS];

// Per-surface mip hysteresis ...
signed char	last_miplevel;
```

New:
```c
struct surfcache_s	*cachespots[MIPLEVELS][NUM_DITHER_BUCKETS];

// Per-surface mip hysteresis ...
signed char	last_miplevel;
// Per-surface bucket hysteresis: remembers the dither bucket chosen last
// frame this surface was visible. Used together with last_miplevel to
// prevent cache thrash when standing still inside a dither transition
// band. -1 = no previous bucket (treat as fresh).
signed char	last_bucket;
```

Note: `NUM_DITHER_BUCKETS` is `#define`d in `d_local.h`. `model.h` is included before `d_local.h` in some translation units, so add a defensive `#ifndef NUM_DITHER_BUCKETS` guard at the top of `model.h` (after the existing includes):

```c
#ifndef NUM_DITHER_BUCKETS
#define NUM_DITHER_BUCKETS 8
#endif
```

- [ ] **Step 1.5: Update `D_CacheSurface` in `d_surf.c` to take `bucket` and index `cachespots[mip][bucket]`**

Find `D_CacheSurface` at line 265. Replace the whole function (about lines 260–339):

Old signature:
```c
surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel)
```

New signature:
```c
surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel, int bucket)
```

Inside the function, every reference to `surface->cachespots[miplevel]` becomes `surface->cachespots[miplevel][bucket]`:

```c
// Line 281
cache = surface->cachespots[miplevel][bucket];

// Line 311–312
cache = D_SCAlloc (r_drawsurf.surfwidth,
				   r_drawsurf.surfwidth * r_drawsurf.surfheight);
surface->cachespots[miplevel][bucket] = cache;
cache->owner = (struct cache_user_s **)&surface->cachespots[miplevel][bucket];

// Line 338
return surface->cachespots[miplevel][bucket];
```

Also set `r_drawsurf.surf_bucket = bucket;` near the existing `r_drawsurf.surfmip = miplevel;` (line 299):

```c
surfscale = 1.0 / (1<<miplevel);
r_drawsurf.surfmip = miplevel;
r_drawsurf.surf_bucket = bucket;
r_drawsurf.surfwidth = surface->extents[0] >> miplevel;
```

- [ ] **Step 1.6: Update the single `D_CacheSurface` call site in `d_edge.c`**

Line 349:

Old:
```c
pcurrentcache = D_CacheSurface (pface, miplevel);
```

New (always pass 0 for now; bucket selection comes in Task 3):
```c
pcurrentcache = D_CacheSurface (pface, miplevel, 0);
```

- [ ] **Step 1.7: Initialize `last_bucket = -1` wherever `last_miplevel = -1` is set**

Grep for `last_miplevel` assignments outside `d_edge.c`:

```bash
grep -rn "last_miplevel" sdlquake/engine_src/ | grep -v d_edge.c
```

Likely matches: somewhere in the BSP loader (`gl_model.c` or `model.c`) initializes `last_miplevel = -1` for newly-loaded surfaces. Add a parallel `last_bucket = -1` assignment at each such site. Expected ~1–2 sites.

- [ ] **Step 1.8: Check `D_FlushCaches` and other cachespots iterators**

Grep:

```bash
grep -rn "cachespots" sdlquake/engine_src/
```

Any code that loops `for (i = 0; i < MIPLEVELS; i++) surface->cachespots[i] = NULL;` (or similar) must be extended to also clear the bucket dimension:

```c
for (i = 0; i < MIPLEVELS; i++)
	for (j = 0; j < NUM_DITHER_BUCKETS; j++)
		surface->cachespots[i][j] = NULL;
```

Expected sites: `D_FlushCaches` in `d_surf.c`, the surfcache LRU sweep, possibly `R_NewMap`. Patch each.

- [ ] **Step 1.9: Build**

Run: `zig build`
Expected: Clean build with no errors or warnings related to the changes. If a warning about `cache->owner` pointer cast appears, double-check Step 1.5's cast to `(struct cache_user_s **)`.

- [ ] **Step 1.10: Run and verify no visual regression**

Run: `zig build run -- +map e1m1`
Expected: Game launches, e1m1 renders identically to baseline. Walk around and confirm mip pop is still visible exactly as before (we haven't enabled dithering yet — `bucket` is always 0).

- [ ] **Step 1.11: Commit**

```bash
git add sdlquake/engine_src/d_local.h sdlquake/engine_src/d_init.c \
        sdlquake/engine_src/d_iface.h sdlquake/engine_src/model.h \
        sdlquake/engine_src/d_surf.c sdlquake/engine_src/d_edge.c \
        sdlquake/engine_src/gl_model.c   # (or whichever file step 1.7 patched)
git commit -m "$(cat <<'EOF'
refactor(renderer): widen surfcache to [mip][bucket]; add mip-dither cvars

Grows msurface_t.cachespots from [MIPLEVELS] to [MIPLEVELS][NUM_DITHER_BUCKETS]
and threads a bucket parameter through D_CacheSurface and the r_drawsurf
struct. All call sites pass bucket=0, so behavior is unchanged from baseline.
Registers r_mipdither and r_mipdither_band cvars (not yet wired up).

Prep for Bayer-dithered blend between adjacent mip levels.
EOF
)"
```

---

### Task 2: Pre-blended scratch fill in `R_DrawSurface` (still no caller-side dithering)

Add the scratch buffer + Bayer fill inside `R_DrawSurface`, gated on `r_drawsurf.surf_bucket > 0`. Caller still always passes `bucket = 0`, so this code path is dead until Task 3 wires up the selection. Adding it now lets us verify the scratch logic in isolation by temporarily forcing `bucket = 1` in a hack edit if needed.

**Files:**
- Modify: `sdlquake/engine_src/r_surf.c`

- [ ] **Step 2.1: Add the scratch buffer and dither fill in `R_DrawSurface`**

In `r_surf.c`, find `R_DrawSurface` (line 523) and locate this block (around line 552–574):

```c
r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];

// r_lightmap: replace texel sampling with a uniform mid-gray ...
if (r_lightmap.value) { ... }

// r_drawflat 2: replace texel sampling with a category-coloured uniform ...
if (r_drawflat.value == 2) { ... }
```

Insert the dither fill **after** the `r_drawflat` block but before `texwidth = mt->width >> r_drawsurf.surfmip;`. The fill must skip when `r_lightmap` or `r_drawflat == 2` is active so those debug overrides keep working.

```c
// Mip-dither blend: when surf_bucket > 0, build a pre-blended scratch
// texture mixing mip N and mip N+1 via Bayer ordered dither, then have
// r_source point at it. The per-mip block writers (surfmiptable[] and
// surfmiptable_rgb[]) are unchanged - they apply the lightmap to the
// dithered palette indices as if it were a normal texture.
//
// Skip when r_lightmap or r_drawflat==2 active so those debug overrides
// keep stomping r_source themselves.
if (r_drawsurf.surf_bucket > 0
	&& !r_lightmap.value
	&& r_drawflat.value != 2)
{
	static byte r_mipdither_scratch[256 * 256];   // largest mip-0 texture
	int N        = r_drawsurf.surfmip;
	int w_N      = mt->width  >> N;
	int h_N      = mt->height >> N;
	int w_N1     = mt->width  >> (N + 1);
	byte *src_N  = (byte *)mt + mt->offsets[N];
	byte *src_N1 = (byte *)mt + mt->offsets[N + 1];
	int threshold = (r_drawsurf.surf_bucket * 16) / NUM_DITHER_BUCKETS;
	int s, t;

	for (t = 0; t < h_N; t++)
	{
		int t_n1 = t >> 1;
		for (s = 0; s < w_N; s++)
		{
			int b = r_bayer4x4[((t & 3) << 2) | (s & 3)];
			r_mipdither_scratch[t * w_N + s] = (b < threshold)
				? src_N1[t_n1 * w_N1 + (s >> 1)]
				: src_N[t * w_N + s];
		}
	}
	r_source = r_mipdither_scratch;
}
```

Note: `r_bayer4x4` is already declared in this file at line 61. `NUM_DITHER_BUCKETS` is reachable via `d_local.h` (transitively included). If the build complains, add `#include "d_local.h"` near the top of `r_surf.c`.

- [ ] **Step 2.2: Build**

Run: `zig build`
Expected: Clean build.

- [ ] **Step 2.3: Smoke-test the dither path with a temporary forced bucket**

Add a temporary hack at the top of `R_DrawSurface` (will be reverted in step 2.5):

```c
if (r_drawsurf.surf != NULL && r_drawsurf.surfmip == 1)
	r_drawsurf.surf_bucket = 4;     // TEMP: force mid-bucket on mip-1 surfaces
```

Run: `zig build run -- +map e1m1`
Expected: Mip-1 surfaces (mid-distance walls) visibly show Bayer noise mixing mip 1 and mip 2 texels. The noise pattern is locked to texture coords (camera motion doesn't slide it across the wall — it stays glued to the wall).

- [ ] **Step 2.4: Smoke-test cache validity by forcing bucket=4 then bucket=0**

With the temporary hack still in place, in-game open the console (`~`) and toggle the hack indirectly: change `r_mipdither_band` to a non-zero number and back. The dithered cache entries are at `cachespots[1][4]`; the bucket-0 path uses `cachespots[1][0]`. Both should coexist without crashes.

(This is a smoke check that the LRU eviction owner pointers from Step 1.5 work correctly.)

- [ ] **Step 2.5: Revert the temporary hack**

Remove the `if (r_drawsurf.surf != NULL && r_drawsurf.surfmip == 1) r_drawsurf.surf_bucket = 4;` line. Confirm by running once more — game should render baseline visuals (no dither anywhere, all surfaces back at `bucket = 0`).

- [ ] **Step 2.6: Commit**

```bash
git add sdlquake/engine_src/r_surf.c
git commit -m "$(cat <<'EOF'
feat(renderer): pre-blended dither scratch path in R_DrawSurface

When r_drawsurf.surf_bucket > 0, build a Bayer-dithered blend of mip N
and mip N+1 raw texels into a scratch buffer and substitute r_source.
Block writers (mono and RGB) and lightmap application are unchanged.

Dead path until mip selection wires up non-zero buckets - lands the
scratch fill in isolation for easier triage.
EOF
)"
```

---

### Task 3: Wire up mip-and-bucket selection in `d_edge.c`

Replace the existing mip-selection block with the spec's pseudocode (both-sides band). Defer bucket hysteresis to Task 4 and dlight skip to Task 5 — this task lands the visible behavior.

**Files:**
- Modify: `sdlquake/engine_src/d_edge.c`

- [ ] **Step 3.1: Replace the mip-selection block at `d_edge.c:323–346`**

The current block reads:

```c
pface = s->data;
{
	float mipscale = s->nearzi * scale_for_mip
		* pface->texinfo->mipadjust;
	int raw_mip = D_MipLevelForScale(mipscale);
	int prev    = pface->last_miplevel;
	// Hysteresis: hold prev mip while scale sits within a
	// 15% deadband of the threshold that separates prev
	// from raw. ...
	if (prev >= 0 && prev != raw_mip)
	{
		int b = (prev < raw_mip) ? prev : raw_mip;
		if (b >= 0 && b < MIPLEVELS - 1)
		{
			float t = d_scalemip[b];
			if (mipscale > t * 0.85f
			 && mipscale < t * 1.15f)
				raw_mip = prev;
		}
	}
	pface->last_miplevel = (signed char)raw_mip;
	miplevel = raw_mip;
}

// FIXME: make this passed in to D_CacheSurface
	pcurrentcache = D_CacheSurface (pface, miplevel, 0);
```

Replace with:

```c
pface = s->data;
{
	float mipscale = s->nearzi * scale_for_mip
		* pface->texinfo->mipadjust;
	int raw_mip = D_MipLevelForScale(mipscale);
	int prev    = pface->last_miplevel;
	int bucket  = 0;

	// Existing mip-level hysteresis: hold prev mip while scale
	// sits within a 15% deadband of the boundary.
	if (prev >= 0 && prev != raw_mip)
	{
		int b = (prev < raw_mip) ? prev : raw_mip;
		if (b >= 0 && b < MIPLEVELS - 1)
		{
			float t = d_scalemip[b];
			if (mipscale > t * 0.85f
			 && mipscale < t * 1.15f)
				raw_mip = prev;
		}
	}

	miplevel = raw_mip;

	// Mip-dither bucket selection. Two boundary bands can apply:
	// the one below raw_mip (between raw_mip and raw_mip+1) and
	// the one above (between raw_mip-1 and raw_mip). They cannot
	// overlap because bands are 2 * r_mipdither_band of threshold
	// and thresholds are ~2x apart in mipscale.
	if (r_mipdither.value)
	{
		float band = r_mipdither_band.value;
		if (band > 0.0f)
		{
			// Lower boundary: dither toward raw_mip+1.
			if (raw_mip + 1 <= MIPLEVELS - 1)
			{
				float t_lo = d_scalemip[raw_mip];
				float hi   = t_lo * (1.0f + band);
				float lo   = t_lo * (1.0f - band);
				if (mipscale < hi && mipscale > lo)
				{
					float t = (hi - mipscale) / (hi - lo);
					int   q = (int)(t * (NUM_DITHER_BUCKETS - 1) + 0.5f);
					if (q < 0) q = 0;
					if (q > NUM_DITHER_BUCKETS - 1)
						q = NUM_DITHER_BUCKETS - 1;
					miplevel = raw_mip;
					bucket   = q;
				}
			}
			// Upper boundary: dither toward raw_mip-1 (sharper).
			else if (raw_mip - 1 >= d_minmip && raw_mip >= 1)
			{
				float t_hi = d_scalemip[raw_mip - 1];
				float hi   = t_hi * (1.0f + band);
				float lo   = t_hi * (1.0f - band);
				if (mipscale < hi && mipscale > lo)
				{
					float t = (hi - mipscale) / (hi - lo);
					int   q = (int)(t * (NUM_DITHER_BUCKETS - 1) + 0.5f);
					if (q < 0) q = 0;
					if (q > NUM_DITHER_BUCKETS - 1)
						q = NUM_DITHER_BUCKETS - 1;
					miplevel = raw_mip - 1;
					bucket   = q;
				}
			}
		}
	}

	pface->last_miplevel = (signed char)miplevel;
	pface->last_bucket   = (signed char)bucket;
}

pcurrentcache = D_CacheSurface (pface, miplevel, bucket);
```

Note: the `if`/`else if` for the two bands is correct per the spec — only one band can match because the bands don't overlap.

- [ ] **Step 3.2: Build**

Run: `zig build`
Expected: Clean build.

- [ ] **Step 3.3: Verify the disable path is bit-identical to baseline**

Run: `zig build run -- +map e1m1`
At the console: `r_mipdither 0`
Expected: Identical visuals to pre-Task-3 baseline. Walk around — mip pops are visible exactly as before. This confirms the disable path leaks nothing.

- [ ] **Step 3.4: Verify dither is visible with the default cvar value**

At the console: `r_mipdither 1` (the default).
Expected: Walk slowly toward a flat wall (e.g. the start area of e1m1, or the wall just past the slipgate). The mip transition is no longer a single-frame snap — instead a band of visible Bayer noise smoothly resolves as you cross it. Standing still inside the band, the noise is static (it doesn't shimmer between frames).

- [ ] **Step 3.5: Tune-test `r_mipdither_band`**

Try `r_mipdither_band 0.5` — band is much wider, dither is obvious across larger distance. Try `r_mipdither_band 0.1` — band is barely visible.

Reset to `r_mipdither_band 0.3` after testing.

- [ ] **Step 3.6: Commit**

```bash
git add sdlquake/engine_src/d_edge.c
git commit -m "$(cat <<'EOF'
feat(renderer): mip-and-bucket selection for dithered surface cache

Replaces the per-surface mip pick in d_edge.c with a both-sides band
selector that, when mipscale is within r_mipdither_band of a d_scalemip
threshold, picks the sharper mip + a non-zero bucket. R_DrawSurface
(prior commit) consumes the bucket and pre-blends mip N and mip N+1
into the cached surface.

r_mipdither 0 falls straight through to the existing single-mip path
(bit-identical to baseline). No bucket hysteresis yet - standing still
inside a band may show one bucket-step thrash; fixed in next commit.
EOF
)"
```

---

### Task 4: Bucket hysteresis (prevent rebuild thrash when standing still)

Without hysteresis, a surface sitting near a bucket boundary will rebuild its dithered cache on each frame as `mipscale` jitters. The fix is a deadband: hold the previous bucket while `t * (N-1)` is within 0.75 of the previous bucket's index (instead of the natural round-to-nearest 0.5).

**Files:**
- Modify: `sdlquake/engine_src/d_edge.c`

- [ ] **Step 4.1: Add bucket hysteresis in `d_edge.c`**

In the block from Step 3.1, replace the bucket-quantization in both branches (lower and upper boundary). The new pattern:

In the lower boundary branch, replace:

```c
float t = (hi - mipscale) / (hi - lo);
int   q = (int)(t * (NUM_DITHER_BUCKETS - 1) + 0.5f);
if (q < 0) q = 0;
if (q > NUM_DITHER_BUCKETS - 1)
	q = NUM_DITHER_BUCKETS - 1;
miplevel = raw_mip;
bucket   = q;
```

With:

```c
float t   = (hi - mipscale) / (hi - lo);
float tn  = t * (NUM_DITHER_BUCKETS - 1);
int   q;
// Bucket hysteresis: hold prev_bucket while we're within 0.75 of it
// (vs. the natural round() boundary at 0.5). Only applies when the
// previous frame's surface was on the same (miplevel, raw_mip) pair.
if (pface->last_miplevel == raw_mip
	&& pface->last_bucket   >= 0
	&& pface->last_bucket   <= NUM_DITHER_BUCKETS - 1
	&& fabsf(tn - (float)pface->last_bucket) < 0.75f)
{
	q = pface->last_bucket;
}
else
{
	q = (int)(tn + 0.5f);
	if (q < 0) q = 0;
	if (q > NUM_DITHER_BUCKETS - 1)
		q = NUM_DITHER_BUCKETS - 1;
}
miplevel = raw_mip;
bucket   = q;
```

In the upper boundary branch (`miplevel = raw_mip - 1`), make the same change but with the `last_miplevel == raw_mip - 1` guard:

```c
float t   = (hi - mipscale) / (hi - lo);
float tn  = t * (NUM_DITHER_BUCKETS - 1);
int   q;
if (pface->last_miplevel == raw_mip - 1
	&& pface->last_bucket   >= 0
	&& pface->last_bucket   <= NUM_DITHER_BUCKETS - 1
	&& fabsf(tn - (float)pface->last_bucket) < 0.75f)
{
	q = pface->last_bucket;
}
else
{
	q = (int)(tn + 0.5f);
	if (q < 0) q = 0;
	if (q > NUM_DITHER_BUCKETS - 1)
		q = NUM_DITHER_BUCKETS - 1;
}
miplevel = raw_mip - 1;
bucket   = q;
```

Make sure `<math.h>` is included for `fabsf` (it almost certainly is via `quakedef.h` already; if the build complains, add `#include <math.h>` to `d_edge.c`).

- [ ] **Step 4.2: Build**

Run: `zig build`
Expected: Clean build.

- [ ] **Step 4.3: Verify bucket stability when standing still**

Run: `zig build run -- +map e1m1`
Walk to a position where a wall is clearly inside a dither band (you'll see the noise pattern on it). Stand perfectly still for ~10 seconds. The dither pattern should be static — no visible bucket changes, no flickering. Move slightly forward and back; the bucket should change smoothly with motion, not flap.

If you have the dev overlay (`F3`) and a frame-time counter, confirm no per-frame stutter that would indicate cache rebuilds every frame.

- [ ] **Step 4.4: Commit**

```bash
git add sdlquake/engine_src/d_edge.c
git commit -m "$(cat <<'EOF'
feat(renderer): bucket hysteresis to prevent dither cache thrash

Holds the previous bucket while the continuous bucket index (t * (N-1))
stays within 0.75 of it - 50% wider than the natural round-to-nearest
boundary at 0.5. Stops surfaces sitting on a bucket seam from rebuilding
their dithered cache every frame.

Hysteresis only applies when last_miplevel matches the currently-selected
sharper mip; a mip-level change resets bucket memory.
EOF
)"
```

---

### Task 5: Skip dither when surface is touched by a dlight

Dlit surfaces already rebuild their cache every frame. Adding the Bayer fill to that per-frame rebuild costs more than the dither buys visually — the colored light masks any mip step anyway. Force `bucket = 0` whenever the surface is dlit this frame.

**Files:**
- Modify: `sdlquake/engine_src/d_edge.c`

- [ ] **Step 5.1: Add dlight skip in `d_edge.c`**

In the block from Step 3.1 (as modified by Step 4.1), add an early-exit at the start of the `if (r_mipdither.value)` block:

```c
if (r_mipdither.value)
{
	// Dlit surfaces rebuild cache every frame; skip dither to keep
	// that path cheap. Coloured dlight masks mip-step pop anyway.
	if (pface->dlightframe == r_framecount)
	{
		// fall through with miplevel = raw_mip, bucket = 0
	}
	else
	{
		float band = r_mipdither_band.value;
		if (band > 0.0f)
		{
			// ... existing lower / upper boundary branches ...
		}
	}
}
```

- [ ] **Step 5.2: Build**

Run: `zig build`
Expected: Clean build.

- [ ] **Step 5.3: Verify dlit surfaces don't pay dither cost**

Run: `zig build run -- +map e1m1`
With `r_mipdither 1`, find a wall inside a dither band (showing noise). Switch to the lightning gun (or any rocket/explosion light source). Fire at the wall. While the dlight is active, the noise pattern on that wall should briefly disappear (replaced by the pure-mip lit texture); after the dlight fades, the dither should return.

- [ ] **Step 5.4: Commit**

```bash
git add sdlquake/engine_src/d_edge.c
git commit -m "$(cat <<'EOF'
perf(renderer): skip mip-dither on dlit surfaces

Dlight-touched surfaces rebuild their surface cache every frame. Adding
a Bayer fill to each rebuild has measurable cost while the dlight's
coloured intensity already masks any mip-step pop. Force bucket = 0
whenever dlightframe matches r_framecount.
EOF
)"
```

---

### Task 6: Invalidate dithered caches on `r_mipdither_band` change

Without this, after the user types `r_mipdither_band 0.5` at the console, existing dithered cache entries are stale (built with the old band, so their per-texel choices reflect the old quantization). The fix is a generation counter: bump on cvar change, store on each cache entry, treat mismatches as stale.

**Files:**
- Modify: `sdlquake/engine_src/d_surf.c`
- Modify: `sdlquake/engine_src/d_edge.c`
- Modify: `sdlquake/engine_src/d_local.h` (for `surfcache_t`)

- [ ] **Step 6.1: Find the `surfcache_t` definition**

```bash
grep -n "surfcache_s\|surfcache_t" sdlquake/engine_src/d_local.h
```

Locate the `surfcache_s` struct definition (it'll have fields like `data`, `owner`, `texture`, `lightadj`, `dlight`, `mipscale`, `stain_gen`). Add a new field at the end:

```c
typedef struct surfcache_s
{
	struct surfcache_s	*next;
	struct surfcache_s	**owner;
	...
	int					stain_gen;
	int					dither_gen;	// matches r_mipdither_generation snapshot at build
	unsigned			data[4];
} surfcache_t;
```

- [ ] **Step 6.2: Add the global generation counter in `d_surf.c`**

Near the top of `d_surf.c` (after the existing externs):

```c
int r_mipdither_generation = 0;
static float r_mipdither_band_last = -1.0f;
static int   r_mipdither_last      = -1;

// Called by D_CacheSurface before each lookup. Bumps the generation
// counter whenever an r_mipdither* cvar changes so stale cache entries
// can be detected via the cached dither_gen field.
static void D_MipDither_CheckCvars (void)
{
	if (r_mipdither.value      != (float)r_mipdither_last
	 || r_mipdither_band.value != r_mipdither_band_last)
	{
		r_mipdither_generation++;
		r_mipdither_last      = (int)r_mipdither.value;
		r_mipdither_band_last = r_mipdither_band.value;
	}
}
```

Expose the counter in `d_local.h`:

```c
extern int r_mipdither_generation;
```

- [ ] **Step 6.3: Wire the check into `D_CacheSurface` validity test**

In `D_CacheSurface` (now modified for bucket from Task 1.5), add the cvar check + extend the validity check:

```c
surfcache_t *D_CacheSurface (msurface_t *surface, int miplevel, int bucket)
{
	surfcache_t     *cache;

	D_MipDither_CheckCvars ();   // <<< new line

	r_drawsurf.texture = R_TextureAnimation (surface->texinfo->texture);
	r_drawsurf.lightadj[0] = d_lightstylevalue[surface->styles[0]];
	...

	cache = surface->cachespots[miplevel][bucket];

	{
		int cur_stain_gen = surface->stain ? surface->stain->generation : 0;
		if (cache && !cache->dlight && surface->dlightframe != r_framecount
				&& cache->texture == r_drawsurf.texture
				&& cache->lightadj[0] == r_drawsurf.lightadj[0]
				&& cache->lightadj[1] == r_drawsurf.lightadj[1]
				&& cache->lightadj[2] == r_drawsurf.lightadj[2]
				&& cache->lightadj[3] == r_drawsurf.lightadj[3]
				&& cache->stain_gen == cur_stain_gen
				&& (bucket == 0
					|| cache->dither_gen == r_mipdither_generation) )   // <<< new
			return cache;
	}
	...
```

After the cache build (just before `return surface->cachespots[miplevel][bucket];` at the end), set the generation:

```c
	cache->stain_gen = surface->stain ? surface->stain->generation : 0;
	cache->dither_gen = r_mipdither_generation;	// <<< new

	r_drawsurf.surf = surface;

	c_surf++;
	R_DrawSurface ();

	return surface->cachespots[miplevel][bucket];
}
```

- [ ] **Step 6.4: Build**

Run: `zig build`
Expected: Clean build.

- [ ] **Step 6.5: Verify live cvar tuning rebuilds caches correctly**

Run: `zig build run -- +map e1m1`
Walk to a wall in a dither band. Open the console.

1. `r_mipdither_band 0.5` — the noise band on visible walls should widen visibly within a frame or two as their caches rebuild.
2. `r_mipdither_band 0.1` — band narrows, noise becomes subtle.
3. `r_mipdither 0` — all noise disappears (rebuilds to bucket-0 entries).
4. `r_mipdither 1` — noise returns at default 0.3 band.

No crashes, no stale/garbled cache entries (which would look like obviously-wrong colored noise persisting on walls).

- [ ] **Step 6.6: Commit**

```bash
git add sdlquake/engine_src/d_surf.c sdlquake/engine_src/d_local.h
git commit -m "$(cat <<'EOF'
fix(renderer): invalidate dithered surface caches on cvar change

Adds dither_gen to surfcache_t and a global r_mipdither_generation
counter bumped whenever r_mipdither or r_mipdither_band changes.
D_CacheSurface treats bucket>0 entries with stale generation as
invalid and rebuilds them. Bucket-0 entries are unaffected (they
don't depend on dither cvars).

Lets r_mipdither_band be tuned live from the console without leaving
stale cache content on walls.
EOF
)"
```

---

### Task 7: Final acceptance pass (no code changes)

Walk through the spec's verification section to confirm everything works end-to-end.

- [ ] **Step 7.1: Bit-identical baseline with `r_mipdither 0`**

Boot `+map e1m1` with `r_mipdither 0` set in `id1/config.cfg` (or via `~/.quakerc` — whichever the project uses). Compare side-by-side to a baseline build (master before Task 1). Visuals should be indistinguishable.

- [ ] **Step 7.2: Visible dither with default cvars**

`r_mipdither 1`, `r_mipdither_band 0.3`. Walk through e1m1 — confirm transitions are no longer pops, they're brief noise bands.

- [ ] **Step 7.3: No frame-rate stutter inside transition bands**

Stand inside a transition band; use the dev overlay's frame-time counter (`F3`). No periodic spikes.

- [ ] **Step 7.4: Dlit surfaces stay on the fast path**

Fire a rocket near a dithered wall; the dither vanishes from the dlit region, returns when the light decays.

- [ ] **Step 7.5: Live cvar tuning works**

Toggle `r_mipdither` 0/1 and step `r_mipdither_band` through 0.1, 0.3, 0.5 — visuals respond within a frame, no crashes.

- [ ] **Step 7.6: Verify mip-3 (coarsest) and `d_mipcap`-bound surfaces don't dither**

Set `d_mipcap 1` — mip 0 is forbidden. Walls that would have dithered between mip 0 and 1 now sit at pure mip 1 (no dither across the cap'd boundary). Walls between mip 1 and 2, or mip 2 and 3, still dither normally.

For mip 3: at very long distance, walls hit mip 3 (the coarsest) and stop dithering further out (no mip 4 to blend toward). Visually confirm: distant geometry has no noise band — it's just snapped to mip 3 plain.

- [ ] **Step 7.7: Save the plan as done**

If everything passes, the plan is complete. No code changes in this step.

---

## Risks and watch-outs

- **`r_bayer4x4` symbol clash:** it lives in `r_surf.c` and is `const unsigned char r_bayer4x4[16]`. If it's not exported, no problem — Task 2's scratch fill is in the same TU. If a future task needs it elsewhere, add an `extern` in `d_local.h`.

- **`mt->offsets[N+1]` valid for all textures?** Quake textures always have 4 mip levels (`MIPLEVELS = 4`). `offsets[N+1]` is only dereferenced when `N+1 <= 3`. The mip-selection guard at Step 3.1 prevents accessing `offsets[4]`.

- **256×256 scratch size:** Verify no built-in or custom texture exceeds this. Stock Quake textures cap at 256×256. If a mod ships larger, the static scratch overflows. Run a sanity check with `grep` on `mt->width` ceilings or just trust Quake's hard limit.

- **`cachespots` size in `msurface_t`:** Grows by 21 pointers per surface (~168 bytes on x64). Per CLAUDE.md this is a 1996 engine; no struct-size assumptions in serialized save files (save files store entity state, not surface cache pointers).

- **Endianness/alignment:** No new packed structs or binary file changes. Adding fields to existing structs is safe; `surfcache_t.dither_gen` (`int`) is naturally aligned.

## Self-review notes (writer)

- **Spec coverage:** every numbered section of the spec maps to a task. Cvars → Task 1.2; cache layout → Task 1.4–1.6; mip-and-bucket selection (both bands) → Task 3.1; bucket hysteresis → Task 4; dithered cache build → Task 2; dlight skip → Task 5; cvar invalidation → Task 6; mip-3 and `d_mipcap` edges → guarded in Task 3.1 selection logic and acceptance-checked in Task 7.6.
- **Placeholder scan:** no TBDs. All code blocks shown in full. Step 1.7 and 1.8 require a `grep` because the surface area of `cachespots` and `last_miplevel` callers isn't fully enumerated in the spec — the grep gives an exact list, and patches are shown.
- **Type consistency:** `bucket` is `int` everywhere; `last_bucket` is `signed char` (matches `last_miplevel`); `NUM_DITHER_BUCKETS` is a `#define`, not a cvar (per the spec: "fixed at compile time"). `surfcache_t.dither_gen` is `int`, matching the global counter.

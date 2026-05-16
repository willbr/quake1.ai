# Cached Lightmap Deltas — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-frame dlight pump used by `paint_light_preview` (and `Light_AddOverride` from Gust) with a persistent `live_rgblightdata` buffer that the renderer reads via `surf->rgb_samples`. Light deltas (editor preview lights + extinguished torches) are written once into the buffer on dirty events instead of recomputed every frame. The 64-override cap in `sim_light.c` is lifted to 1024 so all map lights can be turned off.

**Architecture:** A new engine-side mutable lightmap buffer (`s_engine_live_rgb`) is allocated at map load and seeded from the baked `rgblightdata` (.lit data). Every `surf->rgb_samples` points at this buffer. The engine maintains an override list `(pos, radius, color, owner)`; `Lightmap_AddDelta` appends + applies additively, `Lightmap_ClearOwner` rebuilds (restore baseline + replay survivors). The dlight pipeline (`R_PushDlights`, `dlightbits`, `R_AddDynamicLights_RGB`) is taken out of the preview/extinguish path entirely — it stays for transient gameplay dlights (muzzle flashes, explosions). ABI bump 18 → 19 adds `Lightmap_AddDelta` + `Lightmap_ClearOwner` to `engine_api_t`. `Light_AddOverride` (DLL, sim_light.c) calls into the engine; the editor preview pump is rewritten as a dirty-driven rebuilder. After-bake completion in `light_bake_thread.c` reseeds the baseline and replays overrides.

**Tech Stack:** C (gnu89 in engine_src, modern C in engine/editor/game), Zig build, SDL3.

**Verification model:** No test framework — verification is `zig build`, smoke run with `+map`, and MCP screenshot A/B for visible changes (per memory `feedback_visual_verification_via_mcp.md` and `feedback_smoke_test.md`). Map for the editor preview test: `e1m1`. Map for the gust extinguish test: `m7_skeleton`. Commit + push directly to master after each task lands (memory `feedback_no_pr.md`).

---

## File map

| File | Action | Purpose |
|---|---|---|
| `sdlquake/engine_src/model.h` | modify | Add `live_rgblightdata` field to `model_t`. |
| `sdlquake/engine_src/model.c` | modify | At `Mod_LoadLighting`/`Mod_LoadFaces`, allocate the live buffer + repoint `surf->rgb_samples` at it. |
| `sdlquake/engine_src/r_livelight.h` | create | Public API for the engine-side live-lightmap state machine. |
| `sdlquake/engine_src/r_livelight.c` | create | `Lightmap_Init`, `Lightmap_Shutdown`, `Lightmap_CommitBaseline`, `Lightmap_AddDelta`, `Lightmap_ClearOwner`, `Lightmap_ClearAll`; falloff math; surface-cache invalidation. |
| `build.zig` | modify | Add the new `.c` to the engine compile list. |
| `sdlquake/engine_src/cl_main.c` | modify | Call `Lightmap_ClearAll` on `CL_ClearState` (level change). |
| `sdlquake/engine/editor/light_bake_thread.c` | modify | After `apply_snapshot`, re-seed live buffer from the new baked data + replay overrides. |
| `sdlquake/game/game_api.h` | modify | Add `Lightmap_AddDelta`, `Lightmap_ClearOwner` to `engine_api_t`; bump `GAME_API_VERSION` 18 → 19. |
| `sdlquake/engine/hotreload.c` | modify | Populate the new function-pointer slots. |
| `sdlquake/game/sim/sim_light.c` | modify | Lift `MAX_OVERRIDES` 64 → 1024; have `Light_AddOverride` call `eng->Lightmap_AddDelta`. |
| `sdlquake/engine/editor/editor.c` | modify | Delete per-frame `Editor_RefreshDlights` call; add `Editor_LightPreview_Rebuild` (dirty-driven); call it on toggle / `Editor_ApplyKV` / bake completion. |
| `sdlquake/engine_src/view.c` | modify | Remove `Editor_RefreshDlights` call from `V_RenderView`. |

**Owner tags** (defined in `r_livelight.h`):
- `LIGHTMAP_OWNER_EDITOR = 1` — paint_light_preview adds.
- `LIGHTMAP_OWNER_GAMEPLAY = 2` — Light_AddOverride (Gust extinguishes).

---

## Task 1 — Live RGB lightmap buffer at map load

**Goal:** Allocate `live_rgblightdata` at .lit-load time, memcpy from baked, point every `surf->rgb_samples` at it. No callers yet — visual result must be **byte-identical** to the existing behaviour.

**Files:**
- Modify: `sdlquake/engine_src/model.h`
- Modify: `sdlquake/engine_src/model.c`
- Modify: `sdlquake/engine_src/cl_main.c`
- Modify: `build.zig` (no — defer to Task 2 since no new file yet)

### Steps

- [ ] **Step 1.1: Add `live_rgblightdata` field**

In `sdlquake/engine_src/model.h`, find the `model_t` struct (look for the existing `rgblightdata` field). Add right after it:

```c
	byte		*rgblightdata;
	byte		*live_rgblightdata;	/* engine-mutable copy of rgblightdata;
									   surf->rgb_samples points here. Init
									   from rgblightdata at load; mutated
									   by r_livelight.c overrides. NULL
									   when rgblightdata is NULL. */
```

- [ ] **Step 1.2: Allocate + seed live buffer at load**

In `sdlquake/engine_src/model.c`, locate `Mod_LoadLITFile` (around line 519). After the line `loadmodel->rgblightdata = raw + 8;`, append:

```c
	/* Engine-side mutable copy. surf->rgb_samples points here so the
	 * delta system in r_livelight.c can write per-event without
	 * disturbing the baked rgblightdata which stays canonical for
	 * "restore baseline" rebuilds. */
	{
		int rgb_size = mono_size * 3;
		loadmodel->live_rgblightdata = Hunk_AllocName(rgb_size, "livelit");
		memcpy(loadmodel->live_rgblightdata, loadmodel->rgblightdata, rgb_size);
	}
```

Also at the top of `Mod_LoadLITFile`, find `loadmodel->rgblightdata = NULL;` and right under it add:
```c
	loadmodel->live_rgblightdata = NULL;
```

And in `Mod_LoadLighting`, in the empty-lightmap branch (`if (!l->filelen)`):
```c
	loadmodel->lightdata = NULL;
	loadmodel->rgblightdata = NULL;
	loadmodel->live_rgblightdata = NULL;
	return;
```

- [ ] **Step 1.3: Point surf->rgb_samples at live buffer**

In `sdlquake/engine_src/model.c`, find `Mod_LoadFaces` (look for `out->rgb_samples = loadmodel->rgblightdata`, around line 865). Change:

```c
			out->rgb_samples = loadmodel->rgblightdata
				? loadmodel->rgblightdata + i * 3
				: NULL;
```

to:

```c
			out->rgb_samples = loadmodel->live_rgblightdata
				? loadmodel->live_rgblightdata + i * 3
				: NULL;
```

- [ ] **Step 1.4: Build + smoke**

Run:
```sh
zig build run -- +map e1m1
```

Expected: builds cleanly, e1m1 renders identical to before (the live buffer is a verbatim copy of the baked data, so visual must match exactly). Walk into the lit corridor; nothing should look different.

- [ ] **Step 1.5: MCP screenshot baseline**

Before/after comparison screenshot via MCP:
- Pick a stable view in e1m1 (start area).
- Take screenshot, save as `docs/scratch/livelit_baseline.png`.

- [ ] **Step 1.6: Commit + push**

```sh
git add sdlquake/engine_src/model.h sdlquake/engine_src/model.c
git commit -m "feat(engine): add live_rgblightdata buffer (no callers yet)"
git push
```

---

## Task 2 — `r_livelight.c` with Lightmap_AddDelta / ClearOwner / ClearAll

**Goal:** Engine-side state machine for the override list + the per-texel falloff math + cache invalidation. Add a `r_livelight_test` console command for manual exercise. No DLL/editor wiring yet.

**Files:**
- Create: `sdlquake/engine_src/r_livelight.h`
- Create: `sdlquake/engine_src/r_livelight.c`
- Modify: `build.zig` (add to engine sources list)
- Modify: `sdlquake/engine_src/cl_main.c` (call `Lightmap_ClearAll` on `CL_ClearState`)

### Steps

- [ ] **Step 2.1: Write the header**

Create `sdlquake/engine_src/r_livelight.h`:

```c
#ifndef R_LIVELIGHT_H
#define R_LIVELIGHT_H

/*
 * Live RGB lightmap delta system. Editor preview lights and gust-
 * extinguished torches write into the per-map mutable rgblightdata
 * (model.live_rgblightdata) once per dirty event; the per-frame
 * dlight pipeline (R_PushDlights / R_AddDynamicLights_RGB) is no
 * longer involved for these long-lived contributions.
 *
 * Owners:
 *   LIGHTMAP_OWNER_EDITOR    paint_light_preview entries.
 *   LIGHTMAP_OWNER_GAMEPLAY  DLL-driven (sim_light Light_AddOverride).
 *
 * The override list is the source of truth; live_rgblightdata is a
 * cache. ClearOwner walks the list, drops matching entries, then
 * restores baseline from rgblightdata and replays survivors.
 */

#include "quakedef.h"

#define LIGHTMAP_OWNER_EDITOR    1
#define LIGHTMAP_OWNER_GAMEPLAY  2

#define LIGHTMAP_MAX_OVERRIDES   1024

void Lightmap_Init(void);
void Lightmap_Shutdown(void);

/* Called on level change (CL_ClearState). Drops every override and
 * lets the next Mod_LoadLighting re-seed via the model loader path. */
void Lightmap_ClearAll(void);

/* pos, radius in world units. color in 0..1 per channel; will be
 * applied with the same per-texel falloff as R_AddDynamicLights_RGB,
 * then clamped to byte range. Negative components darken (gust). */
void Lightmap_AddDelta(const vec3_t pos, float radius,
                       const vec3_t color, int owner);

/* Remove every override matching `owner`, rebuild buffer = baseline
 * + Σ(surviving overrides). D_FlushCaches at end. */
void Lightmap_ClearOwner(int owner);

#endif
```

- [ ] **Step 2.2: Write the C implementation**

Create `sdlquake/engine_src/r_livelight.c`:

```c
#include "quakedef.h"
#include "r_livelight.h"
#include "r_local.h"

typedef struct {
	vec3_t pos;
	float  radius;
	vec3_t color;
	int    owner;
} livelight_override_t;

static livelight_override_t s_overrides[LIGHTMAP_MAX_OVERRIDES];
static int                  s_override_count;

void Lightmap_Init(void)
{
	s_override_count = 0;
	Cmd_AddCommand("r_livelight_test", Lightmap_TestCmd_f);
	Cmd_AddCommand("r_livelight_restore", Lightmap_RestoreCmd_f);
	Cmd_AddCommand("r_livelight_dump", Lightmap_DumpCmd_f);
}

void Lightmap_Shutdown(void)
{
	s_override_count = 0;
}

void Lightmap_ClearAll(void)
{
	s_override_count = 0;
	/* Buffer reseeding happens implicitly: a fresh map calls
	 * Mod_LoadLighting which Hunk_AllocNames a fresh live buffer
	 * pre-seeded from rgblightdata. Nothing to copy here. */
}

/* Walk a surface within the light's BSP cull window and accumulate
 * per-texel falloff into live_rgblightdata. Math mirrors
 * R_AddDynamicLights_RGB in r_surf.c so the visual matches the
 * existing per-frame preview. */
static void apply_to_surface(msurface_t *surf,
                             const vec3_t pos, float radius,
                             const vec3_t color, int sign)
{
	int    smax, tmax;
	float  dist, rad, minlight;
	vec3_t impact, local;
	int    s, t, sd, td;
	mtexinfo_t *tex;
	byte   *lm;

	if (!surf->rgb_samples) return;
	if (surf->flags & SURF_DRAWTILED) return;

	rad  = radius;
	dist = DotProduct(pos, surf->plane->normal) - surf->plane->dist;
	rad -= fabs(dist);
	if (rad < 0) return;
	minlight = rad;

	tex = surf->texinfo;
	smax = (surf->extents[0] >> 4) + 1;
	tmax = (surf->extents[1] >> 4) + 1;

	{
		int i;
		for (i = 0; i < 3; i++)
			impact[i] = pos[i] - surf->plane->normal[i] * dist;
	}
	local[0] = DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3];
	local[1] = DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3];
	local[0] -= surf->texturemins[0];
	local[1] -= surf->texturemins[1];

	lm = surf->rgb_samples;
	for (t = 0; t < tmax; t++) {
		td = (int)(local[1] - t*16);
		if (td < 0) td = -td;
		for (s = 0; s < smax; s++) {
			int    idx = (t*smax + s) * 3;
			float  d, add;
			int    ch;
			sd = (int)(local[0] - s*16);
			if (sd < 0) sd = -sd;
			d = (sd > td) ? (float)sd + (td>>1) : (float)td + (sd>>1);
			if (d >= minlight) continue;
			/* (rad - d) is 0..rad. Scale picked to roughly match the
			 * old per-frame preview at d_lightstylevalue['m'-'a']=264.
			 * Tweak if visual A/B drifts. */
			add = (rad - d);
			for (ch = 0; ch < 3; ch++) {
				int v = (int)lm[idx + ch] + sign * (int)(add * color[ch]);
				if (v < 0)   v = 0;
				if (v > 255) v = 255;
				lm[idx + ch] = (byte)v;
			}
		}
	}

	/* Force surface cache rebuild for this surf next frame. */
	if (surf->dlightframe != r_framecount) {
		DLIGHTBITS_CLEAR(&surf->dlightbits);
		surf->dlightframe = r_framecount;
	}
}

/* Recursive BSP walk gated on light's plane distance, same shape as
 * R_MarkLights but applies the delta and doesn't touch dlightbits as
 * a marker (we use dlightframe stamping only for cache invalidation). */
static void apply_recursive(mnode_t *node,
                            const vec3_t pos, float radius,
                            const vec3_t color, int sign)
{
	float dist;
	msurface_t *surf;
	int i;

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
	VectorCopy(pos, o->pos);
	VectorCopy(color, o->color);
	o->radius = radius;
	o->owner  = owner;
	apply_recursive(cl.worldmodel->nodes, pos, radius, color, +1);
	D_FlushCaches();
}

/* Restore live_rgblightdata = rgblightdata (the baked baseline)
 * over the entire lightmap region. */
static void restore_baseline(void)
{
	model_t *m = cl.worldmodel;
	int size;
	msurface_t *surf;
	int i;
	if (!m || !m->live_rgblightdata || !m->rgblightdata) return;
	/* Size: the .lit file is 3× the mono lightdata; loadmodel set
	 * live_rgblightdata as `Hunk_AllocName(mono_size*3,...)`. We
	 * don't store the size, but mono buffer's size is tracked by
	 * Mod_LoadLighting via l->filelen — easier route: walk surfaces
	 * and copy their per-surface region. Each face is contiguous in
	 * the lightmap; we use the same offset math the loader used. */
	(void)size;
	/* Simpler: bulk-memcpy the whole region. The .lit file format
	 * gives us a flat 3× mono buffer; we copy that many bytes. */
	{
		extern int loadmodel_rgblightdata_size; /* see Step 2.3 */
		if (loadmodel_rgblightdata_size > 0)
			memcpy(m->live_rgblightdata, m->rgblightdata,
			       loadmodel_rgblightdata_size);
	}
	surf = m->surfaces;
	for (i = 0; i < m->numsurfaces; i++, surf++) {
		if (surf->dlightframe != r_framecount) {
			DLIGHTBITS_CLEAR(&surf->dlightbits);
			surf->dlightframe = r_framecount;
		}
	}
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

/* Console commands for manual A/B. */
static void Lightmap_TestCmd_f(void)
{
	vec3_t pos, color;
	float radius;
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
```

Note: `restore_baseline` references `loadmodel_rgblightdata_size` which we need to expose. See next step.

- [ ] **Step 2.3: Expose rgblightdata size**

In `sdlquake/engine_src/model.c` near the top file-scope:

```c
int loadmodel_rgblightdata_size = 0;
```

In `Mod_LoadLITFile` after `loadmodel->rgblightdata = raw + 8;` set:

```c
	loadmodel_rgblightdata_size = mono_size * 3;
```

In `Mod_LoadLighting`'s empty branch and at `loadmodel->rgblightdata = NULL;` paths set:
```c
	loadmodel_rgblightdata_size = 0;
```

In `r_livelight.c`, drop the `extern int loadmodel_rgblightdata_size;` from inside `restore_baseline` and add it at top file-scope:

```c
extern int loadmodel_rgblightdata_size;
```

- [ ] **Step 2.4: Forward-declare the cmd functions**

In `r_livelight.c`, add forward declarations before `Lightmap_Init`:

```c
static void Lightmap_TestCmd_f(void);
static void Lightmap_RestoreCmd_f(void);
static void Lightmap_DumpCmd_f(void);
```

- [ ] **Step 2.5: Call Lightmap_Init from R_Init**

Find `R_Init` in `sdlquake/engine_src/r_main.c`. At the end of the function (after all the cvar registrations and command registrations), add:

```c
	Lightmap_Init();
```

Add include at top of `r_main.c`:
```c
#include "r_livelight.h"
```

- [ ] **Step 2.6: Hook level change**

In `sdlquake/engine_src/cl_main.c`, find `CL_ClearState`. Near the top (after the `memset(&cl, 0, sizeof(cl))` is fine), add:

```c
	Lightmap_ClearAll();
```

Add include at top:
```c
#include "r_livelight.h"
```

- [ ] **Step 2.7: Add to build.zig**

Open `build.zig`. Find the engine sources list (search for `r_light.c` to locate it). Add `r_livelight.c` to the same list. Build the file with the same flags as other engine_src files (`-std=gnu89 -fcommon -fno-sanitize=undefined`).

- [ ] **Step 2.8: Build**

```sh
zig build
```

Expected: clean build, no warnings about implicit declarations.

- [ ] **Step 2.9: Smoke test the console commands**

```sh
zig build run -- +map e1m1
```

In the in-game console:
```
r_livelight_dump
r_livelight_test 472 -344 88 200 1 0.5 0.5
r_livelight_dump
r_livelight_restore
```

Expected: After `test`, see a reddish patch near coords (472, -344, 88) in e1m1 (the spawn area). After `restore`, returns to baked. `dump` reports 1 override → 0 overrides.

Take an MCP screenshot of the reddish patch as evidence. Save as `docs/scratch/livelit_test_red.png`.

- [ ] **Step 2.10: Commit + push**

```sh
git add sdlquake/engine_src/r_livelight.h sdlquake/engine_src/r_livelight.c
git add sdlquake/engine_src/model.c sdlquake/engine_src/r_main.c sdlquake/engine_src/cl_main.c
git add build.zig
git commit -m "feat(engine): r_livelight delta state machine + test cmds"
git push
```

---

## Task 3 — ABI bump + engine_api wiring

**Goal:** Expose `Lightmap_AddDelta` and `Lightmap_ClearOwner` to the hot-reloadable game.dll. Bump `GAME_API_VERSION` 18 → 19.

**Files:**
- Modify: `sdlquake/game/game_api.h`
- Modify: `sdlquake/engine/hotreload.c`

### Steps

- [ ] **Step 3.1: Add function pointers**

In `sdlquake/game/game_api.h`, locate the end of `engine_api_t` (after `Sample_Lightmap`). Add:

```c
    // Lightmap delta system (Phase 8 / cached preview + gust extinguish).
    // Applies a per-texel additive contribution into the live rgb
    // lightmap. owner: 1=editor preview, 2=gameplay (gust).
    void  (*Lightmap_AddDelta)(const vec3_t pos, float radius,
                               const vec3_t color, int owner);
    void  (*Lightmap_ClearOwner)(int owner);
```

Bump the version macro at top of file:

```c
#define GAME_API_VERSION 19
```

Update the CLAUDE.md note about this:
- Modify the `engine_api_t` ABI bumps line in `CLAUDE.md` to add: `, 18 → 19 (Lightmap_AddDelta + Lightmap_ClearOwner for cached preview/gust)`.

- [ ] **Step 3.2: Wire engine impl**

In `sdlquake/engine/hotreload.c`, find where `engine_api_t` is populated (search for `Sample_Lightmap` — the assignment block). Add right after that line:

```c
	g_engine_api.Lightmap_AddDelta = Lightmap_AddDelta;
	g_engine_api.Lightmap_ClearOwner = Lightmap_ClearOwner;
```

Add include:
```c
#include "../engine_src/r_livelight.h"
```

(Confirm the relative path matches existing includes in hotreload.c.)

- [ ] **Step 3.3: Build the engine + DLL**

```sh
zig build
```

Expected: clean build. Both engine and game.dll compile.

- [ ] **Step 3.4: Smoke test**

```sh
zig build run -- +map e1m1
```

Open the in-game console. Confirm normal play works (the DLL version handshake should accept the new API surface since we bumped both sides).

- [ ] **Step 3.5: Commit + push**

```sh
git add sdlquake/game/game_api.h sdlquake/engine/hotreload.c CLAUDE.md
git commit -m "feat(api): bump GAME_API_VERSION 18->19 with Lightmap_AddDelta"
git push
```

---

## Task 4 — Lift MAX_OVERRIDES + wire Light_AddOverride to the engine

**Goal:** Gust extinguishes flammable lights → visible darkening on the rendered lightmap. Test on m7_skeleton.

**Files:**
- Modify: `sdlquake/game/sim/sim_light.c`

### Steps

- [ ] **Step 4.1: Lift the cap**

In `sdlquake/game/sim/sim_light.c`, change:

```c
#define MAX_OVERRIDES       64
```

to:

```c
#define MAX_OVERRIDES       1024   /* matches editor MAX_LIGHT_CANDIDATES;
                                      sized so every map light in start.bsp
                                      (267) and bigger jam maps can be
                                      extinguished simultaneously. */
```

- [ ] **Step 4.2: Wire Light_AddOverride to the engine**

In `Light_AddOverride`, after the existing `s_overrides[]` append, add:

```c
    /* Mirror the override into the renderer's live lightmap so the
     * player visually sees the room dim (not just the AI sense filter).
     * `delta` here is a scalar luminance; broadcast across RGB. The
     * approximation matches AI side: no shadow occlusion, so walls
     * behind the torch will dim too. The bake remains authoritative
     * for "correct" results. */
    if (eng->Lightmap_AddDelta) {
        vec3_t color = { delta, delta, delta };
        eng->Lightmap_AddDelta(pos, radius, color, 2 /* GAMEPLAY */);
    }
```

(Defensive `if`: in case a stale game.dll is loaded against a newer engine, gracefully skip rather than crash. The version handshake should already prevent this, but cheap belt-and-braces.)

- [ ] **Step 4.3: Wire Light_LevelInit to clear engine-side gust overrides**

In `Light_LevelInit`, after the existing `s_override_count = 0;` line:

```c
    /* Engine-side gust overrides are also cleared on level change via
     * Lightmap_ClearAll in CL_ClearState; this is a belt-and-braces
     * for the case where Light_LevelInit fires after re-entering a
     * map via 'changelevel' without a full CL_ClearState. */
    if (eng->Lightmap_ClearOwner)
        eng->Lightmap_ClearOwner(2);
```

- [ ] **Step 4.4: Build**

```sh
zig build
```

- [ ] **Step 4.5: Smoke test gust extinguish**

```sh
zig build run -- +map m7_skeleton
```

Per the M7 skeleton design, m7 should have a flammable torch you can gust out. Walk up to it, fire +gust (default f key). MCP screenshot before/after — the wall near the torch should visibly darken in the after-shot.

If m7_skeleton lacks an extinguishable torch ready-to-play, use the `Light_AddOverride` debug command instead — check whether sim_light registers a console command, or trigger via existing M7 wiring per the plan in `docs/superpowers/plans/2026-05-14-phase8-m7-bespoke-level.md`. If neither path works, fall back to manually firing a gust at a placed torch.

Save MCP screenshots as `docs/scratch/gust_extinguish_before.png` and `_after.png`.

- [ ] **Step 4.6: Commit + push**

```sh
git add sdlquake/game/sim/sim_light.c
git commit -m "feat(sim): wire Light_AddOverride to engine lightmap deltas; cap 64->1024"
git push
```

---

## Task 5 — Editor preview rewire (off the per-frame dlight pump)

**Goal:** `paint_light_preview` no longer recalculates 512 dlights every frame. Instead a dirty flag drives a one-shot `Lightmap_AddDelta` pass per preview-eligible light. Toggle off → `Lightmap_ClearOwner(EDITOR)` restores baked. Edits redirty.

**Files:**
- Modify: `sdlquake/engine/editor/editor.c`
- Modify: `sdlquake/engine_src/view.c`

### Steps

- [ ] **Step 5.1: Add a dirty flag + rebuild function**

In `sdlquake/engine/editor/editor.c`, find the `s_light_pref_origin_idx` static (around line 1916). Add:

```c
static int s_preview_dirty = 0;

void Editor_LightPreview_MarkDirty(void) { s_preview_dirty = 1; }
```

Add prototype in `sdlquake/engine/editor/editor.h` (or whichever editor public header — locate by grepping `Editor_LightBake_Poll` declaration site):

```c
void Editor_LightPreview_MarkDirty(void);
```

- [ ] **Step 5.2: Replace Editor_RefreshDlights body**

Find `Editor_RefreshDlights` (around line 1966). Replace the entire function body with a thin wrapper that does the dirty-driven rebuild:

```c
/* Dirty-driven RGB-lightmap preview pump. Replaces the per-frame
 * dlight stuffing that previously ran here. Callers: V_RenderView
 * (unchanged call site) and any code path that wants to force a
 * rebuild after marking dirty. */
void Editor_RefreshDlights(void)
{
	extern void Lightmap_ClearOwner(int owner);
	extern void Lightmap_AddDelta(const vec3_t, float, const vec3_t, int);
	extern vec3_t r_origin;
	int i;

	if (!s_open) {
		/* Editor closed while preview is on -> drop preview overrides
		 * so gameplay sees the baked map. Idempotent if already clean. */
		if (s_preview_dirty || paint_light_preview.value == 0) {
			Lightmap_ClearOwner(LIGHTMAP_OWNER_EDITOR);
			s_preview_dirty = 0;
		}
		return;
	}

	if (!s_preview_dirty) return;
	s_preview_dirty = 0;

	Lightmap_ClearOwner(LIGHTMAP_OWNER_EDITOR);

	if (paint_light_preview.value == 0) return;

	(void)r_origin;  /* no longer used; preview applies every eligible light */

	for (i = 0; i < edit_scene.numentities; i++)
	{
		edit_entity_t *e = &edit_scene.entities[i];
		const char *cls;
		vec3_t color = { 1, 1, 1 };
		float intensity = 300;
		int   style    = 0;
		int   has_targetname = 0;
		vec3_t origin;
		int j;

		if (e->classname_idx < 0 || e->classname_idx >= e->numkv) continue;
		cls = e->kv[e->classname_idx].value;
		if (strncmp(cls, "light", 5) != 0) continue;
		if (!Entity_GetOrigin(e, origin)) continue;

		for (j = 0; j < e->numkv; j++)
		{
			if (!strcmp(e->kv[j].key, "_color"))
				parse_color_value(e->kv[j].value, color);
			else if (!strcmp(e->kv[j].key, "light"))
				intensity = (float)atof(e->kv[j].value);
			else if (!strcmp(e->kv[j].key, "style"))
				style = atoi(e->kv[j].value);
			else if (!strcmp(e->kv[j].key, "targetname")
			         && e->kv[j].value[0])
				has_targetname = 1;
		}
		if (intensity <= 0) intensity = 300;
		if (has_targetname)            continue;
		if (style != 0 && style != 32) continue;

		Lightmap_AddDelta(origin, intensity, color,
		                  LIGHTMAP_OWNER_EDITOR);
	}
}
```

Add `#include "../../engine_src/r_livelight.h"` near the top of editor.c (match existing include style).

- [ ] **Step 5.3: Mark dirty on toggle / kv edit / scene rebuild**

Find `paint_light_preview` registration in `Editor_Init` (around line 1732). It's a `Cvar_RegisterVariable(&paint_light_preview)` call — Quake's cvar system doesn't have callbacks, so we poll instead. Add a cached previous-value at file scope:

```c
static float s_paint_light_preview_prev = 0;
```

In `Editor_PreRender` (around line 2079), at the start, add:

```c
	if (paint_light_preview.value != s_paint_light_preview_prev) {
		s_paint_light_preview_prev = paint_light_preview.value;
		Editor_LightPreview_MarkDirty();
	}
```

Find `Editor_ApplyKV` (grep for the function definition). When the kv being applied is on an entity with classname matching `light*`, call `Editor_LightPreview_MarkDirty()`. The simplest hook: at the end of `Editor_ApplyKV`, if the entity's classname starts with "light":

```c
	/* If we just touched a light entity's kv, the preview is stale. */
	{
		const char *cls = (e->classname_idx >= 0
		                   && e->classname_idx < e->numkv)
			? e->kv[e->classname_idx].value : NULL;
		if (cls && !strncmp(cls, "light", 5))
			Editor_LightPreview_MarkDirty();
	}
```

(Adjust to match the actual structure of Editor_ApplyKV — locate by reading the function first.)

Find places where a new light entity is created (Brush new, paste, etc.) or deleted, and call MarkDirty there. Search for `numentities++` or similar in editor.c — there should be a small number of call sites. Add MarkDirty after each.

Also find where the editor scene is fully rebuilt (e.g., after Compile, after entity-paste). Call MarkDirty there too.

- [ ] **Step 5.4: Bake-completion hook**

In `sdlquake/engine/editor/light_bake_thread.c`, find `Editor_LightBake_Poll` (around line 305). After `apply_snapshot()`:

```c
    /* The new baked lightmap replaces the baseline; preview overrides
     * applied to the old baseline are now invalid. Replay editor-side
     * overrides on top of the fresh baseline. */
    Editor_LightPreview_MarkDirty();
```

Add the declaration include at top of light_bake_thread.c:
```c
#include "editor.h"   /* or wherever Editor_LightPreview_MarkDirty is declared */
```

- [ ] **Step 5.5: Remove per-frame call from V_RenderView (no — keep it)**

The call site in `sdlquake/engine_src/view.c` (around line 1071-1078, "Editor live-preview pump: refresh cl_dlights from edit_scene") stays — but Editor_RefreshDlights is now an O(1) no-op when clean. Leave the call.

Verify the comment in view.c still makes sense or update it:

```c
	/* Editor live-preview pump (now dirty-driven). When the editor is
	 * open and paint_light_preview has changed, or a light entity
	 * was edited, this rebuilds the engine's live_rgblightdata. On
	 * clean frames it returns in O(1). */
	if (Editor_RefreshDlights) Editor_RefreshDlights();
```

(Adjust the conditional to match the existing call shape — likely an unconditional call, not a function-pointer null-check.)

- [ ] **Step 5.6: Build**

```sh
zig build
```

- [ ] **Step 5.7: Smoke test the editor preview**

```sh
zig build run -- +map e1m1
```

In-game:
1. Open editor (F1).
2. With paint_light_preview = 0: take MCP screenshot of a lit corridor. Save as `docs/scratch/preview_off.png`.
3. Toggle paint_light_preview = 1 via the Light bake popup (per `af38a9d feat(editor): render preview toggles in Light bake popup`). Wait one frame.
4. MCP screenshot. Save as `docs/scratch/preview_on.png`. Compare to old per-frame preview behaviour — should look similar (additive light contribution on top of the baked corridor).
5. Tweak a light entity's `_color` or `light` value via the inspector; see the preview update on the next frame (single-shot rebuild).
6. Toggle preview off; see it return to baked.

**Performance check:** Watch the FPS counter or `host_frametime`. With preview ON, FPS should be at or near the no-preview baseline. (Old behaviour: FPS dropped substantially. Goal: minimal-to-zero drop.)

- [ ] **Step 5.8: Commit + push**

```sh
git add sdlquake/engine/editor/editor.c sdlquake/engine/editor/editor.h
git add sdlquake/engine/editor/light_bake_thread.c
git add sdlquake/engine_src/view.c
git commit -m "feat(editor): paint_light_preview moves to cached rgblightdata deltas"
git push
```

---

## Task 6 — Verification: full A/B + memory update

**Goal:** Confirm the perf win is real, the visuals are acceptable, and capture the design points worth remembering.

### Steps

- [ ] **Step 6.1: Side-by-side perf measurement**

In e1m1 with the editor open and paint_light_preview ON:
- Note `host_frametime` average over a few seconds, or use a timedemo if one is wired up.
- Compare to: paint_light_preview OFF (control) and the pre-change behaviour (would need a stash + rebuild — skip if too involved; rely on subjective "FPS counter ≈ control" as evidence).

Record findings in the commit message of the verification commit.

- [ ] **Step 6.2: Side-by-side visual A/B**

Open `docs/scratch/preview_off.png` and `docs/scratch/preview_on.png`. Confirm:
- Preview shows additive contribution roughly matching the old per-frame look (the formula `(rad - d) * color[ch]` is approximate; if it's clearly off in brightness, tune the multiplier).
- No flickering when stationary.
- No accumulation drift (toggle on→off→on cycles should return to identical state).

If the brightness is off, tune the multiplier in `r_livelight.c`'s `apply_to_surface` — try multiplying `add` by 0.5, 1.0, or 2.0 and re-screenshot until it matches. Commit any tuning separately.

- [ ] **Step 6.3: Gust extinguish A/B**

In m7_skeleton (or whatever map has a flammable torch wired up), MCP screenshot before and after a gust extinguishes a torch. Confirm the room dims visibly.

- [ ] **Step 6.4: Save updated memory**

Update `MEMORY.md` with a new entry: `[Cached lightmap deltas via live_rgblightdata](project_cached_lightmap_deltas.md)`. Write the body memory file with the key gotchas:
- `surf->rgb_samples` now points at `live_rgblightdata`, not `rgblightdata`. Anything that wants the baked baseline reads `rgblightdata` directly.
- Override list lives in engine (r_livelight.c). DLL's sim_light.c keeps its own list for the AI sense filter — they're parallel records of the same data.
- Owner tags: 1=editor preview, 2=gameplay. ClearOwner is the standard "this system's overrides go away" call.
- Per-frame cost of dlight preview is now ~zero; dirty events do one-shot BSP walks.

- [ ] **Step 6.5: Commit memory + final cleanup**

```sh
git add docs/superpowers/plans/2026-05-16-cached-lightmap-deltas.md
git add C:/Users/wjbr/.claude/projects/C--Users-wjbr-src-quake1-ai/memory/MEMORY.md
git add C:/Users/wjbr/.claude/projects/C--Users-wjbr-src-quake1-ai/memory/project_cached_lightmap_deltas.md
git commit -m "docs: plan + memory for cached lightmap deltas"
git push
```

---

## Notes for the executor

- The falloff formula `(rad - d) * color[ch]` is a starting point. It's approximately what `R_AddDynamicLights_RGB` writes into `blocklights_rgb` divided out by the typical lightstyle scale (~264). Iterate by eye in Task 6 if brightness is off.
- `R_PushDlights` and the surface `dlightbits` are NOT touched by this system. They stay for transient gameplay dlights (muzzle flashes, explosions). Don't remove them.
- The cap lift to 1024 makes `Light_TierAt`'s linear scan over `s_overrides[]` O(N) on every AI sense call. At 1024 × 10 AIs × ~100 stims/frame ≈ 1M comparisons/frame — probably fine, but if a profile shows it hot later, swap for a spatial hash on `o->pos`. Not a blocker.
- After Compile+Light (the live-bake worker), `loadmodel->rgblightdata` may be repointed at `s_live_rgblightdata` from `light_bake_thread.c`. Our `restore_baseline` reads `cl.worldmodel->rgblightdata`, so it picks up the new baseline automatically. The `loadmodel_rgblightdata_size` global needs to be kept in sync — find where light_bake_thread sets new sizes and update there too.
- Gust shadow correctness is a known approximation (lights through walls). Acceptable for the first cut; future upgrade path is per-light bake-time contribution buffers.

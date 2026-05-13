# Model Frame Interpolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Smoothly blend alias-model vertex positions between keyframes (monster anims, viewmodel, torches/flames) instead of snapping at the QC 10 Hz tick.

**Architecture:** Add three fields to `entity_t` (`prev_frame`, `prev_frame_observed`, `frame_start_time`) populated in `CL_RelinkEntities` whenever a QC-driven frame change is observed. In `R_AliasSetupFrame`, compute a 0..1 blend factor and pick two vertex bases (current frame + prev frame). Patch the two C vertex-transform paths in `r_alias.c` to lerp `pverts->v` against `pverts_prev->v` before the existing `DotProduct` calls. Gated by a new `r_lerpmodels` cvar.

**Tech Stack:** C (gnu89), Zig 0.16 build system, SDL3 platform. No test suite — verification is `zig build run -- +map <map>` + visual inspection per `CLAUDE.md`.

**Spec reference:** `docs/superpowers/specs/2026-05-13-model-interpolation-design.md`

---

## File Map

- `sdlquake/engine_src/render.h` — add 3 fields to `entity_t` struct.
- `sdlquake/engine_src/r_main.c` — declare + register `r_lerpmodels` cvar.
- `sdlquake/engine_src/r_local.h` — extern declarations for `r_lerpmodels`, `r_apverts_prev`, `r_framelerp`.
- `sdlquake/engine_src/cl_main.c` — populate `prev_frame` / `prev_frame_observed` / `frame_start_time` in `CL_RelinkEntities`.
- `sdlquake/engine_src/r_alias.c` — globals, `R_LerpVert` helper, edited `R_AliasSetupFrame`, `R_AliasTransformFinalVert`, `R_AliasTransformAndProjectFinalVerts`, `R_AliasPreparePoints`, `R_AliasCheckBBox`.

No new files. All edits live inside the forked engine — never `ref/Quake-master/`.

---

### Task 1: Scaffolding — struct fields, cvar, externs

**Files:**
- Modify: `sdlquake/engine_src/render.h` (around line 60, inside `entity_t`)
- Modify: `sdlquake/engine_src/r_main.c` (cvar declaration ~line 126, registration ~line 229)
- Modify: `sdlquake/engine_src/r_local.h` (add extern after `r_coloredlight` extern at line 63)

- [ ] **Step 1: Add the three fields to `entity_t`**

Open `sdlquake/engine_src/render.h`. Find the `entity_t` struct (starts line 39). After the `int frame;` line (line 54), insert:

```c
	int						frame;
	int						prev_frame;			// frame we are lerping FROM
	int						prev_frame_observed;	// last frame value seen, for change detection
	float					frame_start_time;	// cl.time when ent->frame last changed
```

Leave the rest of the struct untouched.

- [ ] **Step 2: Declare `r_lerpmodels` cvar**

Open `sdlquake/engine_src/r_main.c`. After the `r_colored_dlights` line (currently line 126):

```c
cvar_t	r_coloredlight    = {"r_coloredlight",    "1", true};	// archived
cvar_t	r_colored_dlights = {"r_colored_dlights", "1", true};	// archived
cvar_t	r_lerpmodels      = {"r_lerpmodels",      "1", true};	// archived
```

Note the trailing `true` — the third struct field marks the cvar as archived to config (matches `r_coloredlight` convention).

- [ ] **Step 3: Register `r_lerpmodels`**

In the same file (`r_main.c`), in `R_Init` around line 229, after the `r_colored_dlights` registration:

```c
	Cvar_RegisterVariable (&r_coloredlight);
	Cvar_RegisterVariable (&r_colored_dlights);
	Cvar_RegisterVariable (&r_lerpmodels);
```

- [ ] **Step 4: Expose `r_lerpmodels` via header**

Open `sdlquake/engine_src/r_local.h`. After the `extern cvar_t r_coloredlight;` line (currently line 63):

```c
extern cvar_t	r_coloredlight;
extern cvar_t	r_lerpmodels;
```

- [ ] **Step 5: Build to confirm scaffolding compiles**

Run: `zig build`
Expected: clean build, no warnings related to the new fields/cvar.

- [ ] **Step 6: Smoke-test that the cvar exists in-game**

Run: `zig build run -- +map start`
In the dropdown console (`~`), type `r_lerpmodels` and press Enter.
Expected: console prints `"r_lerpmodels" is:"1" default:"1"`.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/engine_src/render.h sdlquake/engine_src/r_main.c sdlquake/engine_src/r_local.h
git commit -m "feat(render): add entity_t lerp fields and r_lerpmodels cvar"
```

---

### Task 2: Track frame transitions in CL_RelinkEntities

**Files:**
- Modify: `sdlquake/engine_src/cl_main.c` (`CL_RelinkEntities`, after the `if (!ent->model)` empty-slot check around line 487-499)

- [ ] **Step 1: Insert transition-tracking block**

Open `sdlquake/engine_src/cl_main.c`. Find `CL_RelinkEntities` (line 446). Look for the per-entity loop (line 485):

```c
	for (i=1,ent=cl_entities+1 ; i<cl.num_entities ; i++,ent++)
	{
		if (!ent->model)
		{	// empty slot
			if (ent->forcelink)
				R_RemoveEfrags (ent);	// just became empty
			continue;
		}

// if the object wasn't included in the last packet, remove it
		if (ent->msgtime != cl.mtime[0])
		{
			ent->model = NULL;
			continue;
		}
```

Immediately AFTER the `if (ent->msgtime != cl.mtime[0])` block (so we only update entities that are still active in this packet), insert:

```c
		// track frame transitions for vertex interpolation
		if (ent->forcelink || ent->frame != ent->prev_frame_observed)
		{
			ent->prev_frame = ent->forcelink ? ent->frame
			                                 : ent->prev_frame_observed;
			ent->prev_frame_observed = ent->frame;
			ent->frame_start_time = cl.time;
		}
```

Rationale: on first-ever observation (or a respawn / `U_NOLERP` packet), `forcelink` is true and we slam `prev_frame = frame` — the next `R_AliasSetupFrame` will see `frame == prev_frame` and emit a no-op blend factor of 1.0.

- [ ] **Step 2: Build**

Run: `zig build`
Expected: clean.

- [ ] **Step 3: Visual smoke test (no behaviour change yet)**

Run: `zig build run -- +map e1m1`
Expected: game runs identically to before. Monsters still snap (lerp is not wired up yet). The point of this test is to confirm the new write path doesn't crash or cause memory corruption.

Walk around for ~30 seconds, fire a few shots, observe a couple of monsters. No asserts, no crashes.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/cl_main.c
git commit -m "feat(client): record alias-model frame transitions on entity update"
```

---

### Task 3: Renderer globals + setup (lerp factor pinned to 1.0)

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (globals near line 35; `R_AliasSetupFrame` line 652)

This task wires every plumbing change EXCEPT changing the visible output. After it, `r_framelerp` is always 1.0 and `r_apverts_prev` always equals `r_apverts` — output is bit-identical to current behaviour. We verify this by running the game and confirming monsters still snap exactly as before.

- [ ] **Step 1: Add the new globals**

Open `sdlquake/engine_src/r_alias.c`. Find the global `r_apverts` declaration at line 35:

```c
trivertx_t		*r_apverts;
```

Replace with:

```c
trivertx_t		*r_apverts;
trivertx_t		*r_apverts_prev;
float			r_framelerp;
```

- [ ] **Step 2: Rewrite `R_AliasSetupFrame`**

Replace the entire body of `R_AliasSetupFrame` (currently `r_alias.c:652-695`). New body:

```c
void R_AliasSetupFrame (void)
{
	int				frame, prev_frame;
	int				i, prev_i, numframes;
	maliasgroup_t	*paliasgroup;
	float			*pintervals, fullinterval, targettime, time;

	frame = currententity->frame;
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("R_AliasSetupFrame: no such frame %d\n", frame);
		frame = 0;
	}

	prev_frame = currententity->prev_frame;
	if ((prev_frame >= pmdl->numframes) || (prev_frame < 0))
		prev_frame = frame;

	// compute lerp factor; will stay 1.0 (no blend) until Task 5 wires the time formula
	r_framelerp = 1.0f;

	if (paliashdr->frames[frame].type == ALIAS_SINGLE)
	{
		r_apverts = (trivertx_t *)
				((byte *)paliashdr + paliashdr->frames[frame].frame);

		if (paliashdr->frames[prev_frame].type == ALIAS_SINGLE)
		{
			r_apverts_prev = (trivertx_t *)
					((byte *)paliashdr + paliashdr->frames[prev_frame].frame);
		}
		else
		{
			// mixed single/group transition — don't lerp
			r_apverts_prev = r_apverts;
			r_framelerp = 1.0f;
		}
		return;
	}

	// ALIAS_GROUP path
	paliasgroup = (maliasgroup_t *)
				((byte *)paliashdr + paliashdr->frames[frame].frame);
	pintervals = (float *)((byte *)paliashdr + paliasgroup->intervals);
	numframes = paliasgroup->numframes;
	fullinterval = pintervals[numframes-1];

	time = cl.time + currententity->syncbase;

	// when loading in Mod_LoadAliasGroup, we guaranteed all interval values
	// are positive, so we don't have to worry about division by 0
	targettime = time - ((int)(time / fullinterval)) * fullinterval;

	for (i=0 ; i<(numframes-1) ; i++)
	{
		if (pintervals[i] > targettime)
			break;
	}

	r_apverts = (trivertx_t *)
				((byte *)paliashdr + paliasgroup->frames[i].frame);

	prev_i = (i == 0) ? (numframes - 1) : (i - 1);
	r_apverts_prev = (trivertx_t *)
				((byte *)paliashdr + paliasgroup->frames[prev_i].frame);
	// keep r_framelerp at 1.0 here; Task 6 wires the intra-group alpha
}
```

Note: we set `r_apverts_prev` to a valid pointer in all paths (even the mixed single/group case) so downstream code can read it unconditionally. Combined with `r_framelerp = 1.0`, the blend math collapses to "use current frame".

- [ ] **Step 3: Add extern declarations**

Open `sdlquake/engine_src/r_local.h`. Find the `extern aliashdr_t *paliashdr;` line (line 268):

```c
extern aliashdr_t		*paliashdr;
```

After it, add:

```c
extern trivertx_t		*r_apverts_prev;
extern float			r_framelerp;
```

(`r_apverts` already has its declaration; we don't need to add a second.)

- [ ] **Step 4: Build**

Run: `zig build`
Expected: clean.

- [ ] **Step 5: Visual smoke test — must be visually identical to before**

Run: `zig build run -- +map e1m1`
Walk past a knight or zombie. Animation should snap exactly as before this commit — no smoothing yet. If you observe any visual difference, something in `R_AliasSetupFrame` is wrong.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine_src/r_alias.c sdlquake/engine_src/r_local.h
git commit -m "feat(render): wire alias frame setup for vertex interpolation (no-op)"
```

---

### Task 4: Patch vertex-transform functions (still no visible change)

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_AliasTransformFinalVert`, `R_AliasTransformAndProjectFinalVerts`, `R_AliasPreparePoints`)

With `r_framelerp == 1.0f` from Task 3, the lerp helper collapses to `out = pverts->v` and we should still get bit-identical output.

- [ ] **Step 1: Add the `R_LerpVert` helper**

Open `sdlquake/engine_src/r_alias.c`. Just below the externs and globals (after line 56, before the `aedge_t` typedef on line 58), insert:

```c
static inline void R_LerpVert (trivertx_t *cur, trivertx_t *prev, vec3_t out)
{
	if (r_framelerp >= 1.0f)
	{
		out[0] = cur->v[0];
		out[1] = cur->v[1];
		out[2] = cur->v[2];
	}
	else
	{
		float inv = 1.0f - r_framelerp;
		out[0] = cur->v[0] * r_framelerp + prev->v[0] * inv;
		out[1] = cur->v[1] * r_framelerp + prev->v[1] * inv;
		out[2] = cur->v[2] * r_framelerp + prev->v[2] * inv;
	}
}
```

- [ ] **Step 2: Patch `R_AliasTransformFinalVert`**

Find `R_AliasTransformFinalVert` (line 415). Change its signature to accept the previous-frame vert, and replace the three `DotProduct(pverts->v, ...)` calls with a lerped vector.

Replace the existing function body (lines 415-449) with:

```c
void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	trivertx_t *pverts, trivertx_t *pverts_prev, stvert_t *pstverts)
{
	int		temp;
	float	lightcos, *plightnormal;
	vec3_t	v;

	R_LerpVert (pverts, pverts_prev, v);

	av->fv[0] = DotProduct(v, aliastransform[0]) +
			aliastransform[0][3];
	av->fv[1] = DotProduct(v, aliastransform[1]) +
			aliastransform[1][3];
	av->fv[2] = DotProduct(v, aliastransform[2]) +
			aliastransform[2][3];

	fv->v[2] = pstverts->s;
	fv->v[3] = pstverts->t;

	fv->flags = pstverts->onseam;

// lighting (use current frame's normal index — not lerped, see spec)
	plightnormal = r_avertexnormals[pverts->lightnormalindex];
	lightcos = DotProduct (plightnormal, r_plightvec);
	temp = r_ambientlight;

	if (lightcos < 0)
	{
		temp += (int)(r_shadelight * lightcos);

	// clamp; because we limited the minimum ambient and shading light, we
	// don't have to clamp low light, just bright
		if (temp < 0)
			temp = 0;
	}

	fv->v[4] = temp;
}
```

Also update the prototype at the top of the file (line 79-80):

```c
void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	trivertx_t *pverts, stvert_t *pstverts);
```

becomes:

```c
void R_AliasTransformFinalVert (finalvert_t *fv, auxvert_t *av,
	trivertx_t *pverts, trivertx_t *pverts_prev, stvert_t *pstverts);
```

- [ ] **Step 3: Patch `R_AliasPreparePoints` to thread the prev pointer**

Find `R_AliasPreparePoints` (line 268). Replace its body. The change is: declare a parallel `r_apverts_prev` loop pointer and advance it in lockstep, and pass it to `R_AliasTransformFinalVert`.

The current loop (lines 282-300):

```c
	for (i=0 ; i<r_anumverts ; i++, fv++, av++, r_apverts++, pstverts++)
	{
		R_AliasTransformFinalVert (fv, av, r_apverts, pstverts);
```

Replace the loop initialisation and body. The full updated function:

```c
void R_AliasPreparePoints (void)
{
	int			i;
	stvert_t	*pstverts;
	finalvert_t	*fv;
	auxvert_t	*av;
	mtriangle_t	*ptri;
	finalvert_t	*pfv[3];

	pstverts = (stvert_t *)((byte *)paliashdr + paliashdr->stverts);
	r_anumverts = pmdl->numverts;
 	fv = pfinalverts;
	av = pauxverts;

	for (i=0 ; i<r_anumverts ; i++, fv++, av++, r_apverts++, r_apverts_prev++, pstverts++)
	{
		R_AliasTransformFinalVert (fv, av, r_apverts, r_apverts_prev, pstverts);
		if (av->fv[2] < ALIAS_Z_CLIP_PLANE)
			fv->flags |= ALIAS_Z_CLIP;
		else
		{
			 R_AliasProjectFinalVert (fv, av);

			if (fv->v[0] < r_refdef.aliasvrect.x)
				fv->flags |= ALIAS_LEFT_CLIP;
			if (fv->v[1] < r_refdef.aliasvrect.y)
				fv->flags |= ALIAS_TOP_CLIP;
			if (fv->v[0] > r_refdef.aliasvrectright)
				fv->flags |= ALIAS_RIGHT_CLIP;
			if (fv->v[1] > r_refdef.aliasvrectbottom)
				fv->flags |= ALIAS_BOTTOM_CLIP;
		}
	}

//
// clip and draw all triangles
//
	r_affinetridesc.numtriangles = 1;

	ptri = (mtriangle_t *)((byte *)paliashdr + paliashdr->triangles);
	for (i=0 ; i<pmdl->numtris ; i++, ptri++)
	{
		pfv[0] = &pfinalverts[ptri->vertindex[0]];
		pfv[1] = &pfinalverts[ptri->vertindex[1]];
		pfv[2] = &pfinalverts[ptri->vertindex[2]];

		if ( pfv[0]->flags & pfv[1]->flags & pfv[2]->flags & (ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP) )
			continue;		// completely clipped

		if ( ! ( (pfv[0]->flags | pfv[1]->flags | pfv[2]->flags) &
			(ALIAS_XY_CLIP_MASK | ALIAS_Z_CLIP) ) )
		{	// totally unclipped
			r_affinetridesc.pfinalverts = pfinalverts;
			r_affinetridesc.ptriangles = ptri;
			D_PolysetDraw ();
		}
		else
		{	// partially clipped
			R_AliasClipTriangle (ptri);
		}
	}
}
```

Only two lines actually change inside the function — the `for` advance list adds `r_apverts_prev++`, and the call to `R_AliasTransformFinalVert` passes `r_apverts_prev` as the new third argument.

- [ ] **Step 4: Patch `R_AliasTransformAndProjectFinalVerts` (unclipped path)**

Find this function (line 459). Replace its body:

```c
void R_AliasTransformAndProjectFinalVerts (finalvert_t *fv, stvert_t *pstverts)
{
	int			i, temp;
	float		lightcos, *plightnormal, zi;
	trivertx_t	*pverts;
	trivertx_t	*pverts_prev;
	vec3_t		v;

	pverts = r_apverts;
	pverts_prev = r_apverts_prev;

	for (i=0 ; i<r_anumverts ; i++, fv++, pverts++, pverts_prev++, pstverts++)
	{
		R_LerpVert (pverts, pverts_prev, v);

	// transform and project
		zi = 1.0 / (DotProduct(v, aliastransform[2]) +
				aliastransform[2][3]);

	// x, y, and z are scaled down by 1/2**31 in the transform, so 1/z is
	// scaled up by 1/2**31, and the scaling cancels out for x and y in the
	// projection
		fv->v[5] = zi;

		fv->v[0] = ((DotProduct(v, aliastransform[0]) +
				aliastransform[0][3]) * zi) + aliasxcenter;
		fv->v[1] = ((DotProduct(v, aliastransform[1]) +
				aliastransform[1][3]) * zi) + aliasycenter;

		fv->v[2] = pstverts->s;
		fv->v[3] = pstverts->t;
		fv->flags = pstverts->onseam;

	// lighting
		plightnormal = r_avertexnormals[pverts->lightnormalindex];
		lightcos = DotProduct (plightnormal, r_plightvec);
		temp = r_ambientlight;

		if (lightcos < 0)
		{
			temp += (int)(r_shadelight * lightcos);

		// clamp; because we limited the minimum ambient and shading light, we
		// don't have to clamp low light, just bright
			if (temp < 0)
				temp = 0;
		}

		fv->v[4] = temp;
	}
}
```

- [ ] **Step 5: Build**

Run: `zig build`
Expected: clean. The forward-declared prototype in r_local.h does not include `R_AliasTransformFinalVert`'s signature (it's file-local), so no further header edits are needed.

- [ ] **Step 6: Visual smoke test — STILL must be identical to original**

Run: `zig build run -- +map e1m1`
Walk around. Animations should still snap exactly as before. Any visible "blend" at this point indicates a bug — `r_framelerp` is still pinned to 1.0 in `R_AliasSetupFrame` so the helper must early-out.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(render): thread previous-frame verts through transform paths"
```

---

### Task 5: Wire up the real lerp factor (single-frame lerp goes live)

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_AliasSetupFrame`)
- Modify: `sdlquake/engine_src/r_alias.c` (top of file — add `#include "client.h"` if not already included, to access `cl_nolerp` and `cl`)

- [ ] **Step 1: Check existing includes**

Open `sdlquake/engine_src/r_alias.c` and verify `quakedef.h` is included at the top (it is — line 22). That transitively brings in `client.h` (provides `cl.time` and `cl_nolerp`). No new include needed.

- [ ] **Step 2: Replace the `r_framelerp = 1.0f` placeholder**

In `R_AliasSetupFrame` (the function rewritten in Task 3), find the line:

```c
	// compute lerp factor; will stay 1.0 (no blend) until Task 5 wires the time formula
	r_framelerp = 1.0f;
```

Replace with:

```c
	if (!r_lerpmodels.value || cl_nolerp.value || frame == prev_frame)
	{
		r_framelerp = 1.0f;
	}
	else
	{
		r_framelerp = (cl.time - currententity->frame_start_time) / 0.1f;
		if (r_framelerp > 1.0f) r_framelerp = 1.0f;
		if (r_framelerp < 0.0f) r_framelerp = 0.0f;
	}
```

This applies to single-frame animations. Group frames are still untouched (the ALIAS_GROUP branch sets `r_framelerp` after this — Task 6 wires its alpha).

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean.

- [ ] **Step 4: Visual smoke test — monster animations should now be smooth**

Run: `zig build run -- +map e1m1`

Walk down the corridor to the first knight. Observe its idle/walk animation: it should now blend continuously instead of snapping at 10 Hz. Fire your shotgun at it; its hit-react should blend in.

Then verify the cvar gates correctly:
- Console: `r_lerpmodels 0` — should snap (classic).
- Console: `r_lerpmodels 1` — should blend.
- Console: `cl_nolerp 1` — should snap (even with r_lerpmodels 1). Set it back to `0`.

Verify forcelink path: type `kill` in the console to suicide. After respawn, no monster (or your gibs) should "slide" from a stale frame into the spawn pose.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(render): enable single-frame alias model interpolation"
```

---

### Task 6: Group-frame interpolation

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_AliasSetupFrame`, group-frame branch)

- [ ] **Step 1: Compute intra-group alpha**

In `R_AliasSetupFrame`, the ALIAS_GROUP path (added in Task 3) currently sets `r_apverts_prev` to subframe `prev_i` but leaves `r_framelerp` at 1.0. Replace the section starting after the `r_apverts = ...paliasgroup->frames[i].frame` assignment with:

```c
	r_apverts = (trivertx_t *)
				((byte *)paliasgroup->frames[i].frame + (byte *)paliashdr);
	/* note: deliberately keeping the existing pointer arithmetic shape */
```

Wait — keep the original assignment style. Find the lines added in Task 3:

```c
	r_apverts = (trivertx_t *)
				((byte *)paliashdr + paliasgroup->frames[i].frame);

	prev_i = (i == 0) ? (numframes - 1) : (i - 1);
	r_apverts_prev = (trivertx_t *)
				((byte *)paliashdr + paliasgroup->frames[prev_i].frame);
	// keep r_framelerp at 1.0 here; Task 6 wires the intra-group alpha
```

Replace the trailing comment line with:

```c
	if (r_lerpmodels.value && !cl_nolerp.value)
	{
		float t_lo = (i == 0) ? 0.0f : pintervals[i-1];
		float t_hi = pintervals[i];
		float span = t_hi - t_lo;
		if (span > 0.0f)
		{
			r_framelerp = (targettime - t_lo) / span;
			if (r_framelerp > 1.0f) r_framelerp = 1.0f;
			if (r_framelerp < 0.0f) r_framelerp = 0.0f;
		}
		else
		{
			r_framelerp = 1.0f;
		}
	}
	else
	{
		r_framelerp = 1.0f;
	}
```

- [ ] **Step 2: Build**

Run: `zig build`
Expected: clean.

- [ ] **Step 3: Visual smoke test — torch flames should be continuous**

Run: `zig build run -- +map e1m1`
There are several torches on the walls in e1m1. Look at one closely with `r_lerpmodels 1` — the flame should flow continuously instead of stepping. Set `r_lerpmodels 0` — confirm classic stepping returns.

Also visit `e1m3` (Necropolis) which has the `flame.mdl` standing flame entity — same test.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(render): interpolate group-frame animations (torches, flames)"
```

---

### Task 7: Union bounding box across both frames

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_AliasCheckBBox`, lines 89-138)

`R_AliasCheckBBox` reads `pframedesc->bboxmin/max` for a single frame. With lerp, a vertex of the previous frame could briefly extend outside the current frame's bbox and cause a visible pop-clip on the screen edge. Union both frames' bboxes when they differ.

- [ ] **Step 1: Patch the bbox construction**

In `R_AliasCheckBBox` (line 89), find this block (lines 110-138):

```c
// construct the base bounding box for this frame
	frame = currententity->frame;
// TODO: don't repeat this check when drawing?
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("No such frame %d %s\n", frame,
				pmodel->name);
		frame = 0;
	}

	pframedesc = &pahdr->frames[frame];

// x worldspace coordinates
	basepts[0][0] = basepts[1][0] = basepts[2][0] = basepts[3][0] =
			(float)pframedesc->bboxmin.v[0];
	basepts[4][0] = basepts[5][0] = basepts[6][0] = basepts[7][0] =
			(float)pframedesc->bboxmax.v[0];

// y worldspace coordinates
	basepts[0][1] = basepts[3][1] = basepts[5][1] = basepts[6][1] =
			(float)pframedesc->bboxmin.v[1];
	basepts[1][1] = basepts[2][1] = basepts[4][1] = basepts[7][1] =
			(float)pframedesc->bboxmax.v[1];

// z worldspace coordinates
	basepts[0][2] = basepts[1][2] = basepts[4][2] = basepts[5][2] =
			(float)pframedesc->bboxmin.v[2];
	basepts[2][2] = basepts[3][2] = basepts[6][2] = basepts[7][2] =
			(float)pframedesc->bboxmax.v[2];
```

Replace with:

```c
// construct the base bounding box, unioned across current and previous frames
	frame = currententity->frame;
// TODO: don't repeat this check when drawing?
	if ((frame >= pmdl->numframes) || (frame < 0))
	{
		Con_DPrintf ("No such frame %d %s\n", frame,
				pmodel->name);
		frame = 0;
	}

	{
		int prev_frame = currententity->prev_frame;
		maliasframedesc_t *pf_cur, *pf_prev;
		float bbmin[3], bbmax[3];
		int k;

		if ((prev_frame >= pmdl->numframes) || (prev_frame < 0))
			prev_frame = frame;

		pf_cur  = &pahdr->frames[frame];
		pf_prev = &pahdr->frames[prev_frame];

		for (k = 0; k < 3; k++)
		{
			float a_min = (float)pf_cur->bboxmin.v[k];
			float a_max = (float)pf_cur->bboxmax.v[k];
			float b_min = (float)pf_prev->bboxmin.v[k];
			float b_max = (float)pf_prev->bboxmax.v[k];
			bbmin[k] = (a_min < b_min) ? a_min : b_min;
			bbmax[k] = (a_max > b_max) ? a_max : b_max;
		}

		// x worldspace coordinates
		basepts[0][0] = basepts[1][0] = basepts[2][0] = basepts[3][0] = bbmin[0];
		basepts[4][0] = basepts[5][0] = basepts[6][0] = basepts[7][0] = bbmax[0];

		// y worldspace coordinates
		basepts[0][1] = basepts[3][1] = basepts[5][1] = basepts[6][1] = bbmin[1];
		basepts[1][1] = basepts[2][1] = basepts[4][1] = basepts[7][1] = bbmax[1];

		// z worldspace coordinates
		basepts[0][2] = basepts[1][2] = basepts[4][2] = basepts[5][2] = bbmin[2];
		basepts[2][2] = basepts[3][2] = basepts[6][2] = basepts[7][2] = bbmax[2];
	}
```

The `pframedesc` local declared earlier in the function is no longer used after this change — leave it declared (the `int i, flags, frame, numv;` line is fine; just delete `pframedesc` from the local declarations if your compiler warns about unused locals).

- [ ] **Step 2: Remove the unused `pframedesc` local**

At the top of `R_AliasCheckBBox` (around line 96), the line:

```c
	maliasframedesc_t	*pframedesc;
```

Delete it (no longer referenced after the patch above). If the build complains about leftover references, double-check the function body — there should be none.

- [ ] **Step 3: Build**

Run: `zig build`
Expected: clean, no unused-variable warnings.

- [ ] **Step 4: Visual smoke test — no clip-pop at screen edge**

Run: `zig build run -- +map e1m1`
Position yourself at the corner of a doorway so a moving monster (e.g. a knight) walks across the edge of your view. With `r_lerpmodels 1`, watch the monster as it enters / exits the frustum. With the union bbox, you should NOT see the monster suddenly pop in/out at the screen edge during a frame transition.

You won't see this every time — it's a rare visual bug — but the fix is cheap and correct.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "fix(render): union alias bbox across lerp frames to avoid edge clip-pop"
```

---

## Self-Review

Cross-checking the spec sections against the plan:

| Spec section | Covered by |
|---|---|
| §1 State additions: entity_t fields | Task 1 step 1 |
| §1 State additions: cvar `r_lerpmodels` | Task 1 steps 2-4 |
| §1 State additions: renderer globals | Task 3 step 1, step 3 |
| §2 Tracking frame transitions in CL_RelinkEntities | Task 2 step 1 |
| §3 Lerp factor (single-frame) | Task 5 step 2 |
| §3 Lerp factor (group-frame) | Task 6 step 1 |
| §3 Mixed single↔group fallback | Task 3 step 2 (the `else r_apverts_prev = r_apverts; r_framelerp = 1.0` branch) |
| §4 `R_LerpVert` helper + early-out | Task 4 step 1 |
| §4 `R_AliasTransformFinalVert` patch | Task 4 step 2 |
| §4 `R_AliasTransformAndProjectFinalVerts` patch | Task 4 step 4 |
| §4 `R_AliasPreparePoints` parallel pointer | Task 4 step 3 |
| §4 Lighting normals NOT lerped | Task 4 steps 2 & 4 (preserved `pverts->lightnormalindex`) |
| §5 Bounding box union | Task 7 |
| §6 Performance — handled by `R_LerpVert` early-out at `r_framelerp == 1.0f` | Task 4 step 1 |
| Forcelink one-frame skip | Task 2 step 1 (the `ent->forcelink ? ent->frame : ...` branch) |
| `cl_nolerp` integration | Task 5 step 2 (single), Task 6 step 1 (group) |

No spec gaps. No placeholders / TBDs. Type/name consistency check: `prev_frame`, `prev_frame_observed`, `frame_start_time`, `r_lerpmodels`, `r_apverts_prev`, `r_framelerp`, `R_LerpVert` — used identically across all tasks.

One self-flag during review: in Task 4 step 3 I assumed `r_apverts_prev` increments in lockstep with `r_apverts` inside `R_AliasPreparePoints`'s loop. The spec described this as making them indexed bases instead of incremented pointers; the plan uses the lockstep-increment style because it's a smaller diff and the existing increment convention is the engine's. Both achieve the same correctness. Flagging for awareness — not a defect.

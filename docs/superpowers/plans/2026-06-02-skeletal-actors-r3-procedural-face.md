# Skeletal Actors R3 — Procedural Face (look-at + eye-gaze + breathing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a `mod_iqm` actor procedurally *look at the player* (head + eyes swivel, clamped to a cone) and *breathe* (chest mesh scale oscillates), layered over the R2 clip pose — delivering the user's "look around / moveable eyes" intent.

**Architecture:** The procedural layer mutates the per-joint **local** pose during the existing hierarchy compose in `R_IQMDrawModel` (so head→eye parenting propagates), then applies breathing as a **non-propagating** post-hoc scale on the chest joint's skin matrix only. Role joints (head / chest / jaw / eyes) are resolved by **name convention** in the IQM loader and cached on `lm_iqm_t`. Tunables are engine cvars (per-actor overrides via the IQM comment lump are deferred to E3).

**Tech Stack:** C (engine `-std=gnu89`), `libmodel` (portable C) for role resolution, the existing software rasterizer path. No new dependencies, no ABI bump (engine-side only; the dev actor is client-side).

**Scope note — lipsync deferred.** The design's lipsync is *mouth skin-swap*, which needs **textured** heads; R1/R2 render solid-colour synth skins. So R3 ships **look-at + eye-gaze + breathing** (pure geometry/scale, fully doable now). The `jaw`/`mouth_skins` role is *resolved* (for forward-compat) but unused until heads are textured (editor/texturing milestone). This matches the design's deferred "real visemes" boundary.

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `sdlquake/libmodel/iqm.h` | modify | Add role-joint index fields to `lm_iqm_t` |
| `sdlquake/libmodel/iqm.c` | modify | Resolve role joints by name after the joint parse |
| `sdlquake/engine_src/r_alias.c` | modify | Actor cvars; look-at/gaze override in compose; breathing on chest skin |
| `sdlquake/engine_src/r_local.h` | modify | Declare `R_IQMInitCvars` |
| `sdlquake/engine_src/r_main.c` | modify | Call `R_IQMInitCvars()` from `R_Init` |
| `CLAUDE.md` | modify | Document R3 done |

---

## Task 1: Resolve role joints by name in the IQM loader

**Files:**
- Modify: `sdlquake/libmodel/iqm.h` (the `lm_iqm_t` struct)
- Modify: `sdlquake/libmodel/iqm.c` (after the joint parse loop)

- [ ] **Step 1: Add role fields to `lm_iqm_t`**

In `sdlquake/libmodel/iqm.h`, inside `struct lm_iqm_s`, after the `frametrs` animation block and before `qalloc_t alloc;`, add:

```c
    /* role joints (R3), resolved by name convention at load; -1 / 0 = absent.
       head/chest/jaw match a substring; eyes match the "eye" prefix. */
    int             head_joint;
    int             chest_joint;
    int             jaw_joint;     /* resolved for forward-compat; unused until lipsync */
    int             eye_joint[4];
    int             num_eye;
```

- [ ] **Step 2: Resolve roles in the loader**

In `sdlquake/libmodel/iqm.c`, immediately after the joint parse loop (the `for (i = 0; i < (int)num_joint; i++)` block that ends at the line with `m->joints[i].scale[j] = rdf(jo+36 + j*4);` and its closing `}`), insert:

```c
    /* ---- resolve role joints by name convention (R3) ---- */
    m->head_joint = m->chest_joint = m->jaw_joint = -1;
    m->num_eye = 0;
    for (i = 0; i < (int)num_joint; i++){
        const char *nm = m->joints[i].name;
        if (m->head_joint  < 0 && strstr(nm, "head"))  m->head_joint  = i;
        if (m->chest_joint < 0 && strstr(nm, "chest")) m->chest_joint = i;
        if (m->jaw_joint   < 0 && strstr(nm, "jaw"))   m->jaw_joint   = i;
        if (strstr(nm, "eye") && m->num_eye < 4)       m->eye_joint[m->num_eye++] = i;
    }
```

(`string.h` is already included at the top of `iqm.c`, so `strstr` is available.)

- [ ] **Step 3: Print roles in `actor_dump` for verification**

In `sdlquake/engine_src/iqm_dev.c`, in `Actor_Dump_f`, after the existing `anim:` line (`Con_Printf ("  anim: %d frames @ %g fps%s\n", ...)`), add:

```c
	Con_Printf ("  roles: head %d  chest %d  jaw %d  eyes %d\n",
		iqm->head_joint, iqm->chest_joint, iqm->jaw_joint, iqm->num_eye);
```

- [ ] **Step 4: Build**

Run: `zig build 2>&1 | tail -20`
Expected: builds cleanly (no errors; pre-existing IDE-only diagnostics about missing include paths are not build errors).

- [ ] **Step 5: Verify roles resolve headlessly**

Run the game with the MCP HTTP transport and dump the dummy:
```bash
./zig-out/bin/quake --mcp-http 9876 +map start +wait 30 &
sleep 6
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_dump actors/dummy.iqm"}'
sleep 1
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"quit"}'
```
Expected console output includes: `roles: head 2  chest 1  jaw -1  eyes 2` (dummy has joints base=0, chest=1, head=2, eye.L=3, eye.R=4; no jaw).

- [ ] **Step 6: Commit**

```bash
git add sdlquake/libmodel/iqm.h sdlquake/libmodel/iqm.c sdlquake/engine_src/iqm_dev.c
git commit -m "feat(actors): R3 — resolve role joints (head/chest/jaw/eyes) by name in IQM loader"
```

---

## Task 2: Actor procedural cvars

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (cvar defs + `R_IQMInitCvars`)
- Modify: `sdlquake/engine_src/r_local.h` (declare `R_IQMInitCvars`)
- Modify: `sdlquake/engine_src/r_main.c` (call it from `R_Init`)

- [ ] **Step 1: Define the cvars + an init function**

In `sdlquake/engine_src/r_alias.c`, just before `#define MAX_IQM_JOINTS 128`, add:

```c
// R3 procedural-face cvars (head look-at, eye gaze, breathing). Registered from R_Init.
cvar_t	actor_lookat       = {"actor_lookat",       "1"};
cvar_t	actor_gaze_yaw     = {"actor_gaze_yaw",     "50"};   // head yaw clamp (deg)
cvar_t	actor_gaze_pitch   = {"actor_gaze_pitch",   "30"};   // head pitch clamp (deg)
cvar_t	actor_eye_yaw      = {"actor_eye_yaw",      "25"};   // eye yaw clamp (deg)
cvar_t	actor_eye_pitch    = {"actor_eye_pitch",    "20"};   // eye pitch clamp (deg)
cvar_t	actor_breathe_rate = {"actor_breathe_rate", "0.25"}; // breaths/sec
cvar_t	actor_breathe_amp  = {"actor_breathe_amp",  "0.04"}; // chest scale +/- fraction

void R_IQMInitCvars (void)
{
	Cvar_RegisterVariable (&actor_lookat);
	Cvar_RegisterVariable (&actor_gaze_yaw);
	Cvar_RegisterVariable (&actor_gaze_pitch);
	Cvar_RegisterVariable (&actor_eye_yaw);
	Cvar_RegisterVariable (&actor_eye_pitch);
	Cvar_RegisterVariable (&actor_breathe_rate);
	Cvar_RegisterVariable (&actor_breathe_amp);
}
```

- [ ] **Step 2: Declare it in `r_local.h`**

In `sdlquake/engine_src/r_local.h`, after the `void R_IQMDrawModel (alight_t *plighting);` line, add:

```c
void R_IQMInitCvars (void);
```

- [ ] **Step 3: Call it from `R_Init`**

In `sdlquake/engine_src/r_main.c`, in `R_Init`, after the last `Cvar_RegisterVariable (&...)` in the registration block (around line 294, `r_dspeeds`), add:

```c
	R_IQMInitCvars ();
```

- [ ] **Step 4: Build**

Run: `zig build 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 5: Verify cvars exist**

```bash
./zig-out/bin/quake --mcp-http 9876 +map start +wait 30 &
sleep 6
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_lookat"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_breathe_amp"}'
sleep 1
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"quit"}'
```
Expected: console reports `"actor_lookat" is "1"` and `"actor_breathe_amp" is "0.04"`.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine_src/r_alias.c sdlquake/engine_src/r_local.h sdlquake/engine_src/r_main.c
git commit -m "feat(actors): R3 — register procedural-face cvars (gaze/eye/breathe)"
```

---

## Task 3: Head look-at override

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_IQMDrawModel` + a look-at helper)

- [ ] **Step 1: Add the look-at-local helper + an eye test**

In `sdlquake/engine_src/r_alias.c`, after `IQM_Invert34` (ends at line ~907) and before the `R_IQMSetUpTransform` comment block, add:

```c
// Build a local 3x4 that aims the joint's +X axis at tgt_actor (actor space),
// with yaw/pitch clamped (degrees). Keeps the joint's base translation (trs[0..2]);
// replaces its rotation. parent = curwld[pp] (rotation assumed unit-scale, like
// IQM_Invert34's caveat). pp < 0 means root.
static void R_IQMLookAtLocal (const float *trs, int pp,
		float curwld[][3][4], const vec3_t tgt_actor,
		float maxyaw_deg, float maxpitch_deg, float local[3][4])
{
	vec3_t	org, dir, dirp;
	float	yaw, pitch, hyp, c1, s1, c2, s2, maxyaw, maxpitch;

	if (pp >= 0)
	{
		org[0] = DotProduct (trs, curwld[pp][0]) + curwld[pp][0][3];
		org[1] = DotProduct (trs, curwld[pp][1]) + curwld[pp][1][3];
		org[2] = DotProduct (trs, curwld[pp][2]) + curwld[pp][2][3];
	}
	else
	{
		org[0] = trs[0]; org[1] = trs[1]; org[2] = trs[2];
	}

	VectorSubtract (tgt_actor, org, dir);
	if (pp >= 0)
	{	// into parent space: dirp = Rot(curwld[pp])^T * dir
		dirp[0] = dir[0]*curwld[pp][0][0] + dir[1]*curwld[pp][1][0] + dir[2]*curwld[pp][2][0];
		dirp[1] = dir[0]*curwld[pp][0][1] + dir[1]*curwld[pp][1][1] + dir[2]*curwld[pp][2][1];
		dirp[2] = dir[0]*curwld[pp][0][2] + dir[1]*curwld[pp][1][2] + dir[2]*curwld[pp][2][2];
	}
	else
	{
		VectorCopy (dir, dirp);
	}

	yaw   = atan2 (dirp[1], dirp[0]);
	hyp   = sqrt (dirp[0]*dirp[0] + dirp[1]*dirp[1]);
	pitch = atan2 (dirp[2], hyp);

	maxyaw   = maxyaw_deg   * (M_PI / 180.0);
	maxpitch = maxpitch_deg * (M_PI / 180.0);
	if (yaw   >  maxyaw)   yaw   =  maxyaw;
	if (yaw   < -maxyaw)   yaw   = -maxyaw;
	if (pitch >  maxpitch) pitch =  maxpitch;
	if (pitch < -maxpitch) pitch = -maxpitch;

	c1 = cos (yaw);   s1 = sin (yaw);
	c2 = cos (pitch); s2 = sin (pitch);
	// M = Rz(yaw) * Ry(-pitch); maps +X -> (c1*c2, s1*c2, s2)
	local[0][0] = c1*c2; local[0][1] = -s1; local[0][2] = -c1*s2; local[0][3] = trs[0];
	local[1][0] = s1*c2; local[1][1] =  c1; local[1][2] = -s1*s2; local[1][3] = trs[1];
	local[2][0] = s2;    local[2][1] =   0; local[2][2] =    c2;  local[2][3] = trs[2];
}

static int R_IQMJointIsEye (lm_iqm_t *iqm, int jj)
{
	int e;
	for (e = 0; e < iqm->num_eye; e++)
		if (iqm->eye_joint[e] == jj)
			return 1;
	return 0;
}
```

(`M_PI` is available via `mathlib.h`/`math.h` already used in this file.)

- [ ] **Step 2: Compute the player target in actor space**

In `R_IQMDrawModel`, inside the `if (animated)` block, *after* the bind-matrix loop (`IQM_Invert34 (bindwld[jj], bindinv[jj]);`'s closing `}`) and *before* the `ff = (int)(cl.time * iqm->framerate);` line, add:

```c
		// R3: player position (r_origin) expressed in actor space, for look-at.
		// world->actor = Rot(entity)^T * (world - entity_origin); the entity
		// rotation columns are (alias_forward, -alias_right, alias_up).
		qboolean	lookat;
		vec3_t		tgt_actor, delta;

		lookat = (actor_lookat.value != 0);
		VectorSubtract (r_origin, currententity->origin, delta);
		tgt_actor[0] =  DotProduct (delta, alias_forward);
		tgt_actor[1] = -DotProduct (delta, alias_right);
		tgt_actor[2] =  DotProduct (delta, alias_up);
```

(Declarations at block top satisfy `-std=gnu89`; `qboolean`/`vec3_t` are engine types already in scope.)

- [ ] **Step 3: Apply the head override in the compose loop**

In `R_IQMDrawModel`, replace the existing animated compose loop body. The current loop is:

```c
		for (jj = 0; jj < iqm->numjoints; jj++)
		{
			float *src = iqm->frametrs + ((size_t)ff * iqm->numjoints + jj) * 10;
			for (cc = 0; cc < 10; cc++)
				trs[cc] = src[cc];
			IQM_LocalMat (trs, local);
			pp = iqm->joints[jj].parent;
			if (pp >= 0)
				R_ConcatTransforms (curwld[pp], local, curwld[jj]);
			else
				memcpy (curwld[jj], local, sizeof(local));
			R_ConcatTransforms (curwld[jj], bindinv[jj], skin[jj]);
		}
```

Replace it with (adds the head branch; eyes are added in Task 4):

```c
		for (jj = 0; jj < iqm->numjoints; jj++)
		{
			float *src = iqm->frametrs + ((size_t)ff * iqm->numjoints + jj) * 10;
			for (cc = 0; cc < 10; cc++)
				trs[cc] = src[cc];
			pp = iqm->joints[jj].parent;

			if (lookat && jj == iqm->head_joint)
				R_IQMLookAtLocal (trs, pp, curwld, tgt_actor,
					actor_gaze_yaw.value, actor_gaze_pitch.value, local);
			else
				IQM_LocalMat (trs, local);

			if (pp >= 0)
				R_ConcatTransforms (curwld[pp], local, curwld[jj]);
			else
				memcpy (curwld[jj], local, sizeof(local));
			R_ConcatTransforms (curwld[jj], bindinv[jj], skin[jj]);
		}
```

- [ ] **Step 4: Build**

Run: `zig build 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 5: Verify the head tracks the player in-game**

```bash
./zig-out/bin/quake --mcp-http 9876 +map start +wait 30 &
sleep 6
# spawn the actor in front of the player and look at it
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_spawn actors/dummy.iqm"}'
sleep 1
# strafe so the player is off to the actor's side; the head should yaw toward us
python3 scripts/mcp_call.py 9876 teleport '{"x":0,"y":120,"z":40,"angles":[0,225,0]}'
sleep 1
python3 scripts/mcp_call.py 9876 screenshot_gpu '{"name":"r3_head_left"}'
python3 scripts/mcp_call.py 9876 teleport '{"x":0,"y":-120,"z":40,"angles":[0,135,0]}'
sleep 1
python3 scripts/mcp_call.py 9876 screenshot_gpu '{"name":"r3_head_right"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"quit"}'
```
Expected: in `r3_head_left` vs `r3_head_right` the head box is rotated toward the camera in *opposite* directions (eyes-bearing face turns to follow the viewpoint); the base/chest boxes are identical between shots. (Exact camera coords are approximate — the test is "head yaw differs with viewpoint, body does not." Adjust teleport coords if the actor is off-frame.) Toggling `actor_lookat 0` returns the head to the baked R2 sweep.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(actors): R3 — head look-at override tracks the player (clamped cone)"
```

---

## Task 4: Eye gaze

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_IQMDrawModel` compose loop)

- [ ] **Step 1: Add the eye branch to the compose loop**

In `R_IQMDrawModel`, in the compose loop edited in Task 3, change the override condition so eyes (children of the head) also aim, with the tighter eye clamp. Replace:

```c
			if (lookat && jj == iqm->head_joint)
				R_IQMLookAtLocal (trs, pp, curwld, tgt_actor,
					actor_gaze_yaw.value, actor_gaze_pitch.value, local);
			else
				IQM_LocalMat (trs, local);
```

with:

```c
			if (lookat && jj == iqm->head_joint)
				R_IQMLookAtLocal (trs, pp, curwld, tgt_actor,
					actor_gaze_yaw.value, actor_gaze_pitch.value, local);
			else if (lookat && R_IQMJointIsEye (iqm, jj))
				R_IQMLookAtLocal (trs, pp, curwld, tgt_actor,
					actor_eye_yaw.value, actor_eye_pitch.value, local);
			else
				IQM_LocalMat (trs, local);
```

(Eyes are processed after the head because IQM guarantees parent index < child index, so `curwld[head]` — already aimed — is in place when the eyes compute their parent-space residual.)

- [ ] **Step 2: Build**

Run: `zig build 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 3: Verify eyes move independently**

The dummy's eyes are small (2u); independent swivel is most visible when the head is *clamped* (player beyond `actor_gaze_yaw`), so the eyes carry the residual. Test by tightening the head clamp:

```bash
./zig-out/bin/quake --mcp-http 9876 +map start +wait 30 &
sleep 6
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_spawn actors/dummy.iqm"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_gaze_yaw 10"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_eye_yaw 40"}'
sleep 1
python3 scripts/mcp_call.py 9876 teleport '{"x":0,"y":140,"z":48,"angles":[0,250,0]}'
sleep 1
python3 scripts/mcp_call.py 9876 screenshot_gpu '{"name":"r3_eyes_side"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"quit"}'
```
Expected: with the head barely turned (10° clamp) the two eye boxes are visibly shifted toward the camera side of the face (40° eye clamp). Compare against an `actor_eye_yaw 0` shot — the eyes sit centred. (Eyes are tiny; zoom the PNG. The functional check is "eye boxes shift when `actor_eye_yaw` changes and head clamp is small.")

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(actors): R3 — eye-gaze swivel (tighter clamp, residual past head clamp)"
```

---

## Task 5: Breathing (non-propagating chest scale)

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c` (`R_IQMDrawModel`, after the compose loop)

- [ ] **Step 1: Scale the chest skin matrix about its posed origin**

In `R_IQMDrawModel`, immediately after the compose loop's closing `}` (the loop that ends with `R_ConcatTransforms (curwld[jj], bindinv[jj], skin[jj]);`) and still inside the `if (animated)` block, add:

```c
		// R3 breathing: scale ONLY the chest mesh about its posed origin, by
		// post-multiplying its skin matrix. Non-propagating (children use their
		// own skin matrices), matching the design's "uniform, non-propagating
		// mesh scale" decision. v' = org + s*(v - org) on the posed vertex.
		if (iqm->chest_joint >= 0 && iqm->chest_joint < iqm->numjoints &&
			actor_breathe_amp.value != 0.0f)
		{
			int		ci = iqm->chest_joint;
			float	s  = 1.0f + actor_breathe_amp.value *
						sin (cl.time * actor_breathe_rate.value * 2.0f * M_PI);
			float	org[3];
			int		rr;

			org[0] = curwld[ci][0][3];
			org[1] = curwld[ci][1][3];
			org[2] = curwld[ci][2][3];
			for (rr = 0; rr < 3; rr++)
			{
				skin[ci][rr][0] *= s;
				skin[ci][rr][1] *= s;
				skin[ci][rr][2] *= s;
				skin[ci][rr][3] = s * skin[ci][rr][3] + (1.0f - s) * org[rr];
			}
		}
```

- [ ] **Step 2: Build**

Run: `zig build 2>&1 | tail -20`
Expected: clean build.

- [ ] **Step 3: Verify the chest pulses in-game**

Breathing is slow (0.25 Hz = one breath / 4 s); exaggerate the amplitude to make the still-frame difference obvious, and capture the extremes by stepping `cl.time` via wait:

```bash
./zig-out/bin/quake --mcp-http 9876 +map start +wait 30 &
sleep 6
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_spawn actors/dummy.iqm"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_breathe_amp 0.30"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"actor_breathe_rate 0.5"}'
python3 scripts/mcp_call.py 9876 teleport '{"x":0,"y":110,"z":40,"angles":[0,270,0]}'
sleep 1
python3 scripts/mcp_call.py 9876 screenshot_gpu '{"name":"r3_breathe_a"}'
sleep 1
python3 scripts/mcp_call.py 9876 screenshot_gpu '{"name":"r3_breathe_b"}'
python3 scripts/mcp_call.py 9876 console_exec '{"cmd":"quit"}'
```
Expected: between `r3_breathe_a` and `r3_breathe_b` (taken ~1 s apart, half a breath at 0.5 Hz) the chest box is visibly wider/narrower while the base box (joint 0, not the chest) is unchanged — the head/eyes ride on top but do **not** themselves scale (non-propagating). At default `actor_breathe_amp 0.04` the motion is a subtle idle pulse.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(actors): R3 — breathing via non-propagating chest skin scale"
```

---

## Task 6: Documentation

**Files:**
- Modify: `CLAUDE.md` (the "Skeletal actors" section)

- [ ] **Step 1: Note R3 done**

In `CLAUDE.md`, under "## Skeletal actors (IQM, TR1-style — in progress)", after the **R2 done** bullet, add an **R3 done** bullet summarising: procedural face layer in `R_IQMDrawModel` — role joints (head/chest/jaw/eyes) resolved by name in the IQM loader; head + eye look-at track the player (`r_origin`) clamped to a cone; breathing scales the chest mesh non-propagatingly; cvars `actor_lookat`/`actor_gaze_{yaw,pitch}`/`actor_eye_{yaw,pitch}`/`actor_breathe_{rate,amp}`; lipsync (mouth skin-swap) deferred until heads are textured.

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude): note skeletal actors R3 (procedural look-at + breathing)"
```

---

## Self-review

- **Spec coverage:** R3 row of the decomposition = "Override joints by role: head look-at, eye gaze, jaw-open from voice amplitude + mouth skin-swap, breathing scale." Covered: head look-at (T3), eye gaze (T4), breathing (T5). **Deferred with rationale:** jaw-open + mouth skin-swap (lipsync) — needs textured heads (stated in the scope note + design's "real visemes" non-goal). Role resolution for `jaw` is in place (T1) so lipsync drops in later with no loader change.
- **Type consistency:** `R_IQMLookAtLocal(trs, pp, curwld, tgt_actor, maxyaw, maxpitch, local)` — same signature used in T3/T4. `R_IQMJointIsEye(iqm, jj)` consistent. cvar names identical across def/register/use. `lm_iqm_t` fields `head_joint/chest_joint/jaw_joint/eye_joint/num_eye` match between loader (T1) and renderer (T3–T5).
- **Placeholder scan:** none — every step has concrete code/commands.
- **Math check:** look-at `M = Rz(yaw)·Ry(-pitch)` maps local +X → (cos yaw·cos pitch, sin yaw·cos pitch, sin pitch) = the (clamped) target direction; breathing post-scale is `v' = s·(skin·v) + (1−s)·org`, scaling the chest mesh about its posed origin without touching child joints.

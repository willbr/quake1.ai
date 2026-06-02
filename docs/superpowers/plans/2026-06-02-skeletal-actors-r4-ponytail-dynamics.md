# Skeletal Actors R4 — Secondary Dynamics (self-animating ponytail) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A chain of "ponytail" joints on a `mod_iqm` actor self-animates — hanging under gravity and swinging with inertia as the actor/head moves — with no keyframes and no networking.

**Architecture:** A client-side **Verlet point-chain** solver, engine-side, run inside `R_IQMDrawModel` after the procedural (R3) pose is composed. The chain root is pinned to its rigid posed position (so it follows the head look-at); the free points integrate gravity + inertia in **world space** (correct gravity, immune to camera motion) and satisfy distance constraints; the simulated node transforms are converted back to actor space and written into the `skin[]` matrices for the ponytail joints. Per-actor sim state lives in a small pool keyed by `entity_t*`. Ponytail joints are resolved by the `pony` name convention in the IQM loader (R3's role-resolution pattern).

**Tech Stack:** C (engine `-std=gnu89`), `libmodel` for the joint-name resolution, the existing IQM render path. No ABI bump, no protocol (cosmetic/client-derived per the design).

---

## Design decisions (resolving the design's open questions)

- **Spring form:** **Verlet point-chain** (positions + distance constraints), not per-joint angular springs. Simplest stable hair/cloth model; no tuning of inertia tensors.
- **Wind:** **deferred to a follow-up.** `sim_wind` lives in `game.dll`; the engine-side solver can't read it without an ABI hook. R4 is gravity + inertia only; a `wind` hook is a later add (noted in the design's R4 open question).
- **Bend stiffness:** a light **straighten-toward-parent-direction** constraint keeps the tail from collapsing/tangling (one cheap term), tunable via `actor_pony_stiffness`.
- **Multi-actor:** a fixed pool of sim states keyed by `entity_t*` (cap 16, LRU-evict). The dev actor is the only consumer now; the pool keeps it correct if more actors spawn.

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `scripts/make_test_actor_iqm.py` | modify | Add a 3-segment ponytail chain (joints + segment meshes); regenerate `id1/actors/dummy.iqm` |
| `sdlquake/libmodel/iqm.h` | modify | Add ponytail role fields to `lm_iqm_t` |
| `sdlquake/libmodel/iqm.c` | modify | Resolve ponytail joints (ordered chain) by name |
| `sdlquake/engine_src/iqm_dynamics.c` | create | Verlet ponytail solver + per-actor state pool |
| `sdlquake/engine_src/iqm_dynamics.h` | create | Solver entry point |
| `sdlquake/engine_src/r_alias.c` | modify | Call the solver after R3 compose; write ponytail `skin[]` |
| `sdlquake/engine_src/r_local.h` | modify | Declare the R4 cvar-init (added to `R_IQMInitCvars`) |
| `build.zig` | modify | Add `iqm_dynamics.c` to the engine source list |
| `CLAUDE.md` | modify | Document R4 done |

---

## Task 1: Add a ponytail chain to the test asset

**Files:**
- Modify: `scripts/make_test_actor_iqm.py`
- Regenerate: `id1/actors/dummy.iqm`

- [ ] **Step 1: Add ponytail joints + segment parts**

In `scripts/make_test_actor_iqm.py`, extend the `JOINTS` list (after `("eye.R", 2, (8, -4, 4))`) with a 3-link chain hanging off the head (joint 2), going back (−X) and down (−Z):

```python
    ("ponytail_01", 2, (-10, 0, 2)),
    ("ponytail_02", 5, (0, 0, -10)),
    ("ponytail_03", 6, (0, 0, -10)),
```

(So joint indices: 5,6,7. `ponytail_01` parents to head (2); each next link parents to the previous.)

Extend `PARTS` (after the two eye entries) with a small cube segment per ponytail joint:

```python
    (5, (3, 3, 5), (0, 0, 0), "p_pony"),
    (6, (3, 3, 5), (0, 0, 0), "p_pony"),
    (7, (3, 3, 5), (0, 0, 0), "p_pony"),
```

- [ ] **Step 2: Regenerate the asset**

Run: `python3 scripts/make_test_actor_iqm.py`
Expected: prints `wrote .../dummy.iqm ... 70 verts 90 tris 8 joints, 16 frames 2 framechannels` (5 original parts × 8 verts = 40, + 3 pony × 8 = 64 verts? — exact counts: 8 parts × 8 verts = 64 verts, 8 parts × 12 tris = 96 tris, 8 joints). Confirm the printed counts match `8 joints` and the file is rewritten. The `assert len(out) == filesize` guard must pass.

- [ ] **Step 3: Verify the asset loads with the new joints**

```bash
zig build 2>&1 | tail -5
./zig-out/bin/quake -nosound -nofocus --headless --mcp-http 9876 +map start > /tmp/r4.log 2>&1 &
python3 - <<'PY'
import socket,time
for _ in range(60):
    try: socket.create_connection(("127.0.0.1",9876),1).close(); break
    except OSError: time.sleep(0.5)
PY
python3 scripts/mcp_call.py console_exec '{"command":"actor_dump actors/dummy.iqm"}' >/dev/null 2>&1
python3 scripts/mcp_call.py console_exec '{"command":"quit"}' >/dev/null 2>&1
grep -n "joints\|ponytail" /tmp/r4.log | head
```
Expected: `8 joints` and joints 5/6/7 named `ponytail_01/02/03`.

- [ ] **Step 4: Commit**

```bash
git add scripts/make_test_actor_iqm.py id1/actors/dummy.iqm
git commit -m "feat(actors): R4 — add 3-link ponytail chain to the test actor asset"
```

---

## Task 2: Resolve ponytail joints (ordered chain) in the loader

**Files:**
- Modify: `sdlquake/libmodel/iqm.h`
- Modify: `sdlquake/libmodel/iqm.c`

- [ ] **Step 1: Add ponytail fields to `lm_iqm_t`**

In `sdlquake/libmodel/iqm.h`, in the role block added in R3 (after `int num_eye;`), add:

```c
    int             pony_joint[8];  /* ordered chain root..tip (R4 dynamics) */
    int             num_pony;
```

- [ ] **Step 2: Resolve the chain in load order**

In `sdlquake/libmodel/iqm.c`, in the R3 role-resolution loop, extend the body to also collect ponytail joints (they are emitted root-first in the asset, so load order == chain order):

```c
        if (strstr(nm, "pony") && m->num_pony < 8)       m->pony_joint[m->num_pony++] = i;
```

And initialise `m->num_pony = 0;` alongside `m->num_eye = 0;` just above the loop.

- [ ] **Step 3: Print in `actor_dump`**

In `sdlquake/engine_src/iqm_dev.c`, extend the roles line:

```c
	Con_Printf ("  roles: head %d  chest %d  jaw %d  eyes %d  pony %d\n",
		iqm->head_joint, iqm->chest_joint, iqm->jaw_joint, iqm->num_eye, iqm->num_pony);
```

- [ ] **Step 4: Build + verify**

```bash
zig build 2>&1 | tail -5
```
Then dump as in Task 1 Step 3; expected roles line includes `pony 3`.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/libmodel/iqm.h sdlquake/libmodel/iqm.c sdlquake/engine_src/iqm_dev.c
git commit -m "feat(actors): R4 — resolve ordered ponytail joint chain by name"
```

---

## Task 3: The Verlet ponytail solver

**Files:**
- Create: `sdlquake/engine_src/iqm_dynamics.h`
- Create: `sdlquake/engine_src/iqm_dynamics.c`
- Modify: `build.zig`

- [ ] **Step 1: Header**

Create `sdlquake/engine_src/iqm_dynamics.h`:

```c
#ifndef IQM_DYNAMICS_H
#define IQM_DYNAMICS_H

#include "iqm.h"

// Simulate the actor's ponytail chain and write the resulting per-joint skin
// matrices into skin[] (actor space, bind->posed). curwld holds the R3-posed
// joint world matrices (actor space). ent identifies the actor (per-actor state).
// a2w_rot columns are the entity actor->world rotation (alias_forward,
// -alias_right, alias_up); a2w_org is the entity world origin. now = cl.time.
// No-op if the actor has no ponytail or dynamics are disabled.
struct entity_s;
void IQM_SolveDynamics (struct entity_s *ent, lm_iqm_t *iqm,
		float curwld[][3][4], float bindinv[][3][4], float skin[][3][4],
		const float a2w_rot[3][3], const float a2w_org[3], float now);

void IQM_DynamicsInitCvars (void);

#endif
```

- [ ] **Step 2: Solver implementation**

Create `sdlquake/engine_src/iqm_dynamics.c`:

```c
// iqm_dynamics.c -- R4 client-side ponytail dynamics (Verlet point-chain).
// Engine-side, cosmetic, never networked. Simulates in world space (gravity is
// world-down, camera motion injects no force), then writes actor-space skin
// matrices for the ponytail joints.

#include "quakedef.h"
#include "iqm.h"
#include "iqm_dynamics.h"

cvar_t actor_dynamics       = {"actor_dynamics",       "1"};
cvar_t actor_pony_gravity   = {"actor_pony_gravity",   "320"}; // units/s^2 down
cvar_t actor_pony_damping   = {"actor_pony_damping",   "0.92"};// velocity retain/frame
cvar_t actor_pony_stiffness = {"actor_pony_stiffness", "0.25"};// straighten toward bind dir [0..1]
cvar_t actor_pony_iters     = {"actor_pony_iters",     "4"};   // constraint relax passes

void IQM_DynamicsInitCvars (void)
{
	Cvar_RegisterVariable (&actor_dynamics);
	Cvar_RegisterVariable (&actor_pony_gravity);
	Cvar_RegisterVariable (&actor_pony_damping);
	Cvar_RegisterVariable (&actor_pony_stiffness);
	Cvar_RegisterVariable (&actor_pony_iters);
}

#define PONY_MAX     8
#define DYN_POOL     16

typedef struct {
	void	*ent;            // owning entity (NULL = free slot)
	float	last;            // cl.time of last sim
	int		n;               // node count (anchor + free)
	vec3_t	pos[PONY_MAX+1]; // world-space node positions
	vec3_t	prev[PONY_MAX+1];
	float	rest[PONY_MAX+1];// rest length node[i-1]->node[i]
	int		inited;
} dyn_state_t;

static dyn_state_t s_pool[DYN_POOL];

static dyn_state_t *dyn_get (void *ent)
{
	int i, lru = 0;
	float oldest = 1e30f;
	for (i = 0; i < DYN_POOL; i++)
		if (s_pool[i].ent == ent)
			return &s_pool[i];
	for (i = 0; i < DYN_POOL; i++)
		if (!s_pool[i].ent) { memset (&s_pool[i], 0, sizeof(s_pool[i])); s_pool[i].ent = ent; return &s_pool[i]; }
	for (i = 0; i < DYN_POOL; i++)
		if (s_pool[i].last < oldest) { oldest = s_pool[i].last; lru = i; }
	memset (&s_pool[lru], 0, sizeof(s_pool[lru]));
	s_pool[lru].ent = ent;
	return &s_pool[lru];
}

// actor-space point -> world
static void a2w_point (const float rot[3][3], const float org[3], const vec3_t a, vec3_t w)
{
	w[0] = org[0] + rot[0][0]*a[0] + rot[0][1]*a[1] + rot[0][2]*a[2];
	w[1] = org[1] + rot[1][0]*a[0] + rot[1][1]*a[1] + rot[1][2]*a[2];
	w[2] = org[2] + rot[2][0]*a[0] + rot[2][1]*a[1] + rot[2][2]*a[2];
}
// world vector -> actor space (rot^T), no translation
static void w2a_vec (const float rot[3][3], const vec3_t w, vec3_t a)
{
	a[0] = rot[0][0]*w[0] + rot[1][0]*w[1] + rot[2][0]*w[2];
	a[1] = rot[0][1]*w[0] + rot[1][1]*w[1] + rot[2][1]*w[2];
	a[2] = rot[0][2]*w[0] + rot[1][2]*w[1] + rot[2][2]*w[2];
}

void IQM_SolveDynamics (struct entity_s *ent, lm_iqm_t *iqm,
		float curwld[][3][4], float bindinv[][3][4], float skin[][3][4],
		const float a2w_rot[3][3], const float a2w_org[3], float now)
{
	dyn_state_t	*st;
	int			N, i, it, iters;
	float		dt, damp, grav, stiff;
	vec3_t		anchor_w, restdir_a[PONY_MAX+1];
	int			pj, parent;

	if (!iqm || iqm->num_pony < 1 || actor_dynamics.value == 0)
		return;
	N = iqm->num_pony;
	if (N > PONY_MAX) N = PONY_MAX;

	st = dyn_get ((void *)ent);

	// node[0] = anchor (rigid posed position of pony root's parent attach =
	// pony_joint[0]'s posed origin). node[1..N] = pony_joint[0..N-1] tips.
	// We treat each ponytail joint origin as a free node; node[0] is the
	// anchor pinned to the root joint's RIGID posed origin.
	{
		vec3_t a;
		int    root = iqm->pony_joint[0];
		parent = iqm->joints[root].parent;
		// anchor in actor space = parent posed origin + parent-rot * root.bindtranslate
		if (parent >= 0)
		{
			a[0] = DotProduct (iqm->joints[root].translate, curwld[parent][0]) + curwld[parent][0][3];
			a[1] = DotProduct (iqm->joints[root].translate, curwld[parent][1]) + curwld[parent][1][3];
			a[2] = DotProduct (iqm->joints[root].translate, curwld[parent][2]) + curwld[parent][2][3];
		}
		else { VectorCopy (iqm->joints[root].translate, a); }
		a2w_point (a2w_rot, a2w_org, a, anchor_w);
	}

	// rest lengths + per-node bind direction (actor space, parent->node) from the
	// rigid posed skeleton, used for stiffness straightening.
	{
		vec3_t prevw, curw, a;
		VectorCopy (anchor_w, prevw);
		for (i = 0; i < N; i++)
		{
			pj = iqm->pony_joint[i];
			a[0] = curwld[pj][0][3]; a[1] = curwld[pj][1][3]; a[2] = curwld[pj][2][3];
			a2w_point (a2w_rot, a2w_org, a, curw);
			VectorSubtract (curw, prevw, restdir_a[i+1]); // world dir parent->node
			st->rest[i+1] = VectorLength (restdir_a[i+1]);
			if (st->rest[i+1] < 0.01f) st->rest[i+1] = 0.01f;
			VectorCopy (curw, prevw);
		}
	}

	st->n = N + 1;

	// (re)initialise to the rigid posed positions on first use / asset change
	if (!st->inited)
	{
		vec3_t prevw, a, curw;
		VectorCopy (anchor_w, st->pos[0]); VectorCopy (anchor_w, st->prev[0]);
		VectorCopy (anchor_w, prevw);
		for (i = 0; i < N; i++)
		{
			pj = iqm->pony_joint[i];
			a[0] = curwld[pj][0][3]; a[1] = curwld[pj][1][3]; a[2] = curwld[pj][2][3];
			a2w_point (a2w_rot, a2w_org, a, curw);
			VectorCopy (curw, st->pos[i+1]); VectorCopy (curw, st->prev[i+1]);
			VectorCopy (curw, prevw);
		}
		st->inited = 1;
		st->last = now;
	}

	dt = now - st->last;
	st->last = now;
	if (dt <= 0.0f) dt = 0.0f;
	if (dt > 0.1f)  dt = 0.1f;       // clamp huge gaps (pause/teleport)

	damp  = actor_pony_damping.value;
	grav  = actor_pony_gravity.value;
	stiff = actor_pony_stiffness.value;
	iters = (int)actor_pony_iters.value; if (iters < 1) iters = 1;

	// pin anchor
	VectorCopy (anchor_w, st->pos[0]);
	VectorCopy (anchor_w, st->prev[0]);

	// Verlet integrate free nodes
	for (i = 1; i <= N; i++)
	{
		vec3_t vel, np;
		VectorSubtract (st->pos[i], st->prev[i], vel);
		VectorScale (vel, damp, vel);
		np[0] = st->pos[i][0] + vel[0];
		np[1] = st->pos[i][1] + vel[1];
		np[2] = st->pos[i][2] + vel[2] - grav * dt * dt;
		VectorCopy (st->pos[i], st->prev[i]);
		VectorCopy (np, st->pos[i]);
	}

	// constraints: distance to parent (+ light straighten toward bind dir)
	for (it = 0; it < iters; it++)
	{
		for (i = 1; i <= N; i++)
		{
			vec3_t d, want; float len, diff;
			// straighten: blend node toward parent + bind direction
			if (stiff > 0.0f)
			{
				want[0] = st->pos[i-1][0] + restdir_a[i][0];
				want[1] = st->pos[i-1][1] + restdir_a[i][1];
				want[2] = st->pos[i-1][2] + restdir_a[i][2];
				st->pos[i][0] += (want[0] - st->pos[i][0]) * stiff * 0.5f;
				st->pos[i][1] += (want[1] - st->pos[i][1]) * stiff * 0.5f;
				st->pos[i][2] += (want[2] - st->pos[i][2]) * stiff * 0.5f;
			}
			VectorSubtract (st->pos[i], st->pos[i-1], d);
			len = VectorLength (d);
			if (len < 0.0001f) { d[0]=0;d[1]=0;d[2]=-1; len=1; }
			diff = (len - st->rest[i]) / len;
			// move only the child (parent is closer to the pinned anchor)
			st->pos[i][0] -= d[0] * diff;
			st->pos[i][1] -= d[1] * diff;
			st->pos[i][2] -= d[2] * diff;
		}
	}

	// write skin matrices: place each ponytail joint at its simulated node,
	// oriented so +X (the bone's bind forward... here the chain axis) points
	// from parent node to this node. Convert world->actor for skin.
	for (i = 0; i < N; i++)
	{
		vec3_t fwdw, fwda, upa, refa, leftw, ssegw;
		float  posed_a[3][4], simw[3][4], wpos_a[3];
		pj = iqm->pony_joint[i];

		VectorSubtract (st->pos[i+1], st->pos[i], fwdw);
		if (VectorLength (fwdw) < 0.0001f) { fwdw[0]=0;fwdw[1]=0;fwdw[2]=-1; }
		w2a_vec (a2w_rot, fwdw, fwda);
		VectorNormalize (fwda);
		// build a stable basis (ref up = actor +Z unless near-parallel)
		refa[0]=0; refa[1]=0; refa[2]=1;
		if (fwda[2] > 0.99f || fwda[2] < -0.99f) { refa[0]=1; refa[1]=0; refa[2]=0; }
		CrossProduct (refa, fwda, upa);   VectorNormalize (upa);   // "left"
		CrossProduct (fwda, upa, refa);   VectorNormalize (refa);  // "up"
		// node position world->actor
		{ vec3_t rel; VectorSubtract (st->pos[i+1], a2w_org, rel); w2a_vec (a2w_rot, rel, wpos_a); }
		// posed_a columns: +X=fwda, +Y=upa(left), +Z=refa(up); translation=wpos_a
		posed_a[0][0]=fwda[0]; posed_a[0][1]=upa[0]; posed_a[0][2]=refa[0]; posed_a[0][3]=wpos_a[0];
		posed_a[1][0]=fwda[1]; posed_a[1][1]=upa[1]; posed_a[1][2]=refa[1]; posed_a[1][3]=wpos_a[1];
		posed_a[2][0]=fwda[2]; posed_a[2][1]=upa[2]; posed_a[2][2]=refa[2]; posed_a[2][3]=wpos_a[2];
		R_ConcatTransforms (posed_a, bindinv[pj], skin[pj]);
		(void)simw; (void)leftw; (void)ssegw;
	}
}
```

- [ ] **Step 3: Add to build**

In `build.zig`, add `"iqm_dynamics.c"` to the engine source list (next to `"iqm_dev.c"`).

- [ ] **Step 4: Build**

Run: `zig build 2>&1 | tail -8`
Expected: clean build (the solver is not yet called — this step just compiles it).

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/iqm_dynamics.c sdlquake/engine_src/iqm_dynamics.h build.zig
git commit -m "feat(actors): R4 — Verlet ponytail solver (engine-side, per-actor state)"
```

---

## Task 4: Wire the solver into the render + cvars

**Files:**
- Modify: `sdlquake/engine_src/r_alias.c`

- [ ] **Step 1: Include + register cvars**

In `sdlquake/engine_src/r_alias.c`, add near the other includes (after `#include "iqm.h"`):

```c
#include "iqm_dynamics.h"
```

In `R_IQMInitCvars`, after the last `Cvar_RegisterVariable`, add:

```c
	IQM_DynamicsInitCvars ();
```

- [ ] **Step 2: Call the solver after the R3 breathing block**

In `R_IQMDrawModel`, immediately after the breathing `if` block and before the closing `}` of `if (animated)`, add:

```c
		// R4: ponytail dynamics. Build the actor->world rotation (entity columns
		// alias_forward / -alias_right / alias_up) and origin, then solve + write
		// the ponytail joints' skin matrices.
		if (iqm->num_pony > 0)
		{
			float	a2w_rot[3][3], a2w_org[3];
			int		rr2;
			for (rr2 = 0; rr2 < 3; rr2++)
			{
				a2w_rot[rr2][0] =  alias_forward[rr2];
				a2w_rot[rr2][1] = -alias_right[rr2];
				a2w_rot[rr2][2] =  alias_up[rr2];
			}
			VectorCopy (currententity->origin, a2w_org);
			IQM_SolveDynamics (currententity, iqm, curwld, bindinv, skin,
				(const float (*)[3])a2w_rot, a2w_org, cl.time);
		}
```

- [ ] **Step 3: Build**

Run: `zig build 2>&1 | tail -8`
Expected: clean build.

- [ ] **Step 4: Verify the ponytail hangs + swings in-game**

```bash
./zig-out/bin/quake -nosound -nofocus --mcp-http 9876 +map m7_skeleton > /tmp/r4v.log 2>&1 &
# (poll port as before)
python3 scripts/mcp_call.py console_exec '{"command":"r_fullbright 1"}'
python3 scripts/mcp_call.py console_exec '{"command":"actor_spawn actors/dummy.iqm 284 0 64"}'
python3 scripts/mcp_call.py console_exec '{"command":"noclip"}'
# view the actor's back/side so the tail (hanging behind, -X) is visible
python3 scripts/mcp_call.py teleport '{"origin":[200,120,96],"angles":[6,310,0]}'
# whip the head (and thus the tail) by swinging gaze, capture mid-swing frames
python3 scripts/mcp_call.py screenshot_gpu '{"path":"r4_pony_a.png"}'
# move the camera so the head look-at turns the head; the tail should lag
python3 scripts/mcp_call.py teleport '{"origin":[200,-120,96],"angles":[6,50,0]}'
python3 scripts/mcp_call.py screenshot_gpu '{"path":"r4_pony_b.png"}'
python3 scripts/mcp_call.py console_exec '{"command":"quit"}'
```
Expected: the ponytail segments hang **down/back** from the head at rest, and when the head turns (camera moves → look-at swings the head) the tail visibly **lags then catches up** (inertia). Toggling `actor_dynamics 0` freezes the tail rigid to the head (snaps with it, no lag) — the A/B that proves the solver is live. Adjust camera coords so the tail is in frame.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/r_alias.c
git commit -m "feat(actors): R4 — drive ponytail skin from the Verlet solver each frame"
```

---

## Task 5: Documentation

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Note R4 done** — add an R4 bullet under the skeletal-actors section: client-side Verlet ponytail (`iqm_dynamics.c`), world-space sim pinned to the rigid posed root, distance + light straighten constraints, per-actor state pool keyed by `entity_t*`, written into ponytail `skin[]`; cvars `actor_dynamics`/`actor_pony_gravity`/`actor_pony_damping`/`actor_pony_stiffness`/`actor_pony_iters`; wind tie-in deferred; no ABI/protocol.

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(claude): note skeletal actors R4 (ponytail dynamics)"
```

---

## Self-review

- **Spec coverage:** R4 row = "Client-side spring solver for `dynamic` joints; reacts to motion + gravity; optional `sim_wind` tie-in. No protocol." Covered: Verlet solver (T3), reacts to head/actor motion via the pinned anchor + inertia, gravity (T3), no protocol (engine-side). **Deferred with rationale:** `sim_wind` tie-in (needs an ABI hook to read the game.dll grid) — noted.
- **Type consistency:** `IQM_SolveDynamics(ent, iqm, curwld, bindinv, skin, a2w_rot, a2w_org, now)` — same signature in header (T3), definition (T3), and call site (T4). `pony_joint[]`/`num_pony` consistent between loader (T2) and solver (T3). Cvars defined+registered in `iqm_dynamics.c`, registration chained from `R_IQMInitCvars` (T4).
- **Placeholder scan:** none.
- **Math check:** sim in world space (gravity world-down, camera-independent); anchor pinned to the rigid posed root each frame so the tail follows the head look-at; child-only distance correction keeps the chain hanging from the pinned end; world→actor (`rot^T`) conversion before writing `skin = posed_actor ∘ bindinv`. The `a2w_rot` columns `(alias_forward, -alias_right, alias_up)` match the entity actor→world rotation used by R3's look-at inverse.
- **Caveat to watch at verify:** the `(const float (*)[3])a2w_rot` cast in T4 must match the header's `const float a2w_rot[3][3]` parameter; if the compiler warns, pass `a2w_rot` directly (engine builds with `-w` for engine files? confirm) — adjust at build time.

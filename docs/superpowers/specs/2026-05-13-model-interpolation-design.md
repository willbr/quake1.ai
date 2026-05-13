# Model Frame Interpolation — Design

**Date:** 2026-05-13
**Status:** draft
**Touches:** `sdlquake/engine_src/r_alias.c`, `sdlquake/engine_src/cl_main.c`, `sdlquake/engine_src/render.h`, `sdlquake/engine_src/r_main.c`

## Goal

Quake's software renderer snaps between alias-model keyframes at the QC tick rate (~10 Hz). Monsters, the player viewmodel, and animated entities all flicker between discrete poses. This spec covers vertex-level interpolation so single-frame transitions (e.g. ogre `walk1` → `walk2`) and group-frame cycles (torches, flames) blend smoothly.

Out of scope: GL renderer changes (project ships only the software path), sprite interpolation, lightmap interpolation, lerping the per-vertex `lightnormalindex`.

## Background

The engine already interpolates entity **origin** and **angles** between server snapshots in `CL_LerpPoint` / `CL_RelinkEntities` (`cl_main.c:395`, `cl_main.c:446`), gated by `cl_nolerp`. What's missing is **vertex-position** lerp inside `R_AliasSetupFrame` / `R_AliasTransformFinalVert` / `R_AliasTransformAndProjectFinalVerts` in `r_alias.c`.

The id386 ASM path (`r_aliasa.s`) is dead on x64 — only the C transform functions need editing.

## Design

### 1. State additions

**`entity_t`** (`render.h:39`) gains three fields:

```c
int     prev_frame;          // frame value we are lerping FROM
int     prev_frame_observed; // last frame value seen by CL_RelinkEntities
float   frame_start_time;    // cl.time when ent->frame last changed
```

`prev_frame_observed` exists so we can detect QC-driven frame changes by comparing against `ent->frame`; `prev_frame` is the actual lerp source (decoupled so forcelink can collapse it to the current frame without losing the change-detection bit).

**Cvar** (`r_main.c`):

```c
cvar_t r_lerpmodels = {"r_lerpmodels", "1", true};  // archived
```

`0` disables vertex lerp (classic snap); `1` enables. Vertex lerp runs only when `r_lerpmodels != 0` **and** `cl_nolerp == 0` — either zeroed/non-zeroed in the disabling direction skips the blend.

**Renderer globals** (`r_alias.c`):

```c
trivertx_t *r_apverts;        // current frame verts (existing)
trivertx_t *r_apverts_prev;   // previous frame verts (new)
float       r_framelerp;      // 0..1 blend factor (new); 1.0 = pure current
```

### 2. Tracking frame transitions

In `CL_RelinkEntities` (per-entity loop, `cl_main.c:485+`), before the entity is queued for rendering:

```c
if (ent->forcelink || ent->frame != ent->prev_frame_observed) {
    ent->prev_frame         = ent->forcelink ? ent->frame
                                             : ent->prev_frame_observed;
    ent->prev_frame_observed = ent->frame;
    ent->frame_start_time    = cl.time;
}
```

On forcelink (respawn, teleport, `U_NOLERP`, first sighting) we slam `prev_frame = frame` so the next `R_AliasSetupFrame` computes `r_framelerp = 1.0` and skips the blend. No "monster slides from death-pose into idle" glitch.

### 3. Lerp factor

In `R_AliasSetupFrame` (`r_alias.c:652`):

```c
qboolean nolerp = !r_lerpmodels.value || cl_nolerp.value ||
                  currententity->frame == currententity->prev_frame;

if (nolerp) {
    r_framelerp = 1.0f;
} else {
    r_framelerp = (cl.time - currententity->frame_start_time) / 0.1f;
    if (r_framelerp > 1.0f) r_framelerp = 1.0f;
    if (r_framelerp < 0.0f) r_framelerp = 0.0f;
}
```

`0.1f` is the assumed transition duration — standard QC `self.nextthink` cadence and what FitzQuake / QuakeSpasm / ezQuake all use.

Then pick the two vertex bases:

- **Both frames `ALIAS_SINGLE`:**
  ```c
  r_apverts      = (trivertx_t *)((byte *)paliashdr + paliashdr->frames[frame].frame);
  r_apverts_prev = (trivertx_t *)((byte *)paliashdr + paliashdr->frames[prev].frame);
  ```
- **Both `ALIAS_GROUP`:** existing time-based subframe selection runs and yields index `i`. Let `t_lo = (i == 0) ? 0.0f : pintervals[i-1]` and `t_hi = pintervals[i]`; then `alpha = (targettime - t_lo) / (t_hi - t_lo)`. `r_apverts` = subframe `i`, `r_apverts_prev` = subframe `(i == 0) ? numframes-1 : i-1`, and `r_framelerp = alpha`. The wrap on the prev index gives a continuous cycle across the group boundary.
- **Mixed (single↔group boundary):** set `r_framelerp = 1.0` and `r_apverts_prev = r_apverts`. Lerping across the boundary has no defined meaning. Rare in practice (QC almost never does this).

### 4. Vertex transform

Two functions in `r_alias.c` are patched. Both consume the vertex globals; we change `r_apverts` from a per-vertex incrementing pointer into an **indexed base**, so `r_apverts_prev` stays in lockstep without parallel increments scattered through the call sites.

**Helper** (file-static, in `r_alias.c`):

```c
static inline void R_LerpVert(trivertx_t *cur, trivertx_t *prev, vec3_t out) {
    out[0] = cur->v[0] * r_framelerp + prev->v[0] * (1.0f - r_framelerp);
    out[1] = cur->v[1] * r_framelerp + prev->v[1] * (1.0f - r_framelerp);
    out[2] = cur->v[2] * r_framelerp + prev->v[2] * (1.0f - r_framelerp);
}
```

Fast-path: when `r_framelerp == 1.0f` the transform helpers early-out and read `pverts->v` directly, so disabled-lerp performance matches the pristine engine.

**`R_AliasTransformFinalVert`** (`r_alias.c:415`, clipped path) — takes an additional `trivertx_t *pverts_prev` argument; substitutes the lerped `v` for `pverts->v` in the three `DotProduct` calls.

**`R_AliasTransformAndProjectFinalVerts`** (`r_alias.c:459`, unclipped path) — uses both `pverts` and `pverts_prev`, advancing both per iteration.

**`R_AliasPreparePoints`** (`r_alias.c:268`) — the per-vertex loop also advances `pverts_prev` alongside `pverts`. Callers of `R_AliasTransformFinalVert` pass it in.

**Lighting normals** are **not** lerped. We use the current frame's `lightnormalindex`. The 162-entry quantised normal table plus already-integer light grades make the visible pop trivial, and lerping would require interpolating across the normal table — overkill.

### 5. Bounding box

`R_AliasCheckBBox` (`r_alias.c:89`) reads `pframedesc->bboxmin/max` for a single frame. With lerp, a vertex of the previous frame could briefly extend outside the current frame's bbox and pop-clip on the screen edge. Fix: when `prev_frame != frame`, build `basepts[]` from the per-component min/max of both frames. Six `min`/`max` ops, runs once per draw — negligible.

### 6. Performance

Software Quake is rasterisation-bound at 320×200. The added cost is 6 mul-add ops per vertex per frame. A 500-vertex ogre × 30 entities at 70 fps ≈ 7M ops/sec — well under 1% of the rasteriser's existing budget. No special optimisation needed beyond the `r_framelerp == 1.0f` early-out.

## Cvar / control summary

| Cvar | Default | Behaviour |
|---|---|---|
| `r_lerpmodels` | `1` | Vertex lerp on/off (archived to config). |
| `cl_nolerp` | `0` | Existing; if non-zero, also disables vertex lerp. |

`U_NOLERP` / `ent->forcelink` always skips one frame of lerp per-entity (respawn, teleport).

## Test plan

Quick visual smoke test on `e1m1`:

1. `zig build run -- +map e1m1` with `r_lerpmodels 1`. Walk through to find a knight/zombie; confirm walk/attack animations are smooth, no slide on death.
2. Set `r_lerpmodels 0`, confirm classic snap returns.
3. Find a torch (group-frame anim) — flame should flow continuously, not stutter.
4. Save/load mid-fight: monsters should resume animating without "slide-from-zero" pop (forcelink path).
5. `noclip` to test viewmodel: fire weapons rapidly; gun anim should still feel responsive (re-evaluate if it feels mushy).

No automated tests exist for the engine; visual confirmation is the bar per `CLAUDE.md`.

## Open future work (out of scope here)

- `r_lerpmodels 2` for "everyone but viewmodel" if the gun anim feels too soft.
- Per-model lerp override (table or QC field) if a specific monster needs a non-100ms transition.
- GL renderer parity (no GL renderer exists in this project yet).

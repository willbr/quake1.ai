# Skeletal Actors R2 — IQM Animation Playback — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** Play a baked IQM animation clip on the actor — the head turns left/right while the body stays put — proving per-joint keyframe evaluation + hierarchy compose + rigid skinning.

**Architecture:** Extend the bootstrap generator to bake a simple "look" clip (head joint yaws ±30°). Extend `lm_load_iqm` to parse poses/anims/frames and **decode** per-frame per-joint local TRS at load. In the renderer, precompute each joint's **bind-pose world-inverse** once; per frame, evaluate the clip → per-joint local → world matrices, form `skin = world · bindInv` per joint, and transform each vertex by `skin[vert.bone]` before the existing view transform. At bind pose `skin == identity`, so R1 stays a special case. A client clock drives the dev actor's clip time.

**Tech Stack:** C (gnu89 engine, modern-C libmodel), Python generator, Quake software renderer.

**Verification:** build + in-game (the head visibly turns); `actor_dump` extended to print clip count/frames.

---

## Key formulas (IQM v2)

- **Pose decode**, per frame, per pose (joint), channels in order `0,1,2=Tx,Ty,Tz · 3,4,5,6=Qx,Qy,Qz,Qw · 7,8,9=Sx,Sy,Sz`:
  `value[c] = channeloffset[c] + (mask&(1<<c) ? (*framedata++) * channelscale[c] : 0)`.
  Channels with the mask bit **unset** take `channeloffset[c]` as a constant (so the bind/base TRS is stored there).
- **Joint local matrix** = T(translate) · R(quat xyzw, normalized) · S(scale), as a 3×4.
- **World** = parent.world · local (root: world = local). `R_ConcatTransforms` for 3×4·3×4.
- **Bind world** = same compose over the bind-pose joint TRS (already in `lm_iqm_joint_t`).
- **Skin matrix** = world · inverse(bindWorld). Rigid vertex = skin[bone] · pos.
- Quaternion xyzw → 3×4 rotation (column-vector convention matching `R_ConcatTransforms`): standard q→matrix; renormalize q first.

---

## Task 1: Bake a "look" clip into the generator

**Files:** Modify `scripts/make_test_actor_iqm.py`; regenerate `id1/actors/dummy.iqm`.

- [ ] **Step 1:** Add animation arrays. After the joints, emit:
  - **poses** (one per joint, parallel to JOINTS): each `iqmpose` = `parent(int) + mask(uint) + channeloffset[10] + channelscale[10]` (88 bytes). For every joint set `channeloffset = [tx,ty,tz, 0,0,0,1, 1,1,1]` (bind translate, identity quat, unit scale) and `channelscale = 0`. For the **head** joint (index 2), set `mask = (1<<3)|(1<<4)|(1<<5)|(1<<6)` (rotate xyzw) and pick `channeloffset`/`channelscale` per rotate channel to span the keyframed quaternion range (e.g. offset = min, scale = (max-min)/65535 per channel; if a channel is constant set scale 0 and bake it via offset with mask bit cleared).
  - **anims** (1): `iqmanim` = `name + first_frame(0) + num_frames(N) + framerate(10.0) + flags(IQM_LOOP=1)` (20 bytes).
  - **frames**: for each of N frames, for each pose, for each set mask bit (channel order), emit one `uint16 = round((value - channeloffset[c]) / channelscale[c])` (clamp 0..65535). `num_framechannels` = total set bits across all poses (here 4, head only).
  - Head per-frame quaternion: yaw angle = `30° * sin(2π * f / N)`, quat about +Z = `(0,0,sin(a/2),cos(a/2))`.
- [ ] **Step 2:** Update header: set `num_poses/ofs_poses`, `num_anims/ofs_anims`, `num_frames`, `num_framechannels`, `ofs_frames`; lay these lumps out (poses 88B, anims 20B, frames `num_frames*num_framechannels*2` B) with the existing offset/align bookkeeping. Keep the `len(out)==filesize` assert.
- [ ] **Step 3:** Regenerate; `python3 scripts/make_test_actor_iqm.py` should report frames>0. Commit script + asset.

---

## Task 2: Parse + decode animation in the reader

**Files:** Modify `sdlquake/libmodel/iqm.h`, `iqm.c`.

- [ ] **Step 1 (iqm.h):** Add to `lm_iqm_t`:
  ```c
  int    numframes;                 /* 0 = no animation (R1 asset) */
  float  framerate;
  /* decoded local pose per (frame,joint): translate[3], rotate[4], scale[3] */
  float *frametrs;                  /* numframes*numjoints*10 floats, or NULL */
  ```
- [ ] **Step 2 (iqm.c):** Read header `num_poses`(0x4C)/`ofs_poses`(0x50), `num_anims`(0x54)/`ofs_anims`(0x58), `num_frames`(0x5C)/`num_framechannels`(0x60)/`ofs_frames`(0x64). If `num_frames==0 || num_poses!=num_joints`, leave `frametrs=NULL` (static — R1 path). Else allocate `frametrs = QALLOC_ARR(&a, float, numframes*numjoints*10)` and decode: walk the `uint16` frame stream once, for each frame/pose/channel applying the pose-decode formula into `frametrs[(f*numjoints+p)*10 + c]`. Bounds-check `ofs_frames` for `num_frames*num_framechannels*2` bytes. Set `numframes`, `framerate` (from anim 0).
- [ ] **Step 3:** Extend `Actor_Dump_f` (iqm_dev.c) to print `numframes`/`framerate`. Build; `actor_dump actors/dummy.iqm` shows frames>0. Commit.

---

## Task 3: Pose evaluation + rigid skinning in the renderer

**Files:** Modify `sdlquake/engine_src/r_alias.c` (R_IQMDrawModel). Add small static helpers (quat→3×4, 3×4 invert, 3×4 compose via `R_ConcatTransforms`).

- [ ] **Step 1:** Add `static void IQM_QuatMat(const float q[4], float m[3][4])` (normalize, fill rotation, zero translation) and `static void IQM_LocalMat(const float trs[10], float m[3][4])` (quat→rot, scale columns, set translation). Add `static void IQM_Invert34(const float in[3][4], float out[3][4])` (transpose rotation, `out_t = -R^T·t` — valid for rotation+uniform-scale; for R2's rigid+rotation it is exact).
- [ ] **Step 2:** In `R_IQMDrawModel`, after fetching `iqm`, if `iqm->frametrs`:
  - Compute `bindworld[joint]` (compose bind TRS down the hierarchy) and `bindinv[joint] = invert(bindworld)` — cache per draw (small: ≤ a few dozen joints; arrays sized `[MAX_IQM_JOINTS][3][4]`, define `MAX_IQM_JOINTS 128`, skip if exceeded).
  - Pick frame: `f = (int)(cl.time * iqm->framerate) % iqm->numframes` (looping; snap for R2, lerp later).
  - Compute `world[joint]` from `frametrs[(f*numjoints+joint)*10]` composed down the hierarchy.
  - `skin[joint] = world · bindinv` (R_ConcatTransforms).
  - Per vertex: `posed = skin[vert.bone] · pos` (3×4 · point), then feed `posed` into the existing view transform (replace `p` with `posed` in the DotProduct lines).
  - If `frametrs==NULL` keep the R1 path (use `pos` directly).
- [ ] **Step 3:** Build. In-game `actor_spawn actors/dummy.iqm`: the **head turns left/right** while body/base stay still; eyes (children of head) turn with it. Screenshot two frames to confirm motion. Commit.

---

## Task 4: Smoke + docs

- [ ] **Step 1:** Confirm static R1 actors (no anim) still render (frametrs NULL path).
- [ ] **Step 2:** Update the CLAUDE.md skeletal-actors note: R2 done (clip decode + rigid skinning + looping playback).
- [ ] **Step 3:** Commit.

---

## Notes / risks
- **Joint count** small here (5); the `[128][3][4]` caches are ~7.5 KB each on stack — fine.
- **Quaternion convention** must match `R_ConcatTransforms` (column-vector, row-major 3×4). Verify by eye: a +Z yaw should turn the head about vertical; if it tilts wrong, transpose the rotation fill.
- **Invert34** assumes rotation (+ uniform scale). R2 uses unit scale, so exact. Breathing (non-unit scale) is R3; revisit invert then (or store inverse-bind via full decompose).
- **Frame lerp** deferred (snap per frame at 10 Hz reads fine; interpolation is a later polish, mirroring the alias `r_framelerp`).

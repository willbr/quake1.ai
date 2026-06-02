# Skeletal Actors E2 — In-Engine Animation Timeline — Design

**Date:** 2026-06-02
**Status:** Design approved (decisions below). Build MVP next.
**Context:** Final editor sub-project. The cube-first editor already authors an
actor's full static structure (part CRUD + skeleton CRUD + bind) and saves with
animation preserved (`lm_write_iqm` re-encodes `frametrs`). E2 adds **authoring
new motion** — setting joint keyframes over time.

## Locked decisions (brainstorm)

| Decision | Choice |
|---|---|
| Keyframe model | **Sparse keys + interpolation** — author a few key frames per joint; in-between frames interpolate (slerp rotation, lerp translation). Baked into the per-frame `frametrs` (which is what renders/saves). |
| Channels | **Rotation + translation** per joint (scale stays at bind; breathing/look-at remain procedural). |
| Posing method | **Numeric fields** (euler rotation + translation), consistent with the geometry editor. Gizmo posing is a later layer. |

## Model

IQM stores a pose **every frame** (`frametrs[(frame*numjoints+joint)*10 + c]`,
c = 0..2 T, 3..6 Q, 7..9 S). E2 keeps the per-frame array as the bake target and
layers a **sparse key set** on top, in editor state (not in the IQM):

- `s_key[joint]` = sorted list of key frame indices (absolute into `frametrs`),
  bounded (`E2_MAXKEYS`). The **pose at a key** lives directly in
  `frametrs[keyframe][joint]` (T + Q).
- **Set key (joint J, frame F):** add F to `s_key[J]`; write the current numeric
  pose (euler→quat for Q, translation for T) into `frametrs[F][J]`.
- **Delete key:** remove F from `s_key[J]`.
- **Bake (per edited clip):** for each joint J with ≥1 key, fill every frame of
  the clip's range: before the first key / after the last → clamp to that key;
  between keys A,B → `slerp(QA,QB,u)` + `lerp(TA,TB,u)`, `u=(f-A)/(B-A)`. Joints
  with no keys keep their existing `frametrs` (bind/loaded). Bake runs whenever a
  key or the current pose changes, so the preview reflects edits live.
- The edit copy already carries `frametrs` (the writer preserves animation), and
  the preview plays `actor_clip` = the edited clip. **Save** writes the baked
  `frametrs` via the existing `lm_write_iqm` (no new save path).

Keys are **per editor session** (derived nowhere from the file on load for the
MVP): a freshly loaded clip shows no keys until the user sets them; baking only
touches joints the user keyed, so loaded motion is preserved until overwritten.

## UI (in the existing "Edit geometry" mode, a new "Animation" section)

- **Clip:** select the clip to edit (reuse the clip list); shows frame count /
  framerate / loop.
- **Frame:** a scrubber `0 .. clip.num_frames-1` (current frame within the clip);
  the preview is paused on the current frame while editing.
- **Joint + pose:** select a joint (reuse the joint selector); numeric euler
  **rotation** + **translation** fields show/edit that joint's pose at the
  current frame. Editing writes the pose and (if the frame is a key) updates it.
- **Set key / Delete key** at the current frame for the selected joint; the key
  frames are listed/marked.
- Live: bake-on-change so the orbit preview plays the edited motion.

## Console twins (scriptable / headless test)

`actor_anim_clip <n>`, `actor_anim_frame <f>`, `actor_anim_key <joint> <pitch>
<yaw> <roll> <tx> <ty> <tz>` (set a key at the current frame), `actor_anim_unkey
<joint>`, `actor_anim_bake`. These let the round-trip/headless tests drive E2.

## MVP scope / deferred

- **MVP:** edit the keyframes of an **existing** clip (set/delete keys, bake,
  preview, save). Rotation+translation, numeric, sparse+interpolated.
- **Deferred:** adding **new** clips (grow `frametrs` by N frames — array growth
  like `lm_iqm_add_joint`), copy/paste keys, easing curves (linear only for MVP),
  gizmo posing, onion-skinning.

## Verification

- Headless console: set 3 keys on the head over a clip, bake, save, `actor_dump`
  + `actor_roundtrip` (frames/clips intact, anim re-encodes). Spawn the saved
  actor and confirm it plays the authored motion.
- Visual: scrub + pose in the editor; the orbit preview plays the new motion.

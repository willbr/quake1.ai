# Expressive Skeletal Actors (TR1-style, IQM) — Design

**Date:** 2026-06-01
**Status:** Design approved. R1 to be planned next.
**Scope:** Umbrella design for the whole feature + detailed design for the first
sub-project (**R1**). Each later sub-project gets its own plan as we reach it
(same pattern as the immersive-sim and fire-and-oil designs).

## Why

Characters are currently single-model, vertex-morph actors that can't *look at*
anything, *talk*, or show secondary motion. We want expressive characters
serving **two** use cases from one capability:

1. **Liven up combat monsters** — track the player, turn heads, eyes follow;
   read as alive and menacing.
2. **Talking NPCs** — speak (lip-synced) and look at you during dialogue.

Aesthetic north star: **"Thunderbirds" / Supermarionation** — fairly stiff
bodies whose life comes from **swivelling eyes** and a **flapping mouth**. That
suits a low-poly, 8-bit software renderer; it embraces rigid parts and small
joint gaps rather than fighting them. We **start with cube parts** — boxy
puppets — and grow primitive variety later.

## What we're building (one sentence)

A **TR1-style rigid skeletal animation system** (runtime) **plus an in-engine
authoring tool** (no external DCC), using **IQM** as the compiled
mesh+skeleton+animation format and a small companion `.actor` KV manifest for
game semantics, with runtime layers for **procedural expression** (look-at,
gaze, jaw-flap lipsync, breathing) and **self-animating secondary motion**
(ponytails), networked so the game.dll brain drives it.

## Locked decisions (brainstorm outcomes)

| Decision | Choice | Why |
|---|---|---|
| Purpose | One capability for combat liveliness **and** talking NPCs | Two payoffs, one system |
| First actor | A **throwaway test actor** in a test map | Prove plumbing before retrofitting real content (cf. `ai_t*`/`m7`) |
| Lipsync | **Amplitude jaw-flap** | Any voice line works from loudness; no phoneme authoring |
| Mouth | **Skin/texture swap** on the face (closed/mid/open) | Cheapest convincing mouth at this fidelity |
| Eyes | **Moving geometry** (swivelling eye joints) | The Supermarionation tell; "think Thunderbirds" |
| Animation model | **Full TR1-style rigid skeletal** (joint hierarchy + per-joint rotation keyframes + root motion) | Head/eyes/jaw become first-class joints; no morph-tag hacks |
| On-disk format | **IQM** (compiled artifact) | Documented, compact, public-domain reference loader; supports TRS keyframes incl. scale; interop/future-import stays open |
| Skinning | **Rigid first** (1 bone/vertex) | TR1/PS1 look; smallest vertex path; upgradable later (IQM keeps weights) |
| Scale / breathing | Joints carry **scale**; breathing is a **procedural** chest-scale oscillator | Native in IQM; procedural needs no authoring |
| Secondary motion | **Self-animating dynamic joints** (ponytail) via client-side spring solver | "Animate themselves"; cosmetic/derived → no networking |
| Wind | **Optional**: dynamic joints sample the existing `sim_wind` grid | Ponytails blow in Gust/wind — a freebie unique to this engine |
| Networking | **Extend protocol** for pose intent (clip+time, head/eye/jaw, talk, expression); root rides origin/angles | game.dll is the brain; works in demos/MP. Dynamics + lipsync amplitude stay client-derived |
| Coexistence | Additive **dual system** — new `mod_iqm` alongside untouched `mod_alias` | Stock content keeps working |
| **Authoring** | **In-engine editor; no Blender; cubes-first** | The editor is the pipeline. Rigid+boxes removes the hard DCC problems (no skinning/UV/sculpt) |
| Editor ↔ format | Editor **round-trips IQM ↔ editable scene** (+ `.actor`) | Mirrors the map editor → `.bsp` pattern: scene is the source, IQM is the artifact |

## Why "full in-engine tool" is actually feasible

The things that make a general in-engine modeler impractical — vertex sculpting,
skinning/weight-painting, UV unwrapping — **largely disappear because parts are
rigid boxes on single joints**: no weights, no unwrap, and "mesh editing" begins
as *place / size / parent boxes*. So "every model is a cube" is the natural MVP
of a rigid-skeletal authoring tool, with an honest growth path:
**cubes → more primitives (cylinder/wedge) → per-face textures → (later) vertex nudging.**

## Reference: TR1 + IQM

**TR1 actors** are ~15 separate **rigid** mesh pieces in a joint hierarchy (a
"MeshTree" with a push/pop stack for branching limbs). Animation is not vertex
morphing — each frame is a **root translation + one rotation per joint**,
composed down the hierarchy; meshes stay rigid (hence small PS1 joint gaps).
That is exactly our model.

**IQM** ("Inter-Quake Model", Lee Salzman, public domain) is a compact binary,
lump/offset based (BSP-like): string table, vertex arrays (position/normal/
texcoord + blend indexes/weights), triangles, **meshes**, **joints** (bind-pose
TRS, quaternions), and **poses/anims/frames** (quantized keyframe channels for
translate + rotate + scale per joint) plus per-frame bounds. A text twin
(**IQE**) exists for inspection/bootstrap. Our reader/writer is based on
Salzman's **public-domain** reference; the vendored **FTEQW** (GPL) is read-only
reference.

## Architecture

### An actor is two files

```
actors/dummy.iqm     # geometry + skeleton + clips (authored in our editor)
actors/dummy.actor   # small KV manifest: game semantics IQM can't carry
```

The `.iqm` is what an entity references via `SV_SetModel`. The engine loads it as
`mod_iqm` and, if a sibling `.actor` exists, attaches the parsed semantics.

### `.actor` manifest (KV, lineage of `.pcl`/`.map`)

```
actor {
    head_joint   "head"          // look-at target joint
    eye_joints   "eye.L eye.R"   // swivel joints for gaze
    jaw_joint    "jaw"           // optional jaw-open joint
    mouth_skins  "0 1 2"         // face material indices: closed / mid / open
    chest_joint  "chest"         // breathing scale target
    gaze_yaw 50  gaze_pitch 30   // head clamps (deg)
    eye_yaw  25  eye_pitch  20   // eye clamps (deg)
    breathe_rate 0.25  breathe_amp 0.04
}
dynamic "ponytail_01" { stiffness 0.30  damping 0.60  gravity 1  wind 1 }
```

All joint references are **by name** (IQM has a joint-name string table). Absent
roles disable that feature for that actor; unresolved names warn, never crash.

### Runtime pipeline (per actor, per frame)

```
1. base pose   ← evaluate IQM clip at (clip, time): per-joint local TRS         [R2]
2. procedural  ← override/blend joints: head→look-at, eyes→gaze, jaw→open,
                 chest→breathe-scale                                            [R3]
3. dynamics    ← spring solve for `dynamic` joints, reacting to step-2 motion
                 + gravity (+ wind)                                             [R4]
4. compose     ← walk hierarchy, accumulate parent → world matrix per joint
5. render      ← per mesh: transform its (rigid, 1-bone) verts by its joint
                 matrix, feed the low-level triangle rasterizer                 [R1]
```

Steps 2–4 operate on the **pose** and are agnostic to its source — which is why
adopting IQM does not touch R3/R4/R5.

### The editor is the pipeline (no Blender)

Mirrors the **map editor → `.bsp`** pattern: an editable **in-memory actor
scene** (box parts + joint hierarchy + clips) is the source of truth; the editor
**round-trips IQM ↔ scene** and writes the `.actor` manifest. It plugs into the
existing editor shell via the `editor_mode_t` vtable / `s_modes[]` registration,
**reusing the particle editor's orbit camera, play/pause clock, hide-world,
reference grid**, and `edit_history` undo/redo. Crucially, it edits *our* data
and previews the *live runtime* (look-at/gaze/jaw/breathing/ponytail) — things
no external DCC could preview.

## Decomposition — two interleaved tracks + content

Build order is dependency-driven and interleaved so each editor phase can
preview the runtime feature it authors. **R-tracks R1–R4 are pure local
runtime** (dev-spawn, no protocol); **R4 and R5 are independent.** The test
actor is the vehicle from R1 on.

| Track | # | Sub-project | Deliverable |
|---|---|---|---|
| Runtime | **R1** | IQM load + static render | Public-domain IQM reader in `libmodel`; `mod_iqm` type; `.actor` parse; **rigid** vertex path → existing low-level rasterizer; render **bind pose**; dev-spawn cmd. → *A many-part rigid actor stands assembled on screen.* |
| Editor | **E1** | Actor editor + box scene + IQM save/load | New editor mode; create/size/parent **box parts** into a joint hierarchy; **IQM writer**; round-trip IQM↔scene; undo/redo. → *Author a static cube actor in-engine and see it render.* |
| Runtime | **R2** | Clip playback | Evaluate IQM anim at (clip,time): per-joint TRS interpolation + compose. → *Actor plays a baked clip.* |
| Editor | **E2** | Keyframe animation | Timeline; pose joints; set/edit keyframes; scrub via R2; save clips into IQM. → *Animate the cube actor in-engine.* |
| Runtime | **R3** | Procedural + face + breathing | Override joints by role: head look-at, eye gaze, **jaw-open from voice amplitude** + mouth skin-swap, **breathing scale**. |
| Editor | **E3** | `.actor` semantics + live tuning | Assign roles (click joints), set mouth-skins/gaze limits, flag ponytail dynamics + tune stiffness/damping/wind; live preview. |
| Runtime | **R4** | Secondary dynamics | Client-side spring solver for `dynamic` joints; reacts to motion + gravity; optional `sim_wind` tie-in. No protocol. |
| Runtime | **R5** | Protocol / pose sync | Extend entity state (clip+time, head/eye/jaw, talk, expression); version bump; game.dll sets it; engine renders from networked state. |
| Content | **C1** | Driver + test actor + verification | game.dll anim state machine + look/gaze/talk behaviours; test actor (authored in the editor) + test map; MCP/console hooks. |

**Recommended order:** `R1 → E1 → R2 → E2 → R3 → E3 → R4 → R5 → C1`.

## R1 detailed (the first build)

**Goal:** an entity whose model is an `.iqm` renders as a correctly-assembled
set of rigid parts in bind pose. No animation, procedural, or networking yet.

### Components
- **IQM reader** (`libmodel`): parse header + lumps into `lm_iqm_t`: meshes
  (vertex ranges + material/skin name), triangles, joints (name, parent,
  bind-pose TRS), and a handle to anim data (parsed, unused until R2). Validate
  magic (`INTERQUAKEMODEL`) + version (2); reject gracefully.
- **`mod_iqm` model type**: extend `model.c` `Mod_LoadModel` dispatch + a new
  `modtype_t` in `model.h`; load into an engine header holding the joint tree +
  meshes (analogous to `aliashdr_t`).
- **`.actor` parse**: if `actors/<name>.actor` exists, KV-parse it (like `.pcl`)
  and resolve role joint **names → indices**. Missing = plain model; unresolved
  name = warning.
- **Rigid vertex path**: precompute bind-pose joint world matrices (hierarchy
  compose of bind TRS). Per mesh, transform vertices by the **single** joint of
  each vertex's first blend index. Feed to the **existing low-level affine
  triangle rasterizer** (reuse the fill, not `trivertx`-based
  `R_AliasSetupFrame`).
- **Render hook**: `case mod_iqm:` in the entity draw switch (`r_main.c`,
  beside `mod_alias`); walk meshes, draw each part. Lighting: reuse the entity's
  single light sample for all parts (cheap).
- **Dev-spawn**: console cmd (e.g. `actor_spawn <file> [x y z]`) creating a
  client-side entity with the IQM model, so R1–R4 run without the server.

### Bootstrap asset (no Blender)
Ship a tiny cube test actor via **IQE text** (hand-written: base→chest→head +
two eye boxes) converted with the reference `iqm` tool, **or** a small C/Zig
generator that emits the IQM directly. The generator doubles as the seed of the
E1 **IQM writer** and lets the R1 reader be tested against known-good output.

### Done when
A dev-spawned `.iqm` renders as a correctly-assembled rigid-part actor in bind
pose, right scale/orientation, `.actor` roles resolved (verify via debug print).
Stock `mod_alias` monsters unaffected. Verified **in-game**, not just compiled.

## Cross-cutting concerns

- **Performance.** Rigid = one transform per part; negligible at Quake poly
  counts — renderer stays fill-bound as today. Dynamics = a few point masses;
  procedural = a few overrides.
- **Networking (R5).** game.dll authoritative for pose intent (clip+time,
  head/eye/jaw, talk, expression), angles quantized to bytes. Client-derived,
  never networked: dynamics (cosmetic) and lipsync amplitude (client has the
  voice sample).
- **Licensing.** Reader/writer based on the **public-domain IQM reference**;
  FTEQW (GPL) read-only reference.
- **Error handling.** Bad/old IQM → clean `Mod_LoadModel` failure, never a
  crash. Unresolved `.actor` joint names → warn + skip role. Missing `.actor` →
  loads/animates, just inexpressive.
- **Aesthetic boundary.** Rigid parts show small joint gaps under rotation —
  intended (Thunderbirds/PS1). Smooth skinning is a deferred, no-format-change
  upgrade.
- **Editor reuse.** E-track builds on the existing editor shell (orbit cam,
  play/pause, hide-world, grid, undo/redo, mode registration) — substantial code
  reuse from `edit_particle.c` / the shell.

## Testing / verification

- **R1–R4 / E-track (local):** dev-spawn or editor-preview; verify visually +
  debug prints (joint resolution, pose). ImGui/debug overlays via MCP
  `screenshot_gpu`.
- **R5+ (networked):** spawn via game.dll into a test map; drive head/eye/talk
  via MCP/console; headless asserts via cvars (peer of the fire system's
  MCP-readable `fire_*_count`).
- Live smoke test before any "done" claim — watch the actor, don't just build.
- A showcase test map (m7-style) lands in C1.

## Deferred / non-goals (YAGNI)

- **Blender / external DCC import** — authoring is in-engine. (IQM keeps the
  door open to *optional* import later, but it's not a goal.)
- **Advanced mesh authoring** — start with **box primitives**; cylinders/wedges,
  per-face texturing, and vertex-level editing are later growth, not MVP.
- **Full vertex skinning** — rigid first; upgrade later (IQM keeps weights).
- **Real visemes / phoneme lipsync** — amplitude jaw-flap only.
- **Non-uniform / hierarchy-propagating scale** — breathing uses uniform,
  non-propagating mesh scale; revisit only if needed.
- **Morph faces, blink, per-eye convergence, expression skin-matrix** — stretch.
- **Retrofitting stock monsters** — the test actor proves the system first.
- **Multiplayer-correct dynamics** — cosmetic, client-side, may differ between
  clients; acceptable.

## Open questions (decide at the relevant sub-project)

- **R3:** amplitude→mouth-level mapping (discrete levels + smoothing).
- **R4:** spring form (per-joint angular spring vs Verlet point-chain); whether
  wind ships in R4 or as a follow-up.
- **E2:** keyframe interpolation UI + clip event model.
- **E1:** whether box parts round-trip from IQM losslessly or need an editor
  sidecar to preserve parametric intent (start: treat any mesh as editable
  geometry; add sidecar only if needed).

# FTEQW ideas worth stealing

Salvage notes from reviewing FTEQW (`ref/fteqw-master/`, a modern QuakeWorld
engine) against this fork, mirroring the other `docs/*-ideas-to-steal.md` docs.
FTE is huge and mostly GL/Vulkan/D3D — irrelevant to a software-only engine —
so this filters hard for our constraints (100% software render, no new C++,
single-player) and our existing systems (data-driven particle editor, IQM
skeletal actors, RGB lightmaps, decals, threaded fill).

Reviewed 2026-06-07. File refs are under `ref/fteqw-master/engine/`; line numbers
orient, not pin. **Headline:** the prize is FTE's scriptable particle grammar —
not new *capabilities* (we have most of the underlying tech) but the unified
*authorable* format that exposes it. Skeletal animation **blending + events**
are the next tier. The software renderer has essentially nothing to steal.

---

## What we already have (so the gaps stay honest)

- **Data-driven particle effects** — `r_emitter.c` registry + `.pcl` editor
  format with color `ramp`, size envelope (`size_start/peak/end`), `gravity`/
  `drag`, `shape`/`dir_mode`/`cone_angle`/`spread`, `style` (dot/blob/smoke).
- **Bouncing particles** — but only in the *hardcoded* engine pool
  (`r_part.c` `PARTFL_BOUNCE`, `r_sparks_restitution`, `pt_spark`), NOT
  expressible from a `.pcl`.
- **Surface decals** (`r_decals.c`) and **lightning beams** (`cl_tent.c`
  `cl_beams`/`CL_ParseBeam`) — again as bespoke engine paths, not as particle
  outputs.
- **IQM skeletal actors** — multi-clip (networked `frame`), procedural look-at /
  eye-gaze / breathing, Verlet ponytail (`iqm_dynamics.c`), full in-engine Actor
  editor.
- **Software renderer** — threaded disjoint-span fill + SIMD; RGB lightmaps; fog;
  stains.

The recurring theme: we have the *tech* but it's scattered across hardcoded
paths. FTE's value is **one declarative grammar** that composes it all.

---

## Top pick — particle effect-script grammar  ★★★

FTE's particle config (`client/r_part.c`, presets in `engine/partcfgs/*.cfg`:
`faithful.cfg`, `high.cfg`, `q2part.cfg`, `spikeset.cfg`) is the gold-standard
declarative effect format. Each effect is an `r_part NAME { ... }` block. The
features below are what our `.pcl` grammar *can't express today* — porting the
grammar is the steal, and much of the runtime already exists to back it.

Ranked within the pick:

1. **Effect chaining / sub-effects** — `assoc NAME` spawns another effect when a
   particle dies; `emit NAME` + `emitinterval T` continuously spawns a child
   effect along a particle's path (`r_part.c`, grammar in `spikeset.cfg`:
   `empcore { emit empshocktrail; emitinterval -1 }`). This is FTE's killer
   feature and our single biggest gap — it turns flat effects into hierarchies
   (rocket → trail → smoke-puff-on-death).
2. **World-bounce → sub-effect** — `cliptype NAME` / `bounce V` / `clipcount N`:
   on hitting world geometry, bounce (restitution `V`) and/or spawn effect
   `NAME`. We already have `PARTFL_BOUNCE` in the engine pool — this just needs
   exposing in the emitter + the "spawn on impact" hook.
3. **Spawn modes (shapes)** — `spawnmode ball|circle|spiral|tracer|telebox|
   lavasplash|distball [p1 p2]` (`r_part.c` spawn-distribution switch). We have
   `shape`/`cone`; `spiral`, `lavasplash`, `telebox` (Q1 teleport cone) are
   cheap, high-variety adds.
4. **Per-particle dynamic light** — `lightradius`, `lightrgb`, `lighttime`,
   `lightradiusfade` (`r_part.c` dlight emission). We have colored dlights; this
   wires them to particles (muzzle flash, glowing trail, explosion flash).
5. **Decals / stains on impact** — `type cdecal|udecal`, `stains R`: project a
   decal where a particle hits. We have `r_decals.c` surfaces — this is the
   "blood/scorch mark from a particle" wiring (bullet holes, blood splatter).
6. **Beams as a particle type** — `type beam`/`vbeam` renders a particle stream
   as continuous line geometry (`spikeset.cfg`: `railtrail240 { type beam;
   spawnmode spiral 256 }`). We have lightning beams as a TE; generalizing to an
   emitter output gives railguns/spell-beams for free.
7. **rampmode keyframes** — `rampmode delta|nearest|lerp` + repeated `ramp r g b
   [a [scale]]` lines build a multi-key color/alpha/size curve over life (richer
   than our single start→end `ramp`).
8. **Rotation** — `rotationstart min max`, `rotationspeed min max` for spinning
   debris/gibs; `orientation none|normal|direction`.
9. **`tcoords` atlas animation** — `tcoords s1 t1 s2 t2 stride [rand]` picks/
   animates a sub-rect of a texture atlas (sprite-sheet particles).
10. **`+NAME` layering & `r_trail MODEL NAME`** — `r_part +NAME { ... }` stacks a
    layer onto an existing effect (multi-pass composition); `r_trail
    "model.mdl" NAME` binds a trail to any moving model at runtime.

**Adaptation:** extend the `.pcl` KV-block grammar (and the Particle editor UI)
with these keys, backed by the runtime we already have (bounce, decals, dlights,
beams). The two non-trivial new runtime bits are the **effect registry
reference** (`assoc`/`emit` need name→def links + spawn-on-event hooks) and
**spawn-on-impact**. Everything else is mostly UI + plumbing onto existing draws.

---

## High value — skeletal animation

Our IQM actors do procedural secondary motion well but treat `frame` as a single
clip selector. FTE's gaps:

### 2. Weighted animation blending / cross-fade  ★★★

`common/com_mesh.c::Alias_BlendBoneData` + the `framestate_t` with up to
`FRAME_BLENDS` simultaneous clips, each with its own frame index, time, and
lerp weight; QC-facing struct `skelblend_t` in `client/pr_skelobj.c::skel_build`
(per-clip `scale[4]`/`animation[4]`, `firstbone`/`lastbone` for per-bone masks,
`prescale` to retain the prior pose for additive layers). We can only hard-cut
between clips today. Blend + cross-fade is the difference between snappy and
lifelike (walk→run, idle→aim). The per-bone range (`firstbone`/`lastbone`) gives
"aim upper body while legs run" without a separate skeleton.

### 3. Per-bone procedural override API  ★★

`pr_skelobj.c::skel_set_bone` / `skel_premul_bone` / `skel_postmul_bone` — set or
multiply a single bone's matrix *after* the animation pass. FTE does look-at /
gun-aim / recoil this way (a clean two-phase "animate, then override") rather
than our baked-in `iqm_dynamics.c` procedural code. Generalizing our look-at into
a per-bone override pass would let game code drive aim/IK/recoil per bone without
new C each time.

### 4. Animation events (frame-triggered callbacks)  ★★

`common/com_mesh.c::Mod_GetModelEvent` over a `galiasevent_t` list per clip
(`{timestamp, code, data}`). Fires footstep/sound/particle/hitbox callbacks at
authored times in a clip — exactly the hook our footstep/stimulus systems want,
and authorable in the Actor editor's animation timeline. We have none today.

### 5. Ragdoll  ★ (low fit)

`pr_skelobj.c::rag_createdoll`/`rag_instanciate` loads a `.doll` (bodies +
joint constraints) and drives bones from **ODE** physics. Real, but it pulls in a
physics-engine dependency we don't have and lean toward avoiding (C-only, no new
heavy deps). Note it exists; don't port unless we commit to rigid-body physics.

---

## Software renderer — honest negative

FTE's `sw/` (`sw_rast.c`, `sw_backend.c`, `sw_spans.h`) is **not** a perf
reference for us. It's a 32-bit RGBA compatibility fallback that *emulates* their
GL/Vulkan shader backend on the CPU: no 8-bit palette, no colored lightmaps, no
decals/fog/dlights (`SWBE_SelectDLight` returns false), no SIMD, no mips. We've
effectively already taken its one good idea:

- **Scanline-interleaved threading** (`sw_rast.c:346` `((y + interlaceline) %
  interlacemod)`, ring-buffer command queue) — the same disjoint-work model as
  our threaded fill. Only actionable item: sanity-check our interlace stride
  against theirs to rule out a per-thread sync edge case. Not a new feature.

Everything else (affine fixed-point texrast, Sutherland-Hodgman tri-clip, binary
alpha-test) we either already do better or don't need. **Nothing to steal here.**

---

## Not worth stealing

- **FTE's GL/Vulkan/D3D renderers, shader system, deluxe/normalmaps, HDR
  tonemap** — violate "keep the software-renderer look / GPU stays present-only."
- **Multi-format skeletal abstraction** (`com_mesh.c` MD5/IQM/PSK/DPM/glTF via
  one `galiasinfo_t`) — we're IQM-only by choice; the dispatch layer is dead
  weight unless we add formats.
- **CSQC / QC VM extensions / builtins** — we removed the VM (NATIVE_GAME); the
  game logic is hand-ported C with its own ABI.
- **Networking / delta protocol / fteqtv** — single-player; same call as the Q2
  netcode (see `docs/quake2-ideas-to-steal.md`).

---

_The particle grammar (#1) is by far the highest-leverage steal and fits an
existing system we actively author in — start there. Skeletal blending + events
(#2/#4) are the natural follow-on for the Actor editor. The software renderer and
everything GPU-side are confirmed non-steals._

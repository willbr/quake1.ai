# Skeletal actors (IQM, TR1-style)

_Extracted from CLAUDE.md (reference detail; CLAUDE.md keeps a summary + pointer here)._

## Skeletal actors (IQM, TR1-style — in progress)

Expressive multi-part characters: an actor is one **IQM** file (geometry +
skeleton + animation clips), authored in-engine (no Blender; cubes-first), with
runtime layers for procedural face (look-at / eye-gaze / jaw-flap lipsync /
breathing), self-animating ponytail dynamics, and protocol pose sync. Design:
`docs/superpowers/specs/2026-06-01-skeletal-actors-design.md`; sub-projects
R1–R5 (runtime) + E1–E3 (in-engine editor) + C1 (content), built one at a time.

- **R1 done** (IQM load + static bind-pose render). New model type `mod_iqm`:
  - `sdlquake/libmodel/iqm.{c,h}` — portable IQM v2 reader (`lm_load_iqm` →
    `lm_iqm_t`): meshes, bind-pose joints (48-byte `iqmjoint`), triangles,
    POSITION/TEXCOORD/BLENDINDEXES. Animation lumps skipped until R2.
  - `model.c::Mod_LoadIQMModel` dispatches on the 16-byte `INTERQUAKEMODEL`
    magic and keeps the parsed `lm_iqm_t` on the hunk (`model_t.iqmdata`).
  - `r_alias.c::R_IQMDrawModel` (+ `R_IQMSetUpTransform`, identity model scale)
    renders rigid parts (1 bone/vertex, **bind pose = no joint math**) through
    the shared `D_PolysetDraw`; flat-lit, solid-colour synth skin per mesh;
    clipped triangles skipped (R1). Dispatched from `R_DrawEntitiesOnList`.
  - `iqm_dev.c` dev harness: `actor_dump <f.iqm>` (prints parsed structure),
    `actor_spawn <f.iqm> [x y z]` / `actor_clear` (persistent client-side entity
    injected into `cl_visedicts`, no server). Test asset
    `id1/actors/dummy.iqm` (5-cube figure) via `scripts/make_test_actor_iqm.py`.
- **R2 done** (animation playback). `lm_load_iqm` decodes poses/anims/frames →
  per-frame per-joint local TRS (`lm_iqm_t.frametrs`/`numframes`/`framerate`).
  `R_IQMDrawModel` builds a per-joint **skin matrix** each frame
  (bind-world-inverse ∘ animated-world via `IQM_QuatMat`/`IQM_LocalMat`/
  `IQM_Invert34` + `R_ConcatTransforms`), rigid-skins each vertex (1 bone), and
  loops the clip off `cl.time`. Bootstrap asset bakes a "look" clip (head yaw
  ±30°). Static (bind-only) models keep the identity-skin fast path.
- **R3 done** (procedural face: look-at + eye-gaze + breathing). Role joints
  (`head_joint`/`chest_joint`/`jaw_joint`/`eye_joint[]`) are resolved by **name
  convention** in `lm_load_iqm` (`strstr` on "head"/"chest"/"jaw"/"eye"; cached
  on `lm_iqm_t`). In `R_IQMDrawModel`'s compose loop, the head and eye joints'
  local rotation is **overridden** by `R_IQMLookAtLocal` to aim each joint's +X
  axis at the player (`r_origin`, transformed into actor space via the entity
  rotation `alias_forward/right/up`), clamped to a yaw/pitch cone — head and
  eyes track you; eyes (children of the head) carry the residual past the head
  clamp. **Breathing** post-multiplies the chest joint's skin matrix by a
  scale-about-posed-origin (`v' = org + s·(v−org)`), so it's **non-propagating**
  (children keep their own skin matrices) per the design's uniform-mesh-scale
  decision. Cvars (registered in `R_Init` via `R_IQMInitCvars`):
  `actor_lookat` (master on/off), `actor_gaze_yaw`/`actor_gaze_pitch` (head
  clamp 50/30°), `actor_eye_yaw`/`actor_eye_pitch` (eye clamp 25/20°),
  `actor_breathe_rate`/`actor_breathe_amp` (0.25 Hz / 0.04). All engine-side, no
  ABI bump. **Lipsync deferred**: the design's mouth skin-swap needs textured
  heads (R1/R2 use solid-colour synth skins); `jaw` is resolved for
  forward-compat but unused until a texturing milestone. On the cube bootstrap
  actor the look-at/gaze read subtly (featureless cube, eye cubes centred on
  their joints); verified from a fixed camera by varying only the gaze/breathe
  cvars. Plan: `docs/superpowers/plans/2026-06-02-skeletal-actors-r3-procedural-face.md`.
- **R4 done** (self-animating ponytail / secondary dynamics). `iqm_dynamics.c`
  — a client-side **Verlet point-chain**, engine-side, cosmetic, never networked.
  Ponytail joints are resolved as an ordered chain by the `pony` name convention
  (`lm_iqm_t.pony_joint[]`/`num_pony`). Run from `R_IQMDrawModel` after the R3
  pose composes: the chain root joint is pinned to its **rigid posed** position
  (so it follows the head look-at) and stays rigid; the free joints integrate
  gravity + inertia in **world space** (camera-motion-immune), satisfy distance
  constraints + a light straighten-toward-rigid term, and their simulated
  transforms are converted back to actor space and written into the free joints'
  `skin[]` (each segment's +Z bind axis aimed along the simulated chain). Per-actor
  sim state lives in a 16-slot pool keyed by `entity_t*`. Cvars `actor_dynamics`
  (on/off), `actor_pony_gravity`/`actor_pony_damping`/`actor_pony_stiffness`/
  `actor_pony_iters`. Verified in-game: the tail **sags under gravity** with
  dynamics on vs rigid with it off. **Wind tie-in deferred** (would need an ABI
  hook to read the game.dll `sim_wind` grid). The bootstrap dummy uses a
  back-sticking tail so it's visible past the chunky body; the solver handles any
  chain config. No ABI/protocol bump. Plan:
  `docs/superpowers/plans/2026-06-02-skeletal-actors-r4-ponytail-dynamics.md`.
- **C1-lite done** (in-game server integration). `sdlquake/game/actor_test.c` —
  proves the whole stack works on a **real server-spawned networked entity**, not
  just the engine dev-spawn. `Actor_TestPrecache()` (called from `worldspawn`)
  precaches `actors/dummy.iqm` into the level's precache list; `impulse 217`
  (`Actor_TestDebugSpawn`) `ED_Alloc`s a `SOLID_NOT`/`MOVETYPE_NONE` entity 96u
  ahead, `SV_SetModel`s it to the IQM, and `SV_SetOrigin` links it. The client
  renders it as `mod_iqm` and R3/R4 (look-at/breathing/ponytail) run for it
  automatically — **all client-side, so no protocol/ABI change** (`R5` would only
  be needed for game.dll to *drive* clip/talk/expression state). Verified
  in-game: the entity spawns and renders with live expression.
- **IQM writer done** (E1 pipeline backend — the editor's save path). `lm_write_iqm`
  (`libmodel/iqm.c`) serializes an `lm_iqm_t`'s geometry + skeleton back to IQM v2
  bytes (string table, 4 vertex arrays POSITION/TEXCOORD/BLENDINDEXES/BLENDWEIGHTS,
  triangles, joints), mirroring the known-good generator layout. **Animation is
  also written**: the decoded `frametrs`/`clips` are re-encoded into poses/anims/
  frames (per-channel min/max quantization, the inverse of the loader), so an
  edited actor keeps its clips through a save. Verified by `actor_roundtrip
  <file.iqm>` (load → write → re-parse → compare): `dummy.iqm` round-trips
  geometry + joints exact (`MATCH`) and animation within quantization tolerance
  (`anim … maxerr 0 OK` — exact here, since the source was quantized the same way). The remaining editor work (the interactive **Actor editor mode**:
  box create/size/parent, gizmos, animation timeline) is UX-heavy and the natural
  next sub-project (E1 proper), templated on `edit_particle.c`.
- **E1 skeleton done** (Actor editor mode foundation). `sdlquake/engine/editor/edit_actor.c`
  registers an `actor_mode` (`editor_mode_t`) in the editor shell's `s_modes[]`
  (third tab "Actor", alongside Map/Particle). It loads an IQM, **previews it with
  the shared orbit camera** (so the R3 head look-at tracks the orbit camera live —
  circle the actor and it watches you), and shows a **read-only inspector**
  (model + counts + roles + joint/mesh lists). Rendering reuses the editor's
  `cl_visedicts` preview-injection (`ActorMode_PushPreview`, called next to
  `Editor_PushPreviewEntities`); the orbit camera + `s_orbit_inited` reset now
  also fire for `actor_mode`. Verified in-game: `editor` + `editor_mode 2` shows
  the dummy actor in the orbit preview with the inspector. The interactive
  authoring tools (create/size/parent box parts, rig joints, animation timeline +
  save via the IQM writer) are the next slices — **UX-heavy, to be designed with
  the user.**
- **Multi-clip done** (animation infrastructure for E2 / R5 / behaviours). The
  loader parses every `iqmanim` into `lm_iqm_t.clips[]` (`lm_iqm_clip_t`:
  name/first_frame/num_frames/framerate/loop, ranges clamped into the decoded
  frame set); `R_IQMDrawModel` plays the `actor_clip` cvar–selected clip (loops
  within its frame range) and falls back to the whole frame set when a model has
  no clips. The bootstrap dummy now bakes **two** clips — `look` (head yaw,
  frames 0–15) and `nod` (head pitch, 16–31); `actor_dump` lists them. Verified
  in-game: `actor_clip 0` vs `1` (with `actor_lookat 0`) play visibly different
  head motion (yaw vs pitch). **Per-entity, game-driven, networked clip selection
  comes free:** `mod_iqm` repurposes the entity's **already-networked `frame`
  field** as the clip index, so `game.dll` setting `self.frame = N` (or
  `actor_spawn <f> [x y z] [clip]` for the dev actor) picks the clip per entity —
  **no protocol/ABI change** (the R5 pose-sync payoff for animation, achieved via
  the existing field). `actor_clip` (default −1) is a global test override; ≥0
  forces a clip for all IQM actors.


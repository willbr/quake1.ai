# Quake 2 ideas worth porting into this fork

Salvage notes taken before deleting `ref/Quake-2-master/` and
`ref/Quake-2-Tools-master/`, mirroring `docs/doom-ideas-to-steal.md` and
`docs/wolf3d-ideas-to-steal.md`. Unlike those, Quake 2 is a *sibling engine*
from the same lineage, so a lot of it is structurally identical to our Q1 base.
This doc captures only the genuine deltas worth the bytes — ranked by fit, and
honest about what we already have.

File/line references are to `ref/Quake-2-master/` (game DLL + engine) and
`ref/Quake-2-Tools-master/` (qbsp3/qvis3/qrad3/qe4), verified against the trees
at removal time (2026-06-07); line numbers are for orientation, not exact pins.

**TL;DR:** the headline steal is Q2's reload-safe function-pointer
serialization — it solves our open #1 Critical bug *and* the savegame-versioning
gap in one mechanism. After that: detail brushes and qrad3 radiosity for the
in-process compile/relight chain, and PHS for AI sound. Most of the rest we
already have or don't need.

---

## Top pick — fixes a real open bug

### 1. Function pointers stored as segment-relative offsets (`game/g_save.c`)

This is the one to internalize. Q2's game DLL has the *exact same problem* our
hot-reload has — raw C function pointers (`think`/`touch`/`use`/etc.) and
data-segment pointers (`mmove_t` animation tables) stored live in edicts, which
dangle the moment the DLL maps at a new base. Q2 solves it generically:

- A `fields[]` metadata table (`g_save.c:27-120`) tags every persisted edict
  field with a type — `F_FUNCTION`, `F_MMOVE`, `F_EDICT`, `F_CLIENT`, `F_ITEM`,
  `F_LSTRING`, …  — plus its `FOFS` offset and flags.
- On write (`WriteField1`, `g_save.c:276-291`): a function pointer is stored as
  `ptr - (byte *)InitGame` — an **offset from a known code-segment anchor**. An
  `mmove_t*` is stored as `ptr - (byte *)&mmove_reloc` — an offset from a known
  **data-segment** anchor. NULL → 0.
- On read (`ReadField`, `g_save.c:372-387`): the offset is re-anchored against
  the *freshly loaded* DLL's `InitGame` / `mmove_reloc` addresses:
  `*p = (byte *)InitGame + index`. Because every function lives at a fixed
  offset from `InitGame` within the same module, the rebind is correct no matter
  where the OS mapped the new copy.

**Why it's the best thing in the Q2 tree for us:** it retires `review.md`
finding **#1** (hot-reload dangles 217 `v.think`/`v.touch`/… pointers) *and*
finding **#11** (savegames silently break on `entvars_t` layout shifts) with a
single, battle-tested pattern. Our current hot-reload has *no* edict fixup at
all; Q2 shows the durable design.

**Fit / adaptation:**
- We don't strictly need the file-serialization half — for a same-process
  reload we could just walk `sv.edicts` and rebase each function field by
  `(new_InitGame - old_InitGame)`, recording the old anchor at load time. That
  is finding #1's "indirection registry" option but far cheaper: one subtract
  per field, no string table.
- The `mmove_reloc` data-segment anchor matters for us specifically because our
  ported monsters and the IQM actor clips will eventually want declarative
  frame→callback tables (see #4) that *also* live in the DLL's data segment —
  same dangling hazard, same fix.
- For real savegame versioning (#11), adopt the `fields[]`-table approach
  wholesale: it's the cleanest way to serialize `entvars_t` field-by-field with
  a version stamp instead of the raw `ED_Write` field walk.

---

## High value — the compile/relight + AI-sound chain

### 2. Detail brushes — `CONTENTS_DETAIL` (`qbsp3`)

Q2 lets a brush be flagged `CONTENTS_DETAIL` (`qfiles.h:363`) so decorative
geometry (railings, crates, trim) is invisible to the vis cluster build:

- `BrushGE` (`qbsp3/csg.c:435-444`) forbids detail brushes from cutting
  structural brushes.
- A two-phase BSP build (`qbsp3/brushbsp.c:760-774`) splits on structural
  brushes first, detail second, marking a `detail_seperator` node where vis
  stops.
- `prtfile.c:63,181-183` skips detail-separator nodes when writing portals, so
  PVS never sees the detail geometry.

**Fit:** this is the single biggest win for our **in-process editor compile**.
It decouples authored detail from vis cost — dense rooms stop exploding PVS and
recompile/relight time. Directly relevant to the live-bake editor and to the
nav bake (smaller cluster space → cheaper portal flood). Q1's qbsp/vis (what we
vendor) has no detail concept; this is a real upgrade to the vendored chain, not
a sidegrade.

### 3. qrad3 radiosity — colored *bounce* lighting (`qrad3`)

We already have live **RGB lightmaps** (`r_surf_rgb.c`, `r_livelight.c`,
`dlight_t.color`), so direct colored light is *not* new to us. What Q2 adds that
we lack is **radiosity bounce**:

- Patch subdivision + per-patch `vec3_t radiosity[]`/`illumination[]`
  (`qrad3/qrad3.c:41-42`).
- Form-factor transfers between patches, PVS-masked so only mutually-visible
  patches interact (`qrad3.c:218-270`, `trans = scale * area / dist²` gated on
  `TestLine`).
- An iterative bounce loop (`qrad3.c:486-515`) multiplying incoming light by
  per-texture `reflectivity[3]` loaded from the texture
  (`patches.c:40-115 CalcTextureReflectivity`).
- RGB output: `FinalLightFace` (`lightmap.c:1186`) writes 3 bytes/sample.

**Fit:** our relight already fills RGB lightmaps interactively; qrad3 is the
*math* for filling them with indirect/bounced colored light, which is what makes
a software-rendered scene read as modern rather than flat-lit. The
precompute-transfers-once / re-shoot-bounces-on-edit split (precompute at edit
start, re-bounce on geometry change) is a natural fit for the editor's async
bake thread. **Caveat:** transfers are O(patches²) — needs the detail-brush
shrink (#2) and PVS masking to stay interactive on real maps. Worth a spike, not
a casual port.

### 4. PHS — Potentially Hearable Set (`qvis3`)

`CalcPHS` (`qvis3/qvis3.c:461-518`) ORs together the PVS of every leaf visible
from a leaf, producing a second bitset: "every cluster from which sound here
could be heard." Cheap post-pass over the PVS we already compute.

**Fit:** our immersive-sim AI already reasons about sound propagation
(`Wind_PathOcclusion` folded into LOS, the stimulus bus's `STIM_SOUND`). A baked
PHS gives a near-free first-pass cull for "could this monster possibly hear that
gunshot?" before the expensive occlusion/trace path. Slots directly into the
M1 stimulus filter as an early-out.

---

## Medium — architecture we partly have

### 5. `mmove_t` data-driven animation FSM (`game/g_local.h:403-414`)

Q2 lifts monster animation out of code into declarative tables: an `mmove_t`
(first/last frame, `mframe_t[]`, `endfunc`) where each `mframe_t` carries an
`aifunc(self, dist)` + `thinkfunc(self)`. Q1 (and our port) inline this as
`ai_run(dist)`-style calls inside hand-written think functions.

**Fit:** medium. Our monsters are already hand-ported to C and work, so this is
a *refactor*, not a feature. It becomes attractive only if (a) we want the IQM
actor system and monsters to share one clip-driven animation path, or (b) we
adopt #1's serialization — at which point `F_MMOVE`-style data tables are the
clean way to make animation state savegame/reload-safe. Pair it with #1 or skip.

### 6. Sound-stimulus entities — validation, not salvage (`g_ai.c`, `p_weapon.c`)

Q2 reifies noise as invisible entities (`PlayerNoise`, `p_weapon.c:58-109`;
`level.sight_entity`/`sound_entity`, `g_ai.c:437-450`) that monsters poll. This
is a *simpler, single-channel* version of what our M1 stimulus bus already does
with typed multi-channel stimuli. **Take it as confirmation** that the
stimulus-entity approach is the right shape — not as something to port. Our
`AI_LOST_SIGHT` + `last_sighting`/`trail_time` search memory
(`g_ai.c:357-360,509-512`) is likewise already covered by our SEARCHING FSM +
navmesh A*.

### 7. Engine-architecture niceties (`qcommon/`)

For a single-player, software-rendered port these are clean-codebase wins, not
features, and none are urgent:

- **Shared deterministic `pmove`** (`qcommon/pmove.c`, one module compiled into
  both client and server) — relevant only if we ever want demo replay or
  mod-authored movement. Architectural hygiene.
- **`configstring[]` per-map state** (`q_shared.h:1088-1111`) — a cleaner way to
  carry per-level model/sound/state than baking into entities; worth a look only
  if we build campaign progression.
- **Delta-compressed entity frames against acked baselines**
  (`server/sv_ents.c:413 SV_WriteFrameToClient`,
  `qcommon/common.c:474 MSG_WriteDeltaEntity`) — elegant, but pure multiplayer
  latency tech. **Irrelevant** to our single-player loopback.

---

## Low / software-renderer polish (`ref_soft/`)

Q2's software rasterizer is ~90% our rasterizer. The small deltas, in
descending order of worth:

- **Subtractive (negative) dynamic lights** — `dl->intensity < 0` subtracts from
  `blocklights` (`ref_soft/r_light.c:313-320,366`). Cheap; enables "dark light"
  effects (cloak fields, damage-flash inverse). ~20 lines on top of our existing
  dlight path.
- **Per-entity alpha translucency in software** — three-tier blend via
  `vid.alphamap[src + dst*256]` for aliases/sprites/particles
  (`r_alias.c:1160-1169`, `r_part.c:55-96`). We render glass/smoke today via
  particles; true translucent *surfaces/models* would be new. Modest, real
  visual win if we want translucent props.
- **`SURF_FLOWING`** — separate scroll rate for flowing water vs. warp
  (`r_edge.c:840-849`). Trivial; nice-to-have.
- **Already covered, do not port:** colored dynamic lighting (we have
  `dlight_t.color` + RGB lightmaps), mip selection (identical to Q1),
  `r_cache_thrash`/`sw_surfcacheoverride` (informational tuning knobs we don't
  need).

---

## Explicitly not worth it

- **MD2 models** (`qfiles.h:81-160`) — a better-packed *keyframe* triangle
  format, still not skeletal. We already have IQM skeletal actors; MD2 is a
  step backward.
- **BSP38 / bigger map limits** (`qfiles.h:219-250`) — scale tech for large Q2
  maps; our single-player Q1 maps don't hit Q1's limits.
- **Client-prediction error correction** (`client/cl_pred.c:29-64`) —
  multiplayer-only.
- **`qe4`** (the Radiant ancestor, `Quake-2-Tools-master/qe4/`) — interesting as
  editor-design history, but we already have a working in-game editor; nothing
  here we'd lift.

---

_When acting on #1, cross-link the fix back to `review.md` findings #1 and #11
and strike them from the open list._

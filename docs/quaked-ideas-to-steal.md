# QuakeEd level-editing ideas worth stealing

Salvage notes from reviewing the two QuakeEd sources against our in-game 3D
editor (`sdlquake/engine/editor/`), mirroring the `docs/*-ideas-to-steal.md`
salvage docs. Both editors are the lineage our editor descends from, so most of
the basics are already present — this captures only the genuine gaps, ranked by
value, with concrete `file:function` citations.

Sources:
- **Original QuakeEd** (Quake 1, NeXTSTEP / Objective-C) — in-tree at
  `ref/Quake-Tools-master/QuakeEd/`. The Quake-1-native editor; canonical home
  of the QUAKED entity-definition format.
- **QuakeEd 4 / qe4** (Quake 2, Win32 / C) — the proto-Radiant, much richer
  (CSG, vertex editing, texture math). It lived in `ref/Quake-2-Tools-master/qe4/`,
  which was **removed 2026-06-07** (see `review.md` Tier 5); recover it any time
  with `git show <pre-removal>:ref/Quake-2-Tools-master/qe4/<file>` (the removal
  commit is `77a9d99`, so use `77a9d99^`).

Reviewed 2026-06-07. Line numbers are for orientation, not exact pins.

---

## What we already have (verified — do NOT re-steal)

So the gap list below stays honest:

- **Plane-based 3-point brush representation** — `edit_scene.h::edit_plane_t`
  stores `points[3]` per face and derives `normal`/`dist`. This is exactly
  QuakeEd's `SetBrush.m:BasePolyForPlane` / qe4 `brush.c:Brush_MakeFacePlanes`
  model; the architectural steal is already done.
- **Per-face texdef** — `edit_plane_t` already carries `s_shift,t_shift`,
  `rotation`, `s_scale,t_scale` (= qe4's `texdef_t`). Texture shift/scale/rotate
  exist.
- **CSG hollow / "make room"** — `edit_scene.c:Scene_HollowBrush` (= qe4
  `csg.c:CSG_MakeHollow`).
- **Entity target/killtarget link arrows + angle arrows** —
  `render_wire.c:draw_link_arrow` / `draw_angle_arrows` (= QuakeEd
  `SetBrush.m:drawConnections` / angle draw in `XYDrawSelf`).
- **Brush→BSP/.lit compile, undo/redo, ray-pick, translate+resize gizmos with
  surface-snap, texture cache** — all present.

---

## Top pick

### 1. Rich QUAKED entity metadata  ★★★

Our classlist (`editor_classlist.c`) caches only the *names* the game DLL
returns from `list_spawn_classes`. It has no per-class **color**, **bounding-box
size**, **spawnflag names**, or **help text**. The QUAKED format encodes all
four in one declarative line + comment block:

```
/*QUAKED func_door (0 .5 .8) ? START_OPEN STONE_SOUND DOOR_DONT_LINK GOLD_KEY SILVER_KEY
  <help text...>
*/
```

- Parser to copy: QuakeEd `EntityClass.m:initFromText` (the canonical Quake-1
  version) and qe4 `eclass.c:Eclass_InitFromText` / `Eclass_ScanFile`.
- Fields extracted: classname, RGB `color[3]` (0–1), `fixedsize` flag + `mins`/
  `maxs` (point entities draw as a fixed coloured box; brush entities draw their
  geometry), up to **8 spawnflag names**, and the trailing comment as help.
- UI payoff: coloured fixed-size point-entity boxes in the viewport
  (`entity.c` fixedsize render path), **named spawnflag checkboxes**
  (qe4 `win_ent.c:SetSpawnFlags`/`GetSpawnFlags` reading `eclass->flagnames[8]`;
  QuakeEd `Things.m:newCurrentEntity` 4×3 grid), and inline per-class docs in
  the inspector.

**Why it's the headline:** it's the single biggest editing-velocity win and it
makes gaps #2–#5 more usable — once point entities have real boxes and colours,
selecting/clipping/region-filtering around them all read correctly.

**Quake-1 adaptation (important):** QUAKED defs traditionally live in `.qc`
source comments, which this fork removed in the NATIVE_GAME port — so the clean
path is *not* "parse .qc". Two options:
- (a) Extend the game ABI: have the DLL expose color/bbox/flagnames/help
  alongside `list_spawn_classes` (the DLL already owns the spawn table). Keeps
  the metadata next to the code that implements each entity.
- (b) Ship a `.def`/`.ent` metadata file the editor parses with the
  `initFromText` grammar. Decouples editor metadata from the DLL.

Either way, the *format + semantics* to copy verbatim are `EntityClass.m:initFromText`.

---

## High value

### 2. Clip tool  ★★★

We have no clip tool. The Quake-1 QuakeEd UX is the better model for in-game
(qe4 has no dedicated clip tool — its splitting is indirect via side-select +
face drag):

- `Clipper.m:XYClick` — click places a grid-snapped clip point at the current
  min-Z; clicking the same XY again toggles it to max-Z (a small state machine
  on a `num` counter), so 2 clicks → a vertical plane, 3 points → a sloped one.
- `Clipper.m:flipNormal` — swap point order to choose which side is kept.
- `Clipper.m:carve` → `[map makeSelectedPerform:@selector(carveByClipper)]` —
  the clipper plane becomes a temporary face that splits the selected brush;
  immediate commit (no drag-preview state to manage).

Maps cleanly onto our plane-based brushes: the clip plane is just another
`edit_plane_t`; split = clip the brush windings against it (we already clip
windings in `brush_compile.c`).

### 3. CSG subtract (carve)  ★★★

We have hollow but not subtract. qe4 `csg.c`:

- `CSG_SplitBrushByFace` (csg.c:33) — split a brush by a plane into front/back
  fragments via `ClipWinding`.
- `CSG_Subtract` (csg.c:124) — carve selected brushes out of world geometry:
  for each face of the cutter, split every overlapping world brush, accumulating
  the front-side fragments. Produces **multiple** result brushes per subtraction
  (the classic Radiant carve). Non-trivial; worth porting the algorithm directly.

### 4. Region compile / bake  ★★

Compile or light-bake only a sub-region for fast iteration — high leverage given
our **async light bake** (bake one room, not the whole map):

- qe4 `map.c:Map_RegionXY` / `Map_RegionBrush` / `Map_RegionOff` — filter
  active brushes to a `region_mins`/`region_maxs` box.
- QuakeEd `SetBrush.m:newRegion` adds content filters on top (hide
  entities / world / lights / path / `clip`-textured / `*`-prefixed water
  brushes) and skips `regioned` brushes on save/export.

### 5. Vertex / edge editing  ★★

We only push whole faces along their normal (`Brush_TranslateFace`). qe4
`vertsel.c` adds free vertex/edge manipulation while keeping the brush convex:

- `vertsel.c:SetupVertexSelection` — build dedup'd `d_points`/`d_edges` from the
  brush's face windings.
- `vertsel.c:SelectVertexByRay` — ray-pick the nearest vertex; mark the 3
  adjacent face planepts movable (the move re-derives each face plane from its
  dragged points, so the brush stays valid).
- `vertsel.c:SelectEdgeByRay` + `SelectFaceEdge` — rotate planepts so an edge's
  two verts lead each adjacent face, enabling edge slides.
- Validity guard during the drag: qe4 `drag.c:MoveSelection` rebuilds and
  cancels the move if any brush inverts or explodes (`maxs<mins` or span>4096).

---

## Medium / verify

### 6. Texture lock  ★★ (verify first)

We store `s_shift,t_shift,rotation,s_scale,t_scale` but it's unconfirmed whether
we re-fixup texcoords when a brush is **moved/rotated** so the texture stays
"glued" to the faces. qe4 does this in its brush-move path on top of
`brush.c:TextureAxisFromPlane` (natural per-plane S/T axis). If we lack the
on-transform fixup, it's a cheap, high-value add given the texdef fields already
exist. **Action: verify our move path before deciding.**

### 7. Selection-by-geometry helpers  ★

qe4 `select.c`: `Select_CompleteTall` (brushes fully inside a reference brush's
XY box), `Select_PartialTall` (overlapping), `Select_Touching` (3D adjacency
with 1u epsilon). Small QoL multi-select ops if/when selection grows.

---

## Not worth stealing

- **qe4 / QuakeEd rendering pipelines** — PostScript (NeXT) and Win32 GDI; our
  GPU palette-LUT + software rasterizer path is superior. (QuakeEd's own README
  calls its texture renderer "crap".)
- **Brush winding / ClipWinding math** — generic; we already clip windings in
  `brush_compile.c`.
- **MRU recent-files, multi-pane XY/Z/Camera window sync, inspector hotkeys** —
  desktop-app workflow tied to a 3-window layout; our editor is ImGui-docked
  with a single Viewport panel, so these don't map.
- **Project/.qe4 file model** — path/config plumbing for a standalone tool; we
  load `cl.worldmodel` directly.

---

_Original QuakeEd is essentially a subset of qe4 except for two things it owns
outright: the canonical Quake-1 QUAKED parser (#1) and the clean interactive
Clipper UX (#2). Those two are the highest-value, most Quake-1-appropriate
steals; #3–#5 are the meatier qe4 geometry ops._

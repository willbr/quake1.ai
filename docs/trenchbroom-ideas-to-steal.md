# TrenchBroom ideas worth stealing

Salvage notes from reviewing TrenchBroom (`ref/TrenchBroom-master/`, the most
advanced idTech/Quake brush editor in existence) against our in-game 3D editor,
mirroring the other `docs/*-ideas-to-steal.md` docs.

**Two framing constraints:**
- TrenchBroom is **C++/Qt**. We have a "no new C++" rule, so everything here is a
  **concept / algorithm / UX pattern to reimplement in C**, never code to port.
- The **QuakeEd review** (`docs/quaked-ideas-to-steal.md`) already flagged the
  basics — clip tool, CSG subtract, vertex/edge editing, texture lock, QUAKED
  entity metadata, region compile. This doc deliberately focuses on what
  TrenchBroom adds **beyond** those, and on how TB does the shared ones more
  robustly. Cross-references to the QuakeEd doc are noted inline.

Reviewed 2026-06-07. File refs are under `ref/TrenchBroom-master/lib/`; line
numbers orient, not pin.

**Headline net-new steals (none of these are in the QuakeEd doc):**
1. **DrawShapeTool** — parametric primitives (cylinder/cone/sphere/stairs).
2. **Validator / issue-browser framework** with one-click quick-fixes.
3. **Linked groups** (prefab instances) + **layers**.
4. **FGD typed entity properties** (choices/flags/colors/defaults) + `@BaseClass`.
5. **In-editor entity model display** (render the real `.mdl`, not a box).
6. The **half-edge polyhedron** brush model (robustness for vertex editing).

---

## What we already have (so the gaps stay honest)

Plane-based 3-point brushes (`edit_plane_t`), per-face texdef
(`s_shift/t_shift/rotation/s_scale/t_scale`), CSG **hollow** (`Scene_HollowBrush`),
translate+resize gizmos with surface-snap, ray-pick, undo/redo, target/angle
link arrows (`render_wire.c`), entity classlist from the DLL's
`list_spawn_classes` (names only), BSP/.lit compile + async light bake. The
shared gaps (clip/CSG-subtract/vertex-edit/texture-lock/QUAKED/region) live in
the QuakeEd doc; below is the TB-specific delta.

---

## Geometry & tools

### 1. DrawShapeTool — parametric primitives  ★★★

`TbUiLib/src/DrawShapeTool.cpp` + `DrawShapeToolExtensions.cpp` +
`TbMdlLib/src/BrushBuilder.cpp` (`createCuboid`/`createCylinder`/`createCone`/
`createUVSphere`/`createIcoSphere` + stairs). Drag a bounding box, pick a shape,
tune live parameters (segments, edge- vs vertex-aligned circle, step count/dir),
snap to grid. **The single biggest content-speed win** — cylinders, cones, and
stairs are miserable to build by hand and Quake mappers build them constantly.
Self-contained, no polyhedron dependency, ~half a day for cuboid+cylinder+stairs.
Nothing like it in QuakeEd.

### 2. Half-edge polyhedron brush model  ★★ (architecture — adopt selectively)

`TbMdlLib/include/mdl/Polyhedron*.h` + `Brush.cpp:updateGeometryFromFaces`. TB
stores a brush as a half-edge polyhedron, so **convexity/validity is invariant by
construction** — vertex/edge drags can't produce an invalid brush, near-coincident
vertices auto-merge (`healEdges(minLength)`), and CSG/clip are clean half-edge
traversals. **Recommendation (from the review): keep our plane representation for
serialization/compile, but spin up a transient polyhedron during a vertex/edge
edit** to get validity tracking + `healEdges` merging for free, instead of
hand-writing convexity checks. This is the enabling substrate for #3 and the
QuakeEd-doc vertex-editing gap.

### 3. Vertex / edge / face tools done right  ★★

`TbUiLib/src/{VertexTool,EdgeTool,FaceTool}.cpp` + `BrushVertexCommands.cpp`.
Beyond plain vertex drag (QuakeEd doc): **dry-run validity** —
`Brush::canTransformVertices(...)` tests before applying so an illegal drag is
rejected, not committed; **mode switching** within the vertex tool — Move /
SplitEdge (click an edge → add a midpoint vertex) / SplitFace (click a face → add
a center vertex); snapping to grid / other vertices / incident planes;
`healEdges` auto-merge on collapse. These are the refinements that make vertex
editing feel safe rather than brush-destroying.

### 4. Extrude tool  ★★

`TbUiLib/src/ExtrudeTool.cpp`. Drag a face along its normal to **create a new
brush** (original face → back, offset face → front, generated sides), distance
snapping to grid and to adjacent brush planes. Distinct from our resize gizmo
(which stretches in place) — extrude is the fast way to grow corridors/rooms off
an existing surface.

### 5. CSG subtract / intersect / convex-merge  ★★

`TbMdlLib/include/mdl/Polyhedron_CSG.h` (`subtract`, `intersect`) +
`Polyhedron_ConvexHull.h:addPoints` (convex merge = hull of all input vertices).
We have hollow; subtract (carve, → 0..N fragments via the plane-clip loop) is the
QuakeEd-doc gap, and TB's algorithm is the cleaner reference. **Convex-merge**
(combine selected brushes into one convex hull) and **intersect** are net-new and
cheap once the plane-clip loop exists.

### 6. Scale / shear / rotate tool UX  ★ (polish, lower ROI)

`TbUiLib/src/{ScaleTool,ShearTool,RotateTool}.cpp`. Bbox corner/edge/face handles
(scale 3/2/1 axes), proportion lock, per-axis grid snap, separately-positionable
rotate pivot. Enhancements to our existing gizmos, not gaps; shear is rarely used
in mostly-axis-aligned Quake geometry. Do last.

---

## Entity system & validation

### 7. Validator / issue-browser framework with quick-fixes  ★★★ (net-new)

`TbMdlLib/include/mdl/{Validator,Issue,IssueQuickFix,ValidatorRegistry}.h` + the
~20 concrete `*Validator.cpp`. Each validator scans a node type and emits typed
`Issue`s, each carrying one-click `IssueQuickFix`es. Shipped checks worth copying:
empty brush-entity (→delete), empty group (→delete), **missing target** (target
set but no matching `targetname` →remove prop), missing classname (→worldspawn),
point-entity-with-brushes (→delete brushes), **non-integer vertices** (→snap to
grid), invalid UV scale 0 (→set 1), long/empty property key or value
(→truncate/remove), mixed brush contents, out-of-world-bounds (→clamp). **This is
the highest-value net-new feature for us** — we have scattered `Con_Printf`
warnings; folding them into a dockable issue list with "Fix"/"Fix all" buttons is
a step-change in map QA, and the framework is ~20 lines per rule.

### 8. FGD typed entity properties + `@BaseClass`  ★★★

`TbMdlLib/src/{FgdParser,DefParser,EntityDefinitionParser}.cpp` +
`PropertyDefinition.h`. Beyond QUAKED's color/size/spawnflag-names (QuakeEd doc),
FGD adds **semantically typed properties** that auto-drive the inspector UI:
`Choice` (enum dropdown), `Flags` (named-bit checkboxes), `Color1/Color255`
(color picker), `Integer`/`Float`/`String`/`Boolean` with **default values** and
**per-property descriptions**, plus `@BaseClass` **inheritance** (DRY shared
property blocks; spawnflags merged, models appended; `EntityDefinitionParser.cpp`
resolves the chain with cycle detection). If we ship a `.def`/`.fgd` metadata
file (option (b) in the QuakeEd doc), adopt this typed-property grammar so the
property editor renders the right widget per field instead of raw text boxes.

### 9. In-editor entity model display  ★★

`TbMdlLib/include/mdl/ModelDefinition.h` + `LoadEntityModel.cpp`. An entity def's
`model` key (literal *or* an expression over the entity's own properties, e.g.
`"progs/armor.mdl"` with skin/frame from properties) is evaluated and the **real
`.mdl` is rendered in the viewport** instead of a colored box. We already load
and render alias models — this is the wiring from def→model so monsters/items/
weapons show their actual geometry while editing. Huge readability win.

### 10. EntityLinkManager — typed links + missing-target detection  ★★

`TbMdlLib/src/EntityLinkManager.cpp` + `LinkTargetValidator.cpp`. We draw
target/killtarget arrows; TB additionally tracks **bidirectional typed links**
(`linksFrom`/`linksTo` per property key), supports custom link keys from the def,
and detects **dangling links** (target set, no matching entity) — which feeds the
validator (#7) as a quick-fix. The "highlight links for selection only" + broken-
link flagging is the upgrade over always-on arrows.

---

## Organization & UV workflow

### 11. Linked groups (prefab instances)  ★★★ (net-new, killer feature)

`TbMdlLib/src/LinkedGroupUtils.cpp` (`updateLinkedGroups`,
`cloneAndTransformChildren`) + `UpdateLinkedGroupsHelper.cpp`. A group can be
instanced many times; editing one instance propagates to all (tracked by a
`linkId` UUID on each node), with per-instance transforms preserved and edits
applied as **one atomic undo step** across all instances. Build a room/trim/light
fixture once, place N copies, tweak once → all update. Massive authoring win for
repeated architecture. Complex (structural diff + selection locking of sibling
instances) — the review suggests prototyping **transform-only** linked groups
first before structural edits.

### 12. Layers (with omit-from-export)  ★★ (net-new, cheap)

`TbMdlLib/src/{Layer,LayerNode}.cpp` + `Map_Layers.cpp`. Named, hideable, lockable
layers with sort order and an **`omitFromExport` flag** (geometry stays in the
editor but isn't compiled/shipped) — perfect for reference geometry, WIP areas,
lighting-preview blocks, gameplay guides. Lowest-friction high-payoff item here:
one flag per layer, filtered at export. Also: **groups** (group/ungroup, nested,
enter/leave editing context) as the lighter-weight sibling.

### 13. Paraxial vs Parallel UV coordinate systems + texture-lock math  ★★

`TbMdlLib/src/{ParaxialUVCoordSystem,ParallelUVCoordSystem}.cpp`. We're paraxial
(axis-aligned, Quake-standard) only. TB cleanly separates **`BrushFaceAttributes`
(data)** from **`UVCoordSystem` (transform behavior)** and supports a second
**parallel** (Valve-220 / free-axis) projection so textures sit straight on
sloped/diagonal faces without manual fixup. Crucially, both implement the
**texture-lock transform** (`transform()` recomputes shift/scale/rotation to keep
UVs glued when a brush moves/rotates/scales) — the concrete math behind the
QuakeEd-doc "texture lock" gap (`ParaxialUVCoordSystem::transform()` is the one to
copy first; parallel is an optional later mode).

### 14. UV editor (live per-face alignment)  ★★

`TbUiLib` UV view + `UVRotateTool`/`UVScaleTool`/`UVOffsetTool`/`UVShearTool` +
`resetUV`/`resetUVToWorld`/`flipUAxis`/`flipVAxis`/`rotateUV*`. A live per-face
texture-alignment panel (see the texture tile on the face, drag to offset/rotate/
scale/shear, one-click fit/reset/flip). Far better than typing texdef numbers;
pairs naturally with #13.

### 15. Grid snapping + selection-by-geometry helpers  ★ (QoL)

- **Grid** (`TbMdlLib/.../Grid.h`): plane/line/segment-aware snapping and an
  explicit **"snap vertices to grid"** op (feeds the non-integer-vertices
  validator). Composable `moveDeltaFor*` helpers for interactive drags.
- **Selection** (`Map_Selection.h`): select touching / contained / siblings /
  by-material / all-linked-instances; "expand selection." Cheap multi-select
  wins for layout work (also flagged lightly in the QuakeEd doc via qe4
  `select.c`).

---

## Not worth stealing

- **TB's GL/Qt rendering, material/shader system** — we're software-rendered with
  a baked `.lit` pipeline; their renderer is irrelevant.
- **The EL expression language** (`TbElLib`) in full — TB uses it for dynamic
  model paths and property defaults; for Quake 1 a literal model path + simple
  defaults suffice. Borrow the *idea* for #9 only if a class needs skin/frame
  variants.
- **Quake 2/3/Valve-specific surface flags & content types** — keep only the
  Quake-1-relevant subset.
- **vcpkg/CMake/Qt build + update machinery** — desktop-app plumbing.

---

## Suggested order (if/when we build these)

Reinforces and extends the QuakeEd-doc plan. Highest leverage first:
1. **DrawShapeTool** (#1) — immediate content speed, self-contained.
2. **Layers + omit-from-export** (#12) and **selection helpers** (#15) — cheap, high QoL.
3. **Validator/issue browser** (#7) — fold existing warnings into a real QA pane.
4. **FGD typed properties + the QuakeEd-doc QUAKED metadata** (#8) — one entity-def effort.
5. **Texture-lock math (paraxial)** (#13) + **UV editor** (#14).
6. **Vertex editing via a transient polyhedron** (#2/#3) + **CSG subtract/merge** (#5).
7. **Linked groups** (#11) — biggest win but most complex; prototype transform-only first.

_TrenchBroom is decades of brush-editor refinement; the parts that survive our
"software-rendered, C, in-engine" constraints are the editing *algorithms* and
*UX*, not the renderer or framework. DrawShapeTool, the validator framework, and
linked groups are the three that would most change how the editor feels._

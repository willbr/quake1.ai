# In-Game Map Editor (Phase 7)

`sdlquake/engine/editor/`. A live `.map` editor that runs inside the
engine — open the editor, place brushes/entities, compile + light + vis
in-process, reload the resulting BSP without leaving Quake. The whole
purpose is to keep edit-test-edit iteration inside a single binary.

The editor is engine-side (not game.dll) because it needs direct access
to `vid.buffer`, the BSP loader, the renderer, and SDL window state.

## Files

```
sdlquake/engine/editor/
├── editor.c              Top-level state, mode handling, console hook
├── edit_scene.c          The editable scene: brush list, entity list
├── edit_history.c        Undo stack
├── edit_texcache.c       Texture preview cache for the ImGui palette
├── editor_classlist.c    Entity-class picker (pulls from game_api->list_spawn_classes)
├── editor_ui.c           Dear ImGui panels: brush props, entity props, history
├── map_io.c              Read/write Worldcraft-style .map files
├── brush_compile.c       Convert scene → qbsp brush format → BSP via vendored qbsp
├── light_bake_thread.c   Background thread that calls vendored light
├── render_wire.c         Wireframe overlay drawn into vid.buffer
├── render_flat.c         Flat-shaded overlay (selected faces)
├── render_tex.c          Textured preview (per-brush)
├── gizmo.c               Selection / transform gizmo (translate, rotate, scale)
└── collide.c             Ray-vs-brush for picking
```

## How it ties together

```
Editor opens (F10 / "editor" command)
   │
   ▼
edit_scene_t (brushes + entities, all in-memory)
   │   ←──── user edits via gizmo / ImGui / MCP editor_* tools
   │
   ▼
"compile" command:
   ├── brush_compile.c  : scene → planes (qbsp's format)
   ├── qbsp_compile_to_memory()  (vendor/qbsp, in-process)  → BSP + .prt
   ├── vis_compile_in_place()    (vendor/vis,  in-process)  → fills dvisdata
   ├── light_compile_to_memory() (vendor/light,in-process)  → bakes lightmap
   └── Mod_LoadBrushModel() reloads the freshly-built BSP
   │
   ▼
Renderer immediately shows the new world — no exe restart.
```

Lighting bakes can be slow, so `light_bake_thread.c` runs the
`light_compile_to_memory` call on a background SDL thread; the engine
keeps rendering the unlit-but-textured intermediate until the bake
finishes.

## Vendored compilers

`sdlquake/vendor/qbsp/`, `vendor/light/`, `vendor/vis/` are unchanged
forks of the original id Software GPLv2 tools, compiled directly into
the engine. They normally have their own `main()` and write/read files
on disk; we instead:

1. **Namespace-prefix** their globals via `-include
   qbsp_namespace.h` / `light_namespace.h` / `vis_namespace.h`. Each
   such header is just a long list of `#define name qbsp_name` lines
   so the engine's `mathlib.h`/`cmdlib.h` symbols don't collide.
2. Expose a `qbsp_compile_to_memory` / `light_compile_to_memory` /
   `vis_compile_in_place` entry point (in `*_lib.c`) that the editor
   calls instead of `main`.
3. Share the BSP data structures across the three tools by leaving
   them in qbsp's globals (`dfaces`, `dplanes`, `dlightdata`, …);
   light and vis run on those in-place rather than reloading from a
   file.

The same in-process pattern is reused by the editor's "compile" path
and by any console command that calls qbsp/light/vis (e.g. recompiling
a map without the editor open).

## .map format

Worldcraft / TrenchBroom-compatible text format. The editor reads and
writes it via `map_io.c`. Briefly:

```
{
"classname" "worldspawn"
"wad" "quake.wad"
// brushes follow inside this { } block
{
( -64 -64 0 ) ( -64  64 0 ) ( 64 64 0 ) WALL07 0 0 0 1 1
( -64  64 0 ) ( -64 -64 0 ) ( 64 -64 0 ) WALL07 0 0 0 1 1
... (six planes for an AABB; more for arbitrary brushes)
}
}
{
"classname" "info_player_start"
"origin" "0 0 24"
}
```

Each brush is defined by ≥4 face planes; each face is three points
(in winding order, determines normal direction) plus a texture name
and four `s/t shift/scale/rotate` values. qbsp clips these planes
into a convex polytope.

## MCP editor tools

The MCP server exposes a parallel set of editor tools:
`editor_get_scene`, `editor_brush_add`, `editor_entity_add`,
`editor_set_kv`, `editor_select`. These let Claude Code drive the
editor without keyboard or mouse — useful for procedurally generating
test maps, for golden-path tests, and for the immersive-sim arenas
(`sim_arena.c` test fixtures).

## History / undo

`edit_history.c` is an immutable-style stack: every change pushes a
snapshot of the affected brush/entity. `Ctrl+Z` pops; `Ctrl+Y`
re-applies. The MCP tools push history entries too, so an interleaved
manual/MCP session is undoable.

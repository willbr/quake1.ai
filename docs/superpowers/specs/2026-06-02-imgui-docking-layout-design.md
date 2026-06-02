# ImGui Docking Layout — Design

**Date:** 2026-06-02
**Status:** Design approved. Plan to be written next.
**Scope:** Replace the editor's hand-pinned floating-window UI with real ImGui
docking: vendor the docking branch, add a passthrough host dockspace that frames
the 3D view, and bake a sensible default layout. Single self-contained
sub-project.

## Why

The in-engine editor's panels read as a pile of floating windows. The vendored
ImGui (`imgui-1.92.8`, **master** branch) has no docking compiled in — no
`DockSpace`, no drag-to-snap, no tabbing, no central viewport node — so every
panel is an independent window. The map editor fakes a three-panel layout by
force-setting window pos/size, but it's half-anchored and everything else
(dialogs, particle/actor panels, dev overlays) genuinely floats over the 3D
scene rather than framing it. The goal is the TrenchBroom/Unity feel: panels
docked to the edges, the 3D viewport in the centre, user-rearrangeable and
persistent.

## Current state (what we replace)

All ImGui goes through a C shim (`IG_*` → `ImGui::*` in `imgui_bridge.cpp`).
Windows today:

- **Map editor** (`editor_ui.c`): `Editor` (toolbar), `Brushes` (left),
  `Inspector` (right) — semi-pinned via `s_dock_cond`; plus floating dialogs
  `Spawn Entity`, `Wrap Brushes`, `Textures`, `Light bake options`.
- **Particle editor** (`edit_particle.c`): `Particle Effects`, `Particle
  Inspector` — float at hardcoded coords.
- **Actor editor** (`edit_actor.c`): `Actor` — floats at hardcoded coords.
- **Dev/debug overlays** (`imgui_layer.c`, `imgui_debug_panel.c`): `FPS`,
  `Profile`, `AI`, `Cvars`, `Console`, `Entities`, `Debug Render`, plus the
  `Editor Mode` switcher.

The pseudo-dock lives in `editor_ui.c`: `s_dock_cond` (line ~210) toggles
between `IG_Cond_Always` (force pos/size on first frame + resize frames) and
`IG_Cond_FirstUseEver`, applied to the toolbar/Brushes/Inspector
(`SetNextWindowPos/Size` at lines ~240, 586, 2349; the toggle at ~2616‑2633).
`edit_particle.c`/`edit_actor.c` use hardcoded `FirstUseEver` coords.

Frame dispatch: `ImguiLayer_Render` (`imgui_layer.c:801`) draws the dev overlays
and then calls `Editor_DrawUI` → the active mode's `draw_ui`
(`editor.c:2864/2883`). This is the single injection point for the host
dockspace.

Build: `build.zig:316-331` compiles the ImGui core + the SDL3 and SDL_GPU3
backends + `imgui_bridge.cpp` from the `imgui_dir` constant (`build.zig:116`).

## Locked decisions

| Decision | Choice | Why |
|---|---|---|
| Docking source | Vendor ImGui's official **docking branch**, pinned to the matching 1.92.x version | Same filenames + backends as current master; supported upstream; the proper fix vs a bespoke layout engine |
| Host dockspace | `DockSpaceOverViewport` with `PassthruCentralNode`, always on | Empty centre is render- and input-transparent → the 3D scene shows through, framed by docked panels |
| Default layout | Baked once via `DockBuilder` when no saved layout exists | Gives the IDE arrangement out of the box; user can rearrange freely afterward |
| Mode panels | Share dock slots by window title (left = primary list, right = inspector) | One mode active at a time; switching empties/fills the slot — no floating |
| Persistence | Existing `imgui.ini` wiring (`IG_SetIniFilename`) | Dock layout persists for free once rearranged |
| Reset | A "Reset layout" toolbar button re-runs the `DockBuilder` bake | Replaces the resize-snap hack with an explicit action |
| Multi-viewport | **Off** | Needs platform-window plumbing + complicates the single SDL_GPU swapchain; not needed |
| Transient dialogs | Stay floating (centre) | Modal-ish; docking a "Spawn Entity" dialog is odd |
| FPS / Profile HUD | Unchanged (stay floating, `NoSavedSettings`) | Intended as lightweight transient HUD |

## Architecture

### 1. Vendored swap
Replace the vendored tree with the docking branch at the matching 1.92.x
revision, in a renamed dir `sdlquake/vendor/imgui-1.92.8-docking`. Update the
single `imgui_dir` constant in `build.zig:116`; no other build change (the
docking branch ships the same `imgui.cpp`/`imgui_draw.cpp`/`imgui_tables.cpp`/
`imgui_widgets.cpp` and the `imgui_impl_sdl3` + `imgui_impl_sdlgpu3` backends).
In `IG_CreateContext` (`imgui_bridge.cpp:12`), add
`io.ConfigFlags |= ImGuiConfigFlags_DockingEnable`.

### 2. Host dockspace (passthrough)
A new shim `IG_DockSpaceOverViewport()` wraps
`ImGui::DockSpaceOverViewport(0, NULL, ImGuiDockNodeFlags_PassthruCentralNode)`.
It is called once at the top of `ImguiLayer_Render` (`imgui_layer.c:801`), before
the dev overlays and `Editor_DrawUI`. The passthrough central node renders
transparent and does not capture mouse, so the engine's 3D view (drawn behind
ImGui) is visible and interactive in the centre; docked panels frame it. Active
always — this also lets the dev overlays dock — at no cost when the centre is
empty (normal play shows the full game with only whatever panels are open).

### 3. Default layout
Baked once, via a `DockBuilder` sequence run only when `imgui.ini` has no saved
layout (first run) or on explicit reset:

```
┌────────────────────────────────────────────────┐
│  Editor (toolbar)                                │ top
├──────────┬─────────────────────────┬────────────┤
│ Brushes  │                         │ Inspector  │
│ /Particle│      3D viewport        │ /Particle  │
│  Effects │  (passthrough centre —  │  Inspector │
│ /Actor   │   scene shows through)  │            │
├──────────┴─────────────────────────┴────────────┤
│  Console · Cvars · Entities · AI · Debug Render  │ bottom (tabbed)
└──────────────────────────────────────────────────┘
  floating HUD (unchanged): FPS, Profile
  on-demand dialogs (float centre): Spawn Entity, Textures, Wrap, Light bake
```

Window→node assignment (by title): top = `Editor` (+ the `Editor Mode`
switcher, tabbed); left = `Brushes` /
`Particle Effects` / `Actor`; right = `Inspector` / `Particle Inspector`;
bottom (tabbed) = `Console`, `Cvars`, `Entities`, `AI`, `Debug Render`. Dock
nodes are keyed by window title, so the per-mode panels share the left/right
slots; switching mode empties one mode's windows (they stop drawing) and the
next mode's windows appear in the same slots.

### 4. Persistence + reset
Dock arrangement persists through the existing `imgui.ini` (the docking branch
serializes dock node state into the same file). The `DockBuilder` bake runs only
when no layout is loaded. The current resize-snap logic in
`MapMode_DrawUI` (`editor_ui.c:2616‑2633`) is replaced by an explicit
**"Reset layout"** toolbar button that calls the bake.

### 5. Removed code
- `editor_ui.c`: `s_dock_cond` and its `SetNextWindowPos/Size` forcing for the
  toolbar/Brushes/Inspector (lines ~210, 240‑241, 586‑587, 2349‑2350) and the
  resize-snap block (~2616‑2633).
- `edit_particle.c` / `edit_actor.c`: the hardcoded `FirstUseEver`
  `SetNextWindowPos/Size` calls.

These windows instead inherit their position from their assigned dock node.

### 6. New C shim surface (`imgui_bridge.cpp` + `imgui_bridge.h`)
~6 thin wrappers: `IG_DockSpaceOverViewport`, and a minimal `IG_DockBuilder*`
set used only by the bake — remove-node, add-node, set-node-size, split-node,
dock-window, finish — plus exposing the dock config flag in `IG_CreateContext`.
Dock node ids and split ratios stay inside the bridge; the bake takes window
titles as plain strings.

## Risks

- **Branch pin parity (primary risk).** Must pin a docking-branch revision whose
  backends match the current 1.92.8 (especially `imgui_impl_sdlgpu3`). Mitigation:
  swap the tree and confirm a clean build on macOS *before* writing any layout
  code. If the SDL_GPU3 backend API drifted, reconcile the backend call sites in
  `imgui_bridge.cpp` first.
- **imgui.ini location/writability.** Verify the ini path is set to a writable
  location and that an existing ini doesn't pre-empt the first-run bake (a stale
  ini from the old build has no dock data — handle gracefully).
- **Input passthrough.** Confirm the passthrough central node lets mouse events
  reach the editor's 3D picking/gizmo paths (they read mouse outside
  `IG_WantCaptureMouse`).

## Verification

Per house rules: build clean, then live smoke test with `screenshot_gpu` (the
software-framebuffer screenshot misses ImGui), launched `-nosound -nofocus`.

1. Editor opens (F8) with panels **docked**: toolbar top, Brushes left,
   Inspector right, and the map visible through the centre.
2. Drag a panel to a new edge / onto another to tab it — it re-docks.
3. Switch Map → Particle → Actor: each mode's panels fill the left/right slots,
   nothing floats.
4. "Reset layout" restores the baked default.
5. Normal play (editor closed) renders the game full-screen; F3/showperf dev
   overlays appear and can be docked.
6. Editor 3D picking + gizmo drag still work over the passthrough centre.

## Deferred / non-goals (YAGNI)

- **Multi-viewport** (panels as separate OS windows).
- **Per-mode saved layouts** — one shared layout.
- **Theme/style overhaul**, docking the transient dialogs, changing the
  FPS/Profile HUD.
- **Touch screen / gamepad nav** for docking.

# Editor Viewport Panel — Design

**Date:** 2026-06-02
**Status:** Approach A (render-to-texture) approved. Plan next.
**Scope:** In the editor, render the 3D scene into an offscreen texture and show
it inside a dockable **Viewport** window, instead of the current passthrough-hole
centre. Editor-only; normal gameplay rendering is untouched. Builds on the
ImGui-docking work (`2026-06-02-imgui-docking-layout-design.md`).

## Why

After docking, the editor's centre is a transparent hole the swapchain scene
shows through — not a real panel. The user wants the game view to be a
first-class **dockable panel**: tabbable, resizable, and correct when hidden or
tabbed behind another panel (which the passthrough hole can't do — it would
bleed).

## Current state

- `gpu_render_frame` (`vid_sdl.c:484`): a single swapchain render pass binds the
  palette-LUT pipeline, draws `gpu_fb_tex` (the uploaded 8-bit software
  framebuffer) into an integer-letterboxed viewport rect, optionally draws the
  crop overlay, then `ImguiLayer_RenderGPU` composites the panels on top. The
  editor's 3D scene is the normal engine render (free-fly camera) composited into
  `vid.buffer` → `gpu_fb_tex`.
- `gpu_create_frame_textures` (`vid_sdl.c:278`) makes the sampler textures;
  `gpu_scene_tex` will be created alongside.
- Editor input: `window_to_vid` (`editor.c:37`, exposed as `Editor_WindowToVid`)
  maps a window mouse position → vid-space via the window letterbox;
  `Editor_ScreenToRay` (`render_wire.c:58`) builds a world ray from **vid-space**
  coords (callers convert window→vid first). Picking/camera gating reads
  `IG_WantCaptureMouse()` (e.g. `editor.c:2488,2667,2674,2900`).
- ImGui can already display a `SDL_GPUTexture*` as an `ImTextureID` (the editor
  thumbnail cache, `edit_texcache.c`; bridge `IG_Image`).

## Locked decisions

| Decision | Choice | Why |
|---|---|---|
| Approach | Render-to-texture + `ImGui::Image` in a `Viewport` window | True first-class panel; correct when tabbed/hidden |
| Scene texture | `gpu_scene_tex`, swapchain format, `COLOR_TARGET\|SAMPLER`, sized `vid_render_w × vid_render_h` | Native render resolution; ImGui scales it into the panel |
| Second pass | Editor-open only: palette pass → `gpu_scene_tex`; swapchain pass becomes clear + ImGui | Normal play keeps the existing single-pass path untouched |
| Image fit | Aspect-preserved fit inside the panel content region (letterbox inside the panel) | No stretch; matches the scene's aspect |
| Input remap | Centralized in `window_to_vid`: map relative to the captured Viewport **image rect** | One function fixes picking + gizmos + everything downstream |
| Pick-over-viewport | New `Editor_MouseOverViewport()` exempts the viewport image from the `WantCaptureMouse` suppression | The scene now lives inside an ImGui window |
| Dock | Bake docks `Viewport` into the central node; drop `PassthruCentralNode` | The centre is now a real opaque panel |

## Architecture

### Render — `vid_sdl.c`
- **`gpu_scene_tex`**: created in `gpu_create_frame_textures` (or a sibling) with
  `format = gpu_swapchain_format`, `usage = COLOR_TARGET | SAMPLER`, size
  `vid_render_w × vid_render_h`. Recreated when the render size changes.
- **Editor-open branch** in `gpu_render_frame` (gated on `Editor_IsOpen()`):
  after the framebuffer upload copy pass, run an **offscreen render pass** with
  `color_target.texture = gpu_scene_tex`, viewport = the full texture (no
  letterbox), binding the existing palette pipeline + samplers + scan UBO, and
  draw the 3 vertices. `gpu_scene_tex` now holds the expanded scene. Then the
  swapchain pass does `LOADOP_CLEAR` + `ImguiLayer_RenderGPU` only — **no** scene
  palette draw, **no** crop overlay.
- **Editor-closed branch**: the current single-pass path, unchanged.
- **`VID_EditorSceneTexture()`**: returns `(void *)gpu_scene_tex` for the
  `Viewport` window, or `NULL` when not in editor / not yet created.
- Ordering on the command buffer: upload copy → offscreen scene pass →
  `ImguiLayer_PrepareGPU` (records the `Viewport` window's `Image`, referencing
  `gpu_scene_tex`) → swapchain pass (samples `gpu_scene_tex`). SDL_GPU inserts
  the inter-pass barrier.

### Viewport window — editor side
- In `Editor_DrawUI` (`editor.c:2864`, the shell, so it shows in every mode),
  `Begin("Viewport", NULL, NoScrollbar | NoScrollWithMouse)`. Query the content
  region; compute an aspect-preserved image size for `vid_render_w:vid_render_h`,
  centre it (record the centring offset), `IG_Image(VID_EditorSceneTexture(),
  w, h)`. Record the image's **screen rect** (origin + size) into a file-scope
  `s_viewport_rect` and `IG_IsItemHovered()`/window-hovered into
  `s_viewport_hovered`. If `VID_EditorSceneTexture()` is NULL, draw a placeholder
  label and leave the rect invalid.
- The bake (`ig_build_default_layout`, `imgui_bridge.cpp`) docks `Viewport` into
  the central node and the dockspace flags drop `PassthruCentralNode`.

### Input — `editor.c`
- `window_to_vid`: if `s_viewport_rect` is valid, map
  `vx = (wx - rect.x) / rect.w * vid.width`,
  `vy = (wy - rect.y) / rect.h * vid.height`; otherwise fall back to the existing
  window-letterbox math (editor closed / no viewport yet).
- `Editor_MouseOverViewport()` returns `s_viewport_hovered`.
- Replace the bare `IG_WantCaptureMouse()` checks on the editor **pick/orbit**
  path with `(IG_WantCaptureMouse() && !Editor_MouseOverViewport())`, so clicks
  on the viewport image pick the scene while clicks on real panels don't.
  `Editor_ScreenToRay` is unchanged (it already works in vid space).

### Bridge additions — `imgui_bridge.{cpp,h}`
- `IG_GetContentRegionAvail(float *w, float *h)`, `IG_GetCursorScreenPos(float
  *x, float *y)`, `IG_IsWindowHovered(void)` — to size/place the image and detect
  hover. (`IG_Image`, `IG_IsItemHovered` already exist.)

## Risks

- **Input remap correctness (primary).** Clicking a brush must select *that*
  brush. Verify live: pick near panel edges + after resizing the viewport split.
- **`WantCaptureMouse` exemption.** Must exempt only the viewport image — scroll
  / clicks over Brushes/Inspector still suppress picking. Verify both.
- **Viewport-rect frame lag.** The rect is captured during the ImGui frame and
  used by the next frame's mouse events — a one-frame lag, negligible (the rect
  only moves on resize/redock).
- **Scanlines in the offscreen pass.** The CRT scanline overlay is keyed off
  swapchain-space `gl_FragCoord.y`; in the offscreen target it maps to texture
  space. Scanlines in the editor viewport are unwanted anyway — acceptable
  (note, don't fight it).

## Verification (live, `screenshot_gpu`)

1. Open the editor: the 3D scene appears inside a **Viewport** panel; side panels
   dock around it; it can be resized and tabbed.
2. Click-pick a brush → correct selection. Gizmo drag works. RMB mouse-look
   works. (Input remap.)
3. Resize the viewport dock split → the scene scales to the panel and picking
   still lands correctly.
4. Drag a panel over / tab the Viewport behind another panel → the scene is
   hidden with no bleed; tab back → it returns.
5. Click on Brushes/Inspector → does **not** pick the scene.
6. Close the editor → normal gameplay renders exactly as before (single-pass).

## Deferred / non-goals (YAGNI)

- Re-rendering the software scene at the panel's live resolution (keep
  `vid_render_*`; scale the texture into the panel).
- Multiple viewports / split views.
- Per-viewport screenshot.
- Docking the F3 dev-overlay panels (separate optional task from the docking
  plan).

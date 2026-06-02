# Editor Viewport Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the editor, render the 3D scene into an offscreen texture and show it inside a dockable `Viewport` window, with editor mouse-picking remapped to that panel. Normal gameplay rendering is untouched.

**Architecture:** Add `gpu_scene_tex` (a color-target+sampler texture). When the editor is open, `gpu_render_frame` runs the palette-LUT pass into `gpu_scene_tex` and the swapchain pass becomes clear+ImGui-only; a `Viewport` ImGui window `Image`s the texture, docked into the central node. Editor input is remapped by making `window_to_vid` map relative to the captured viewport image rect, and exempting that image from the `WantCaptureMouse` pick-suppression. Design: `docs/superpowers/specs/2026-06-02-editor-viewport-panel-design.md`.

**Tech Stack:** C (engine + editor), C++ (ImGui bridge), SDL_GPU, Dear ImGui docking branch, Zig build.

---

## Conventions

- **No unit-test suite** (per CLAUDE.md). Each task verifies with `zig build` (exit 0) plus a live in-game check.
- **Build:** `zig build` from `/Users/wjbr/src/quake1.ai`.
- **Live test (controller-run):** launch `./zig-out/bin/quake -nosound -nofocus --mcp-http 9876 +map e1m1` in the background; drive with `python3 scripts/mcp_call.py console_exec '{"command":"editor"}'`, `... wait_frames '{"frames":30}'`, `... screenshot_gpu '{}'` (writes `screenshots/shot_*.png`; GPU source includes ImGui). `editor_mode 0|1|2` switches Map/Particle/Actor. Implementer subagents do build-only; the controller does the visual + picking verification.
- **Commits** end with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- The clang language server reports `quakedef.h`/`SDL3/SDL.h` "not found" false positives in these files — ignore; only `zig build` matters.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `sdlquake/engine/imgui_bridge.cpp` / `.h` | Modify | 4 new shims: `IG_GetContentRegionAvail`, `IG_GetCursorScreenPos`, `IG_SetCursorScreenPos`, `IG_IsWindowHovered` |
| `sdlquake/platform/vid_sdl.c` | Modify | `gpu_scene_tex` + offscreen palette pass + editor-open branch in `gpu_render_frame` + `VID_EditorSceneTexture()` |
| `sdlquake/engine/editor/editor.c` | Modify | `Viewport` window (Image + rect/hover capture), `Editor_MouseOverViewport()`, `window_to_vid` remap, `WantCaptureMouse` exemption |
| `sdlquake/engine/imgui_bridge.cpp` (`ig_build_default_layout`) | Modify | Dock `Viewport` into the centre; drop `PassthruCentralNode` |

---

## Task V1: Bridge shims for the viewport window

**Files:** Modify `sdlquake/engine/imgui_bridge.h`, `sdlquake/engine/imgui_bridge.cpp`.

- [ ] **Step 1: Declare the shims**

In `imgui_bridge.h`, after the existing `IG_GetContentRegionAvailX` declaration (search for it; it's in the "Layout queries" group), add:

```c
void  IG_GetContentRegionAvail(float *w, float *h);
void  IG_GetCursorScreenPos(float *x, float *y);
void  IG_SetCursorScreenPos(float x, float y);
int   IG_IsWindowHovered(void);   // 1 if the current window is hovered
```

- [ ] **Step 2: Implement them**

In `imgui_bridge.cpp`, in the "Layout queries" section (near `IG_GetContentRegionAvailX`), add:

```cpp
void IG_GetContentRegionAvail(float *w, float *h)
{
    ImVec2 a = ImGui::GetContentRegionAvail();
    if (w) *w = a.x;
    if (h) *h = a.y;
}
void IG_GetCursorScreenPos(float *x, float *y)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (x) *x = p.x;
    if (y) *y = p.y;
}
void IG_SetCursorScreenPos(float x, float y) { ImGui::SetCursorScreenPos(ImVec2(x, y)); }
int  IG_IsWindowHovered(void) { return ImGui::IsWindowHovered() ? 1 : 0; }
```

- [ ] **Step 3: Build**

Run `zig build`. Expected: exit 0 (functions compile, unused for now).

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine/imgui_bridge.cpp sdlquake/engine/imgui_bridge.h
git commit -m "feat(editor): bridge shims for the viewport panel

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task V2: Render the scene into a Viewport panel (display)

Adds the offscreen scene texture + the `Viewport` window that displays it, and switches the editor-open swapchain pass to clear+ImGui-only. After this task the scene shows **inside** a dockable panel. Picking is NOT yet remapped (that's V3) — clicking to select will be off until then; that's expected.

**Files:** Modify `sdlquake/platform/vid_sdl.c`, `sdlquake/engine/editor/editor.c`, `sdlquake/engine/imgui_bridge.cpp`.

- [ ] **Step 1: Add the scene texture globals + creation helper (`vid_sdl.c`)**

Near the other GPU texture globals (around `vid_sdl.c:96-101`), add:

```c
static SDL_GPUTexture          *gpu_scene_tex     = NULL;
static int                      gpu_scene_w       = 0;
static int                      gpu_scene_h       = 0;
```

After `gpu_create_frame_textures` (ends at `vid_sdl.c:302`), add a sibling:

```c
// Offscreen colour target the editor's Viewport panel samples. Same format as
// the swapchain so the palette pipeline (built for gpu_swapchain_format) can
// render into it; COLOR_TARGET so it's a render target, SAMPLER so ImGui can
// Image() it.
static void gpu_ensure_scene_tex(int w, int h) {
    if (gpu_scene_tex && w == gpu_scene_w && h == gpu_scene_h) return;
    if (gpu_scene_tex) SDL_ReleaseGPUTexture(gpu_device, gpu_scene_tex);
    SDL_GPUTextureCreateInfo ti = {0};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = gpu_swapchain_format;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width                = (Uint32)w;
    ti.height               = (Uint32)h;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.sample_count         = SDL_GPU_SAMPLECOUNT_1;
    gpu_scene_tex = SDL_CreateGPUTexture(gpu_device, &ti);
    gpu_scene_w = w;
    gpu_scene_h = h;
}

// ImTextureID for the editor Viewport panel (NULL when not yet rendered).
void *VID_EditorSceneTexture(void) { return (void *)gpu_scene_tex; }
```

- [ ] **Step 2: Declare the externs the render path needs (`vid_sdl.c`)**

Near the top of `vid_sdl.c` with the other externs (e.g. by `ImguiLayer_PrepareGPU` at line 481), add:

```c
extern int Editor_IsOpen(void);
```

- [ ] **Step 3: Run the offscreen scene pass + branch the swapchain pass (`vid_sdl.c`)**

In `gpu_render_frame`, immediately after the framebuffer upload copy pass + `Perf_PopScope();` (after `vid_sdl.c:535`, before `ImguiLayer_PrepareGPU(cmd);`), insert the offscreen pass:

```c
    int editor_open = Editor_IsOpen();
    if (editor_open) {
        gpu_ensure_scene_tex(vid_render_w, vid_render_h);
        SDL_GPUColorTargetInfo sct = {0};
        sct.texture  = gpu_scene_tex;
        sct.load_op  = SDL_GPU_LOADOP_CLEAR;
        sct.store_op = SDL_GPU_STOREOP_STORE;
        sct.clear_color.a = 1.0f;
        SDL_GPURenderPass *spass = SDL_BeginGPURenderPass(cmd, &sct, 1, NULL);
        SDL_BindGPUGraphicsPipeline(spass, gpu_pipeline);
        SDL_GPUViewport svp = {0};
        svp.w = (float)vid_render_w; svp.h = (float)vid_render_h; svp.max_depth = 1.0f;
        SDL_SetGPUViewport(spass, &svp);
        SDL_GPUTextureSamplerBinding sb[3];
        sb[0].texture = gpu_fb_tex;         sb[0].sampler = gpu_sampler;
        sb[1].texture = gpu_palette_id_tex; sb[1].sampler = gpu_sampler;
        sb[2].texture = gpu_palette_tex;    sb[2].sampler = gpu_sampler;
        SDL_BindGPUFragmentSamplers(spass, 0, sb, 3);
        struct { float intensity, size, supersample, pad1; } sp =
            { 0.0f, 1.0f, (float)vid_supersample_active, 0.0f };   // no scanlines in the editor
        SDL_PushGPUFragmentUniformData(cmd, 0, &sp, sizeof(sp));
        SDL_DrawGPUPrimitives(spass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(spass);
    }
```

Then wrap the swapchain pass's **scene draw** so it's skipped when the editor is open. The scene draw is from the pipeline bind through the crop overlay (`vid_sdl.c:573` `SDL_BindGPUGraphicsPipeline(pass, gpu_pipeline);` through the end of the crop `if (Crop_Active()) { … }` block at `:649`). Wrap exactly that span:

```c
    if (!editor_open) {
        SDL_BindGPUGraphicsPipeline(pass, gpu_pipeline);
        /* …existing letterbox viewport, sampler bind, scan UBO, DrawGPUPrimitives,
           and the entire `if (Crop_Active()) { … }` block, unchanged… */
    }

    ImguiLayer_RenderGPU(cmd, pass);
```
(The `SDL_BeginGPURenderPass`/`color_target` with `LOADOP_CLEAR` at `:565-571` stays as-is — it clears the swapchain; when `editor_open`, only ImGui then draws into it. `ImguiLayer_RenderGPU(cmd, pass)` at `:651` stays.)

- [ ] **Step 4: Add the Viewport window + rect/hover capture (`editor.c`)**

Near the top-of-file statics in `editor.c` (after `window_to_vid` / by `s_open` at line 62), add:

```c
// Viewport panel: the editor's 3D scene drawn as an ImGui Image. The image's
// screen rect (set each frame by draw_viewport_window) is what window_to_vid
// maps mouse coords into; s_viewport_hovered exempts it from WantCaptureMouse.
extern void *VID_EditorSceneTexture(void);
static float s_viewport_rect[4] = { 0, 0, 0, 0 };  // x, y, w, h (screen px)
static int   s_viewport_valid   = 0;
static int   s_viewport_hovered = 0;

static void draw_viewport_window(void)
{
    if (!IG_Begin("Viewport", NULL, IG_WF_NoScrollbar | IG_WF_NoScrollWithMouse)) {
        s_viewport_valid = 0;
        IG_End();
        return;
    }
    void *tex = VID_EditorSceneTexture();
    float availw = 0, availh = 0;
    IG_GetContentRegionAvail(&availw, &availh);
    if (tex && availw > 1.0f && availh > 1.0f) {
        float aspect = (float)vid.width / (float)vid.height;
        float iw = availw, ih = availw / aspect;
        if (ih > availh) { ih = availh; iw = availh * aspect; }
        float bx = 0, by = 0;
        IG_GetCursorScreenPos(&bx, &by);
        float ox = (availw - iw) * 0.5f, oy = (availh - ih) * 0.5f;
        IG_SetCursorScreenPos(bx + ox, by + oy);
        IG_Image(tex, iw, ih);
        s_viewport_rect[0] = bx + ox; s_viewport_rect[1] = by + oy;
        s_viewport_rect[2] = iw;      s_viewport_rect[3] = ih;
        s_viewport_valid   = 1;
        s_viewport_hovered = (IG_IsItemHovered() || IG_IsWindowHovered());
    } else {
        IG_TextUnformatted("(no scene)");
        s_viewport_valid   = 0;
        s_viewport_hovered = 0;
    }
    IG_End();
}
```

Then call it from `Editor_DrawUI` (`editor.c:2864`). Add the call right after the `Editor Mode` window's `IG_End();` (line 2881) and before `if (m->draw_ui) m->draw_ui();`:

```c
    IG_End();

    draw_viewport_window();

    if (m->draw_ui) m->draw_ui();
```

- [ ] **Step 5: Dock the Viewport into the centre; drop passthrough (`imgui_bridge.cpp`)**

In `ig_build_default_layout`, after the `right` split and before `DockBuilderFinish`, dock the viewport into the central node (`centre` is the leftover central node id):

```c
    ImGui::DockBuilderDockWindow("Viewport", centre);
```
And change the `DockBuilderAddNode` flags to drop passthrough (the centre is now an opaque panel):

```c
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
```
(was `… | ImGuiDockNodeFlags_PassthruCentralNode`). Leave the `IG_HostDockSpace` `DockSpaceOverViewport(…, PassthruCentralNode)` call as-is — a passthrough host with the centre filled by the Viewport window is fine; only the *baked node* flag changes.

- [ ] **Step 6: Build**

Run `zig build`. Expected: exit 0. If `gpu_swapchain_format`, `gpu_pipeline`, `gpu_sampler`, `vid_render_w/h`, or `vid_supersample_active` aren't visible at the insertion point, they're file-scope statics in `vid_sdl.c` already used by `gpu_render_frame` — confirm the new code is inside that file (it is).

- [ ] **Step 7: Live verify (controller)**

Launch, `editor`, screenshot. Expected: the 3D scene now appears **inside a Viewport panel** in the centre (with the side panels docked around it), not as a bare hole. Switch `editor_mode 1` / `2` — the particle/actor previews appear in the Viewport panel too. The scene should look correct (right colors, not stretched/garbled). **Picking is expected to be wrong here** — that's V3. If the scene is garbled/blank, debug the offscreen pass (Step 3) before continuing.

- [ ] **Step 8: Commit**

```bash
git add sdlquake/platform/vid_sdl.c sdlquake/engine/editor/editor.c sdlquake/engine/imgui_bridge.cpp
git commit -m "feat(editor): render the 3D scene into a dockable Viewport panel

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task V3: Remap editor input to the Viewport panel

Makes mouse-picking/orbit work inside the panel: `window_to_vid` maps relative to the captured viewport image rect, and the viewport image is exempted from `WantCaptureMouse` pick-suppression.

**Files:** Modify `sdlquake/engine/editor/editor.c`.

- [ ] **Step 1: Map `window_to_vid` to the viewport rect**

Replace the body of `window_to_vid` (`editor.c:37-57`) so it maps relative to the viewport image rect when valid, falling back to the window letterbox otherwise:

```c
static void window_to_vid(float wx, float wy, float *vx, float *vy)
{
    // When the editor's Viewport panel is showing, the scene lives inside that
    // image rect — map relative to it. (s_viewport_rect / s_viewport_valid are
    // set each frame by draw_viewport_window.)
    if (s_viewport_valid && s_viewport_rect[2] > 1.0f && s_viewport_rect[3] > 1.0f) {
        *vx = (wx - s_viewport_rect[0]) / s_viewport_rect[2] * (float)vid.width;
        *vy = (wy - s_viewport_rect[1]) / s_viewport_rect[3] * (float)vid.height;
        return;
    }

    SDL_Window *w = VID_GetWindow();
    int ww = 320, wh = 200;
    if (w) SDL_GetWindowSize(w, &ww, &wh);
    if (ww <= 0 || wh <= 0) { *vx = wx; *vy = wy; return; }
    float scale_x = (float)ww / (float)vid.width;
    float scale_y = (float)wh / (float)vid.height;
    float scale   = scale_x < scale_y ? scale_x : scale_y;
    float ox = (ww - vid.width  * scale) * 0.5f;
    float oy = (wh - vid.height * scale) * 0.5f;
    *vx = (wx - ox) / scale;
    *vy = (wy - oy) / scale;
}
```
(`window_to_vid` is defined at line 37, above the `s_viewport_*` statics added in V2 Step 4 — those statics live near line 62, which is *after* line 37. So **move the `s_viewport_rect`/`s_viewport_valid`/`s_viewport_hovered` static declarations up to just above `window_to_vid`** (before line 36) so they're in scope here. Keep `draw_viewport_window` where it is — it's after the statics regardless.)

- [ ] **Step 2: Expose `Editor_MouseOverViewport` + a capture helper**

Just below `Editor_WindowToVid` (`editor.c:60`), add:

```c
int  Editor_MouseOverViewport(void) { return s_viewport_hovered; }

// True when ImGui owns the mouse for a real panel — i.e. captured but NOT over
// the Viewport image (where clicks should pick/orbit the scene).
static int editor_view_captured(void)
{
    return IG_WantCaptureMouse() && !s_viewport_hovered;
}
```

- [ ] **Step 3: Apply the exemption at the four gate sites**

In `editor.c`, replace `IG_WantCaptureMouse()` with the helper at exactly these pick/orbit sites:

- Line 2488: `if (rmb && !s_orbit_prev_rmb && IG_WantCaptureMouse()) rmb = 0;`
  → `if (rmb && !s_orbit_prev_rmb && editor_view_captured()) rmb = 0;`
- Line 2667: `if (IG_WantCaptureMouse()) return 0;`
  → `if (editor_view_captured()) return 0;`
- Line 2674: `if (IG_WantCaptureMouse()) return 0;`
  → `if (editor_view_captured()) return 0;`
- Line 2900: `if (ev->type == SDL_EVENT_MOUSE_WHEEL && m == &particle_mode && !IG_WantCaptureMouse())`
  → `… && !editor_view_captured())`

(Do NOT change any other `IG_WantCaptureMouse` uses outside the editor.)

- [ ] **Step 4: Build**

Run `zig build`. Expected: exit 0.

- [ ] **Step 5: Live verify (controller + user eyes)**

Launch, `editor`. Verify:
- **Picking:** click a brush/face in the Viewport → that brush selects (inspector updates). Pick again after dragging the dock split to resize the viewport — still lands correctly.
- **Camera:** RMB-drag in the viewport enters look-mode and rotates; particle/actor mode RMB-orbit + wheel-zoom work over the viewport.
- **Panels still suppress:** clicking/scrolling over Brushes/Inspector does NOT pick or move the camera.
- **Normal play:** close the editor → gameplay renders exactly as before.
Capture `screenshot_gpu` for the docked layout; picking/drag confirmation is by the user (MCP can't drag).

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine/editor/editor.c
git commit -m "feat(editor): remap mouse picking to the Viewport panel rect

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:**
- Render-to-texture + Image in a Viewport window → V2. ✓
- `gpu_scene_tex` color-target+sampler, swapchain format, render-res → V2 Step 1. ✓
- Editor-only second pass; swapchain clear+ImGui only when open; normal play single-pass → V2 Steps 2-3. ✓
- `VID_EditorSceneTexture()` → V2 Step 1. ✓
- Aspect-preserved fit + centred image → V2 Step 4 (`draw_viewport_window`). ✓
- Dock Viewport centre; drop passthrough node → V2 Step 5. ✓
- Input remap centralized in `window_to_vid` → V3 Step 1. ✓
- `Editor_MouseOverViewport()` + `WantCaptureMouse` exemption at the four sites → V3 Steps 2-3. ✓
- Bridge shims → V1. ✓
- YAGNI (no panel-res re-render, no multi-viewport) — honored (scene_tex is render-res, single viewport). ✓

**Placeholder scan:** No TBD/TODO. The one prose instruction (V2 Step 3 "wrap the existing scene-draw span") names exact line bounds (`:573`–`:649`) and what to wrap; acceptable since duplicating ~80 lines of unchanged GPU code verbatim would be error-prone — the bounds are explicit.

**Consistency:** `s_viewport_rect`/`s_viewport_valid`/`s_viewport_hovered` are declared in V2 (Step 4) and V3 Step 1 notes they must sit above `window_to_vid` (line 37) — flagged explicitly so the V2-added statics are relocated correctly. `editor_view_captured` / `Editor_MouseOverViewport` / `VID_EditorSceneTexture` names match across tasks. `gpu_scene_tex`/`gpu_scene_w`/`gpu_scene_h` consistent.

**Ordering note for the implementer:** because V3 Step 1 needs the `s_viewport_*` statics above `window_to_vid`, the cleanest is to declare them near the top (before line 36) in V2 Step 4 already. Either placement builds; V3 Step 1 calls it out so it isn't missed.

## Risks (from spec)
- **Input remap correctness (primary):** verified in V3 Step 5 by actually clicking brushes. If picks are offset, check that `s_viewport_rect` holds the *image* rect (post-centring) and that `vid.width/height` is the right coordinate space (it's the same space the old `window_to_vid` used).
- **Offscreen render correctness:** verified in V2 Step 7 (scene must look right in the panel). If garbled, the palette shader's coordinate basis vs the full-texture viewport is the place to look.
- **WantCaptureMouse exemption scope:** verified in V3 Step 5 (panels still suppress; viewport doesn't).

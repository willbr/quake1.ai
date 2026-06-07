# Render pipeline (SDL_GPU palette-LUT shader)

_Extracted from CLAUDE.md (reference detail; CLAUDE.md keeps a summary + pointer here)._

### Render pipeline (SDL_GPU palette-LUT shader)

Quake's software renderer writes 8-bit palette indices into `vid.buffer`
and per-pixel palette-slot ids into `vid_palette_id` (for Doom
weapon overlays). Every frame `vid_sdl.c::gpu_render_frame` uploads both
buffers as `R8_UINT` GPU textures, plus a 3×256 RGBA8 LUT, and a
fullscreen-triangle pipeline running `shaders/palette.{vert,frag}.glsl`
does the per-pixel `dst = palette[palette_id[px] * 256 + framebuffer[px]]`
lookup on the GPU. The CPU-side `palette_expand` loop (≈5 ms/frame at
3x scale) is gone.

Shaders compile at build time via `tools/build_shaders.zig` (a host Zig
tool run from `build.zig` — replaced the old `build_shaders.sh` so the
build needs no `/bin/sh`; on Windows `glslang` is scoop-installable):
glslangValidator GLSL → SPIR-V, then spirv-cross SPIR-V → MSL with
`--flip-vert-y`, embedded as C arrays in a generated `palette_shaders.h`.
When `spirv-cross` is absent (e.g. a Windows-only checkout) the MSL
strings are emitted empty — the Vulkan/SPIR-V path is unaffected;
regenerate on a macOS checkout (`brew install spirv-cross`) to fill in
the Metal path. The vendored-header drift guard (`bspfile.h`/`cmdlib.h`/
`mathlib.h` across vendor/{qbsp,light,vis}) is likewise pure Zig now —
`checkVendorHeaders` inline in `build.zig`, no shell script.
SPIR-V serves Vulkan on Linux *and* Windows
(SDL_GPU picks the Vulkan backend when the requested shader formats
are SPIRV|MSL and the system has Vulkan drivers — true for any
Intel HD 4th gen / NVIDIA Kepler / AMD GCN or later, i.e. effectively
every Windows GPU since 2014). MSL source string is compiled at
runtime by Metal on macOS. A DXIL pipeline would let SDL_GPU pick
D3D12 instead of Vulkan on Windows; it's a fallback for very old
hardware without Vulkan drivers, not a blocker.

ImGui composites through the `imgui_impl_sdlgpu3` backend in the same
render pass. The editor's texture-thumbnail cache (`edit_texcache.c`)
stores SDL_GPUTextures referenced from ImGui via raw `SDL_GPUTexture*`
as `ImTextureID`. Window→logical mouse coords go through
`VID_WindowToLogical`, which reproduces the integer-scale letterbox
math used at present time.

The editor UI uses **ImGui docking** (vendored docking branch under
`sdlquake/vendor/imgui-1.92.8-docking/`, `ConfigFlags |=
DockingEnable`): `IG_HostDockSpace` (`imgui_bridge.cpp`) submits a
host `DockSpaceOverViewport`; a `DockBuilder` default layout (toolbar
top, Brushes left, Inspector right) bakes on first run and via the
toolbar's "Reset layout" button, persisting through `imgui.ini`. The
3D scene is its **own dockable `Viewport` panel**: when the editor or
the F3 dev overlay is open (`scene_in_panel`) `gpu_render_frame` runs
the palette pass into an offscreen `gpu_scene_tex`
(`VID_EditorSceneTexture`) and the swapchain pass becomes
clear+ImGui-only, while the `Viewport` window `IG_Image`s the texture;
editor mouse-picking maps through that panel's image rect in
`window_to_vid` (exempted from `WantCaptureMouse` via
`Editor_MouseOverViewport`). Normal gameplay keeps the single-pass
direct-to-swapchain path. The **F3 dev overlay** shares the same
dockspace — Console/Cvars/Entities/AI/Debug/Profile dock into a bottom
strip (FPS stays a floating HUD) and the running game shows in the
Viewport panel; **clicking it plays the game** (`s_vp_play` /
`Editor_ViewportPlay*` grab the mouse and route input through the
`Editor_AllowGameInput` / `Editor_LookmodeActive` gates; Esc releases).
Design: `docs/superpowers/specs/
2026-06-02-imgui-docking-layout-design.md` +
`2026-06-02-editor-viewport-panel-design.md`.

The CRT scanline overlay (`vid_scanlines` / `vid_scanline_intensity` /
`vid_scanline_size`) is implemented inside `palette.frag.glsl`: a
fragment UBO at set=3 binding=0 carries `(intensity, size)`, and the
shader darkens every other `size`-pixel band of `gl_FragCoord.y`
(swapchain space, so bands stay locked to the physical pixel grid).

The crop-screenshot dim+border overlay is drawn by a second SDL_GPU
pipeline (`gpu_rect_pipeline` + `shaders/rect_overlay.{vert,frag}.glsl`)
inside the same render pass, with standard alpha blending; the rect
fragment shader emits border / discard / dim per pixel from a UBO at
set=3 binding=0. Rect coords are stored in super-pixel space (g.w/g.h)
and scaled by `vid_supersample_active` during mouse-event handling so
ss>1 selects the correct slab of the frozen framebuffer.

Known migration TODOs: DXIL bytecode would let SDL_GPU pick D3D12 on
Windows as an alternative to Vulkan; nothing else outstanding.


# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires Zig (tested on 0.14.1 and 0.16). Shareware assets are committed loose under `id1/` (extracted from the freely-redistributable `pak0.pak`); registered episodes 2–4 are not — if you have a `pak1.pak`, drop it alongside and the engine will pick it up. We extract rather than ship a pak because Quake's filesystem prefers paks over loose files, which silently shadows local customizations in `id1/default.cfg`, `id1/quake.rc`, etc. SDL3 is vendored per-OS — no system install needed. Phase 6 Doom/Wolf3D guns extract automatically from committed reference WADs (`ref/doom-data/DOOM1.WAD`, `ref/wolf3d-data/VSWAP.WL1`) on first build; outputs are gitignored and regenerated as needed (`rm id1/progs/v_doom*.spr` to force re-extraction).

Supported hosts: **Windows x64** (vendored `SDL3.dll` + `.lib` under `sdlquake/vendor/SDL3-3.4.8/lib/x64/`) and **macOS arm64** (vendored `libSDL3.0.dylib` under `…/lib/macos/`). Linux is untested but the build paths in `build.zig` fall through to system SDL3 via `linkSystemLibrary`.

```sh
zig build run
zig build run -- +map e1m1
```

On macOS, `build.zig` runs `install_name_tool` on the executable after install to rewrite its Homebrew-derived `LC_LOAD_DYLIB` entry to `@rpath/libSDL3.0.dylib`; the binary's `@executable_path` rpath then finds the vendored dylib alongside it. No `DYLD_LIBRARY_PATH` needed.

No test suite exists yet. Build success and visual/audio correctness in-game are the verification methods.

## Architecture

This project is a port of the original WinQuake (1996 software renderer) from Win32/DirectX to SDL3, using Zig as the build system. The engine source has been forked into `sdlquake/engine_src/` so we can patch it as needed; the platform layer is fully replaced.

### Source split

- `sdlquake/engine_src/` — forked WinQuake engine source. We own and edit this. Compare against `ref/Quake-master/WinQuake/` for the pristine upstream baseline.
- `sdlquake/platform/` — SDL3 platform layer.
- `sdlquake/vendor/SDL3-3.4.8/` — vendored SDL3 headers + pre-built shared libraries per OS (`lib/x64/SDL3.dll`+`SDL3.lib` for Windows, `lib/macos/libSDL3.0.dylib` for Apple Silicon). The macOS dylib's install_name is set to `@rpath/libSDL3.0.dylib`.
- `sdlquake/mcp/` — MCP server (Phase 2, complete).
- `sdlquake/engine/` — engine-side hot-reload + ImGui glue (Phase 3 / 4).
- `sdlquake/game/` — hot-reloadable game DLL source (Phase 3).

### Platform layer files

| File | Replaces | Role |
|---|---|---|
| `sys_sdl.c` | `sys_win.c` | `main()`, timing (`SDL_GetTicksNS`), `Sys_Error`, `Sys_SendKeyEvents` |
| `vid_sdl.c` | `vid_win.c` | `SDL_Window` + `SDL_Renderer`; 8-bit framebuffer → `SDL_Texture` → present |
| `in_sdl.c` | `in_win.c` | SDL event polling; scancode → Quake key mapping; relative mouse |
| `snd_sdl.c` | `snd_win.c` | `SDL_AudioStream` get-callback feeding Quake's DMA ring buffer |
| `net_sdl.c` | `net_wins.c` | Winsock net stub |
| `winquake.h` | `winquake.h` | Stub: replaces DirectDraw/DirectSound types with no-op equivalents; shadows the original |

`sdlquake/platform/` is on the include path **before** `sdlquake/engine_src/`, so our `winquake.h` shadows the original.

### Build flags

Engine files (`sdlquake/engine_src/*.c`) are compiled with `-std=gnu89 -fcommon -fno-sanitize=undefined`. The `-fno-sanitize=undefined` is intentional — the original engine relies on float→int truncation UB that is well-defined on x86.

Platform files (`sdlquake/platform/*.c`) omit `-std=gnu89` (they're written in modern C).

### Key fixes applied to get Phase 1 working

- `d_surf.c:143` — `surfcache_t` alignment changed from 4→8 bytes for x64 pointer members.
- `host.c` — `S_Init()` gated behind `#ifdef SDLQUAKE` guard (was `#ifndef _WIN32`).
- `in_sdl.c` `IN_Move` — calls `V_StopPitchDrift()` every frame, not just on mouse movement, to prevent re-arming after `v_centermove` seconds of walking.
- `vid_sdl.c` — `vid.conbuffer` points to the same buffer as `vid.buffer`; `Draw_Character` writes to `conbuffer`.

### Render pipeline (SDL_GPU palette-LUT shader)

Quake's software renderer writes 8-bit palette indices into `vid.buffer`
and per-pixel palette-slot ids into `vid_palette_id` (for Doom/Wolf3D
weapon overlays). Every frame `vid_sdl.c::gpu_render_frame` uploads both
buffers as `R8_UINT` GPU textures, plus a 3×256 RGBA8 LUT, and a
fullscreen-triangle pipeline running `shaders/palette.{vert,frag}.glsl`
does the per-pixel `dst = palette[palette_id[px] * 256 + framebuffer[px]]`
lookup on the GPU. The CPU-side `palette_expand` loop (≈5 ms/frame at
3x scale) is gone.

Shaders compile at build time via `scripts/build_shaders.sh`
(glslangValidator GLSL → SPIR-V, then spirv-cross SPIR-V → MSL with
`--flip-vert-y`), embedded as C arrays in a generated
`palette_shaders.h`. SPIR-V serves Vulkan; MSL source string is
compiled at runtime by Metal. DXIL for D3D12 is a TODO.

ImGui composites through the `imgui_impl_sdlgpu3` backend in the same
render pass. The editor's texture-thumbnail cache (`edit_texcache.c`)
stores SDL_GPUTextures referenced from ImGui via raw `SDL_GPUTexture*`
as `ImTextureID`. Window→logical mouse coords go through
`VID_WindowToLogical`, which reproduces the integer-scale letterbox
math used at present time.

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

Known migration TODOs: DXIL bytecode for Windows D3D12.

### MCP server (Phase 2)

`sdlquake/mcp/mcp_server.c` — JSON-RPC 2.0. A background thread reads requests and pushes to a mutex-protected queue; the main loop calls `MCP_Frame()` each frame to drain and respond, so all game-state access stays on the main thread.

Two transports:
- `--mcp-stdio` — stdio (Claude Code spawns the process; see `.mcp.json`).
- `--mcp-http <port>` — HTTP/SSE on `localhost:<port>` (attach Claude Code to an already-running game session).

Tools include `get_player_state`, `list_entities`, `set_cvar`, `console_exec`, and `screenshot` (writes sandboxed to `screenshots/`). `scripts/mcp_call.py` is a one-shot CLI against the HTTP transport.

### Hot-reload game DLL (Phase 3)

`sdlquake/engine/hotreload.c` + `sdlquake/game/` — `game_api_t` ABI separates game logic from the engine. `HotReload_Init()` loads `zig-out/bin/game.dll` once at startup. With `--hot-reload`, `HotReload_Frame()` then polls the DLL's mtime every ~1 s; on change it copies the DLL to `game_loaded.dll` (so zig can overwrite the original), unloads the old copy, loads the new one, and calls `game_api->init()` again. Without the flag, polling is off — the DLL is loaded once and stays put. The fast-iteration workflow is `zig build run -- --hot-reload` in one terminal + `zig build game` in another.

`game_api.h` defines two vtable structs:
- `engine_api_t` — functions the engine exposes (Con_Print, Cvar_SetValue, Cvar_VariableValue, Sys_FloatTime)
- `game_api_t` — functions the DLL exposes (version, init, shutdown, server_frame)

Bump `GAME_API_VERSION` in `game_api.h` whenever the struct layout changes; the loader rejects mismatched DLLs.

### Build commands

```sh
zig build run -- +map e1m1               # build everything (engine + game.dll) and run
zig build run -- --hot-reload +map e1m1  # same, but enable game.dll auto-reload polling
zig build game                           # rebuild only game.dll (fast hot-reload iteration; pair with --hot-reload above)
```

### Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | ✅ done | SDL3 port + Zig build |
| 2 | ✅ done | MCP server |
| 3 | ✅ done | Hot-reload (`game_api_t` ABI, `game.dll`) |
| 4 | ✅ done | Dear ImGui dev overlay |
| 5 | ✅ done | QuakeC → C (port progs to hot-reloadable game.dll) |
| 6 | ✅ done | Port Wolf3D & Doom1 guns into Quake (sprites, sounds, behaviour) |
| 7 | ✅ done | In-game 3D map editor |
| 8 | M3–M6 done; M7 stub | Immersive-sim systems (physics, reactive AI, wind/smoke, light tier, Blink + Gust) |

### Phase 8 references

- Design spec: `docs/superpowers/specs/2026-05-04-immersive-sim-systems-design.md`
- M1+M2+M2.5 plan: `docs/superpowers/plans/2026-05-04-immersive-sim-m1-m2-ai-substrate.md`
- M7 design + skeleton: `docs/superpowers/plans/2026-05-14-phase8-m7-bespoke-level.md` and `id1/maps/m7_skeleton.map`
- All sim code lives in `sdlquake/game/sim/` inside the hot-reloadable `game.dll` (Approach 1 from the spec).
- `engine_api_t` ABI bumps in Phase 8: 16 → 17 (M3 added `button3`/`button4` in `entvars_t`), 17 → 18 (M5 added `Sample_Lightmap`), 18 → 19 (cached-lightmap-deltas: added `Lightmap_AddDelta` + `Lightmap_ClearOwner`), 19 → 20 (decals: `entvars_t._phase6_pad` replaced by `decal_on_bounce` flag), 20 → 21 (M4 visible-smoke: added `SV_Smoke`). Current `GAME_API_VERSION` is 21.

### Phase 8 milestones (2026-05-14)

| M | Status | What it adds |
|---|---|---|
| M1 | ✅ | Stimulus bus + sense filter |
| M2 | ✅ | AI FSM (IDLE/SUSPICIOUS/SEARCHING/COMBAT) with stand-and-sweep search |
| M2.5 | ✅ | Navmesh bake + A* path-driven SEARCHING |
| M3 | ✅ | `abilities.c`: Blink (hold-aim/release-commit, grate-pass) + Gust (cone push, prop kick + STIM_SOUND). `func_grate` entity. `+blink`/`+gust` cmds through new `clc_move` bits → `button3`/`button4`. q/f default binds. |
| M4 | ✅ | `sim_wind.c`: voxel grid (≤64³ cells), semi-Lagrangian smoke advection, Gust impulse + clear, `info_wind_source` + `misc_smokegrenade` entities, `Wind_PathOcclusion` folded into AI sight LOS. |
| M5 | ✅ | `sim_light.c`: `engine_api->Sample_Lightmap` (reuses `R_LightPoint`), `Light_TierAt` thresholds at 128, Gust extinguishes flammable lights via DLL-side override table. |
| M6 | ✅ | `sim_retrofit.c`: id1 maps auto-get patrol routes from nearby navmesh points at level init. |
| M7 | 🚧 skeleton | `id1/maps/m7_skeleton.map` exercises every Phase 8 system in one room; three-area layout + playtest is deferred content work. |

### Sim module map (`sdlquake/game/sim/`)

All Phase 8 sim systems live inside the hot-reloadable `game.dll` and share `sim.h`:

- `sim_main.c` — frame entry: `Sim_Frame` orders stimulus → AI → wind → light each tick.
- `sim_arena.c` — bump arena for per-tick allocations (paths, candidate lists); cleared each frame.
- `sim_stimulus.c` — M1 stimulus bus (sound/sight/damage events).
- `sim_ai.c` — M2/M2.5 FSM brains, path-following SEARCHING.
- `sim_nav.c` — navmesh bake from BSP, A* pathfinder, in-game debug overlay (`sim_nav_debug` cvar).
- `sim_wind.c` — M4 voxel wind grid + smoke advection; `Wind_PathOcclusion` feeds AI LOS.
- `sim_light.c` — M5 light-tier sampling via `engine_api->Sample_Lightmap`; Gust-extinguishable lights table.
- `sim_retrofit.c` — M6 patrol-route auto-wiring for id1 maps.

### Editor module map (`sdlquake/engine/editor/`, Phase 7)

The in-game 3D editor is engine-side (not in `game.dll`) so it can touch `cl.worldmodel`, BSP loaders, and the framebuffer directly:

- `editor.c` / `editor.h` — public entry points, mode toggle, frame tick.
- `editor_ui.c`, `editor_classlist.c` — ImGui panels (inspector, entity browser).
- `edit_scene.c` — in-memory editable scene; the live brush/entity graph the editor mutates.
- `edit_history.c` — undo/redo stack.
- `edit_texcache.c` — texture-name pool shared by brush faces.
- `gizmo.c` — translate/resize gizmos with surface-snap support.
- `collide.c` — picking + ray casts against the live scene.
- `render_wire.c`, `render_flat.c`, `render_tex.c` — three overlay render modes.
- `brush_compile.c`, `map_io.c` — `.map` ↔ in-memory scene; `editor_compile_export` writes `.bsp` + `.lit`.
- `light_bake_thread.c` — async progressive light baking on a worker thread.

## Perf instrumentation

`sdlquake/engine/perf.{c,h}` — scoped per-frame timers feeding both a live
overlay graph and an offline capture format.

- `PERF_SCOPE("name") { ... }` wraps a block; `Perf_PushScope`/`PopScope` for
  free-form pairs. Names must be string literals (stored by pointer).
- Engine side: `_Host_Frame` (root), `input`, `Cbuf_Execute`, `server`,
  `SV_Physics.edicts`, `dll_overlays`, `SCR_UpdateScreen`, `V_RenderView`
  (with `R_SetupFrame` / `R_EdgeDrawing` / `R_RenderWorld` / `R_ScanEdges` /
  `R_DrawBEntities` / `R_DrawEntitiesOnList` / `R_DrawParticles` /
  `R_DrawViewModel_2D` children), `VID_Update` (with `palette_expand` /
  `SDL_present` children), `S_Update`, `ImguiLayer_Render`,
  `CL_ReadFromServer`.
- Game DLL side (across the ABI via `eng->Perf_PushScope` —
  `GAME_API_VERSION` bumped to 26): `game_dll.start_frame` →
  `Sim_Frame` (→ `Sim_Retrofit`, `Sim_AI`, `Wind`, `Sim_Arena`),
  `Spike_GibPathScan`, `Missile_SmokeWake`, `StartFrame`. Use the
  `SIM_PERF("name")` macro inside `sdlquake/game/` to add more.
- **Live overlay during play**: `showperf 1` (cvar) renders the Perf panel
  as a non-interactive HUD over the running game. F3 is no use here — it
  pauses physics, so the graph would freeze. The F3 dev overlay still
  contains the same Perf panel for inspection when paused is fine.
- The panel shows smoothed FPS, last-256-frames frametime sparkline, and a
  flame graph of the most recent frame (hover for ms / start / depth).
- Console: `profile <n>` captures `n` frames to `profiles/perf_<ts>.json`
  (Chrome trace, drop into chrome://tracing or speedscope.app) plus
  `perf_<ts>_summary.json` (per-scope avg/p50/p95/max/calls).
- `scripts/perf_diff.py old_summary.json new_summary.json` diffs two
  captures for regression tracking.

Captures can be loaded back into the live flame graph: pick a file from
the Profile window's "Source" combo (or `perf_replay <path>` from the
console). The frametime histogram above the flame graph colours each
frame green / yellow / red by ms — click the worst bar to jump straight
to that frame. The FPS sparkline stays live so you can compare against
real-time engine load.

Below the flame graph is the **aggregate table** — per-scope totals over
the whole capture (replay mode) or the live frame ring (last 256 frames):
columns are `scope / calls / total ms / avg ms / max ms / % window`.
Sort combo above the table cycles total / avg / max / calls / name. Use
it to find scopes that are individually cheap but consistently expensive
(e.g. `palette_expand` at 5 ms × 60 fps = 30% of frame time, which
wouldn't stand out in any single frame's flame graph).

## Reference data

- `id1/` — Quake PAK files at repo root (required at runtime)
- `ref/doom-data/` — Doom 1.9 shareware WAD (read by `zig build extract`)
- `ref/wolf3d-data/` — Wolf3D shareware data files (read by `zig build extract`)
- `ref/Quake-master/` — pristine upstream WinQuake (id-Software/Quake), kept as a diff baseline against `sdlquake/engine_src/`
- `ref/Quake-2-master/`, `ref/Quake-Tools-master/`, `ref/TrenchBroom-master/`, `ref/fteqw-master/`, `ref/DOOM-master/`, `ref/wolf3d-master/`, `ref/quake106/`, `ref/quake_map_source-master/`, `ref/Quake-2-Tools-master/` — upstream references, do not modify

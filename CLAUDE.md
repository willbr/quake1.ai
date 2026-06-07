# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires Zig (tested on 0.14.1 and 0.16). Shareware assets are committed loose under `id1/` (originally extracted from the freely-redistributable `pak0.pak`, which is no longer shipped); registered episodes 2–4 are not — if you have a `pak1.pak`, drop it alongside and the engine will pick it up. `COM_AddGameDirectory` is patched so the loose directory sits at the head of the searchpath LIFO, i.e. loose files shadow same-named entries in any pak. SDL3 is vendored per-OS — no system install needed. (The Phase 6 Doom **and** Wolf3D guns were removed 2026-06-07, along with their reference data/source trees (`ref/doom-data/`, `ref/DOOM-master/`, `ref/wolf3d-*`) and the `tools/extract_phase6/` asset extractor; the `weapon2`/`items2` second-weapon selector they introduced survives and now hosts only the M8 oil-gun/flamethrower. Mechanics worth salvaging from each are noted in `docs/doom-ideas-to-steal.md` and `docs/wolf3d-ideas-to-steal.md`.)

Supported hosts: **Windows x64** (vendored `SDL3.dll` + `.lib` under `sdlquake/vendor/SDL3-3.4.8/lib/x64/`) and **macOS arm64** (vendored `libSDL3.0.dylib` under `…/lib/macos/`). Linux is untested but the build paths in `build.zig` fall through to system SDL3 via `linkSystemLibrary`.

```sh
zig build run
zig build run -- +map e1m1
```

On macOS, `build.zig` runs `install_name_tool` on the executable after install to rewrite its Homebrew-derived `LC_LOAD_DYLIB` entry to `@rpath/libSDL3.0.dylib`; the binary's `@executable_path` rpath then finds the vendored dylib alongside it. No `DYLD_LIBRARY_PATH` needed.

No test suite exists yet. Build success and visual/audio correctness in-game are the verification methods.

### Deferred command-line commands

Server-forwarded client commands (`god`, `notarget`, `noclip`, `fly`, `give`,
`kill`, `impulse`, …) and `profile level` issued from the command line or a
startup config run before the loopback signon finishes, so they'd normally be
dropped ("not connected", or — for `impulse` — eaten by a discarded move
message). They now **defer**: a small queue in `host_cmd.c`
(`Host_QueueForwardedCmd` / `Host_ApplyPendingClientCmds`, drained once
`cls.signon == SIGNONS && cl.movemessages > 2`) replays them once the player is
in the level. So `quake +map e1m1 +god +impulse 9` works. Hooked from
`Cmd_ForwardToServer` (cmd.c, covers all forwarded cheats) and `IN_Impulse`
(cl_input.c); `profile level` self-defers via its own latch in `perf.c`.

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

The scene is **100% software-rendered**; the GPU only does present + an 8-bit→
RGBA **palette-LUT** lookup (and ImGui compositing). The software renderer writes
palette indices to `vid.buffer`; `vid_sdl.c::gpu_render_frame` uploads it as an
`R8_UINT` texture + a 256-entry LUT, and a fullscreen-triangle pass
(`shaders/palette.{vert,frag}.glsl`) does the per-pixel lookup. Shaders compile
at build time (`tools/build_shaders.zig`: glslang→SPIR-V→spirv-cross MSL,
embedded in a generated `palette_shaders.h`); SPIR-V serves Vulkan (Linux +
Windows), MSL serves Metal (macOS). The editor and F3 dev overlay render the
scene into an offscreen texture shown in a dockable `Viewport` ImGui panel;
normal play goes direct-to-swapchain. CRT scanlines (`vid_scanlines`) and the
crop-screenshot dim/border are extra fragment passes in the same render pass.

**Full detail (ImGui docking layout, offscreen Viewport panel, scanline UBO,
rect-overlay pipeline, DXIL/D3D12 TODO):
[docs/render-pipeline.md](docs/render-pipeline.md).**

### Threaded span fill (software rasterizer)

The scene stays **100% software-rendered**; render perf comes from threading +
SIMD of the CPU rasterizer (see *Render pipeline*). The dominant cost is the
span fill, now parallel. `D_DrawSurfaces` (`d_edge.c`) splits into a serial
**resolve** pass (`D_SetupSurfaceFill`: mip/dither, `D_CacheSurface` — the only
allocator — bmodel transform, `D_CalcGradients`, recorded into `surf_t.fill`)
and a **parallel fill** (`D_DrawSurfaceBody` loads `s->fill` and draws).
Correctness rests on the edge sweep resolving occlusion *before* fill, so
visible spans are disjoint → workers write disjoint framebuffer/z pixels with no
locks; verified byte-identical to serial. Per-surface span-drawer globals are
`__thread`. A pre-fork cache-eviction recovery guards the recorded cacheblock
pointers against same-frame eviction. Pool: `sdlquake/engine/r_threadfill.c`.

Cvars: **`r_threads`** (default 1; `0` = serial oracle), **`r_threads_check`**
(debug disjoint-span check). Result on a 10-core Mac (e1m1): fill 3.14→0.75 ms.

**Full detail + the cache-eviction hazard:
[docs/renderer-threaded-fill.md](docs/renderer-threaded-fill.md), design
`docs/superpowers/specs/2026-06-04-renderer-fill-threading-design.md`.**

### Video recording (dev screen capture)

`sdlquake/platform/video_record.c` — dev-only MPEG-1 screen recorder over
vendored `sdlquake/vendor/jo_mpeg/`. Console: **`recordvideo [name] [dur]`** /
**`stopvideo`**; cvars **`record_fps`** (default 60) and **`record_maxdim`**
(default 960, long-edge cap on the encoded frame). `dur` accepts `30s` / `2m` /
`level` (mirrors `profile`). Encoding is parallel: the render thread expands
frames into a 12-slot ring → N encode workers → one writer thread, so a CFR
stream plays at correct speed. Output to gitignored `videos/`; no audio yet —
transcode with ffmpeg.

**Full detail: [docs/video-recording.md](docs/video-recording.md).**

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
| 6 | ✅ done (guns removed) | Ported Doom1 + Wolf3D guns into Quake (sprites, sounds, behaviour). Both rosters removed 2026-06-07; the `weapon2` selector remains for M8 fire weapons. See `docs/{doom,wolf3d}-ideas-to-steal.md`. |
| 7 | ✅ done | In-game 3D map editor |
| 8 | M3–M6 done; M7 stub; M8 done | Immersive-sim systems (physics, reactive AI, wind/smoke, light tier, Blink + Gust, Fire & Oil) |

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
- `sim_nav.c` — navmesh bake from BSP, A* pathfinder, in-game debug overlay (`sim_nav_debug` cvar). On `start.bsp` it bakes a **conditional-gate union mesh**: edges carry `requires_items`/`forbids_items` predicates gated on the four episode sigils (item bits 28–31, `IT_SIGIL1..4` in `game_defs.h`), so one cached mesh serves every `serverflags` state. The `func_bossgate` slab is kept solid for the primary flood (slab-top edges `forbids=ALL_SIGILS`) and a supplemental non-solid flood adds the shaft-descent edges (`requires=ALL_SIGILS`); `func_episodegate` entry passages are tagged `forbids=rune_bit`. Dev/test: `serverflags <n>` (engine console) sets the sigils; `nav_testpath x1 y1 z1 x2 y2 z2` prints the A* waypoint count under the live sigils. `nav_edges_near` (MCP) reports each edge's `requires`/`forbids`. `NAV_VERSION` is 22; `GAME_API_VERSION` 37 (33 added `nav_test_path`; 34 added `SV_Fire` for M8 fire; 35 added `button5`/`+pouroil` for M8/F2 hold-to-paint oil; 36 added `SV_Decal` for M8/F2 oil + scorch floor decals; 37 added `SpawnParticleEffect` for the data-driven particle editor (`r_emitter.c`)).
- `sim_wind.c` — M4 voxel wind grid + smoke advection; `Wind_PathOcclusion` feeds AI LOS.
- `sim_light.c` — M5 light-tier sampling via `engine_api->Sample_Lightmap`; Gust-extinguishable lights table.
- `sim_retrofit.c` — M6 patrol-route auto-wiring for id1 maps.
- `sim_fire.c` / `flammables.c` / `weapons_fire.c` — **M8 Fire & Oil** (complete, DLL-side). Burn registry + 10 Hz DOT, flammable corpses, rising `SV_Fire` flame plume (`pt_fireblob`); floor-oil pool (`s_oil[256]`) with decals, cascade, coating and float; oil gun + flamethrower (via the `weapon2` selector); `misc_oilbarrel` / `func_breakable` / live torch props; Gust extinguish + contact-spread + fire-light. Debug ignite `impulse 210`, oil `impulse 211`; `GAME_API_VERSION` 36. **Full F1–F6 detail: [docs/phase8-fire-oil.md](docs/phase8-fire-oil.md).**

### Editor module map (`sdlquake/engine/editor/`, Phase 7)

In-game 3D editor, engine-side (touches `cl.worldmodel`, BSP loaders, the
framebuffer directly). ImGui-docked; the 3D scene renders into a dockable
`Viewport` panel. A multi-mode shell (`editor_mode_t` vtable, `editor_mode.h`)
switches between **Map** mode (`map_mode`), **Particle** mode (`edit_particle.c`,
over the `r_emitter.c` registry — see Reference data), and **Actor** mode
(`edit_actor.c`, skeletal-actor authoring incl. geometry/skeleton/animation CRUD
+ a viewport translate gizmo).

Files: `editor.c/.h` (shell), `editor_ui.c` / `editor_classlist.c` (panels),
`edit_scene.c` (editable scene graph), `edit_history.c` (undo/redo),
`edit_texcache.c` (texture pool), `gizmo.c` (gizmos), `collide.c` (picking),
`render_{wire,flat,tex}.c` (overlay modes), `brush_compile.c` / `map_io.c`
(`.map` ↔ scene, `.bsp`/`.lit` export), `light_bake_thread.c` (async light bake).

**Full detail (Particle + Actor mode features, gizmo, console twins):
[docs/editor-modules.md](docs/editor-modules.md).**

## Skeletal actors (IQM, TR1-style)

Expressive multi-part characters: one **IQM** file (geometry + skeleton + anim
clips), authored in-engine (cubes-first, no Blender), with runtime layers for
procedural face (look-at / eye-gaze / breathing), a self-animating ponytail
(Verlet), and game-driven clip selection via the networked `frame` field. New
model type `mod_iqm`; reader/writer in `sdlquake/libmodel/iqm.{c,h}`, render in
`r_alias.c::R_IQMDrawModel`, dynamics in `iqm_dynamics.c`, in-game test actor in
`sdlquake/game/actor_test.c` (`impulse 217`). **Feature-complete**: runtime
R1–R4 + multi-clip + IQM read/write with animation + full in-engine Actor editor
mode. Remaining work is content (lipsync needs textured heads; monster retrofit
needs an IQM monster model). Cvars: `actor_lookat`, `actor_gaze_*`,
`actor_breathe_*`, `actor_dynamics`, `actor_pony_*`, `actor_clip`.

**Full R1–R5 / E1–E3 detail:
[docs/skeletal-actors.md](docs/skeletal-actors.md) and design
`docs/superpowers/specs/2026-06-01-skeletal-actors-design.md`. Actor editor
mode: [docs/editor-modules.md](docs/editor-modules.md).**

## Perf instrumentation

`sdlquake/engine/perf.{c,h}` — scoped per-frame timers feeding a live overlay
graph + offline Chrome-trace capture. `PERF_SCOPE("name") { ... }` (engine) /
`SIM_PERF("name")` (game DLL, across the ABI via `eng->Perf_PushScope`) wrap
blocks. Live overlay: `showperf 1`; the F3 Profile panel has Play/Pause
(`perf_live`, default on — keeps profiling + sim live under F3) and a Record
button. Console: **`profile <n | 10s | level>`** captures to
`profiles/perf_<ts>.json` (+ `_summary.json`); `perf_replay <path>` (or the
Source combo) loads a capture back into the flame graph, with an aggregate
per-scope table below it. `devoverlay` is the console twin for the F3 toggle.
`scripts/perf_diff.py` diffs two summaries.

**Full detail (scope tree, replay UI, frametime histogram, aggregate table):
[docs/perf-instrumentation.md](docs/perf-instrumentation.md).**

## Reference data

- `id1/` — Quake PAK files at repo root (required at runtime)
- `id1/particles/` — data-driven particle-effect presets (`*.pcl`, Quake KV-block); all `*.pcl` here load at startup (globbed via `COM_EnumMatchingFiles`, no index file). Authored in the in-game Particle editor mode (`edit_particle.c`); runtime in `r_emitter.c`.
- `ref/Quake-master/` — pristine upstream WinQuake (id-Software/Quake), kept as a diff baseline against `sdlquake/engine_src/`
- `ref/Quake-2-master/`, `ref/Quake-Tools-master/`, `ref/TrenchBroom-master/`, `ref/fteqw-master/`, `ref/quake106/`, `ref/quake_map_source-master/`, `ref/Quake-2-Tools-master/` — upstream references, do not modify

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires Zig (tested on 0.14.1 and 0.16). Shareware assets are committed loose under `id1/` (originally extracted from the freely-redistributable `pak0.pak`, which is no longer shipped); registered episodes 2–4 are not — if you have a `pak1.pak`, drop it alongside and the engine will pick it up. `COM_AddGameDirectory` is patched so the loose directory sits at the head of the searchpath LIFO, i.e. loose files shadow same-named entries in any pak. SDL3 is vendored per-OS — no system install needed. Phase 6 Doom/Wolf3D guns extract automatically from committed reference WADs (`ref/doom-data/DOOM1.WAD`, `ref/wolf3d-data/VSWAP.WL1`) on first build; outputs are gitignored and regenerated as needed (`rm id1/progs/v_doom*.spr` to force re-extraction).

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
`palette_shaders.h`. SPIR-V serves Vulkan on Linux *and* Windows
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

### Threaded span fill (software rasterizer)

The scene stays **100% software-rendered** (see the `keep-software-renderer-look`
constraint); rendering perf comes from threading + SIMD the CPU rasterizer, not
from GPU triangles. The dominant cost is the **span fill** (`D_DrawSurfaces`,
~92% of `R_ScanEdges`), which is now parallelized across a worker pool.
Design+results: `docs/superpowers/specs/2026-06-04-renderer-fill-threading-design.md`.

`D_DrawSurfaces` (`d_edge.c`) is split into a serial **resolve** pass and a
parallel **fill**:
- **Resolve (serial):** `D_SetupSurfaceFill` runs per-surface setup — mip/dither
  resolution + `D_CacheSurface` (the only allocator + dlit re-light, so the
  non-thread-safe surface-cache pool is only ever touched here), the bmodel
  transform, and `D_CalcGradients` — and **records** the gradient OUTPUTS +
  cacheblock into `surf_t.fill`/`rcache`/`rmip`/`rbucket`. Recording the *outputs*
  decouples the draw from the transform, so the fill is uniform for world and
  bmodel surfaces.
- **Fill (parallel):** `D_DrawSurfaceBody` just LOADS `s->fill` into the
  per-surface span-drawer globals and draws — no transform, no `D_CalcGradients`,
  no allocator. Those globals (`cacheblock`, `d_sdivz*`/`sadjust`/`bbextents`/…,
  the `r_turb_*` scratch, `miplevel`) are `__thread` (`d_vars.c`/`d_scan.c`/
  `d_edge.c`), so workers don't collide; the framebuffer/z-buffer base pointers
  stay shared (read-only base, disjoint writes).

**Correctness foundation:** the edge sweep resolves occlusion BEFORE fill, so
visible spans of different surfaces are **disjoint** in screen space → workers
write disjoint framebuffer + z pixels with no locks. Verified byte-identical
serial-vs-threaded.

`sdlquake/engine/r_threadfill.c` is the pool (pure C / SDL3, like
`light_bake_thread.c`): persistent `min(cores,16)-1` workers parked on a
semaphore; `R_ThreadFill_Run(fn, begin, end)` publishes the range, releases
workers, runs the same atomic work-stealing grab-loop (`SDL_AddAtomicInt`) on
the caller, then waits one done-signal per worker — the barrier (SDL semaphore
signal/wait give the release/acquire fences). Dispatched from `D_DrawSurfaces`
for indices `[1, surface_p-surfaces)`.

Cvars: **`r_threads`** (default 1; **`0` = serial path**, the regression oracle —
also auto-serial on 1-core hosts and on cache-thrash frames, since the rare
thrash re-setup calls the allocator). **`r_threads_check`** (default 0, debug):
runs a serial coverage pass (`D_CheckSpansDisjoint`) that warns once if any pixel
is covered by two surfaces' spans — self-polices the disjoint-span invariant
against future sweep/transparency changes. Result on a 10-core Mac (e1m1): fill
3.14 ms → 0.75 ms (4.2×), `R_ScanEdges` 3.41 → 1.02 ms, frame 5.65 → 3.21 ms.
Phasing was Phase 0 (single-pass fill) → 1 (cache pre-resolve) → 2 (gradient
recording) → 3a (`__thread`) → 3b (fork), each bit-identical-verified. Next:
SIMD the gather-free inner loops (`D_DrawZSpans`, the lightmap×texture combine)
in 128-bit C intrinsics / simde for both ISAs (Deck x86-64, M4 aarch64).

### Video recording (dev screen capture)

`sdlquake/platform/video_record.c` — a development-only MPEG-1 screen recorder
(Mac/Windows desktop) over the vendored single-file `sdlquake/vendor/jo_mpeg/`
writer. `Video_Record_CaptureFrame` runs each `VID_Update` (after
`gpu_render_frame`, while `vid.buffer` still holds the frame): a wallclock CFR
sampler keyed off `Sys_FloatTime()` palette-expands the 8-bit framebuffer to
RGBX (via `d_8to24table`, same as the screenshot path) at the engine's current
`vid_scale` resolution. Output goes to gitignored `videos/`.

- Console: **`recordvideo [name] [dur]`** / **`stopvideo`**; cvar **`record_fps`**
  (default 30, snapped to jo_mpeg's legal 24/25/30/50/60). `dur` mirrors the
  `profile` command — `30s`, `2m`, or `level` (record until the level ends:
  map change / intermission / disconnect, with the same pending-latch so
  `recordvideo level` from the command line defers until a map loads). Command
  names avoid Quake's existing demo `record`/`stop`. No audio (yet).
- **Encoding is parallel** — the MPEG-1 DCT is far slower than a frame
  (inline it tanked the game to ~1fps; a single worker only sustained ~15fps at
  1280×800, dropping half the frames so a CFR stream played ~2× too fast). Since
  each jo_mpeg frame is an independent intra-coded sequence, frames encode
  concurrently: the **render thread (producer)** expands a frame into a free
  slot of a 12-slot ring and queues it (it *blocks* rather than drops when the
  window is full, so no frame is lost and playback speed stays correct); **N =
  clamp(cores−2, 2, 6) encode workers** turn slots into MPEG byte buffers; **one
  writer thread** concatenates them to the file in capture order. Sync is one
  `SDL_Mutex` + four `SDL_Condition`s, drained on stop so the tail isn't lost.
  jo_mpeg was given an in-memory entry point (`jo_encode_mpeg_frame` → malloc'd
  bytes; `jo_write_mpeg(FILE*)` kept as a wrapper, bitstream unchanged).
- Convert/transcode outside the engine with ffmpeg (e.g. `ffmpeg -i clip.mpg
  -c:v libx264 -crf 20 -pix_fmt yuv420p clip.mp4`).

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
- `sim_fire.c` — M8 (F1) burn registry + 10 Hz DOT (`T_Damage`), `EF_DIMLIGHT` glow, and a rising flame plume via the new `engine_api->SV_Fire` (spawns across the entity's world bbox so it engulfs standing monsters *and* prone corpses — corpses are pre-flattened by `Corpse_LayProne`). The engine draws the plume as `pt_fireblob` particles: `D_DrawFireParticle` (d_part.c) stamps a solid-colour blob whose size follows an ADSR grow-then-shrink envelope keyed off the fire `ramp` (peak size = `r_fire_size` cvar); a sibling of the `pt_smoke` billboard but writing colour, not fog. Dead bodies are flammable (death no longer extinguishes; DOT skips corpses). Water/slime douse the fire (lava does not). Debug: `impulse 210` or `fire_ignite_num <N>` ignite; `fire_noflee` freezes burning monsters; `sim_fire_debug`, `fire_dps`, `fire_secs` cvars. **F2 (oil substance):** a fixed `oil_patch_t s_oil[256]` floor-oil pool (not edicts — keeps the 600-edict budget free) ticked by `oil_frame()` inside `Fire_Frame`. Patches deposit via `Fire_AddOil` (merge-nearby / allocate / recycle-oldest), render as a persistent dark floor **decal** (`SV_Decal` → engine `R_SpawnDecal`, `DECAL_OIL` = a soft Gaussian darkening painted into the lightmap, *not* smoke) and evaporate after 60 s; once lit they do area DOT, ignite edicts standing in them (via `Fire_IgniteMaybeCoated` — oil-**coated** edicts burn `OIL_COAT_BURN_SECS` instead of the base), throw a rising `SV_Fire` plume + `Wind_AddSmoke` + `STIM_FIRE`, and **cascade** to neighbouring patches after a short delay so fire races down a trail. Lit patches (and burning bodies, once per second) also paint a `DECAL_SCORCH` mark so fire permanently scorches the surface like blood spills do. Fresh deposits coat any edict standing in them (`s_coated_until[]`, cleared on burn-end). Debug: `impulse 211` / `fire_oil_num <N>` deposit; `fire_oil_ignite 1` lights nearest-to-player; `fire_oil_count` reports the live patch count (MCP-readable). Ignition: `impulse 210` and bullets light the oil patch at the crosshair/impact (`Fire_LightOilNear`), and explosions light all oil in the blast (hooked in `T_RadiusDamage`, so rockets/grenades/future barrels all qualify). Oil floats: a submerged deposit rises to the water/slime surface. Hold `+pouroil` (e.g. `bind mouse2 +pouroil`) to paint a continuous trail — the held command runs the silent `Fire_PourOil` at ~16 Hz from `Abilities_Frame`, riding engine `button5` (clc_move bit 4; ABI 34→35). The oil core is DLL-side; the engine gained two F2 hooks: `+pouroil` (button5/`clc_move`) and the decal renderer (`SV_Decal`→`R_SpawnDecal`, `DECAL_OIL`/`DECAL_SCORCH` soft-Gaussian kernels in `r_decals.c`, ABI 34→36). Decal-visibility gotcha: per-luxel delta is `(dr*w)/knorm`; set `knorm` to the Gaussian *peak* (not its full sum, which dilutes to ~invisible; not 1 with a solid kernel, which clamps to hard black squares on dark floors). **F3 (weapons):** oil gun + flamethrower in the new `weapons_fire.c`, dispatched through the Phase 6 `weapon2` selector (`IT2_OILGUN`/`IT2_FLAMETHROWER`) so the stock 8-weapon switching stays untouched. Both are continuous held-fire (10 Hz self-perpetuating think, modeled on the lightning gun's `player_light1/2_think`) drawing shared `ammo_cells`, with reused `.mdl` view models (oil gun `v_rock.mdl`, flamethrower `v_light.mdl`). Oil gun sprays via `Fire_PourOil` (deposits + coats monsters); flamethrower is a LOS-gated forward cone (adapted from the Gust cone) that `Fire_IgniteMaybeCoated`s `takedamage` edicts + `Fire_LightOilNear`s oil along its axis + emits a visible `SV_Fire` jet + `EF_MUZZLEFLASH`. Select `impulse 40`/`41`; grant `impulse 212` (also folded into `impulse 100`); world pickups `weapon_oilgun`/`weapon_flamethrower` grant via `weapon_touch_fire` (items.c). Cvars `fire_flame_range`(220)/`fire_flame_cone`(25)/`fire_flame_secs`(3)/`fire_flame_tick`(0.1)/`fire_flame_cost`(1) and `fire_oilgun_tick`(0.12)/`fire_oilgun_cost`(1); ignite DPS reuses `fire_dps`. **All DLL-side — no ABI bump (`GAME_API_VERSION` stays 36).** **F4 (flammables):** new `flammables.c` adds four interactions, all DLL-side. `misc_oilbarrel` (debug `impulse 213`) mirrors `misc_explobox` but spills a ring of `Fire_AddOil` before reusing `barrel_explode` — whose `T_RadiusDamage` auto-lights the spill (`combat.c:478`) → trail→barrel→boom. `func_breakable` (brush) + `misc_breakable` (point, box bmodel, debug `impulse 214`) get `takedamage`+`health`+`th_die`, so the fire DOT burns them down to `breakable_die` (emits the long-dormant `STIM_PROP_BROKEN` + splinter puff + `ax1.wav`) with no fire-specific wiring. The four `light_torch`/`light_flame` entities **drop `SV_MakeStatic`** (which freed the edict) to become live `SOLID_NOT` props, so `Torch_Extinguish`/`Torch_Relight` can toggle the flame: "lit" == `modelindex != 0` (extinguish nulls `modelindex` keeping the model string; relight `SV_SetModel`s it back) plus a ∓80 `Light_AddOverride`. Gust now calls `Torch_Extinguish` (the latent M5 snuff finally shows visibly), the flamethrower cone relights extinguished torches in the same pass, lit torches ignite oil within `radius+TORCH_OIL_REACH` (folded into `oil_frame`'s edict scan — the torch test runs before the `Fire_IsBurning` early-continue), and debug `impulse 215` toggles the nearest torch. Player-burns: standing in burning oil already ignites/DOTs the player (no `FL_CLIENT` filter) with free `EF_DIMLIGHT` glow + red-flash feedback; F4 adds flamethrower **backdraft** self-ignite (`fire_flame_backdraft` units, default 40, 0=off — spraying point-blank into a wall lights you). **No ABI bump (`GAME_API_VERSION` stays 36).** MVP limits: an extinguished torch still crackles (baked `SV_AmbientSound` can't be stopped without engine work); live torches consume edict slots (ample headroom on episode 1, watch dense maps). **F5 (interactions):** Gust now also **puts fire out** — `abilities.c::gust_fire` calls the new DLL-internal `Fire_ExtinguishRegion` (sim_fire.c): always extinguishes the **caster** (self-rescue, regardless of the forward cone), extinguishes burning edicts in the cone, and **consumes** lit oil patches in the cone (cancelling scheduled cascades there) — the F4 torch-snuff loop is untouched; the cone math (`fire_point_in_cone`) mirrors `gust_fire`'s flat/3D cone. **Contact-spread:** a burning edict ignites *other* nearby `takedamage` edicts each 10 Hz fire tick within `fire_spread_radius` (64u) — the "burning enemy lights allies" moment; spread credits the source's igniter (player-or-world via `fire_credit_igniter`), **never the burning monster**, which closes the F1 freed-and-reused-monster-igniter attribution risk by construction (player credit propagates transitively down a chain). `STIM_FIRE` is throttled to **2 Hz per source** (`FIRE_STIM_INTERVAL`, per-source `next_fire_stim` on burn slots + oil patches) so a burning oil trail can't saturate the 512-entry stim ring — AI *avoidance* is unaffected (it queries the registry via `Fire_NearestHazard`, not the ring; closes the F1/F2 stim-ring carry-forward). Lit oil patches **brighten the room** via a **paired** ± `Light_AddOverride` (`fire_light`, default 96: +delta on ignite in `oil_light_patch`, the matching −delta on *every* lit→inactive path — burnout, Gust-consume via `oil_extinguish_patch`, pool-recycle in `Fire_AddOil` — so the lightmap nets to zero and the shared 1024-slot override table can't leak), feeding both the visible lightmap (`Lightmap_AddDelta`→`D_FlushCaches`) and the M5 AI light-tier ("fire reveals you"); burning edicts keep `EF_DIMLIGHT` (render-only). `fire_smoke` cvar exposes the per-tick smoke amount. **All DLL-side — no ABI bump (`GAME_API_VERSION` stays 36).** Known emergent behaviors (uniform with F2's oil area-DOT, *not* F5-specific): contact-spread/area-DOT can ignite gibs and health-triggered brush entities (`func_door`/etc.); a pathological oil-spam session could exhaust the append-only override table (2 slots per burn cycle, never freed) — a reclaimable override slot is follow-up polish. **F6 (test level + tooling — M8 complete):** `id1/maps/ai_t10_fire.map` is a single showcase arena (built on the `m7_skeleton` box shell; rebuild with `zig build mapcompile -- id1 ai_t10_fire`) wiring every fire/oil system into one room — a pre-placed oil slick, an oil-trail→`misc_oilbarrel`→boom lane, four wall torches, breakable crates, and a grunt+ogre cluster — with `weapon_oilgun`/`weapon_flamethrower`/`item_cells` pickups (worldspawn marker `AI-TEST t10_fire`; deliberately **not** added to `run_ai_tests.sh`'s gating t01–t09 list, since fire isn't a nav test). New map-spawn seed entity `misc_oilslick` (in `flammables.c`) deposits an oil patch at its origin via `Fire_AddOil` (deferred one frame so the sim is live, then re-pours every 40 s so the slick never lapses the 60 s `OIL_TTL_SECS`). Headless-inspection cvars `fire_burning_count` + `fire_lit_oil_count` (peers of `fire_oil_count`, written each fire tick at the end of `oil_frame`) give MCP `get_cvar` pass/fail asserts — a full positional `fire_query` MCP tool was deferred (it'd need a `game_api_t` vtable fn + bridge = an ABI bump). Balance left at defaults (control/area-denial: grunt ~3.75 s to kill by fire alone, ogre effectively needs combat too); perf under 5 simultaneous lit patches + 5 burning edicts = worst frame 10.9 ms / Fire scope ≤3.4 ms on the 10 Hz tick (the O(patches×edicts) scan; spatial buckets remain the noted follow-up). **All DLL-side — no ABI bump (`GAME_API_VERSION` stays 36); M8 (Fire & Oil) complete.**

### Editor module map (`sdlquake/engine/editor/`, Phase 7)

The in-game 3D editor is engine-side (not in `game.dll`) so it can touch `cl.worldmodel`, BSP loaders, and the framebuffer directly. The editor UI is **ImGui-docked** (docking-branch ImGui; default layout baked + persisted, "Reset layout" button) with the 3D scene rendered into its own dockable `Viewport` panel (`VID_EditorSceneTexture` → `IG_Image`; picking remapped to the panel rect) — see the render-pipeline section above:

- `editor.c` / `editor.h` — public entry points, mode toggle, frame tick. **Hosts a multi-mode shell:** an `editor_mode_t` vtable (`editor_mode.h`) lets the editor switch between Map mode (the original `.map` editor, registered as `map_mode`) and Particle mode (`edit_particle.c`); the public `Editor_DrawUI`/`Editor_RenderScene`/`Editor_ProcessEvent`/`Editor_HideTransientFX`/`Editor_ShouldDrawPlayer` dispatch to the active mode (shared free-fly camera / open-close stays in the shell). Planned `.mdl`/texture editors plug in as new `s_modes[]` registrations.
- `editor_mode.h` — the `editor_mode_t` vtable type (mode-specific slots: enter/exit/draw_ui/render_scene/process_event/hide_transient_fx/should_draw_player).
- `edit_particle.c` — **Particle editor mode** (Phase: particle editor). ImGui panels (effect list, inspector, palette-swatch color-ramp editor, size envelope) over the data-driven emitter registry in `r_emitter.c`; preview spawns at the orbit-camera focus (RMB-drag to orbit, wheel/W,S to zoom, A,D to spin) with a play/pause clock independent of the paused world. **Auto-preview** (checkbox, default on): clicking an effect in the list shows it live — continuous effects loop, bursts re-fire on an interval (`preview_burst_period`, ~life+0.6s) so even one-shots read as live; the per-frame `preview_tick` keeps the preview tracking the selection. The mode's `enter`/`exit` now also run on editor open/close (not just `Editor_SetMode`), so the preview's live emitters are torn down (`R_EmitterStopAll`) when the editor closes rather than leaking into gameplay. The gameplay crosshair is suppressed in this mode (`Editor_ParticleModeActive` gates `V_DrawCrosshair` in `view.c`) — it otherwise lands on the orbit pivot / spawn point and reads as the spawn point "rotating with the crosshair" as you circle the effect. The Preview controls include a **Hide world** toggle (`editor_particle_hide_world` cvar, default on): when set, `r_main.c` paints the 3D view over with `r_clearcolor` + zeroes the z-buffer span (`D_ClearViewToBackdrop` in `d_part.c`) right before the particle pass, so effects preview against a clean backdrop instead of the loaded map (gated by `Editor_ParticleHideWorld` — open + Particle mode only; normal play untouched). A **reference grid** (`editor_particle_grid` cvar, default on; `Grid` checkbox) helps judge effect size: `Editor_DrawParticleGrid` draws a 16u ground grid (major lines every 64u) centred on the orbit focus / spawn point with coloured X/Y axes + a 64u +Z tick, via the z-tested `Editor_DrawLine3D` and called from r_main.c *after* the hide-world clear (so it survives) and *before* the particle pass (so the effect draws over it). Effects persist as `id1/particles/*.pcl` (loaded/saved by `r_emitter.c`).
- `edit_actor.c` — **Actor editor mode** (skeletal actors, E1 skeleton). Registered as `actor_mode` (third `s_modes[]` tab). Loads an IQM (`Mod_ForName`) and previews it with the shared orbit camera (`ActorMode_PushPreview` injects the actor entity into `cl_visedicts` at the orbit focus next to `Editor_PushPreviewEntities`; the R3 head look-at then tracks the orbit camera live). ImGui inspector (model/counts/roles + joint/mesh lists) plus **preview controls** (per-clip selector that sets the previewed entity's `frame`, and a "Look at camera" toggle for `actor_lookat`, which the editor defaults **off** on enter and restores on close — a head that swivels to follow the orbit camera distracts while editing, so it's opt-in). **Geometry editing** (first authoring slice — "edit the loaded actor", numeric-fields-first per the user): an "Edit geometry" toggle deep-copies the loaded actor (round-trip via `lm_write_iqm`/`lm_load_iqm`), then full per-part **CRUD — add/move/resize/rotate/delete** (`DragFloat3` move/size/rotate — scale + euler-rotate about the part centroid, rotation in the engine fwd/-right/up convention; **add** = `lm_iqm_add_box` grows the arrays with a new 8-vert cube bound to joint 0; delete = in-place vertex/triangle shrink with tri-reindex; joints + anim kept throughout) rebuild the rendered copy each frame; **Save IQM** writes the result via `lm_write_iqm` + `COM_WriteFile` (edited geometry **plus** the original clips — the writer re-encodes animation). A part can also be **bound to a joint** (`actor_edit_bind`/`edit_bind` sets the verts' blend index) so it follows that joint under animation — the "parent"/rig op; persists through save. **Skeleton editing** has joint **move** (`actor_edit_jmove`/`edit_joint_move`, "Joints" UI section): **move-the-whole-assembly** semantics — the joint, its bound part(s), and its whole subtree shift, by moving the subtree's *vertices* **and** the joint's bind+anim translate together (moving only the joint is a no-op for move-assembly — skinning `curwld ∘ bindinv` is invariant to a matched shift). The complement, **re-pivot** (`actor_edit_jpivot`/`edit_joint_pivot`, "re-pivot" field), moves the joint's bind+anim translate **without** the verts — so the mesh stays put but the rotation centre moves (future rotations of that joint pivot around the new point); verified: head pivot `t(0 0 28)→(0 0 58)`, geometry unchanged. Joints can also be **added** (`actor_edit_jadd`/`lm_iqm_add_joint`, "Add joint" button): grows `joints[]` and re-lays-out `frametrs` with one more static joint slot per frame (num_poses stays == numjoints) — bind a part to it to rig custom skeletons. Scriptable/headless console twins: `actor_edit <0|1>`, `actor_edit_add`, `actor_edit_move/scale/rot <mesh> …`, `actor_edit_bind <mesh> <joint>`, `actor_edit_jmove <joint> dx dy dz`, `actor_edit_jadd [parent] [dx dy dz]`, `actor_edit_del <mesh>`, `actor_edit_save [path]`. The cube-first editor now does full part **and** skeleton CRUD (add/move/resize/rotate/delete parts; add/move joints; bind; save with animation). **Animation timeline (E2):** an "Animation" section authors clips by **sparse keyframes** — pick a clip + joint, scrub the frame, type per-joint **euler rotation + translation**, **Set key** / **Del key** / **Bake**; the bake clamps outside the key span and **slerps rotation** + lerps translation inside (euler→quat per key), writing into `frametrs` which saves via the writer (design: `docs/superpowers/specs/2026-06-02-skeletal-actors-e2-animation-timeline.md`). **New clips** can be created from scratch ("New clip" button / `actor_anim_addclip [nframes] [fps]` → `lm_iqm_add_clip` grows `frametrs` by N bind-pose frames + a clip entry), then keyed. **Copy/paste keys** (`actor_anim_copy/paste`, Copy/Paste buttons — duplicate a joint's pose to another frame, e.g. frame0→last for a loop) and an **ease-in/out** toggle (smoothstep on the bake fraction). Console twins: `actor_anim_clip <n>`, `actor_anim_addclip [nframes] [fps]`, `actor_anim_frame <rel>`, `actor_anim_key <joint> <pitch> <yaw> <roll> <tx> <ty> <tz>`, `actor_anim_unkey/copy/paste <joint>`, `actor_anim_bake`. Verified: a head key bakes → the preview plays it; a new clip + keys saves with 3 clips / 48 frames, round-trips (anim `maxerr ~6e-8`); copy/paste loop round-trips. **Skeletal-actors feature complete** (runtime R1–R4 + multi-clip + game-driven; in-game server actor; IQM read/write with animation; full geometry+skeleton+animation editor). Remaining are content, not code: **lipsync** (needs textured heads), **monster retrofit** (needs an IQM monster model). **Viewport translate gizmo (MVP, shipped 2026-06-03):** `actor_mode`'s `render_scene`/`process_event` slots now drive a direct-manipulation translate gizmo — three camera-scaled axis handles (X red / Y green / Z blue via `Editor_DrawLine3DOver`) at the selected part's centroid; **left-click a part** ray-picks it (`actor_ray_pick_meshes`, Möller–Trumbore over `s_edit`), **left-drag an axis handle** translates it (ray-vs-axis-line closest-point → writes the same `s_voff` accumulator the numeric `move` field uses, applied by `edit_rebuild`; grabbed axis highlights white via `EDIT_COLOR_AXIS_HOT`). The earlier revert (overlay absent over the *lit* actor view) was unblocked by the hide-world backdrop — handles render on the same cleared canvas the particle grid does. Gated to editor-open + Actor mode + Edit-geometry on; `edit_free` cancels any in-progress drag (resets `s_drag_axis`/`s_drag_mesh`). MCP-verifiable via the `actor_edit_pick [sx sy]` console twin (prints the hit part; defaults to view centre); the literal drag gesture is mouse-only. Design/plan: `docs/superpowers/specs/2026-06-03-actor-viewport-translate-gizmo-design.md` + `docs/superpowers/plans/2026-06-03-actor-viewport-translate-gizmo.md`. Deferred slices: rotate/scale handles, joint gizmo (`edit_joint_move`), plane-drag (2-axis) handles, grid snapping.

The emitter runtime is engine-side in `sdlquake/engine_src/r_emitter.c`: a name-keyed `emitter_def_t` registry + a live-emitter instance pool (`R_UpdateEmitters` each frame) that spawns `pt_emitter` particles into the existing `free_particles` pool. `R_DrawParticles` integrates them (gravity_scale/drag from the def) and `D_DrawEmitterParticle` (`d_part.c`) draws them via the dot/blob/smoke paths with a palette-ramp + size-envelope sampled at age. `R_EmitterLoadAll` discovers presets by globbing `particles/*.pcl` across the searchpath via `COM_EnumMatchingFiles` (loose dirs + paks, loose shadows pak) — no hand-maintained index; drop a `.pcl` in `id1/particles/` and it loads next launch (or live via `particle_reload`). Console `particle_spawn <name> [x y z]` / `particle_reload`; `r_emitter_active` cvar reports live instances (MCP-readable). Gameplay spawns by name via `eng->SpawnParticleEffect` (ABI 37). The shipped set recreates the engine's built-in effects (`r_part.c`): `explosion`, `blob_explosion`, `lava_splash`, `teleport_splash`, `water_splash`, `slime_splash`, `gunshot`, `blood`, `rocket_trail`, `smoke_trail`, `wizard_trail`, `voor_trail`, `brightfield`, plus demo `campfire`/`spark_burst`.
- `editor_ui.c`, `editor_classlist.c` — ImGui panels (inspector, entity browser).
- `edit_scene.c` — in-memory editable scene; the live brush/entity graph the editor mutates.
- `edit_history.c` — undo/redo stack.
- `edit_texcache.c` — texture-name pool shared by brush faces.
- `gizmo.c` — translate/resize gizmos with surface-snap support.
- `collide.c` — picking + ray casts against the live scene.
- `render_wire.c`, `render_flat.c`, `render_tex.c` — three overlay render modes.
- `brush_compile.c`, `map_io.c` — `.map` ↔ in-memory scene; `editor_compile_export` writes `.bsp` + `.lit`.
- `light_bake_thread.c` — async progressive light baking on a worker thread.

## Skeletal actors (IQM, TR1-style — in progress)

Expressive multi-part characters: an actor is one **IQM** file (geometry +
skeleton + animation clips), authored in-engine (no Blender; cubes-first), with
runtime layers for procedural face (look-at / eye-gaze / jaw-flap lipsync /
breathing), self-animating ponytail dynamics, and protocol pose sync. Design:
`docs/superpowers/specs/2026-06-01-skeletal-actors-design.md`; sub-projects
R1–R5 (runtime) + E1–E3 (in-engine editor) + C1 (content), built one at a time.

- **R1 done** (IQM load + static bind-pose render). New model type `mod_iqm`:
  - `sdlquake/libmodel/iqm.{c,h}` — portable IQM v2 reader (`lm_load_iqm` →
    `lm_iqm_t`): meshes, bind-pose joints (48-byte `iqmjoint`), triangles,
    POSITION/TEXCOORD/BLENDINDEXES. Animation lumps skipped until R2.
  - `model.c::Mod_LoadIQMModel` dispatches on the 16-byte `INTERQUAKEMODEL`
    magic and keeps the parsed `lm_iqm_t` on the hunk (`model_t.iqmdata`).
  - `r_alias.c::R_IQMDrawModel` (+ `R_IQMSetUpTransform`, identity model scale)
    renders rigid parts (1 bone/vertex, **bind pose = no joint math**) through
    the shared `D_PolysetDraw`; flat-lit, solid-colour synth skin per mesh;
    clipped triangles skipped (R1). Dispatched from `R_DrawEntitiesOnList`.
  - `iqm_dev.c` dev harness: `actor_dump <f.iqm>` (prints parsed structure),
    `actor_spawn <f.iqm> [x y z]` / `actor_clear` (persistent client-side entity
    injected into `cl_visedicts`, no server). Test asset
    `id1/actors/dummy.iqm` (5-cube figure) via `scripts/make_test_actor_iqm.py`.
- **R2 done** (animation playback). `lm_load_iqm` decodes poses/anims/frames →
  per-frame per-joint local TRS (`lm_iqm_t.frametrs`/`numframes`/`framerate`).
  `R_IQMDrawModel` builds a per-joint **skin matrix** each frame
  (bind-world-inverse ∘ animated-world via `IQM_QuatMat`/`IQM_LocalMat`/
  `IQM_Invert34` + `R_ConcatTransforms`), rigid-skins each vertex (1 bone), and
  loops the clip off `cl.time`. Bootstrap asset bakes a "look" clip (head yaw
  ±30°). Static (bind-only) models keep the identity-skin fast path.
- **R3 done** (procedural face: look-at + eye-gaze + breathing). Role joints
  (`head_joint`/`chest_joint`/`jaw_joint`/`eye_joint[]`) are resolved by **name
  convention** in `lm_load_iqm` (`strstr` on "head"/"chest"/"jaw"/"eye"; cached
  on `lm_iqm_t`). In `R_IQMDrawModel`'s compose loop, the head and eye joints'
  local rotation is **overridden** by `R_IQMLookAtLocal` to aim each joint's +X
  axis at the player (`r_origin`, transformed into actor space via the entity
  rotation `alias_forward/right/up`), clamped to a yaw/pitch cone — head and
  eyes track you; eyes (children of the head) carry the residual past the head
  clamp. **Breathing** post-multiplies the chest joint's skin matrix by a
  scale-about-posed-origin (`v' = org + s·(v−org)`), so it's **non-propagating**
  (children keep their own skin matrices) per the design's uniform-mesh-scale
  decision. Cvars (registered in `R_Init` via `R_IQMInitCvars`):
  `actor_lookat` (master on/off), `actor_gaze_yaw`/`actor_gaze_pitch` (head
  clamp 50/30°), `actor_eye_yaw`/`actor_eye_pitch` (eye clamp 25/20°),
  `actor_breathe_rate`/`actor_breathe_amp` (0.25 Hz / 0.04). All engine-side, no
  ABI bump. **Lipsync deferred**: the design's mouth skin-swap needs textured
  heads (R1/R2 use solid-colour synth skins); `jaw` is resolved for
  forward-compat but unused until a texturing milestone. On the cube bootstrap
  actor the look-at/gaze read subtly (featureless cube, eye cubes centred on
  their joints); verified from a fixed camera by varying only the gaze/breathe
  cvars. Plan: `docs/superpowers/plans/2026-06-02-skeletal-actors-r3-procedural-face.md`.
- **R4 done** (self-animating ponytail / secondary dynamics). `iqm_dynamics.c`
  — a client-side **Verlet point-chain**, engine-side, cosmetic, never networked.
  Ponytail joints are resolved as an ordered chain by the `pony` name convention
  (`lm_iqm_t.pony_joint[]`/`num_pony`). Run from `R_IQMDrawModel` after the R3
  pose composes: the chain root joint is pinned to its **rigid posed** position
  (so it follows the head look-at) and stays rigid; the free joints integrate
  gravity + inertia in **world space** (camera-motion-immune), satisfy distance
  constraints + a light straighten-toward-rigid term, and their simulated
  transforms are converted back to actor space and written into the free joints'
  `skin[]` (each segment's +Z bind axis aimed along the simulated chain). Per-actor
  sim state lives in a 16-slot pool keyed by `entity_t*`. Cvars `actor_dynamics`
  (on/off), `actor_pony_gravity`/`actor_pony_damping`/`actor_pony_stiffness`/
  `actor_pony_iters`. Verified in-game: the tail **sags under gravity** with
  dynamics on vs rigid with it off. **Wind tie-in deferred** (would need an ABI
  hook to read the game.dll `sim_wind` grid). The bootstrap dummy uses a
  back-sticking tail so it's visible past the chunky body; the solver handles any
  chain config. No ABI/protocol bump. Plan:
  `docs/superpowers/plans/2026-06-02-skeletal-actors-r4-ponytail-dynamics.md`.
- **C1-lite done** (in-game server integration). `sdlquake/game/actor_test.c` —
  proves the whole stack works on a **real server-spawned networked entity**, not
  just the engine dev-spawn. `Actor_TestPrecache()` (called from `worldspawn`)
  precaches `actors/dummy.iqm` into the level's precache list; `impulse 217`
  (`Actor_TestDebugSpawn`) `ED_Alloc`s a `SOLID_NOT`/`MOVETYPE_NONE` entity 96u
  ahead, `SV_SetModel`s it to the IQM, and `SV_SetOrigin` links it. The client
  renders it as `mod_iqm` and R3/R4 (look-at/breathing/ponytail) run for it
  automatically — **all client-side, so no protocol/ABI change** (`R5` would only
  be needed for game.dll to *drive* clip/talk/expression state). Verified
  in-game: the entity spawns and renders with live expression.
- **IQM writer done** (E1 pipeline backend — the editor's save path). `lm_write_iqm`
  (`libmodel/iqm.c`) serializes an `lm_iqm_t`'s geometry + skeleton back to IQM v2
  bytes (string table, 4 vertex arrays POSITION/TEXCOORD/BLENDINDEXES/BLENDWEIGHTS,
  triangles, joints), mirroring the known-good generator layout. **Animation is
  also written**: the decoded `frametrs`/`clips` are re-encoded into poses/anims/
  frames (per-channel min/max quantization, the inverse of the loader), so an
  edited actor keeps its clips through a save. Verified by `actor_roundtrip
  <file.iqm>` (load → write → re-parse → compare): `dummy.iqm` round-trips
  geometry + joints exact (`MATCH`) and animation within quantization tolerance
  (`anim … maxerr 0 OK` — exact here, since the source was quantized the same way). The remaining editor work (the interactive **Actor editor mode**:
  box create/size/parent, gizmos, animation timeline) is UX-heavy and the natural
  next sub-project (E1 proper), templated on `edit_particle.c`.
- **E1 skeleton done** (Actor editor mode foundation). `sdlquake/engine/editor/edit_actor.c`
  registers an `actor_mode` (`editor_mode_t`) in the editor shell's `s_modes[]`
  (third tab "Actor", alongside Map/Particle). It loads an IQM, **previews it with
  the shared orbit camera** (so the R3 head look-at tracks the orbit camera live —
  circle the actor and it watches you), and shows a **read-only inspector**
  (model + counts + roles + joint/mesh lists). Rendering reuses the editor's
  `cl_visedicts` preview-injection (`ActorMode_PushPreview`, called next to
  `Editor_PushPreviewEntities`); the orbit camera + `s_orbit_inited` reset now
  also fire for `actor_mode`. Verified in-game: `editor` + `editor_mode 2` shows
  the dummy actor in the orbit preview with the inspector. The interactive
  authoring tools (create/size/parent box parts, rig joints, animation timeline +
  save via the IQM writer) are the next slices — **UX-heavy, to be designed with
  the user.**
- **Multi-clip done** (animation infrastructure for E2 / R5 / behaviours). The
  loader parses every `iqmanim` into `lm_iqm_t.clips[]` (`lm_iqm_clip_t`:
  name/first_frame/num_frames/framerate/loop, ranges clamped into the decoded
  frame set); `R_IQMDrawModel` plays the `actor_clip` cvar–selected clip (loops
  within its frame range) and falls back to the whole frame set when a model has
  no clips. The bootstrap dummy now bakes **two** clips — `look` (head yaw,
  frames 0–15) and `nod` (head pitch, 16–31); `actor_dump` lists them. Verified
  in-game: `actor_clip 0` vs `1` (with `actor_lookat 0`) play visibly different
  head motion (yaw vs pitch). **Per-entity, game-driven, networked clip selection
  comes free:** `mod_iqm` repurposes the entity's **already-networked `frame`
  field** as the clip index, so `game.dll` setting `self.frame = N` (or
  `actor_spawn <f> [x y z] [clip]` for the dev actor) picks the clip per entity —
  **no protocol/ABI change** (the R5 pose-sync payoff for animation, achieved via
  the existing field). `actor_clip` (default −1) is a global test override; ≥0
  forces a clip for all IQM actors.

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
  as a non-interactive HUD over the running game.
- **Live profiling inside F3** (`perf_live` cvar, default on): the F3 dev
  overlay used to freeze the profiler *and* the game sim on open, so the
  graphs never updated. With `perf_live` on, opening F3 keeps the profiler
  ticking and the host keeps running the sim (a `Perf_Live()` bypass added to
  the two `SV_Physics` gates in `host.c`, suppressed inside the F2 editor),
  so the flame graph shows real gameplay load. The Profile panel's
  **Play/Pause** button flips `perf_live` live — Pause freezes both for
  hovering/scrubbing. `perf_live 0` restores the classic freeze-on-open.
  Note: with it on, the world simulates under *all* F3 panels (the passive
  player can take damage while the overlay is up).
- **`devoverlay` console command**: console twin for the F3 toggle (the key
  is otherwise the only path) — `devoverlay` toggles, `devoverlay 0|1` forces
  closed/open (idempotent; MCP/headless/cfg-friendly). Refuses while the F2
  editor owns the overlay. Registered from `ImguiLayer_Init`.
- The panel shows smoothed FPS, last-256-frames frametime sparkline, and a
  flame graph of the most recent frame (hover for ms / start / depth).
- Console: `profile <n>` captures `n` frames to `profiles/perf_<ts>.json`
  (Chrome trace, drop into chrome://tracing or speedscope.app) plus
  `perf_<ts>_summary.json` (per-scope avg/p50/p95/max/calls). The argument
  also accepts a wall-clock duration (`profile 10s` = 10 seconds) or
  `profile level` to capture until the current level ends (map change /
  disconnect / intermission, whichever comes first; auto-stops if the event
  buffer fills on a very long level). `profile level` works from the command
  line — `quake +map e1m1 +profile level` defers until the map finishes
  loading, then captures a clean per-level trace.
- **Record button** (Profile panel): UI twin for `profile` — Record/Stop
  + a duration combo (`1s/10s/30s/60s/120s/level`). Starting forces the profiler live (`Perf_SetLive(1)`)
  and exits replay first, so it captures even from a paused state (a paused
  profiler records nothing — the bare console command stalls there). On
  completion the trace auto-loads into replay for immediate inspection.
  `Perf_StopCapture()` finalizes early.
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
- `id1/particles/` — data-driven particle-effect presets (`*.pcl`, Quake KV-block); all `*.pcl` here load at startup (globbed via `COM_EnumMatchingFiles`, no index file). Authored in the in-game Particle editor mode (`edit_particle.c`); runtime in `r_emitter.c`.
- `ref/doom-data/` — Doom 1.9 shareware WAD (read by `zig build extract`)
- `ref/wolf3d-data/` — Wolf3D shareware data files (read by `zig build extract`)
- `ref/Quake-master/` — pristine upstream WinQuake (id-Software/Quake), kept as a diff baseline against `sdlquake/engine_src/`
- `ref/Quake-2-master/`, `ref/Quake-Tools-master/`, `ref/TrenchBroom-master/`, `ref/fteqw-master/`, `ref/DOOM-master/`, `ref/wolf3d-master/`, `ref/quake106/`, `ref/quake_map_source-master/`, `ref/Quake-2-Tools-master/` — upstream references, do not modify

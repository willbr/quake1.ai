# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires Zig (tested on 0.14.1 and 0.16) and original Quake data files (`pak0.pak`, `pak1.pak`) in `id1/`. SDL3 is vendored per-OS — no system install needed.

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

### Render pipeline (Phase 1)

Fixed 320×200 resolution. Each frame: Quake's software renderer writes 8-bit palette indices into `vid.buffer`. `VID_Update()` expands to 32-bit ARGB via `d_8to24table` and uploads to an `SDL_TEXTUREACCESS_STREAMING` texture, then presents. `SDL_SetRenderLogicalPresentation` with `INTEGER_SCALE` keeps pixels crisp at any window size.

### MCP server (Phase 2)

`sdlquake/mcp/mcp_server.c` — stdio JSON-RPC 2.0. A background thread reads stdin line-by-line and pushes to a mutex-protected queue. The main loop calls `MCP_Frame()` each frame to drain and respond. All game-state access on the main thread. Enabled with `--mcp` flag. Tools: `get_player_state`, `list_entities`, `set_cvar`. See `.mcp.json` for Claude Code integration.

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
- `engine_api_t` ABI bumps in Phase 8: 16 → 17 (M3 added `button3`/`button4` in `entvars_t`), 17 → 18 (M5 added `Sample_Lightmap`), 18 → 19 (cached-lightmap-deltas: added `Lightmap_AddDelta` + `Lightmap_ClearOwner`).

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

## Reference data

- `id1/` — Quake PAK files at repo root (required at runtime)
- `ref/doom-data/` — Doom 1.9 shareware WAD (read by `zig build extract`)
- `ref/wolf3d-data/` — Wolf3D shareware data files (read by `zig build extract`)
- `ref/Quake-master/` — pristine upstream WinQuake (id-Software/Quake), kept as a diff baseline against `sdlquake/engine_src/`
- `ref/Quake-2-master/`, `ref/Quake-Tools-master/`, `ref/TrenchBroom-master/`, `ref/fteqw-master/`, `ref/DOOM-master/`, `ref/wolf3d-master/`, `ref/quake106/`, `ref/quake_map_source-master/`, `ref/Quake-2-Tools-master/` — upstream references, do not modify

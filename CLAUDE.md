# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Requires Zig 0.16 and original Quake data files (`pak0.pak`, `pak1.pak`) in `id1/`. SDL3 is vendored — no system SDL3 needed.

```sh
zig build run
zig build run -- +map e1m1
```

No test suite exists yet. Build success and visual/audio correctness in-game are the verification methods.

## Architecture

This project is a port of the original WinQuake (1996 software renderer) from Win32/DirectX to SDL3, using Zig as the build system. The engine source has been forked into `sdlquake/engine_src/` so we can patch it as needed; the platform layer is fully replaced.

### Source split

- `sdlquake/engine_src/` — forked WinQuake engine source. We own and edit this. Compare against `ref/Quake-master/WinQuake/` for the pristine upstream baseline.
- `sdlquake/platform/` — SDL3 platform layer.
- `sdlquake/vendor/SDL3-3.4.8/` — vendored SDL3 headers + pre-built `.dll`/`.lib` for x64 Windows.
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
| 6 | planned | Port Wolf3D & Doom1 guns into Quake (sprites, sounds, behaviour) |
| 7 | planned | In-game 3D map editor |
| 8 | planned | Immersive-sim systems (physics, reactive AI, wind/smoke, light tier, Blink + Gust) |

### Phase 8 references

- Design spec: `docs/superpowers/specs/2026-05-04-immersive-sim-systems-design.md`
- Implementation plan (M1+M2+M2.5 only — AI substrate): `docs/superpowers/plans/2026-05-04-immersive-sim-m1-m2-ai-substrate.md`
- Future plans (M3 Blink/Gust, M4 wind, M5 light, M6 retrofit, M7 bespoke map) not yet written.
- All sim code lives in `sdlquake/game/sim/` inside the hot-reloadable `game.dll` (Approach 1 from the spec). M1+M2+M2.5 require no `engine_api_t` changes for the sim layer itself; one ABI bump for imgui debug-panel data hooks.

## Reference data

- `id1/` — Quake PAK files at repo root (required at runtime)
- `ref/doom-data/` — Doom 1.9 shareware WAD (read by `zig build extract`)
- `ref/wolf3d-data/` — Wolf3D shareware data files (read by `zig build extract`)
- `ref/Quake-master/` — pristine upstream WinQuake (id-Software/Quake), kept as a diff baseline against `sdlquake/engine_src/`
- `ref/Quake-2-master/`, `ref/Quake-Tools-master/`, `ref/TrenchBroom-master/`, `ref/fteqw-master/`, `ref/DOOM-master/`, `ref/wolf3d-master/`, `ref/quake106/`, `ref/quake_map_source-master/`, `ref/Quake-2-Tools-master/` — upstream references, do not modify

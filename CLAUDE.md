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

This project is a port of the original WinQuake (1996 software renderer) from Win32/DirectX to SDL3, using Zig as the build system. The engine source is unmodified; only the platform layer is replaced.

### Source split

- `Quake-master/WinQuake/` — upstream WinQuake source, **never modify**. Used as-is by `build.zig` (`wq_dir`).
- `sdlquake/platform/` — SDL3 platform layer; these are the files we own and edit.
- `sdlquake/vendor/SDL3-3.4.8/` — vendored SDL3 headers + pre-built `.dll`/`.lib` for x64 Windows.
- `sdlquake/mcp/` — MCP server (Phase 2, in progress).

### Platform layer files

| File | Replaces | Role |
|---|---|---|
| `sys_sdl.c` | `sys_win.c` | `main()`, timing (`SDL_GetTicksNS`), `Sys_Error`, `Sys_SendKeyEvents` |
| `vid_sdl.c` | `vid_win.c` | `SDL_Window` + `SDL_Renderer`; 8-bit framebuffer → `SDL_Texture` → present |
| `in_sdl.c` | `in_win.c` | SDL event polling; scancode → Quake key mapping; relative mouse |
| `snd_sdl.c` | `snd_win.c` | `SDL_AudioStream` get-callback feeding Quake's DMA ring buffer |
| `net_sdl.c` | `net_wins.c` | Winsock net stub |
| `winquake.h` | `winquake.h` | Stub: replaces DirectDraw/DirectSound types with no-op equivalents; shadows the original |

`sdlquake/platform/` is on the include path **before** `Quake-master/WinQuake/`, so our `winquake.h` shadows the original.

### Build flags

Engine files (`Quake-master/WinQuake/*.c`) are compiled with `-std=gnu89 -fcommon -fno-sanitize=undefined`. The `-fno-sanitize=undefined` is intentional — the original engine relies on float→int truncation UB that is well-defined on x86.

Platform files (`sdlquake/platform/*.c`) omit `-std=gnu89` (they're written in modern C).

### Key fixes applied to get Phase 1 working

- `d_surf.c:143` — `surfcache_t` alignment changed from 4→8 bytes for x64 pointer members.
- `host.c` — `S_Init()` gated behind `#ifdef SDLQUAKE` guard (was `#ifndef _WIN32`).
- `in_sdl.c` `IN_Move` — calls `V_StopPitchDrift()` every frame, not just on mouse movement, to prevent re-arming after `v_centermove` seconds of walking.
- `vid_sdl.c` — `vid.conbuffer` points to the same buffer as `vid.buffer`; `Draw_Character` writes to `conbuffer`.

### Render pipeline (Phase 1)

Fixed 320×200 resolution. Each frame: Quake's software renderer writes 8-bit palette indices into `vid.buffer`. `VID_Update()` expands to 32-bit ARGB via `d_8to24table` and uploads to an `SDL_TEXTUREACCESS_STREAMING` texture, then presents. `SDL_SetRenderLogicalPresentation` with `INTEGER_SCALE` keeps pixels crisp at any window size.

### MCP server (Phase 2, in progress)

Planned: `sdlquake/mcp/mcp_server.c` — stdio JSON-RPC 2.0. A background thread reads stdin, parses with `jsmn` (single-header tokenizer), and pushes requests to a queue. The main loop calls `MCP_Frame()` each frame to drain the queue and write responses to stdout. All game-state access happens on the main thread to avoid races. Enabled with `--mcp` flag. MVP tools: `get_player_state`, `list_entities`, `set_cvar`.

### Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | ✅ done | SDL3 port + Zig build |
| 2 | in progress | MCP server |
| 3 | planned | Hot-reload (`game_api_t` ABI, `game.dll`) |
| 4 | planned | cimgui overlay + SQLite |
| 5 | planned | In-game 3D map editor + QuakeC → C |

## Reference data

Game data for reference/future work is committed at the repo root:
- `id1/` — Quake PAK files (required at runtime)
- `doom-data/` — Doom 1.9 shareware WAD
- `wolf3d-data/` — Wolf3D shareware data files
- `Quake-2-master/`, `Quake-Tools-master/`, `TrenchBroom-master/`, `fteqw-master/` etc. — upstream references, do not modify

# Engine Documentation

This directory documents `quake1.ai` — a fork of the 1996 WinQuake software
renderer ported to SDL3/Zig, with hot-reload, an MCP server, an in-game
editor, and an immersive-sim layer (Phase 8) bolted on top.

The original WinQuake source is preserved in `sdlquake/engine_src/` and patched
where necessary; the platform layer (`sys_*`, `vid_*`, `in_*`, `snd_*`,
`net_*`) is fully replaced. Game logic was ported from QuakeC to C
(`sdlquake/game/`) and lives in a hot-reloadable DLL.

## Map

| Doc | Covers |
|---|---|
| [architecture.md](architecture.md) | Source-tree split, build flags, frame loop, threading, Zig build commands |
| [rendering.md](rendering.md) | 8-bit software renderer, palette, lightmaps + coloured lighting, decals, dynamic lights, model interpolation |
| [platform.md](platform.md) | SDL3 platform layer: window/video, audio mixer + DMA ring, input scancode map, crash handler |
| [networking.md](networking.md) | Quake net protocol v15, loopback + datagram drivers, message bits, server/client message types |
| [game-loop.md](game-loop.md) | Server frame, edicts, button bits, hot-reload (`game_api_t` ABI), VFS shim, cvar bridge |
| [mcp.md](mcp.md) | MCP server: JSON-RPC 2.0 over stdio or HTTP/SSE, tool catalogue, queue model, testing |
| [phase8-immersive-sim.md](phase8-immersive-sim.md) | Stimulus bus, AI FSM, navmesh + A\*, wind/smoke voxel grid, light tier, abilities (Blink/Gust) |
| [editor.md](editor.md) | Phase 7 in-game `.map` editor, in-process qbsp/light/vis libs |
| [file-formats.md](file-formats.md) | PAK, BSP29, MDL (alias), SPR, WAD2, `.lit` coloured-light sidecar, `.dem`, `.map`, `.sav`, `.cfg`, `.pcx`, `.lmp` |

## What still lives in the old docs

`docs/port-audit.md` is the function-by-function QuakeC → C port audit that
shipped at the end of Phase 5. `docs/superpowers/specs/` and
`docs/superpowers/plans/` are dated design docs for individual features —
those are useful history, but they don't replace the per-subsystem references
above.

## Build & run quick reference

```sh
zig build run -- +map e1m1               # build engine + game.dll, run shareware ep1m1
zig build run -- --hot-reload +map e1m1  # same, with game.dll auto-reload polling
zig build game                           # rebuild only game.dll (fast iteration)
zig build run -- --mcp-stdio             # stdio MCP transport (Claude Code spawns the game)
zig build run -- --mcp-http 7777 +map e1m1
```

Prerequisites: Zig 0.16, original Quake `id1/pak0.pak` + `id1/pak1.pak` at the
repo root. SDL3 is vendored under `sdlquake/vendor/SDL3-3.4.8/`.

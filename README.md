# quake1.ai

WinQuake (1996 software renderer) ported to SDL3 + Zig, with a hot-reloading game layer, in-game 3D map editor, and an MCP server for Claude Code integration.

## Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | done | SDL3 port + Zig build |
| 2 | done | MCP server |
| 3 | done | Hot-reload (`game_api_t` ABI, `game.dll`) |
| 4 | done | Dear ImGui dev overlay (F12) |
| 5 | done | QuakeC → C (progs ported to hot-reloadable game.dll) |
| 6 | done | Wolf3D + Doom1 guns (sprites, sounds, fire rates) |
| 7 | in progress | In-game 3D map editor |
| 8 | planned | Immersive-sim systems (physics, reactive AI, Blink + Gust) |

## Build

Requires Zig 0.16 and `id1/pak0.pak` + `id1/pak1.pak`. SDL3 is vendored.

```sh
zig build run -- +map e1m1                  # build and run
zig build run -- --hot-reload +map e1m1    # hot-reload game.dll on change
zig build game                              # rebuild game.dll only (fast iteration)
```

## Flags

| Flag | Effect |
|---|---|
| `--mcp` | MCP server on stdio (Claude Code spawns the process) |
| `--mcp-http <port>` | MCP server over HTTP/SSE on `localhost:<port>` (connect to running game) |
| `--hot-reload` | Poll `game.dll` for changes and reload without restart |
| `+map <name>` | Load map on startup (e.g. `+map e1m1`) |
| `--list-cvars` | Print all registered cvars and exit |

## Source

Based on the original [WinQuake source release](https://github.com/id-Software/Quake) by id Software.

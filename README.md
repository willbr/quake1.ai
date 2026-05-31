# quake1.ai

WinQuake (1996 software renderer) ported to SDL3 + Zig, with a hot-reloading game layer, in-game 3D map editor, immersive-sim gameplay systems (reactive AI, player abilities, fire propagation), and an MCP server for Claude Code integration.

## Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | done | SDL3 port + Zig build |
| 2 | done | MCP server |
| 3 | done | Hot-reload (`game_api_t` ABI, `game.dll`) |
| 4 | done | Dear ImGui dev overlay (F3) |
| 5 | done | QuakeC → C (progs ported to hot-reloadable game.dll) |
| 6 | done | Wolf3D + Doom1 guns (sprites, sounds, fire rates) |
| 7 | done | In-game 3D map editor (F2) |
| 8 | M3–M6, M8 done; M7 stub | Immersive-sim systems (stimulus bus, FSM AI + navmesh, Blink + Gust, wind/smoke, light tier, Fire & Oil) |

## Build

Requires Zig (0.14.1 or 0.16). Shareware `id1/` assets are committed; drop in a `pak1.pak` for the registered episodes. SDL3 is vendored for **Windows x64** and **macOS arm64** — no system install needed.

```sh
zig build run -- +map e1m1                  # build and run
zig build run -- --hot-reload +map e1m1    # hot-reload game.dll on change
zig build game                              # rebuild game.dll only (fast iteration)
```

## Flags

| Flag | Effect |
|---|---|
| `--mcp-stdio` | MCP server on stdio (Claude Code spawns the process) |
| `--mcp-http <port>` | MCP server over HTTP/SSE on `localhost:<port>` (connect to running game) |
| `--hot-reload` | Poll `game.dll` for changes and reload without restart |
| `+map <name>` | Load map on startup (e.g. `+map e1m1`) |
| `+bot 1` | Spawn the navmesh-driven AI bot on startup |
| `--headless` | Run the server + bot with no window/audio/GUI (automated tests) |
| `-nofocus` | Open the window in the background without stealing focus |
| `--list-cvars` | Print all registered cvars and exit |

## Scripts

- `scripts/mcp_call.py` — one-shot CLI for the running MCP server (HTTP/SSE).
  ```sh
  python scripts/mcp_call.py screenshot
  python scripts/mcp_call.py console_exec '{"command":"r_decals_debug 1"}'
  python scripts/mcp_call.py get_player_state
  ```
- `scripts/run_ai_tests.sh` — drive the AI bot through the chained `ai_t01..t09` nav test maps and check every map's marker is reached. `HEADLESS=1` runs windowless.
  ```sh
  ./scripts/run_ai_tests.sh             # windowed
  HEADLESS=1 ./scripts/run_ai_tests.sh  # headless
  ```

## Source

Based on the original [WinQuake source release](https://github.com/id-Software/Quake) by id Software.

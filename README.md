# quake1.ai

WinQuake (1996 software renderer) ported to SDL3 + Zig. The scene is still
**100% software-rendered** — the GPU only does an 8-bit→RGBA palette-LUT present
— but everything around it is new: a hot-reloading game layer, a threaded/SIMD
rasterizer, an in-game 3D editor (maps, particles, skeletal actors), immersive-sim
gameplay systems (reactive AI + navmesh, player abilities, wind/smoke, fire
propagation), savegames, a dev MPEG-1 recorder, a per-frame profiler, and an MCP
server for Claude Code integration.

## Systems

| System | What it is | Detail |
|---|---|---|
| Render pipeline | 100% software renderer; GPU only does an 8-bit→RGBA palette-LUT present (SDL_GPU shader), plus CRT scanlines and ImGui compositing | [docs/render-pipeline.md](docs/render-pipeline.md) |
| Threaded span fill | The CPU rasterizer's span fill is parallelised across a worker pool (3.14→0.75 ms on a 10-core Mac); `r_threads` cvar | [docs/renderer-threaded-fill.md](docs/renderer-threaded-fill.md) |
| Immersive-sim (Phase 8) | Stimulus bus + FSM AI + navmesh A*, Blink/Gust abilities, voxel wind/smoke, light tiers, Fire & Oil propagation — all in the hot-reloadable `game.dll` | [docs/phase8-fire-oil.md](docs/phase8-fire-oil.md) |
| In-game 3D editor (F2) | ImGui-docked editor with Map, Particle, and Actor authoring modes | [docs/editor-modules.md](docs/editor-modules.md) |
| Skeletal actors | IQM characters authored in-engine (cubes-first), with runtime procedural face / ponytail / clip layers | [docs/skeletal-actors.md](docs/skeletal-actors.md) |
| Particles | Data-driven particle presets (`id1/particles/*.pcl`) authored in the Particle editor mode | [docs/editor-modules.md](docs/editor-modules.md) |
| Savegames | Full entity-state `save`/`load`; entvars callback pointers relocate across a DLL reload (Quake 2 `g_save` pattern), `SAVEGAME_VERSION` 7 | — |
| Perf instrumentation | Per-frame scoped timers, live overlay graph (`showperf`), Chrome-trace capture (`profile`), and replay | [docs/perf-instrumentation.md](docs/perf-instrumentation.md) |
| Video recording | Dev MPEG-1 screen recorder (`recordvideo`/`stopvideo`), parallel encode | [docs/video-recording.md](docs/video-recording.md) |

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

# quake1.ai

A modern port of the original WinQuake to SDL3, with a hot-reloading game layer, in-game 3D map editor, and an MCP server so Claude can observe and control the running game.

## What this is

WinQuake — Carmack's 1996 software-rendered Quake — rebuilt on a foundation that makes it fun to hack on:

- **SDL3** replaces Win32/DirectX, so it runs everywhere
- **Zig** build system replaces MSVC project files
- **Hot-reload** splits the engine and game at a C ABI boundary; edit game logic and reload in ~1s without restarting
- **cimgui** gives an in-game ImGui overlay for inspecting entities, editing cvars, and watching perf
- **SQLite** replaces `.sav` files and `config.cfg` — everything is in `quake.db`
- **In-game 3D map editor** — press Tab, click a wall to select its brush, drag to move it, hit Recompile; the BSP rebuilds in the background and reloads while you stay in the game. No external tools.
- **MCP server** — run with `--mcp` and Claude Code can call tools to read game state, spawn entities, move brushes, and reshape levels in real time

## Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | planned | SDL3 port + Zig build |
| 2 | planned | Hot-reload core (`game_api_t` ABI, `game.dll`) |
| 3 | planned | cimgui dev overlay + SQLite |
| 4 | planned | In-game 3D map editor + QuakeC → C |
| 5 | planned | MCP server |

## Building

> Phase 1 in progress — build instructions coming soon.

```sh
zig build run
```

Requires SDL3 and Zig 0.14+. You'll need original Quake data files (`pak0.pak`, `pak1.pak`) in `id1/`.

## MCP

```json
{
  "mcpServers": {
    "quake": {
      "command": "zig-out/bin/quake",
      "args": ["--mcp", "+map", "e1m1"]
    }
  }
}
```

Available tools: `get_player_state`, `list_entities`, `spawn_entity`, `set_cvar`, `reload_map`, `query_db`, `get_brush_list`, `move_brush`, `enter_edit_mode`.

## Source

Based on the original [WinQuake source release](https://github.com/id-Software/Quake) by id Software.

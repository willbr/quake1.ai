# quake1.ai

A modern port of the original WinQuake to SDL3, with a hot-reloading game layer, in-game 3D map editor, and an MCP server so Claude can observe and control the running game.

## What this is

WinQuake — Carmack's 1996 software-rendered Quake — rebuilt on a foundation that makes it fun to hack on:

- **SDL3** replaces Win32/DirectX, so it runs everywhere
- **Zig** build system replaces MSVC project files
- **MCP server** — run with `--mcp` and Claude Code can call tools to read game state, spawn entities, move brushes, and reshape levels in real time
- **Hot-reload** splits the engine and game at a C ABI boundary; edit game logic and reload in ~1s without restarting
- **cimgui** gives an in-game ImGui overlay for inspecting entities, editing cvars, and watching perf
- **SQLite** replaces `.sav` files and `config.cfg` — everything is in `quake.db`
- **In-game 3D map editor** — press Tab, click a wall to select its brush, drag to move it, hit Recompile; the BSP rebuilds in the background and reloads while you stay in the game

## Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | ✅ done | SDL3 port + Zig build |
| 2 | ✅ done | MCP server |
| 3 | ✅ done | Hot-reload core (`game_api_t` ABI, `game.dll`) |
| 4 | planned | cimgui dev overlay + SQLite |
| 5 | planned | In-game 3D map editor + QuakeC → C |

## Building

Requires Zig 0.16 and original Quake data files (`pak0.pak`, `pak1.pak`) in `id1/`. SDL3 is vendored.

```sh
zig build run -- +map e1m1   # build everything and run
zig build game               # rebuild only game.dll (hot-reload iteration)
```

## MCP

Add to your Claude Code MCP config (`.mcp.json` in the project root):

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

Available tools (Phase 2 MVP): `get_player_state`, `list_entities`, `set_cvar`.

Later phases add: `spawn_entity`, `reload_map`, `query_db`, `get_brush_list`, `move_brush`, `enter_edit_mode`.

## Source

Based on the original [WinQuake source release](https://github.com/id-Software/Quake) by id Software.

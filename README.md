# quake1.ai

A modern port of the original WinQuake to SDL3, with a hot-reloading game layer, in-game 3D map editor, and an MCP server so Claude can observe and control the running game.

## What this is

WinQuake — Carmack's 1996 software-rendered Quake — rebuilt on a foundation that makes it fun to hack on:

- **SDL3** replaces Win32/DirectX, so it runs everywhere
- **Zig** build system replaces MSVC project files
- **MCP server** — run with `--mcp` and Claude Code can call tools to read game state and set cvars in real time
- **Hot-reload** splits the engine and game at a C ABI boundary; edit game logic and reload in ~1s without restarting
- **Dear ImGui overlay** — press F12 for a live dev overlay: perf stats, filterable cvar editor with descriptions, entity table, and a console with tab-completion and command input
- **In-game 3D map editor** — planned: press Tab, click a wall to select its brush, drag to move it, hit Recompile; the BSP rebuilds in the background and reloads while you stay in the game

## Phases

| Phase | Status | Goal |
|---|---|---|
| 1 | ✅ done | SDL3 port + Zig build |
| 2 | ✅ done | MCP server |
| 3 | ✅ done | Hot-reload (`game_api_t` ABI, `game.dll`) |
| 4 | ✅ done | Dear ImGui dev overlay |
| 5 | planned | In-game 3D map editor + QuakeC → C |

## Building

Requires Zig 0.16 and original Quake data files (`pak0.pak`, `pak1.pak`) in `id1/`. SDL3 is vendored.

```sh
zig build run -- +map e1m1   # build everything and run
zig build game               # rebuild only game.dll (hot-reload iteration)
```

## Dev overlay (F12)

Press **F12** in-game to open the ImGui overlay. All keyboard and mouse input is captured by the overlay while it is open.

| Panel | Description |
|---|---|
| Perf | FPS and frame time |
| Cvars | Filterable table of all cvars with descriptions; edit values inline and press Enter to apply |
| Entities | Live table of all active edicts (classname, origin) |
| Console | Scrolling console log with a command input box; Tab completes commands and cvar names |

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

Available tools: `get_player_state`, `list_entities`, `set_cvar`.

## Source

Based on the original [WinQuake source release](https://github.com/id-Software/Quake) by id Software.

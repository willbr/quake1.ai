# opendoors / opendoors_secret console commands — design

Status: approved 2026-05-13

## Goal

Two always-available console commands:

- `opendoors` — opens every `func_door` in the current level
- `opendoors_secret` — triggers every `func_door_secret`

Useful as a debug aid (skip key hunting, verify door wiring, unblock progression
when iterating on map logic).

## Why two commands

Regular doors and secret doors share a `classname` family but nothing else:
`func_door_secret` uses a six-stage movement state machine
(`fd_secret_use` → `move1..6` → `done`). Folding both under one command would
conflate two unrelated behaviors and would always do too much for whichever
case the user actually wanted.

## Architecture

The commands are **registered in the engine** and **implemented in the
hot-reloadable game DLL**.

Why split:

1. The engine's `Cmd_AddCommand` calls `Sys_Error` after `host_initialized`.
   The game DLL's `game_init` runs again on every hot-reload, so DLL-side
   registration would crash the engine on the second reload.
2. Door state and the relevant `static` helpers (`door_fire`,
   `fd_secret_use`) live in `game.dll` and shouldn't be exported.
3. Hot-reload still works because the engine keeps the command registration;
   the dispatch target (`g_game_api`) is repointed on each reload.

## Changes

### `sdlquake/game/game_api.h`

- Bump `GAME_API_VERSION` from 14 to 15.
- Add two function-pointer slots to `game_api_t`:
  ```c
  void (*open_all_doors)(void);
  void (*open_all_secret_doors)(void);
  ```

### `sdlquake/game/doors.c`

Two new non-static functions.

`Doors_OpenAll()`:
- Walks edicts via `eng->ED_Find(prev, "classname", "door")`.
- Skips non-master doors (`d->v.owner && d->v.owner != d`) — `door_fire`
  walks the `v.enemy` chain on its own.
- Skips doors already at `STATE_UP` or `STATE_TOP` so a `DOOR_TOGGLE` door
  isn't accidentally closed.
- Sets `g->activator = g->world` so `SUB_UseTargets` inside `door_fire` has a
  valid activator.
- Calls `door_fire(d)` on each master.

`Doors_OpenAllSecret()`:
- Walks edicts via `eng->ED_Find(prev, "classname", "func_door_secret")`.
- For each, sets `g->activator = g->world` and calls `d->v.use(d, g->world)`
  (which is `fd_secret_use`).

### `sdlquake/game/game_main.c`

Add `.open_all_doors = Doors_OpenAll,` and
`.open_all_secret_doors = Doors_OpenAllSecret,` to the `s_api` initializer.

### `sdlquake/engine_src/host_cmd.c`

In `Host_InitCommands`, add:

```c
Cmd_AddCommand("opendoors",        Host_OpenDoors_f);
Cmd_AddCommand("opendoors_secret", Host_OpenDoorsSecret_f);
```

Handlers use `g_game_api` (exported from `sdlquake/engine/hotreload.h`) to
dispatch. Order of checks:

1. If `!sv.active`, print `"opendoors: no server running\n"` and return —
   matches how `Host_God_f` and friends gate.
2. If `!g_game_api || !g_game_api->open_all_doors`, print
   `"opendoors: not available in this build\n"` and return.
3. Otherwise call `g_game_api->open_all_doors()`.

Same pattern for `opendoors_secret`.

## Edge cases

- **Linked doors**: only masters are fired; `door_fire` walks the
  `v.enemy` chain itself.
- **Toggle doors**: state check prevents firing an open door (which would
  close it).
- **Locked doors**: `door_fire` doesn't gate on keys — `door_touch` does.
  This naturally bypasses key requirements.
- **Hot-reload**: command stays registered in the engine; dispatch target
  follows `g_game_api`.
- **No level loaded / no game DLL**: handler prints a single info line.

## Out of scope

- `func_button` and other triggers — only doors were requested.
- Cheat gating (sv_cheats / developer) — user chose always-available.
- Multiplayer/dedicated-server restrictions — not relevant to this project.

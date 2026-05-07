# Hot-Reload Flag — Design Spec

**Date:** 2026-05-07
**Phase:** 3 (hot-reloadable game DLL)
**Scope:** Lock the game-DLL auto-reload polling behind an opt-in `--hot-reload` CLI flag. Default: polling off.

## Goal

Today the engine polls `zig-out/bin/game.dll`'s mtime every ~1 s and reloads on change. That's a developer-iteration feature, but it runs unconditionally for every invocation of the engine. Make it opt-in: `--hot-reload` enables polling; without it, the DLL is loaded once at startup and never reloaded.

## Non-goals

- Skipping the initial DLL load. The game lives in `game.dll` (`NATIVE_GAME=1`); it must still load on startup or the engine has nothing to run.
- Static-linking the game into the engine. Out of scope — would require build-system changes to produce two game variants.
- A console cvar toggle. CLI flag only, mirroring `--mcp`.
- Removing or refactoring the existing copy step (`game.dll` → `game_loaded.dll`). It's harmless when polling is off and removing it is unrelated to the goal.
- Behavior change in the `!NATIVE_GAME` stub branch (already a no-op).

## Background

`sdlquake/engine/hotreload.c` exposes three entry points:

- `HotReload_Init()` — called once at startup from `sys_sdl.c:217`. Loads `game.dll`, copies it to `game_loaded.dll`, calls `game_api->init()`.
- `HotReload_Frame(float dt)` — called every frame from `sys_sdl.c:225`. Increments a counter and, every `RELOAD_CHECK_INTERVAL` frames (60), polls `get_mtime()` and reloads if changed.
- `HotReload_Shutdown()` — unload on exit.

Precedent for opt-in dev features lives a few lines up at `sys_sdl.c:200`:

```c
if (COM_CheckParm("--mcp"))
    MCP_Init();
```

`MCP_Init()` flips a `mcp_active` flag that `MCP_Frame()` reads to gate per-frame work. The hot-reload flag follows the same shape.

## Architecture

### Decision 1 — gate the polling, not the initial load

`HotReload_Init()` always runs (unchanged). The flag only controls whether `HotReload_Frame()`'s mtime check fires. With the flag off, `HotReload_Frame()` becomes a near-zero-cost early return.

Rationale: the initial load is functionally required (the game can't run without it). Auto-reload-on-change is the actual dev feature, and the only behavior worth gating.

### Decision 2 — file-static enable flag with a setter

Mirrors the `mcp_active` pattern. Inside `hotreload.c`:

```c
static int polling_enabled = 0;

void HotReload_EnablePolling(void)
{
    polling_enabled = 1;
    Con_Printf("hotreload: polling enabled\n");
}

void HotReload_Frame(float dt)
{
    (void)dt;
    if (!polling_enabled) return;

    static int counter = 0;
    if (++counter >= RELOAD_CHECK_INTERVAL)
    {
        counter = 0;
        SDL_Time t = get_mtime(GAME_DLL_SRC);
        if (t != 0 && t != dll_mtime)
            do_load();
    }
}
```

The `Con_Printf` is for visibility — without it there's no signal in the console that the dev feature is active.

The `!NATIVE_GAME` branch gets a matching stub:

```c
void HotReload_EnablePolling(void) {}
```

### Decision 3 — wire-up in `sys_sdl.c`

After the existing `HotReload_Init();` at `sys_sdl.c:217`:

```c
HotReload_Init();
if (COM_CheckParm("--hot-reload"))
    HotReload_EnablePolling();
```

`COM_CheckParm` is already available (used for `--mcp`, `-dedicated`, `-heapsize`).

### Decision 4 — header

Add to `sdlquake/engine/hotreload.h`:

```c
void HotReload_EnablePolling(void);
```

### Decision 5 — documentation

Update `CLAUDE.md`:

- Phase 3 paragraph: note that polling is opt-in via `--hot-reload`.
- Build commands block: change the fast-iteration recipe to `zig build run -- --hot-reload` in one terminal + `zig build game` in the other.

## Components touched

| File | Why |
|---|---|
| `sdlquake/engine/hotreload.c` | Add `polling_enabled` flag + `HotReload_EnablePolling()`; gate `HotReload_Frame()`'s polling; matching no-op stub in the `!NATIVE_GAME` branch |
| `sdlquake/engine/hotreload.h` | Declare `HotReload_EnablePolling(void)` |
| `sdlquake/platform/sys_sdl.c` | Call `HotReload_EnablePolling()` when `--hot-reload` is on the command line |
| `CLAUDE.md` | Mention the flag in Phase 3 description and build commands |

No `engine_api_t` / `game_api_t` ABI changes. No new translation units. No build-system changes.

## Verification

No automated tests (per CLAUDE.md). Manual checks:

1. **Default off.** `zig build run -- +map e1m1` — game starts normally; `game.dll` loads once. Touch `zig-out/bin/game.dll` (e.g., via `zig build game` in another terminal) and confirm the running engine does **not** reload — no `hotreload: game.dll reloaded` line in the console.
2. **Flag on.** `zig build run -- --hot-reload +map e1m1` — `hotreload: polling enabled` appears in console at startup. Trigger a rebuild via `zig build game`; confirm `hotreload: game.dll reloaded` shows up within ~1 s and the new DLL takes effect (e.g., a Con_Printf added in game code appears).
3. **Flag with `--mcp`.** Both flags together still work — they're independent.
4. **Bad DLL still handled.** With `--hot-reload`, replacing `game.dll` with a build that has an ABI version mismatch should print `hotreload: ABI version mismatch (want N)` and leave the previously loaded copy active. (Pre-existing behavior — verifying the gate didn't accidentally break it.)

## Open questions / future work (out of scope)

- Whether to expose `polling_enabled` as a runtime cvar so it can be toggled without restarting. Skipped — restart is cheap.
- Whether to skip the `game.dll` → `game_loaded.dll` copy when polling is off (the copy exists so zig can overwrite the original mid-run; with polling off the copy is unnecessary). Skipped — harmless and unrelated.

## References

- `sdlquake/engine/hotreload.c` — current polling implementation
- `sdlquake/platform/sys_sdl.c:200-225` — entry point and `--mcp` precedent
- `sdlquake/mcp/mcp_server.h` — `mcp_active` flag pattern this design mirrors
- `CLAUDE.md` § Phase 3 — current Phase 3 description and build commands

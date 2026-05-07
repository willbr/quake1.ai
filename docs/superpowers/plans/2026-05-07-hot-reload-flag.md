# Hot-Reload Flag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an opt-in `--hot-reload` CLI flag that gates the game-DLL mtime polling. Default: polling off; DLL still loads once at startup.

**Architecture:** Add a file-static `polling_enabled` flag in `hotreload.c` set by a new `HotReload_EnablePolling()` setter. `HotReload_Frame()` early-returns when the flag is unset. The setter is called from `sys_sdl.c` after `HotReload_Init()` only when `COM_CheckParm("--hot-reload")` finds the flag. Mirrors the existing `--mcp` opt-in pattern.

**Tech Stack:** C (gnu89 for engine, modern C for platform), Zig 0.16 build, SDL3.

**Spec:** `docs/superpowers/specs/2026-05-07-hot-reload-flag-design.md`

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `sdlquake/engine/hotreload.h` | modify | Declare `HotReload_EnablePolling(void)` |
| `sdlquake/engine/hotreload.c` | modify | Add `polling_enabled` flag, setter, early-return in Frame; matching no-op stub in `!NATIVE_GAME` branch |
| `sdlquake/platform/sys_sdl.c` | modify | Call `HotReload_EnablePolling()` when `--hot-reload` is on the command line |
| `CLAUDE.md` | modify | Mention the flag in Phase 3 description and build commands |

No new files. No ABI changes. No build-system changes.

**Note on testing.** This project has no automated test suite (per `CLAUDE.md`). Verification is by build success and manual in-engine smoke checks. Each task that touches code has a build step; functional verification is consolidated into Task 4.

---

## Task 1: Declare and define `HotReload_EnablePolling`

**Files:**
- Modify: `sdlquake/engine/hotreload.h:6-8`
- Modify: `sdlquake/engine/hotreload.c` (around line 870 for `NATIVE_GAME` branch; around line 902 for the stub branch; around line 878 for `HotReload_Frame`)

- [ ] **Step 1: Add the declaration to `hotreload.h`**

Open `sdlquake/engine/hotreload.h` and replace lines 6-8 with:

```c
void HotReload_Init(void);
void HotReload_Frame(float dt);
void HotReload_Shutdown(void);
void HotReload_EnablePolling(void);
```

- [ ] **Step 2: Add the file-static flag in `hotreload.c`**

In `sdlquake/engine/hotreload.c`, find the `// Public API` divider comment (around line 866-868):

```c
// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

#define RELOAD_CHECK_INTERVAL 60   // frames between mtime polls (~1 s at 60 fps)
```

Insert a `polling_enabled` flag immediately above `#define RELOAD_CHECK_INTERVAL`:

```c
// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static int polling_enabled = 0;

#define RELOAD_CHECK_INTERVAL 60   // frames between mtime polls (~1 s at 60 fps)
```

- [ ] **Step 3: Gate `HotReload_Frame()` on `polling_enabled`**

In `sdlquake/engine/hotreload.c`, find the existing `HotReload_Frame` (around line 878-891):

```c
void HotReload_Frame(float dt)
{
    (void)dt;
    static int counter = 0;
    if (++counter >= RELOAD_CHECK_INTERVAL)
    {
        counter = 0;
        SDL_Time t = get_mtime(GAME_DLL_SRC);
        if (t != 0 && t != dll_mtime)
            do_load();
    }
    // Note: start_frame is now called from sv_phys.c SV_Physics() with proper
    // game_globals sync, not here. HotReload_Frame is just for reload polling.
}
```

Replace with:

```c
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
    // Note: start_frame is now called from sv_phys.c SV_Physics() with proper
    // game_globals sync, not here. HotReload_Frame is just for reload polling.
}
```

- [ ] **Step 4: Add the `HotReload_EnablePolling` definition in the `NATIVE_GAME` branch**

Find `HotReload_Shutdown` in `sdlquake/engine/hotreload.c` (around line 893-896):

```c
void HotReload_Shutdown(void)
{
    do_unload();
}
```

Insert `HotReload_EnablePolling` immediately after `HotReload_Shutdown` (still inside the `#if NATIVE_GAME` block — i.e., **before** `#else /* !NATIVE_GAME — game DLL not used */`):

```c
void HotReload_Shutdown(void)
{
    do_unload();
}

void HotReload_EnablePolling(void)
{
    polling_enabled = 1;
    Con_Printf("hotreload: polling enabled\n");
}
```

- [ ] **Step 5: Add the matching no-op stub in the `!NATIVE_GAME` branch**

Find the `!NATIVE_GAME` stub block at the bottom of `sdlquake/engine/hotreload.c` (around line 898-906):

```c
#else /* !NATIVE_GAME — game DLL not used */

game_api_t *g_game_api = NULL;

void HotReload_Init(void)     {}
void HotReload_Frame(float dt) { (void)dt; }
void HotReload_Shutdown(void) {}

#endif /* NATIVE_GAME */
```

Replace with:

```c
#else /* !NATIVE_GAME — game DLL not used */

game_api_t *g_game_api = NULL;

void HotReload_Init(void)     {}
void HotReload_Frame(float dt) { (void)dt; }
void HotReload_Shutdown(void) {}
void HotReload_EnablePolling(void) {}

#endif /* NATIVE_GAME */
```

- [ ] **Step 6: Build to confirm the engine still compiles**

Run: `zig build`
Expected: build succeeds with no errors. Warnings unchanged.

- [ ] **Step 7: Commit**

```powershell
git add sdlquake/engine/hotreload.h sdlquake/engine/hotreload.c
git commit -m "feat(hotreload): add HotReload_EnablePolling, gate Frame() on flag"
```

---

## Task 2: Wire `--hot-reload` into `sys_sdl.c`

**Files:**
- Modify: `sdlquake/platform/sys_sdl.c:217`

- [ ] **Step 1: Add the `COM_CheckParm` call after `HotReload_Init()`**

Find this block in `sdlquake/platform/sys_sdl.c` (lines 215-217):

```c
    Sys_Printf("Host_Init\n");
    Host_Init(&parms);
    HotReload_Init();
```

Replace with:

```c
    Sys_Printf("Host_Init\n");
    Host_Init(&parms);
    HotReload_Init();
    if (COM_CheckParm("--hot-reload"))
        HotReload_EnablePolling();
```

`COM_CheckParm` is already used at line 198 (`-dedicated`), line 200 (`--mcp`), and line 207 (`-heapsize`); no new include needed. `HotReload_EnablePolling` is declared in `hotreload.h`, which `sys_sdl.c` already includes (it's the source of the `HotReload_Init`/`HotReload_Frame` declarations used at lines 217 and 225).

- [ ] **Step 2: Build to confirm**

Run: `zig build`
Expected: build succeeds with no errors.

- [ ] **Step 3: Commit**

```powershell
git add sdlquake/platform/sys_sdl.c
git commit -m "feat(hotreload): wire --hot-reload CLI flag in sys_sdl.c"
```

---

## Task 3: Update `CLAUDE.md`

**Files:**
- Modify: `CLAUDE.md` (Phase 3 paragraph + Build commands block)

- [ ] **Step 1: Update the Phase 3 description**

In `CLAUDE.md`, find the `### Hot-reload game DLL (Phase 3)` section. Locate this paragraph:

```markdown
`sdlquake/engine/hotreload.c` + `sdlquake/game/` — `game_api_t` ABI separates game logic from the engine. `HotReload_Frame()` polls `zig-out/bin/game.dll` mtime every ~1 s; on change it copies the DLL to `game_loaded.dll` (so zig can overwrite the original), unloads the old copy, loads the new one, and calls `game_api->init()` again. The fast iteration workflow is `zig build game` in a separate terminal.
```

Replace with:

```markdown
`sdlquake/engine/hotreload.c` + `sdlquake/game/` — `game_api_t` ABI separates game logic from the engine. `HotReload_Init()` loads `zig-out/bin/game.dll` once at startup. With `--hot-reload`, `HotReload_Frame()` then polls the DLL's mtime every ~1 s; on change it copies the DLL to `game_loaded.dll` (so zig can overwrite the original), unloads the old copy, loads the new one, and calls `game_api->init()` again. Without the flag, polling is off — the DLL is loaded once and stays put. The fast-iteration workflow is `zig build run -- --hot-reload` in one terminal + `zig build game` in another.
```

- [ ] **Step 2: Update the Build commands block**

Find the `### Build commands` section in `CLAUDE.md`:

````markdown
### Build commands

```sh
zig build run -- +map e1m1    # build everything (engine + game.dll) and run
zig build game                # rebuild only game.dll (fast hot-reload iteration)
```
````

Replace with:

````markdown
### Build commands

```sh
zig build run -- +map e1m1               # build everything (engine + game.dll) and run
zig build run -- --hot-reload +map e1m1  # same, but enable game.dll auto-reload polling
zig build game                           # rebuild only game.dll (fast hot-reload iteration; pair with --hot-reload above)
```
````

- [ ] **Step 3: Commit**

```powershell
git add CLAUDE.md
git commit -m "docs: document --hot-reload flag in CLAUDE.md"
```

---

## Task 4: Manual verification

This task does not change any code. It runs the engine in both modes and confirms behavior matches the spec.

**Files:** none (verification only)

- [ ] **Step 1: Verify default-off behavior**

In one terminal, run:

```powershell
zig build run -- +map e1m1
```

Expected console output:
- No `hotreload: polling enabled` line.
- The game starts on `e1m1`.

While the engine is running, open a second terminal and run:

```powershell
zig build game
```

Expected: `zig-out/bin/game.dll` is rebuilt. **In the first terminal**, no `hotreload: game.dll reloaded` line appears. The running engine continues with the original DLL.

Quit the engine (window close or `quit` in console).

- [ ] **Step 2: Verify flag-on behavior**

In one terminal, run:

```powershell
zig build run -- --hot-reload +map e1m1
```

Expected console output:
- `hotreload: polling enabled` appears at startup (from `HotReload_EnablePolling`).
- The game starts on `e1m1`.

In a second terminal, run:

```powershell
zig build game
```

Expected: within ~1 s of `game.dll` being rebuilt, the engine console shows `hotreload: game.dll reloaded`. The new DLL takes effect.

Quit the engine.

- [ ] **Step 3: Verify `--hot-reload` and `--mcp` are independent**

Run:

```powershell
zig build run -- --hot-reload --mcp +map e1m1
```

Expected: both features start cleanly. `hotreload: polling enabled` appears in the console; `--mcp` initialization runs as before. Engine boots into the map.

Quit the engine.

- [ ] **Step 4: No commit needed**

This task is verification only — there are no file changes to commit. If any of the above steps fail, return to the relevant earlier task and fix before proceeding.

---

## Self-review summary

- **Spec coverage.** Decision 1 (gate polling, not init) → Task 1 Step 3. Decision 2 (file-static flag + setter) → Task 1 Steps 2, 4, 5. Decision 3 (CLI wire-up) → Task 2. Decision 4 (header) → Task 1 Step 1. Decision 5 (CLAUDE.md) → Task 3. Spec verification points 1-4 → Task 4 Steps 1-3 (point 4, ABI mismatch handling, is implicitly preserved because polling code path is unchanged inside the `if (polling_enabled)` block).
- **No placeholders.** All code is shown verbatim; no "TBD" / "similar to" references.
- **Type consistency.** `polling_enabled` (file-static int), `HotReload_EnablePolling(void)` (returns void) — used identically across header, definition, stub, and call site.

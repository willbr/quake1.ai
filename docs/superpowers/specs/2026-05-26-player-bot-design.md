# Player bot design

**Date:** 2026-05-26
**Status:** Draft, decided unilaterally because user went AFK with goal "design and build a player bot for testing while I'm away — get to the end of the levels starting on start.bsp, get keys, kill bad guys, win." No questions allowed.

## Goal

A self-driving player that can be enabled on any Quake map and will try to play through to the level exit. First validated on `start.bsp`, then registered episode maps in order (e1m1 → e1m2 → …).

Success criteria: from a fresh `+map start` launch, with `bot 1` set, the bot reaches a `trigger_changelevel` and the engine loads the next map. Repeat for subsequent maps. Killing every enemy is not required, picking the right keys to unlock doors is.

## Architecture

```
                ┌──────────────────────────────────────┐
                │  engine_src/bot.c          (NEW)     │
 CL_SendCmd ───▶│  Bot_BuildUsercmd(cmd)               │
                │   ├─ Bot_Perceive()  reads sv.edicts │
                │   ├─ Bot_DecideGoal()                │
                │   ├─ Bot_Navigate()  → waypoints     │──┐
                │   └─ Bot_Drive()     → usercmd       │  │
                └──────────────────────────────────────┘  │ engine→DLL call
                                                          ▼
                              game.dll: Sim_Nav_PathTo via game_api_t.nav_path
```

- Bot lives **engine-side** (`sdlquake/engine_src/bot.c` + `bot.h`).
- `CL_SendCmd` calls `Bot_BuildUsercmd(&cmd)` after `CL_BaseMove` / `IN_Move` when `bot.value != 0`. Bot overwrites the usercmd — keyboard input is ignored while bot is active.
- Pathfinding is delegated to the DLL via a new `game_api_t.nav_path(from, to, out, max)` entry. Bump `GAME_API_VERSION` 28 → 29.
- The DLL hook just wraps `Sim_Nav_PathTo`. No new sim code.

Why engine-side and not DLL-side? Because the usercmd is built on the client; the only clean injection point is `CL_SendCmd`. Putting bot in the DLL would mean reaching from a server-tick callback into client state, which is upside-down.

## Components

**`bot.h`** — public API:

```c
void Bot_Init(void);              // register cvars/cmds
void Bot_Frame(usercmd_t *cmd);   // called from CL_SendCmd when active
void Bot_OnHotReload(void);       // re-cache game_api_t pointer
```

**`bot.c`** — state machine + perception:

- `bot_state_t` enum: `BOT_IDLE`, `BOT_COMBAT`, `BOT_GOTO_ITEM`, `BOT_GOTO_KEY`, `BOT_GOTO_EXIT`, `BOT_STUCK`, `BOT_DEAD`.
- Replans goal once per `bot_replan_interval` seconds (default 0.5) or when current target dies/disappears.
- Navigation: cache up to 32 waypoints from `nav_path`. Advance index when within 32 units of waypoint. Replan if blocked.
- Stuck detection: if `|velocity_xy| < 16` for >1s while goal active → jump, try alt path.
- Cvars: `bot` (0/1), `bot_skill` (0–3, picks slipgate at start), `bot_debug` (draws waypoint line via `Con_Print` + `R_DebugLine` if available, else just text).
- Console commands: `bot_status` (print current state + target + waypoint count).

## Perception

Each frame the bot walks `sv.edicts[0..sv.num_edicts]` once and produces:

- `player_e`: `sv_player` (already engine-accessible).
- `enemies[]`: edicts with `FL_MONSTER` and `health > 0` within `bot_aware_radius` (default 1024) AND visible by `SV_LineOfSight` style trace.
- `items[]`: edicts with `FL_ITEM` (health, ammo, armor, weapons, keys).
- `exits[]`: edicts with `classname == "trigger_changelevel"`.

The visibility trace is the existing `PF_traceline`-style trace exposed engine-side as `SV_Move`. Bot uses straight C traces, not the DLL's `eng->trace_line`, because we're already in the engine.

## Goal selection

Single priority cascade, evaluated every replan tick:

1. **Combat** — any enemy in `bot_aware_radius` with line-of-sight and player has ammo for current weapon (or shotgun ≥1 shell) → `BOT_COMBAT`.
2. **Item grab** — useful item (health if hp<75, armor if no armor, ammo if any weapon at <50% max, weapon never picked up, key always) within `bot_pickup_radius` (default 512) → `BOT_GOTO_ITEM` (key prioritized via separate `BOT_GOTO_KEY` if any key edict exists on map).
3. **Exit** — any `trigger_changelevel` exists → `BOT_GOTO_EXIT`. For `start.bsp` we pick the slipgate closest to the easy-skill end based on `bot_skill` cvar; if heuristic fails, just take any exit (start.bsp's exits all go somewhere valid in shareware build).

## Drive layer

Given a target world-space position + current player origin/angles:

1. Compute desired yaw (atan2 of delta xy). Smooth toward it at `bot_turn_speed` deg/s (default 540).
2. `forwardmove = cl_forwardspeed` if angle delta < 60°, else 0 (turn-in-place for sharp angles).
3. `sidemove`: small strafe oscillation in `BOT_COMBAT` to dodge.
4. `cmd.buttons`: `1` (attack) when `BOT_COMBAT` and aim within ~5° of enemy.
5. `cmd.upmove = cl_upspeed` (jump) when stuck, or when an obstacle ≤ 32u high is in front (cheap forward trace).
6. View pitch: aim at target eye height for combat, else 0.

Weapon switching: pick best available weapon by ammo-vs-distance heuristic. Implemented by issuing `impulse N` console commands through `Cbuf_AddText`.

## Map progression

The bot does not orchestrate map sequence — it just tries to reach an exit. Quake's `trigger_changelevel` will load the next map and on the new map's first server tick the bot continues (perception runs again, finds new exit). For `start.bsp`, picking *any* slipgate counts as "winning" — the bot will then play e1m1+, e2m1+, etc.

## Stuck recovery

Per-frame velocity sampling. If the bot has had a goal for >1s with `|v_xy| < 16` and no waypoint progress:

1. Set `cmd.upmove = cl_upspeed` for 0.25s (jump).
2. Pick alternative path: ask `nav_path` again with a small random offset from current pos.
3. After 3 consecutive failures, mark current goal `BOT_STUCK`, force a full replan with the failed target on a 10s cooldown blocklist.

## Death handling

If `sv_player->v.health <= 0`: state = `BOT_DEAD`. Bot issues `Cbuf_AddText("restart\n")` (or `kill; wait; …` — TBD which is most reliable) and re-initializes on next live frame.

## Hot-reload safety

The bot caches `g_game_api->nav_path` as a function pointer for speed. On hot-reload, `HotReload_Frame` already calls `game_api->init()` — bot adds `Bot_OnHotReload` to that callback path to re-cache the pointer.

## Out of scope

- Multi-step puzzle solving (button → door → key behind elevator). The bot relies entirely on the existing navmesh's reachability. If a level requires non-obvious puzzle navigation, it will get stuck. That's acceptable for v1 — measure first, add puzzle hints later if needed.
- Demo recording or replay. The bot drives the live game; the existing demo system can still record it.
- Multiplayer / multiple bots. Single-player listen server only.

## Testing

Manual smoke test: `zig build run -- +map start +set bot 1` and observe. No automated test suite exists in this project (per CLAUDE.md). Verification is by reaching a level transition within ~120s on start.bsp.

# ai_t09_train — func_plat (vertical) + func_train (horizontal, bot-rideable)

**Date:** 2026-05-29
**Status:** Approved design, pre-implementation

## Goal

Add a new AI test map, `ai_t09_train`, that exercises two moving-platform
types in one bot route:

1. **Vertical** — a real `func_plat`. This is the first map to exercise the
   actual `func_plat` nav path (`PLAT_TOP`/`PLAT_BOTTOM`/`PLAT_RIDE`); the
   existing `ai_t07_lift` only fakes lifts with `func_door`.
2. **Horizontal** — a `func_train` ferrying between two `path_corner`s. The
   bot currently has **zero** train-navigation support, so this milestone
   also adds the engine + nav work to let the bot board, ride, and dismount
   a horizontally-moving platform.

## Why trains need their own logic (not the bridge path)

The bot's existing horizontal-mover support is for `func_door` "bridges":
the door extends once and goes **static**, then the bot *walks across* it.
A `func_train` is **never static across the gap** — it shuttles continuously,
so the bot must be *carried*. That makes train-riding mechanically closer to
the vertical lift ("stand still, let `SV_PushMove` carry you") than to the
bridge ("wait for it to open, then walk"). Hence a dedicated `TRAIN_*` nav
class and ride path rather than reusing `BRIDGE_*`.

## Feasibility facts (verified)

- `nav_path` passes waypoint kinds through an `int` array, so adding new
  edge-kind values changes **no struct layout** → **no `GAME_API_VERSION`
  bump**. The only requirement is keeping the engine's `BOT_EDGE_*`
  (`bot.c`) and the game's `NAV_EDGE_*` (`sim_nav.c`) enums in sync and
  rebuilding both.
- `path_corner`s spawn as real edicts with origins (`game/ai.c:spawn_path_corner`).
- `func_train` is `MOVETYPE_PUSH`; `SV_PushMove` carries a rider standing on
  top automatically — the same mechanism lift-riding already depends on.

## Components

### 1. Map — `id1/maps/ai_t09_train.map`

Single room (~`ai_t08_bridge` scale), one continuous bot route that forces
both movers:

```
[start ledge]          [plat pit]            [train gap]           [exit ledge]
info_player_start
  -- walk --> step into pit
  -- ride func_plat UP --> top ledge
  -- wait for train at corner A -- board
  -- ride func_train across the gap to corner B -- dismount
  --> trigger_changelevel "ai_done"
```

- **`func_plat`**: rests at bottom (no `targetname`), bot steps on → rises to
  top. Positioned so the bot must ride it up to progress.
- **`func_train`**: two `path_corner`s (A↔B), `wait 3` at each end (boarding /
  dismount window), auto-running (untargeted, no summon button → continuous
  ferry). The gap below is a pit so riding is mandatory.
- worldspawn `message "AI-TEST t09_train"` (test marker), lights, sky ceiling.

### 2. Nav bake — `sdlquake/game/sim/sim_nav.c` (game.dll)

Mirror the existing `func_plat` bake for `func_train` (classname becomes
`"train"` after spawn):

- Follow the train's `path_corner` chain (handle the 2-corner ferry case).
- New enums, **appended** (no renumber of existing values):
  - `ANCHOR_TRAIN_A`, `ANCHOR_TRAIN_B`
  - `NAV_NODE_TRAIN_END = 10`
  - `NAV_EDGE_TRAIN_RIDE = 8`, `NAV_EDGE_TRAIN_LINK = 9`
- Push a standing anchor on the train's top surface at each parked corner.
- Emit `TRAIN_LINK` edges (ledge ↔ train-top at each end) and a `TRAIN_RIDE`
  edge (top-A ↔ top-B).

### 3. Bot ride logic — `sdlquake/engine_src/bot.c` (engine)

- Add matching `BOT_EDGE_TRAIN_RIDE = 8`, `BOT_EDGE_TRAIN_LINK = 9` and their
  `Bot_EdgeKindName` strings.
- **Board-gate** — new `Bot_TrainParkedAt(pos, radius)`: at the ledge waypoint
  preceding a `TRAIN_LINK`, hold still (no advance, `forwardmove = 0`) until a
  `"train"` edict is parked (≈zero velocity) with its footprint over the board
  corner. Mirrors `Bot_TriggeredBridgeOpen`. Prevents stepping into the gap.
- **Ride-hold** — while the current waypoint kind is `TRAIN_RIDE`, force
  `forwardmove = sidemove = 0` so `SV_PushMove` carries the bot. The existing
  `Bot_AdvanceWaypoint` XY-proximity check then fires only when the train
  parks at B, naturally triggering dismount onto the exit ledge.

### 4. Test-chain wiring

- Retarget `ai_t08_bridge`'s `trigger_changelevel` from `ai_done` →
  `ai_t09_train`; `ai_t09_train` → `ai_done`. Chain becomes
  t08 → t09 → ai_done.
- Add `t09_train` to the `EXPECTED` list in `scripts/run_ai_tests.sh`.

## Verification

1. `zig build mapcompile -- id1 ai_t09_train` → produces `.bsp` + `.lit`.
2. Force nav rebake (remove the per-map `.nav` cache, run with `+nav_rebake`).
3. `+map ai_t09_train +bot 1`: watch the bot ride the plat up → wait for the
   train → board → cross → dismount → hit the exit.
4. `scripts/run_ai_tests.sh` end-to-end (t01…t09 → DONE).
5. MCP screenshot smoke-test.

## Decisions

- Map name `ai_t09_train`.
- Train is an auto-running 2-corner ferry (no summon button).
- Wired into the automated test chain.
- No `GAME_API_VERSION` bump (no struct change; enums stay in sync).

## Rejected alternatives

- **Reuse the `BRIDGE_*` nav/bot path** — wrong model: bridges go static,
  trains don't.
- **Full N-waypoint train pathing** — YAGNI for a test map; the 2-corner
  ferry exercises board/ride/dismount completely.

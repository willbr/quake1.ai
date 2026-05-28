# AI lift test level (`ai_t07_lift`) — Design

**Date:** 2026-05-28
**Status:** Approved pending spec review

## Goal

Add a hand-authored test map, `ai_t07_lift`, that exercises the **player bot**
(`sdlquake/engine_src/bot.c`) against a **button-triggered vertical lift**, in
the style of the first lift at the start of e1m1. The bot must spawn, walk to a
wall button, press it to call the lift, ride the lift up, and reach the exit.

This is the first scenario in the `ai_t0x` suite where the lift is **gated by a
button press** rather than auto-activated by stepping on it (as in
`ai_t01_nav`). The map is the vehicle for the real follow-on work: **finding and
fixing the navmesh bugs** that prevent the bot from chaining
`BUTTON_LINK → PLAT_LINK → PLAT_RIDE` correctly.

## Background

- The bot is strictly navmesh-driven (`bot.c:1-7`). It picks a goal, asks
  `nav_path` for a waypoint list, and walks it. No reactive recovery — if the
  navmesh says "no path," the bot stands still. That makes it an exact probe of
  navmesh correctness.
- The navmesh bake (`sdlquake/game/sim/sim_nav.c`) already special-cases a
  **vertical `func_door` used as a lift** (sim_nav.c:604, comment literally says
  "e.g. e1m1 first lift"). It emits `ANCHOR_PLAT_TOP` / `ANCHOR_PLAT_BOTTOM`
  anchors and the edge kinds `PLAT_LINK` (walk on/off the lift) and `PLAT_RIDE`
  (ride top↔bottom). It also emits `BUTTON_LINK` edges for `func_button`
  (`NAV_NODE_DOOR_BUTTON`).
- The bot's drive layer already understands these edges: it waits on the lift
  when a waypoint is directly above/below it (bot.c:526-540), and it holds at a
  button anchor until the button's state leaves BOTTOM (bot.c:367-369,
  `Bot_NearbyButtonReady`).
- `func_button` is implemented (`sdlquake/game/buttons.c`): on press it runs
  `button_fire` → `SUB_UseTargets`, which fires any entity whose `targetname`
  matches the button's `target`. A `func_door` lift with a matching `targetname`
  will be triggered this way.

So all the *primitives* exist. What is unverified — and the point of this map —
is whether the bake actually **sequences** "press the button, *then* ride the
lift" into a single path. The existing `ai_t01_nav` lift is touch-activated, so
that button→lift dependency has never been exercised by the suite.

## Map design — `id1/maps/ai_t07_lift.map`

Same authoring conventions as the rest of the suite (see
`docs/superpowers/plans/2026-05-27-ai-test-levels.md`): hand-written `.map`,
axis-aligned 16-unit-thick brushes, textures `wbrick1_5` (walls), `sfloor4_2`
(floor), `tlight02` (lift pad), `+0basebtn` (button), `metal1_1` (trim),
`trigger` (trigger volumes), `sky1` (ceiling band).

### Layout (side view, looking along +Y; +X right, +Z up)

```
                                              ┌──────────────┐  ← upper ledge (z=144)
                                              │  exit pad     │     trigger_changelevel
                                              │  (→ ai_done)  │
   ┌──────────────────────────┐──────────────┘              │
   │                          │   lift shaft  │ ▒▒▒▒▒▒▒▒▒▒▒  │  ← lift at TOP (z=144)
   │  spawn                   │   ░░░░░░░░░    │              │
   │  (bot)        [BUTTON]   │   ░ lift  ░    │              │
   │   ●            on wall   │   ▒▒▒▒▒▒▒▒▒    │ ← lift at BOTTOM (z=16)
   └──────────────────────────┴───────────────┘
  z=16  lower room floor                shaft     ledge wall
```

Top-down, the whole thing sits inside one sealed `768×768×~256` room (the shared
skeleton), partitioned into:

- **Lower room** (west half): `info_player_start` on the floor (`z=32`). Open
  floor so the bot can reach both the button and the lift.
- **Wall button** (`func_button`): mounted on a partition/pillar face that the
  bot walks up to. `target "lift1"`, touch-activated (`health 0`),
  `wait "-1"` (stays fired — we don't want it cycling back and dropping the lift
  mid-ride). Texture `+0basebtn`.
- **Lift shaft + lift** (`func_door`, vertical): a pad (`tlight02`) that starts
  at the bottom (`z=16..32`) and travels up `~128` units to align with the upper
  ledge. `targetname "lift1"`, `angle "-1"` (Quake "up" / move toward `angle`),
  `lip`/`speed`/`wait` tuned so it rises on trigger and **holds at top** long
  enough to ride (`wait "-1"` or a large wait). Footprint wide enough for the
  bake's lift heuristic (sim_nav.c uses footprint width to tell a lift from a
  regular door).
- **Upper ledge** (east, `z=144`): reachable only by riding the lift. Holds the
  `trigger_changelevel` exit pad, `map "ai_done"`.

Lighting: a handful of `light` entities (lower room, button face, shaft, ledge)
so both the bot's `R_LightPoint`-based systems and a human observer can see.

### Entity summary

| classname | key fields | role |
|---|---|---|
| `worldspawn` | `message "AI-TEST t07_lift"`, `wad "gfx/base.wad"` | level marker for the suite |
| `info_player_start` | lower-room floor, facing the button | bot spawn |
| `func_button` | `target "lift1"`, `health 0`, `wait -1` | calls the lift |
| `func_door` | `targetname "lift1"`, `angle -1`, vertical travel ~128u | the lift |
| `trigger_changelevel` | `map "ai_done"`, on the upper ledge | exit |
| `light` ×N | scattered | visibility |

No new engine or DLL entity types — every classname above is already registered
(`spawn.c`).

## Chain integration

The suite currently runs `t01 → t02 → t03 → t04 → t05 → t06_wander → ai_done`.
Insert `t07_lift` immediately before the terminator:

1. `id1/maps/ai_t06_wander.map`: change its `trigger_changelevel.map` from
   `ai_done` → `ai_t07_lift`, recompile.
2. `id1/maps/ai_t07_lift.map`: its `trigger_changelevel.map` = `ai_done`.
3. `scripts/run_ai_tests.sh`: add `t07_lift` to the `EXPECTED` tag list
   (between `t06_wander` and `DONE`).

## Build / compile workflow

Use the CLI compiler (cleaner than the GUI `editor_compile_export` route the
earlier plan used):

```sh
zig build mapcompile -- id1 ai_t07_lift
```

This drives the vendored qbsp + vis + light (build.zig:722) and writes
`id1/maps/ai_t07_lift.bsp` + `.lit`. Recompile `ai_t06_wander` the same way
after rewiring its exit.

Smoke-test load (no bot) to verify geometry/triggers manually:

```sh
zig build run -- +map ai_t07_lift
# console: noclip — confirm button presses, lift rises, exit fires
```

## Test methodology (drives the navmesh-fix phase)

1. **Bot run:**
   ```sh
   zig build run -- +map ai_t07_lift +set bot 1
   ```
   Expected end state: bot presses button → rides lift → `<player> exited the
   level` → `AI-TEST DONE`.

2. **When it fails** (the likely outcome — that's why we're here), diagnose with
   the bot's existing tooling:
   - `bot_debug 1` — logs the chosen goal and the full waypoint list with edge
     kinds each replan.
   - `bot_path_dump` — dumps the current path.
   - `bot_debug_viz 1` — draws the waypoint chain colour-coded by edge kind, the
     target marker, and the aim/wall probes.
   - `sim_nav_debug 1` — the navmesh overlay (nodes, anchors, edges) from
     sim_nav.c.

3. **Hypothesised navmesh gaps to investigate** (not prescribed fixes — confirm
   against the debug output first):
   - The bake may not link the **button anchor → lift** dependency, so the path
     skips the button and dead-ends at a lift that never moves.
   - `PLAT_LINK` onto the lift may only be emitted for the BOTTOM anchor when
     the lift is *at* bottom at bake time; a button-gated lift's resting state
     and the "press first" ordering may need explicit handling.
   - The `BUTTON_LINK` "don't advance until pressed" gate
     (`Bot_NearbyButtonReady`, bot.c:367) assumes the button is *on the path to*
     the lift; verify the bake places the button anchor as a required
     predecessor of the lift anchors, not a side spur.

   Each confirmed gap becomes a focused fix in `sim_nav.c` (and, if the drive
   layer needs it, `bot.c`), validated by re-running step 1 until the bot
   completes the map.

## Scope / non-goals

- **In scope:** the `ai_t07_lift` map, chain rewiring, run-script update, and the
  navmesh/bot fixes needed to make the bot complete the button→lift→exit chain.
- **Out of scope:** monsters/combat in this map (it's a pure navigation probe);
  reworking the touch-activated `ai_t01_nav` lift; any non-lift navmesh work.

## Deliverables

- `id1/maps/ai_t07_lift.map` (+ generated `.bsp`, `.lit`).
- Rewired `id1/maps/ai_t06_wander.map` (+ recompiled `.bsp`).
- Updated `scripts/run_ai_tests.sh`.
- Navmesh/bot fixes in `sim_nav.c` (and possibly `bot.c`) as the test surfaces
  them.

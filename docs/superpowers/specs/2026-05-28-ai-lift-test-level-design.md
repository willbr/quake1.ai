# AI lift test level (`ai_t07_lift`) — Design

**Date:** 2026-05-28
**Status:** Approved pending spec review

## Goal

Add a hand-authored test map, `ai_t07_lift`, that exercises the **player bot**
(`sdlquake/engine_src/bot.c`) against **two button-triggered vertical lifts**, in
the style of the lifts at the start of e1m1. The bot spawns on a high ledge,
**rides a first lift down** to a lower floor, crosses to a **second lift and
rides it up** to the exit ledge — each lift gated by its own wall button.

This is the first scenario in the `ai_t0x` suite where a lift is **gated by a
button press** rather than auto-activated by stepping on it (as in
`ai_t01_nav`), and the first with a **descending** ride and **two** lifts in one
map. The map is the vehicle for the real follow-on work: **finding and fixing the
navmesh bugs** that prevent the bot from chaining
`PLAT_LINK → BUTTON_LINK → PLAT_RIDE` correctly in both directions.

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
is whether the bake can (a) sequence "board the lift, *then* press the button,
*then* ride" into a single path, and (b) do it for a **descending** lift and for
**two** lifts in the same map without the anchors cross-linking. The existing
`ai_t01_nav` lift is touch-activated and rises only, so none of this has been
exercised by the suite.

## Lift mechanism — flush-rest, ride-while-pressing

To keep the bot from pressing a button and then watching the lift leave without
it, each lift **rests flush with the floor the bot is standing on**, and its
button is on the adjacent wall at the lift's edge:

1. Bot walks from its current floor straight onto the lift pad (no height
   change — `PLAT_LINK`).
2. Standing on the pad's edge, the bot touches the wall button (`BUTTON_LINK`).
   Because the bot is already aboard, it rides when the lift moves.
3. The lift travels to the far level carrying the bot (`PLAT_RIDE`); the bot
   steps off onto the destination floor (`PLAT_LINK`).
4. The `func_door` returns to its rest position after `wait` seconds — the bot
   has already stepped off, so the return trip is harmless.

This is the canonical e1m1 "ride the lift" feel, but with the button placed so
the *board → press → ride* order is unambiguous. Getting the bake to honour that
order is the heart of the navmesh work.

## Map design — `id1/maps/ai_t07_lift.map`

Same authoring conventions as the rest of the suite (see
`docs/superpowers/plans/2026-05-27-ai-test-levels.md`): hand-written `.map`,
axis-aligned 16-unit-thick brushes, textures `wbrick1_5` (walls), `sfloor4_2`
(floor), `tlight02` (lift pad), `+0basebtn` (button), `metal1_1` (trim),
`trigger` (trigger volumes), `sky1` (ceiling band). One sealed `768×768×256`
room (the shared skeleton); the two ledges are raised platforms inside it and the
skeleton floor (`z=16`) is the lower level.

### Layout (side view, looking along +Y; +X right, +Z up)

```
   ┌─────────────┐                                   ┌─────────────┐  ← exit ledge (top z=144)
   │ start ledge │ [B1]                         [B2] │  exit pad    │     trigger_changelevel
   │ (spawn ●)   ││▒▒▒▒│  lift1 rests here        │▒▒▒▒│ (→ ai_done) │     → ai_done
   │  top z=144  ││lift1│  (flush w/ start ledge)  │lift2│           │
   └─────────────┘│ ░░ │                          │ ░░ │ ← lift2 rests here (flush w/ low floor)
                  │ ░░ │ ↓ rides DOWN              │ ░░ │ ↑ rides UP
   ───────────────┴────┴──────────────────────────┴────┴───────────────  ← low floor (z=16)
        bot rides lift1 down, walks across low floor, boards lift2, rides up
```

- **Start ledge** (west, top surface `z=144`): a raised platform holding
  `info_player_start`. The bot spawns here, facing lift 1.
- **Lift 1** (`func_door`, vertical): pad **rests flush with the start ledge**
  (`top z=144`), travels ~128u **down** to the low floor (`top z=32`).
  `targetname "lift1"`, `wait` a few seconds (returns up after the ride).
  Button **B1** (`func_button`, `target "lift1"`, `health 0`) on the shaft wall
  at the start-ledge level, at the lift's edge.
- **Low floor** (`z=16`, the skeleton floor): the bot walks from the foot of
  lift 1 across to lift 2.
- **Lift 2** (`func_door`, vertical): pad **rests flush with the low floor**
  (`top z=32`), travels ~128u **up** to the exit ledge (`top z=144`).
  `targetname "lift2"`, `angle "-1"`. Button **B2** (`func_button`,
  `target "lift2"`, `health 0`) on the shaft wall at the low-floor level, at the
  lift's edge.
- **Exit ledge** (east, top surface `z=144`): reachable only by riding lift 2.
  Holds the `trigger_changelevel` exit pad, `map "ai_done"`.

Both lifts need a footprint wide enough for the bake's lift heuristic (sim_nav.c
uses footprint width to tell a lift from a regular door) and distinct
`targetname`s so the two button→lift pairs don't cross-wire.

Lighting: `light` entities over each ledge, both shafts, and the low floor so
the bot's `R_LightPoint`-based systems and a human observer can see.

### Entity summary

| classname | key fields | role |
|---|---|---|
| `worldspawn` | `message "AI-TEST t07_lift"`, `wad "gfx/base.wad"` | level marker for the suite |
| `info_player_start` | on the start ledge (`z≈148`), facing lift 1 | bot spawn |
| `func_button` (B1) | `target "lift1"`, `health 0` | calls lift 1 down |
| `func_door` (lift 1) | `targetname "lift1"`, rests at top, ~128u down travel | descending lift |
| `func_button` (B2) | `target "lift2"`, `health 0` | calls lift 2 up |
| `func_door` (lift 2) | `targetname "lift2"`, `angle -1`, rests at bottom, ~128u up travel | ascending lift |
| `trigger_changelevel` | `map "ai_done"`, on the exit ledge | exit |
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
# console: noclip — board lift 1, press B1, confirm it descends with you;
#          cross to lift 2, press B2, confirm it ascends; exit fires.
```

## Test methodology (drives the navmesh-fix phase)

1. **Bot run:**
   ```sh
   zig build run -- +map ai_t07_lift +set bot 1
   ```
   Expected end state: bot boards lift 1 → presses B1 → rides down → crosses →
   boards lift 2 → presses B2 → rides up → `<player> exited the level` →
   `AI-TEST DONE`.

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
   - **Button anchor placement.** For ride-while-pressing to work, each button's
     anchor must sit **on the lift's top surface**, not on the adjacent static
     floor. If the bake anchors the button to static floor, the bot presses and
     the lift leaves without it.
   - **Descending ride.** `PLAT_RIDE` / `PLAT_LINK` may have only ever been
     validated for a lift that *rises*. Verify the bake links
     `start-ledge → board lift1 (top) → ride DOWN → low floor` — the top→bottom
     direction and a lift whose rest state is TOP.
   - **Press-after-board ordering.** The `BUTTON_LINK` "don't advance until
     pressed" gate (`Bot_NearbyButtonReady`, bot.c:367) assumes the button is on
     the path; confirm the bake sequences *board → press → ride*, not *press →
     board* (which would strand the bot).
   - **Two lifts, one map.** Confirm lift1/lift2 anchors and button links don't
     cross-wire (correct `targetname` resolution, no spurious edges between the
     two shafts).

   Each confirmed gap becomes a focused fix in `sim_nav.c` (and, if the drive
   layer needs it, `bot.c`), validated by re-running step 1 until the bot
   completes the map.

## Scope / non-goals

- **In scope:** the `ai_t07_lift` map, chain rewiring, run-script update, and the
  navmesh/bot fixes needed to make the bot complete the
  down-lift → cross → up-lift → exit chain.
- **Out of scope:** monsters/combat in this map (it's a pure navigation probe);
  reworking the touch-activated `ai_t01_nav` lift; any non-lift navmesh work.

## Deliverables

- `id1/maps/ai_t07_lift.map` (+ generated `.bsp`, `.lit`).
- Rewired `id1/maps/ai_t06_wander.map` (+ recompiled `.bsp`).
- Updated `scripts/run_ai_tests.sh`.
- Navmesh/bot fixes in `sim_nav.c` (and possibly `bot.c`) as the test surfaces
  them.

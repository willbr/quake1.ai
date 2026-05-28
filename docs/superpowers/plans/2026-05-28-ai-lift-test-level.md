# AI lift test level (`ai_t07_lift`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `ai_t07_lift` — a hand-authored test map with two button-triggered vertical lifts (bot spawns high, rides lift 1 down, crosses, rides lift 2 up to the exit) — wire it into the `ai_t0x` suite, then find and fix the navmesh bugs that stop the player bot from completing the button→lift chains.

**Architecture:** Two phases. **Phase A (Tasks 1-2):** pure map content — a `.map` file compiled to `.bsp`+`.lit` via the in-process `mapcompile` CLI, plus chain rewiring. No engine/DLL code. **Phase B (Tasks 3-6):** navmesh/bot fixes in `sim_nav.c` (and possibly `bot.c`), driven by the bot's own debug tooling, until the bot completes the map hands-off.

**Tech Stack:** Quake `.map` text format; vendored qbsp+vis+light via `zig build mapcompile`; C (`sdlquake/game/sim/sim_nav.c`, `sdlquake/engine_src/bot.c`); bash run script.

**Spec:** `docs/superpowers/specs/2026-05-28-ai-lift-test-level-design.md`

---

## Background the engineer needs

- **The bot** (`sdlquake/engine_src/bot.c`) is a self-driving player: each frame it picks a goal, calls `g_game_api->nav_path` for a waypoint list, and walks it. No reactive recovery — if `nav_path` returns 0, the bot stands still. So the bot completing the map is a direct proof the navmesh is correct.
- **Enable the bot** with the cvar `bot 1` (e.g. `+set bot 1` on the command line). Debug cvars/commands: `bot_debug 1` (logs goal + waypoint list each replan), `bot_path_dump` (prints current path), `bot_debug_viz 1` (draws the waypoint chain colour-coded by edge kind), `sim_nav_debug 1` (draws the baked navmesh: nodes, anchors, edges).
- **The navmesh bake** is `sdlquake/game/sim/sim_nav.c` (inside the hot-reloadable `game.dll`). It special-cases a vertical `func_door` as a lift (sim_nav.c:618-651), emitting `ANCHOR_PLAT_TOP`/`ANCHOR_PLAT_BOTTOM` anchors, and wires `PLAT_LINK`/`PLAT_RIDE`/`BUTTON_LINK` edges in its "Phase 4.5" logic.
- **`func_door` lift math** (sim_nav.c expectations + `sdlquake/game/doors.c:351-358`): a door travels `|movedir·size| − lip` along `movedir`. `angle "-1"` ⇒ movedir up `(0,0,1)`; `angle "-2"` ⇒ movedir down `(0,0,-1)`. So a lift's **brush height in z sets its travel** — a 128-unit lift needs a 128-tall brush. The brush IS the shaft.
- **Lift detection thresholds** (sim_nav.c:628-630): the door is treated as a lift only if z-travel `> 24` and is the dominant axis (`vertical`), AND footprint `sx > 48 && sy > 48` (`standable`). Our lifts are 96×96 footprint, 128 travel — comfortably inside both.
- **`func_button`** (`sdlquake/game/buttons.c`): a touch button (default, `health 0`) fires `SUB_UseTargets` on contact, triggering any entity whose `targetname` equals the button's `target`. The bot only presses touch buttons in BOTTOM/READY state (`Bot_IsButton`, bot.c:147-154) — do **not** set `health` on these buttons.
- **Map authoring conventions** (from `docs/superpowers/plans/2026-05-27-ai-test-levels.md`): hand-written `.map`, axis-aligned boxes, 6 planes per brush. Textures: `wbrick1_5` walls, `sfloor4_2` floor, `tlight02` lift pad, `+0basebtn` button, `metal1_1` trim, `trigger` trigger volumes, `sky1` ceiling band.

**Axis-aligned cube template** — for a box spanning `[x0..x1, y0..y1, z0..z1]` with texture `T`:

```
{
( x1 y1 z1 ) ( x1 y1 z0 ) ( x1 y0 z0 ) T 0 0 0 1 1
( x0 y0 z1 ) ( x0 y0 z0 ) ( x0 y1 z0 ) T 0 0 0 1 1
( x0 y1 z1 ) ( x0 y1 z0 ) ( x1 y1 z0 ) T 0 0 0 1 1
( x1 y0 z1 ) ( x1 y0 z0 ) ( x0 y0 z0 ) T 0 0 0 1 1
( x1 y1 z1 ) ( x1 y0 z1 ) ( x0 y0 z1 ) T 0 0 0 1 1
( x0 y1 z0 ) ( x0 y0 z0 ) ( x1 y0 z0 ) T 0 0 0 1 1
}
```

### Height / layout reference (all world coords; +X east, +Y north, +Z up)

| Feature | Footprint (x, y) | z (brush) | z (stand) | notes |
|---|---|---|---|---|
| Low floor (skeleton) | whole room | 0..16 | 16 | the lower level |
| Start ledge | -352..-200, -96..96 | 16..144 | 144 | spawn here |
| Lift 1 (descends) | -200..-104, -48..48 | 16..144 | 144→16 | rests TOP, `angle -2` |
| B1 button | -168..-136, 44..60 | 148..180 | — | touch from lift 1 top |
| Exit ledge | 200..352, -96..96 | 16..144 | 144 | exit here |
| Lift 2 (ascends) | 104..200, -48..48 | -112..16 | 16→144 | rests BOTTOM, `angle -1` |
| B2 button | 136..168, 48..64 | 20..52 | — | touch from lift 2 (low); flush outside footprint |

Travel for each lift = brush z-height − lip = 128 − 0 = **128**, taking each stand surface between z=144 and z=16. Lifts adjoin their ledges (lift1 east face x=-104 → low floor; lift1 west face x=-200 = start-ledge east face; lift2 east face x=200 = exit-ledge west face), so the bot steps on/off without a gap.

---

## Task 1: Author and verify `ai_t07_lift.map`

**Files:**
- Create: `id1/maps/ai_t07_lift.map`
- Generated: `id1/maps/ai_t07_lift.bsp`, `id1/maps/ai_t07_lift.lit`

- [ ] **Step 1: Write the map file**

Create `id1/maps/ai_t07_lift.map` with exactly this content:

```
{
"classname" "worldspawn"
"wad" "gfx/base.wad"
"message" "AI-TEST t07_lift"
{
( 384 384 16 ) ( 384 384 0 ) ( 384 -384 0 ) sfloor4_2 0 0 0 1 1
( -384 -384 16 ) ( -384 -384 0 ) ( -384 384 0 ) sfloor4_2 0 0 0 1 1
( -384 384 16 ) ( -384 384 0 ) ( 384 384 0 ) sfloor4_2 0 0 0 1 1
( 384 -384 16 ) ( 384 -384 0 ) ( -384 -384 0 ) sfloor4_2 0 0 0 1 1
( 384 384 16 ) ( 384 -384 16 ) ( -384 -384 16 ) sfloor4_2 0 0 0 1 1
( -384 384 0 ) ( -384 -384 0 ) ( 384 -384 0 ) sfloor4_2 0 0 0 1 1
}
{
( 384 384 256 ) ( 384 384 240 ) ( 384 -384 240 ) sky1 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 240 ) ( -384 384 240 ) sky1 0 0 0 1 1
( -384 384 256 ) ( -384 384 240 ) ( 384 384 240 ) sky1 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 240 ) ( -384 -384 240 ) sky1 0 0 0 1 1
( 384 384 256 ) ( 384 -384 256 ) ( -384 -384 256 ) sky1 0 0 0 1 1
( -384 384 240 ) ( -384 -384 240 ) ( 384 -384 240 ) sky1 0 0 0 1 1
}
{
( 384 384 256 ) ( 384 384 16 ) ( 384 368 16 ) wbrick1_5 0 0 0 1 1
( -384 368 256 ) ( -384 368 16 ) ( -384 384 16 ) wbrick1_5 0 0 0 1 1
( -384 384 256 ) ( -384 384 16 ) ( 384 384 16 ) wbrick1_5 0 0 0 1 1
( 384 368 256 ) ( 384 368 16 ) ( -384 368 16 ) wbrick1_5 0 0 0 1 1
( 384 384 256 ) ( 384 368 256 ) ( -384 368 256 ) wbrick1_5 0 0 0 1 1
( -384 384 16 ) ( -384 368 16 ) ( 384 368 16 ) wbrick1_5 0 0 0 1 1
}
{
( 384 -368 256 ) ( 384 -368 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 16 ) ( -384 -368 16 ) wbrick1_5 0 0 0 1 1
( -384 -368 256 ) ( -384 -368 16 ) ( 384 -368 16 ) wbrick1_5 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 16 ) ( -384 -384 16 ) wbrick1_5 0 0 0 1 1
( 384 -368 256 ) ( 384 -384 256 ) ( -384 -384 256 ) wbrick1_5 0 0 0 1 1
( -384 -368 16 ) ( -384 -384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
}
{
( 384 384 256 ) ( 384 384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
( 368 -384 256 ) ( 368 -384 16 ) ( 368 384 16 ) wbrick1_5 0 0 0 1 1
( 368 384 256 ) ( 368 384 16 ) ( 384 384 16 ) wbrick1_5 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 16 ) ( 368 -384 16 ) wbrick1_5 0 0 0 1 1
( 384 384 256 ) ( 384 -384 256 ) ( 368 -384 256 ) wbrick1_5 0 0 0 1 1
( 368 384 16 ) ( 368 -384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
}
{
( -368 384 256 ) ( -368 384 16 ) ( -368 -384 16 ) wbrick1_5 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 16 ) ( -384 384 16 ) wbrick1_5 0 0 0 1 1
( -384 384 256 ) ( -384 384 16 ) ( -368 384 16 ) wbrick1_5 0 0 0 1 1
( -368 -384 256 ) ( -368 -384 16 ) ( -384 -384 16 ) wbrick1_5 0 0 0 1 1
( -368 384 256 ) ( -368 -384 256 ) ( -384 -384 256 ) wbrick1_5 0 0 0 1 1
( -384 384 16 ) ( -384 -384 16 ) ( -368 -384 16 ) wbrick1_5 0 0 0 1 1
}
{
( -200 96 144 ) ( -200 96 16 ) ( -200 -96 16 ) sfloor4_2 0 0 0 1 1
( -352 -96 144 ) ( -352 -96 16 ) ( -352 96 16 ) sfloor4_2 0 0 0 1 1
( -352 96 144 ) ( -352 96 16 ) ( -200 96 16 ) sfloor4_2 0 0 0 1 1
( -200 -96 144 ) ( -200 -96 16 ) ( -352 -96 16 ) sfloor4_2 0 0 0 1 1
( -200 96 144 ) ( -200 -96 144 ) ( -352 -96 144 ) sfloor4_2 0 0 0 1 1
( -352 96 16 ) ( -352 -96 16 ) ( -200 -96 16 ) sfloor4_2 0 0 0 1 1
}
{
( 352 96 144 ) ( 352 96 16 ) ( 352 -96 16 ) sfloor4_2 0 0 0 1 1
( 200 -96 144 ) ( 200 -96 16 ) ( 200 96 16 ) sfloor4_2 0 0 0 1 1
( 200 96 144 ) ( 200 96 16 ) ( 352 96 16 ) sfloor4_2 0 0 0 1 1
( 352 -96 144 ) ( 352 -96 16 ) ( 200 -96 16 ) sfloor4_2 0 0 0 1 1
( 352 96 144 ) ( 352 -96 144 ) ( 200 -96 144 ) sfloor4_2 0 0 0 1 1
( 200 96 16 ) ( 200 -96 16 ) ( 352 -96 16 ) sfloor4_2 0 0 0 1 1
}
{
( -136 64 148 ) ( -136 64 16 ) ( -136 48 16 ) wbrick1_5 0 0 0 1 1
( -168 48 148 ) ( -168 48 16 ) ( -168 64 16 ) wbrick1_5 0 0 0 1 1
( -168 64 148 ) ( -168 64 16 ) ( -136 64 16 ) wbrick1_5 0 0 0 1 1
( -136 48 148 ) ( -136 48 16 ) ( -168 48 16 ) wbrick1_5 0 0 0 1 1
( -136 64 148 ) ( -136 48 148 ) ( -168 48 148 ) wbrick1_5 0 0 0 1 1
( -168 64 16 ) ( -168 48 16 ) ( -136 48 16 ) wbrick1_5 0 0 0 1 1
}
{
( 168 64 20 ) ( 168 64 16 ) ( 168 48 16 ) wbrick1_5 0 0 0 1 1
( 136 48 20 ) ( 136 48 16 ) ( 136 64 16 ) wbrick1_5 0 0 0 1 1
( 136 64 20 ) ( 136 64 16 ) ( 168 64 16 ) wbrick1_5 0 0 0 1 1
( 168 48 20 ) ( 168 48 16 ) ( 136 48 16 ) wbrick1_5 0 0 0 1 1
( 168 64 20 ) ( 168 48 20 ) ( 136 48 20 ) wbrick1_5 0 0 0 1 1
( 136 64 16 ) ( 136 48 16 ) ( 168 48 16 ) wbrick1_5 0 0 0 1 1
}
}
{
"classname" "info_player_start"
"origin" "-280 0 160"
"angle" "0"
}
{
"classname" "func_door"
"targetname" "lift1"
"angle" "-2"
"lip" "0"
"speed" "100"
"wait" "3"
{
( -104 48 144 ) ( -104 48 16 ) ( -104 -48 16 ) tlight02 0 0 0 1 1
( -200 -48 144 ) ( -200 -48 16 ) ( -200 48 16 ) tlight02 0 0 0 1 1
( -200 48 144 ) ( -200 48 16 ) ( -104 48 16 ) tlight02 0 0 0 1 1
( -104 -48 144 ) ( -104 -48 16 ) ( -200 -48 16 ) tlight02 0 0 0 1 1
( -104 48 144 ) ( -104 -48 144 ) ( -200 -48 144 ) tlight02 0 0 0 1 1
( -200 48 16 ) ( -200 -48 16 ) ( -104 -48 16 ) tlight02 0 0 0 1 1
}
}
{
"classname" "func_door"
"targetname" "lift2"
"angle" "-1"
"lip" "0"
"speed" "100"
"wait" "3"
{
( 200 48 16 ) ( 200 48 -112 ) ( 200 -48 -112 ) tlight02 0 0 0 1 1
( 104 -48 16 ) ( 104 -48 -112 ) ( 104 48 -112 ) tlight02 0 0 0 1 1
( 104 48 16 ) ( 104 48 -112 ) ( 200 48 -112 ) tlight02 0 0 0 1 1
( 200 -48 16 ) ( 200 -48 -112 ) ( 104 -48 -112 ) tlight02 0 0 0 1 1
( 200 48 16 ) ( 200 -48 16 ) ( 104 -48 16 ) tlight02 0 0 0 1 1
( 104 48 -112 ) ( 104 -48 -112 ) ( 200 -48 -112 ) tlight02 0 0 0 1 1
}
}
{
"classname" "func_button"
"target" "lift1"
"angle" "90"
{
( -136 60 180 ) ( -136 60 148 ) ( -136 44 148 ) +0basebtn 0 0 0 1 1
( -168 44 180 ) ( -168 44 148 ) ( -168 60 148 ) +0basebtn 0 0 0 1 1
( -168 60 180 ) ( -168 60 148 ) ( -136 60 148 ) +0basebtn 0 0 0 1 1
( -136 44 180 ) ( -136 44 148 ) ( -168 44 148 ) +0basebtn 0 0 0 1 1
( -136 60 180 ) ( -136 44 180 ) ( -168 44 180 ) +0basebtn 0 0 0 1 1
( -168 60 148 ) ( -168 44 148 ) ( -136 44 148 ) +0basebtn 0 0 0 1 1
}
}
{
"classname" "func_button"
"target" "lift2"
"angle" "90"
{
( 168 64 52 ) ( 168 64 20 ) ( 168 48 20 ) +0basebtn 0 0 0 1 1
( 136 48 52 ) ( 136 48 20 ) ( 136 64 20 ) +0basebtn 0 0 0 1 1
( 136 64 52 ) ( 136 64 20 ) ( 168 64 20 ) +0basebtn 0 0 0 1 1
( 168 48 52 ) ( 168 48 20 ) ( 136 48 20 ) +0basebtn 0 0 0 1 1
( 168 64 52 ) ( 168 48 52 ) ( 136 48 52 ) +0basebtn 0 0 0 1 1
( 136 64 20 ) ( 136 48 20 ) ( 168 48 20 ) +0basebtn 0 0 0 1 1
}
}
{
"classname" "trigger_changelevel"
"map" "ai_done"
{
( 312 32 176 ) ( 312 32 144 ) ( 312 -32 144 ) trigger 0 0 0 1 1
( 248 -32 176 ) ( 248 -32 144 ) ( 248 32 144 ) trigger 0 0 0 1 1
( 248 32 176 ) ( 248 32 144 ) ( 312 32 144 ) trigger 0 0 0 1 1
( 312 -32 176 ) ( 312 -32 144 ) ( 248 -32 144 ) trigger 0 0 0 1 1
( 312 32 176 ) ( 312 -32 176 ) ( 248 -32 176 ) trigger 0 0 0 1 1
( 248 32 144 ) ( 248 -32 144 ) ( 312 -32 144 ) trigger 0 0 0 1 1
}
}
{
"classname" "light"
"origin" "-280 0 210"
"light" "320"
}
{
"classname" "light"
"origin" "-150 0 200"
"light" "300"
}
{
"classname" "light"
"origin" "0 0 200"
"light" "300"
}
{
"classname" "light"
"origin" "150 0 200"
"light" "300"
}
{
"classname" "light"
"origin" "280 0 210"
"light" "320"
}
```

- [ ] **Step 2: Compile the map**

Run: `zig build mapcompile -- id1 ai_t07_lift`
Expected: completes without error and prints a BSP byte count; `id1/maps/ai_t07_lift.bsp` and `.lit` now exist. Verify:

Run: `ls -la id1/maps/ai_t07_lift.bsp id1/maps/ai_t07_lift.lit`
Expected: both files present, non-zero size.

If compile reports a **leak**: the most likely cause is the lift 2 brush resting below the floor (z down to -112) poking into the void. Fix by digging an enclosed pit under lift 2 — replace the single low-floor brush with floor brushes around a `104..200, -48..48` hole, add four pit walls (`wbrick1_5`) from z=-128..16 around that footprint, and a pit floor cube `104..200, -48..48, -144..-128`. Recompile. (Try the as-written version first; entity brush models usually compile fine extending past the world hull.)

- [ ] **Step 3: Smoke-test geometry with noclip + manual play**

Run: `zig build run -- +map ai_t07_lift`
In the console (`~`), confirm `AI-TEST t07_lift` printed on load. Then verify, walking normally (not noclip) from the spawn:

1. You spawn on the start ledge (high, west). **Pass:** you are standing at z≈144, facing east toward lift 1.
2. Walk east onto lift 1 (its top is flush with the ledge). Walk to its **north edge** and into button **B1**. **Pass:** B1 depresses and lift 1 descends ~128 units carrying you to the low floor.
3. Walk east across the low floor to lift 2 (its top is flush with the low floor). Step onto it, walk to its north edge into button **B2**. **Pass:** B2 depresses and lift 2 rises ~128 units carrying you up.
4. Step east off lift 2 onto the exit ledge. **Pass:** crossing the ledge prints `<player> exited the level` and the engine attempts to load `ai_done` (which doesn't chain yet — an error or load is fine here).

Close the engine.

- [ ] **Step 4: Tune if any check failed**

- **Can't reach a button from the lift** (most likely issue): the player bbox is ±16 around its origin and must overlap the button brush from a position that stays on the lift. The two buttons have different constraints:
  - **B1** sits at z=148..180, *above* lift 1's max top (z=144), so it can safely cantilever over the lift footprint. If unreachable, widen it southward, e.g. `y` span `44 60` → `40 60` (more overhang). Keep z ≥ 148.
  - **B2** must stay *outside* the lift 2 footprint (`y ≥ 48`) — lift 2 sweeps its whole footprint up through z=20..52, so any button overlapping the footprint there would jam the rising lift. Do **not** cantilever B2. If unreachable, instead make lift 2 a touch easier to stand near the edge (e.g. nudge B2 east/west to centre on the lift, or widen its `x` span), keeping `y ≥ 48`.
  Recompile (Step 2) and re-verify (Step 3).
- **Lift travels the wrong distance:** travel = brush z-height − `lip`. Both lifts are 128 tall with `lip 0` ⇒ 128. If you change a lift's height, keep top/bottom flush with its ledge/floor.
- **Spawn is stuck/embedded:** raise `info_player_start` z (e.g. `160` → `172`).
- **Lift returns before you step off:** increase `wait` (3 → 6) on that `func_door`.

- [ ] **Step 5: Commit**

```bash
git add id1/maps/ai_t07_lift.map id1/maps/ai_t07_lift.bsp id1/maps/ai_t07_lift.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t07_lift map — two button-triggered lifts

Bot spawns high, rides lift 1 down, crosses, rides lift 2 up to the
exit. Each lift is a vertical func_door gated by a touch func_button
reachable from the lift surface (board -> press -> ride).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Wire `ai_t07_lift` into the suite chain

**Files:**
- Modify: `id1/maps/ai_t06_wander.map` (its `trigger_changelevel.map` value)
- Regenerate: `id1/maps/ai_t06_wander.bsp`, `.lit`
- Modify: `scripts/run_ai_tests.sh` (the `EXPECTED` list)

- [ ] **Step 1: Repoint t06's exit to t07**

In `id1/maps/ai_t06_wander.map`, find the `trigger_changelevel` entity and change its map field:

```
"map" "ai_done"
```
to:
```
"map" "ai_t07_lift"
```

(There is exactly one `trigger_changelevel` in that file.)

- [ ] **Step 2: Recompile t06**

Run: `zig build mapcompile -- id1 ai_t06_wander`
Expected: compiles cleanly, BSP byte count printed.

- [ ] **Step 3: Add t07_lift to the run script's expected tags**

In `scripts/run_ai_tests.sh`, find the `EXPECTED` line:

```sh
EXPECTED="t01_nav t02_combat t03_stimulus t04_smoke t05_light t06_wander DONE"
```
Change it to:
```sh
EXPECTED="t01_nav t02_combat t03_stimulus t04_smoke t05_light t06_wander t07_lift DONE"
```

- [ ] **Step 4: Verify the chain hop loads (no bot)**

Run: `zig build run -- +map ai_t06_wander`
In the console, `noclip`, fly to t06's exit trigger and confirm it now loads `ai_t07_lift` (you'll see `AI-TEST t07_lift`). Close the engine.

- [ ] **Step 5: Commit**

```bash
git add id1/maps/ai_t06_wander.map id1/maps/ai_t06_wander.bsp id1/maps/ai_t06_wander.lit scripts/run_ai_tests.sh
git commit -m "$(cat <<'EOF'
test(ai): chain t07_lift into the suite before ai_done

t06_wander now exits to ai_t07_lift; run_ai_tests.sh expects the
t07_lift marker.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Baseline bot run — capture what the navmesh does

This task makes **no code changes**. It establishes the ground truth: does the bot already complete the map, and if not, exactly where the path breaks. Everything in Tasks 4-5 keys off what you record here.

**Files:** none modified.

- [ ] **Step 1: Run the bot on the map with full diagnostics**

Run: `zig build run -- +map ai_t07_lift +set bot 1`
Once loaded, open the console and run:
```
bot_debug 1
bot_debug_viz 1
sim_nav_debug 1
```
Watch the bot for ~60s. It will do one of:
- **(Success)** board lift 1 → press B1 → ride down → cross → board lift 2 → press B2 → ride up → exit (`<player> exited the level`). If this happens, the navmesh already handles it — skip to Task 6.
- **(Failure)** stand still, loop, or stall at some stage.

- [ ] **Step 2: Record the failure point precisely**

From the `bot_debug` console log and `bot_path_dump`, write down (in the commit message of Step 3 / your working notes):
- The bot's goal state (e.g. `GOTO_EXIT`).
- The full waypoint list with edge kinds (`walk`/`plat`/`ride`/`btn`/...). Note whether it contains the expected sequence: `... plat (board lift1) → btn (B1) → ride (down) → plat (off) → walk → plat (board lift2) → btn (B2) → ride (up) → plat (off) → walk → exit`.
- Where it diverges: is there **no path at all** (`nav_path 0`)? Does the path **skip a button** (no `btn` edge before a `ride`)? Does it **press a button from off the lift** (a `btn` edge whose waypoint is on static floor, not on the lift footprint)? Does it handle lift 1 (descend) but not lift 2 (ascend), or vice-versa?
- From `sim_nav_debug`, whether both lifts produced TOP+BOTTOM anchors and whether B1/B2 button anchors exist and sit **on the lift top surface** vs on adjacent static floor.

- [ ] **Step 3: Commit the findings as notes**

Append a short findings section to the plan file (or a scratch note committed alongside) so Tasks 4-5 have a written target. Example:

```bash
git add docs/superpowers/plans/2026-05-28-ai-lift-test-level.md
git commit -m "$(cat <<'EOF'
test(ai): record t07_lift baseline bot behaviour

[paste the goal/waypoint/divergence notes from Step 2]

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Fix the navmesh so the bot completes the map

> REQUIRED SUB-SKILL: Use superpowers:systematic-debugging for this task — form a hypothesis from the Task 3 findings, find the responsible code, change one thing, re-run, repeat. Do not guess-patch.

This is an investigation-and-fix loop, not a predetermined diff: the exact change depends on the Task 3 divergence. The work happens in `sdlquake/game/sim/sim_nav.c` (the bake — most likely), and possibly `sdlquake/engine_src/bot.c` (the drive layer). After each change, rebuild the game DLL with `zig build game` and re-run the bot (`zig build run -- +map ai_t07_lift +set bot 1`).

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c`
- Possibly modify: `sdlquake/engine_src/bot.c`

**Where to look, by symptom (from Task 3):**

- **No path at all (`nav_path 0`)** — the BFS that connects floor nodes to lift/button anchors isn't bridging. Read sim_nav.c's Phase 4.5 anchor-linking region (the `trace_link_clear_plat` helper at sim_nav.c:365 and the seat-probe logic around sim_nav.c:771-790) and the anchor-push points at sim_nav.c:640-643. Confirm the lift's TOP anchor connects to its adjacent ledge node and the BOTTOM anchor to the low-floor node.

- **Button anchor on static floor, not on the lift** — this is the core spec risk (see spec §"Hypothesised navmesh gaps"). For ride-while-pressing, the `BUTTON_LINK` for B1/B2 must originate from a node **on the lift's top surface**, so that when the bot presses, it's already aboard. Find where button (`NAV_NODE_DOOR_BUTTON`) anchors are created and where `BUTTON_LINK` edges connect them to floor nodes; ensure that when a button's nearest standable node is a lift TOP anchor, the link uses that, not the static floor beside the lift.

- **Path skips the button (a `ride` with no preceding `btn`)** — the bake is treating the lift as free-riding (touch/auto) and not encoding the button as a required predecessor. The bot's drive layer already refuses to advance past a button anchor until the button is pressed (`Bot_NearbyButtonReady`, bot.c:367-369), so the fix is to make the bake **place the button anchor on the path between the boarding node and the ride edge**.

- **Descend works, ascend doesn't (or vice-versa)** — the bake may assume a lift's rest state or ride direction. Compare how the TOP/BOTTOM anchors and ride edge are generated for lift 1 (rests TOP, `angle -2`) vs lift 2 (rests BOTTOM, `angle -1`) at sim_nav.c:618-651; the `z_lo`/`z_hi` ordering there (sim_nav.c:631-639) is direction-agnostic, so a directional bug is more likely in the ride-edge wiring than the anchor placement — verify with `sim_nav_debug`.

- **Two lifts cross-wire** — confirm `targetname` resolution pairs B1↔lift1 and B2↔lift2 only, and no spurious edges link the two shafts. Check any `ED_Find`/targetname matching in the button-link code.

- [ ] **Step 1: Form one hypothesis and locate the code**

From the Task 3 notes, pick the single most likely cause and find the exact function/lines in `sim_nav.c` (or `bot.c`) responsible, using the symptom map above. Read the whole surrounding function before editing — the bake's anchor/edge code is interdependent.

- [ ] **Step 2: Make the minimal change**

Change exactly one thing that the hypothesis predicts will fix the divergence. Keep the diff small and focused (per CLAUDE.md house style: no drive-by refactors, no comments restating the code).

- [ ] **Step 3: Rebuild and re-run**

Run: `zig build game` (rebuilds `game.dll` only)
Then: `zig build run -- +map ai_t07_lift +set bot 1` with `bot_debug 1` + `sim_nav_debug 1`.
Expected: the waypoint list now advances past the previous divergence point. If it stalls somewhere new, return to Step 1 with the new symptom. If the change didn't help, revert it before trying the next hypothesis (don't stack speculative edits).

- [ ] **Step 4: Loop until the bot completes the map**

Repeat Steps 1-3 until, on a clean run (`zig build run -- +map ai_t07_lift +set bot 1`, no manual console help), the bot boards lift 1 → presses B1 → rides down → crosses → boards lift 2 → presses B2 → rides up → and the engine prints `<player> exited the level` followed by `AI-TEST DONE` (it now chains to ai_done from Task 2).

- [ ] **Step 5: Commit each fix**

Commit after each change that moves the bot forward (small, frequent commits). Example:

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "$(cat <<'EOF'
fix(nav): anchor button-link on the lift top so the bot rides while pressing

[one line: which divergence this resolved, per bot_debug output]

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

If `GAME_API_VERSION` or the `engine_api_t`/`game_api_t` structs change (unlikely for nav-internal fixes, but possible if you add an engine call), bump `GAME_API_VERSION` in `sdlquake/game/game_api.h` per CLAUDE.md, and rebuild the engine (`zig build`) not just the DLL.

---

## Task 5: Regression-check the rest of the suite

The Task 4 changes touched the shared navmesh bake — verify they didn't break the lift/button handling the earlier scenarios rely on (notably `ai_t01_nav`, which has a touch-activated lift).

**Files:** none modified (unless a regression is found, then back to Task 4).

- [ ] **Step 1: Run the full suite end-to-end**

Run: `./scripts/run_ai_tests.sh`
Expected within the script's timeout: console ends with `ALL SCENARIOS PASSED`, exit code 0, and the markers list shows all eight:
```
AI-TEST t01_nav
AI-TEST t02_combat
AI-TEST t03_stimulus
AI-TEST t04_smoke
AI-TEST t05_light
AI-TEST t06_wander
AI-TEST t07_lift
AI-TEST DONE
```

- [ ] **Step 2: Triage any regression**

If a previously-passing scenario now MISSes, the navmesh change in Task 4 regressed it. Re-run that scenario alone with `bot_debug 1` (e.g. `zig build run -- +map ai_t01_nav +set bot 1`), compare its waypoint behaviour to before, and narrow the Task 4 change so it only affects the button-gated case (don't broaden a fix into the touch-lift path). Return to Task 4 Step 2 to refine.

- [ ] **Step 3: Commit (only if a refinement was needed)**

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "$(cat <<'EOF'
fix(nav): scope t07_lift fix so touch-activated lifts (t01_nav) still pass

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Final verification

**Files:** none modified.

- [ ] **Step 1: Clean full-suite run**

Run: `./scripts/run_ai_tests.sh`
Expected: `ALL SCENARIOS PASSED`, exit code 0, all eight markers present (including `AI-TEST t07_lift`).

- [ ] **Step 2: Confirm the bot completes t07 unaided one more time**

Run: `zig build run -- +map ai_t07_lift +set bot 1`
Expected: with no console interaction, the bot rides both lifts and the engine prints `<player> exited the level` then loads `ai_done` (`AI-TEST DONE`). Close the engine.

- [ ] **Step 3: Final commit if any uncommitted artefacts remain**

```bash
git status
# commit any remaining recompiled .bsp/.lit or notes
```

---

## Self-review / spec coverage

- **Two button-triggered lifts, down then up** → Task 1 map (lift1 descends, lift2 ascends, B1/B2 touch buttons). ✓
- **Flush-rest, ride-while-pressing mechanism** → Task 1 geometry (lifts flush with boarding floor; buttons cantilevered above the lift top, touchable from the lift). ✓
- **Chain into the suite before ai_done** → Task 2 (t06→t07→ai_done, run-script EXPECTED). ✓
- **Build via `zig build mapcompile`** → Task 1 Step 2, Task 2 Step 2. ✓
- **Find + fix navmesh bugs via bot debug tooling** → Task 3 (baseline capture) + Task 4 (systematic-debugging fix loop, exact code sites per symptom). ✓
- **Don't regress touch-activated lift / rest of suite** → Task 5. ✓
- **No new entity types** → Task 1 uses only worldspawn/info_player_start/func_door/func_button/trigger_changelevel/light, all registered in spawn.c. ✓
- **Out of scope (monsters, ai_t01 rework, non-lift nav)** → not touched. ✓
```

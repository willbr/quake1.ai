# AI test levels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a 6-scenario .map suite + dev gym + bash run script so we can boot the engine with `bot 1` and watch the player bot and Phase 8 monster brains run through their paces hands-off, with PASS markers emitted to stdout.

**Architecture:** Zero engine/DLL code. Each scenario is a small standalone Quake `.map` (hand-authored, compiled via the in-engine editor's existing `editor_compile_export` console command). Scenarios are chained by `trigger_changelevel`. PASS signal is the engine's existing `cl_parse.c:244` print of `cl.levelname` (which we set from `worldspawn.message`), combined with `trigger_changelevel`'s existing `SV_BPrint` of `"<name> exited the level"`.

**Tech Stack:** Quake `.map` text format; `editor_compile_export` console command (drives `qbsp_lib` + `vis_lib` + `light_lib`, all linked into the engine); bash for the run script.

**Spec:** `docs/superpowers/specs/2026-05-27-ai-test-levels-design.md`

---

## Common setup

### Compile workflow (used in every map task)

The in-engine editor has a console command `editor_compile_export` that runs qbsp + vis + light on the currently-loaded editor map and writes `.bsp` + `.lit` next to the source `.map`. To compile a hand-written .map:

```sh
zig build run -- +map start
```

In-game console (`~`):

```
editor 1
editor_load ai_<NAME>
editor_compile_export
```

After the compile finishes (watch console output `editor_compile_export: <path>.bsp = N bytes`), close the engine and verify the files exist:

```sh
ls -la id1/maps/ai_<NAME>.bsp id1/maps/ai_<NAME>.lit
```

**Note:** `editor_compile_export` calls `Scene_Save(map_path)` (editor.c:1097) before running qbsp, which overwrites the hand-authored `.map` with the editor's in-memory canonical form. Brush coordinates round-trip cleanly but comments and any non-standard entity keys are dropped. This is fine for our scenarios (all standard Quake entities); just be aware that the `.map` file on disk after a compile is the editor's reformatted version, not the hand-edited one.

### Shared .map skeleton

Every scenario .map uses this structural skeleton — a hollow 768×768×256 brick room with a sky-textured ceiling band, brushes are axis-aligned 16-unit-thick walls. Substitute `<NAME>` and `<NEXT_MAP>` per scenario. Add internal brushes (sub-walls, doors, lifts) and entities (monsters, items, triggers, lights) on top.

```
{
"classname" "worldspawn"
"wad" "gfx/base.wad"
"message" "AI-TEST <NAME>"
// floor 768×768 at z=0 to z=16
{
( 384 384 16 ) ( 384 384 0 ) ( 384 -384 0 ) sfloor4_2 0 0 0 1 1
( -384 -384 16 ) ( -384 -384 0 ) ( -384 384 0 ) sfloor4_2 0 0 0 1 1
( -384 384 16 ) ( -384 384 0 ) ( 384 384 0 ) sfloor4_2 0 0 0 1 1
( 384 -384 16 ) ( 384 -384 0 ) ( -384 -384 0 ) sfloor4_2 0 0 0 1 1
( 384 384 16 ) ( 384 -384 16 ) ( -384 -384 16 ) sfloor4_2 0 0 0 1 1
( -384 384 0 ) ( -384 -384 0 ) ( 384 -384 0 ) sfloor4_2 0 0 0 1 1
}
// ceiling (sky band) z=240 to z=256
{
( 384 384 256 ) ( 384 384 240 ) ( 384 -384 240 ) sky1 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 240 ) ( -384 384 240 ) sky1 0 0 0 1 1
( -384 384 256 ) ( -384 384 240 ) ( 384 384 240 ) sky1 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 240 ) ( -384 -384 240 ) sky1 0 0 0 1 1
( 384 384 256 ) ( 384 -384 256 ) ( -384 -384 256 ) sky1 0 0 0 1 1
( -384 384 240 ) ( -384 -384 240 ) ( 384 -384 240 ) sky1 0 0 0 1 1
}
// +Y wall: y=368..384, z=16..256
{
( 384 384 256 ) ( 384 384 16 ) ( 384 368 16 ) wbrick1_5 0 0 0 1 1
( -384 368 256 ) ( -384 368 16 ) ( -384 384 16 ) wbrick1_5 0 0 0 1 1
( -384 384 256 ) ( -384 384 16 ) ( 384 384 16 ) wbrick1_5 0 0 0 1 1
( 384 368 256 ) ( 384 368 16 ) ( -384 368 16 ) wbrick1_5 0 0 0 1 1
( 384 384 256 ) ( 384 368 256 ) ( -384 368 256 ) wbrick1_5 0 0 0 1 1
( -384 384 16 ) ( -384 368 16 ) ( 384 368 16 ) wbrick1_5 0 0 0 1 1
}
// -Y wall: y=-384..-368, z=16..256
{
( 384 -368 256 ) ( 384 -368 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 16 ) ( -384 -368 16 ) wbrick1_5 0 0 0 1 1
( -384 -368 256 ) ( -384 -368 16 ) ( 384 -368 16 ) wbrick1_5 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 16 ) ( -384 -384 16 ) wbrick1_5 0 0 0 1 1
( 384 -368 256 ) ( 384 -384 256 ) ( -384 -384 256 ) wbrick1_5 0 0 0 1 1
( -384 -368 16 ) ( -384 -384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
}
// +X wall: x=368..384, z=16..240
{
( 384 384 256 ) ( 384 384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
( 368 -384 256 ) ( 368 -384 16 ) ( 368 384 16 ) wbrick1_5 0 0 0 1 1
( 368 384 256 ) ( 368 384 16 ) ( 384 384 16 ) wbrick1_5 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 16 ) ( 368 -384 16 ) wbrick1_5 0 0 0 1 1
( 384 384 256 ) ( 384 -384 256 ) ( 368 -384 256 ) wbrick1_5 0 0 0 1 1
( 368 384 16 ) ( 368 -384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
}
// -X wall: x=-384..-368, z=16..240
{
( -368 384 256 ) ( -368 384 16 ) ( -368 -384 16 ) wbrick1_5 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 16 ) ( -384 384 16 ) wbrick1_5 0 0 0 1 1
( -384 384 256 ) ( -384 384 16 ) ( -368 384 16 ) wbrick1_5 0 0 0 1 1
( -368 -384 256 ) ( -368 -384 16 ) ( -384 -384 16 ) wbrick1_5 0 0 0 1 1
( -368 384 256 ) ( -368 -384 256 ) ( -384 -384 256 ) wbrick1_5 0 0 0 1 1
( -384 384 16 ) ( -384 -384 16 ) ( -368 -384 16 ) wbrick1_5 0 0 0 1 1
}
}
{
"classname" "info_player_start"
"origin" "-300 0 32"
"angle" "0"
}
{
"classname" "light"
"origin" "0 0 200"
"light" "300"
}
```

This skeleton compiles cleanly and produces a single empty room. Brushes you'll add per scenario use the same 6-plane-per-brush axis-aligned box format. To build any axis-aligned cube spanning `[x0..x1, y0..y1, z0..z1]`, the six faces are:

```
{
( x1 y1 z1 ) ( x1 y1 z0 ) ( x1 y0 z0 ) <TEX> 0 0 0 1 1   // +X face
( x0 y0 z1 ) ( x0 y0 z0 ) ( x0 y1 z0 ) <TEX> 0 0 0 1 1   // -X face
( x0 y1 z1 ) ( x0 y1 z0 ) ( x1 y1 z0 ) <TEX> 0 0 0 1 1   // +Y face
( x1 y0 z1 ) ( x1 y0 z0 ) ( x0 y0 z0 ) <TEX> 0 0 0 1 1   // -Y face
( x1 y1 z1 ) ( x1 y0 z1 ) ( x0 y0 z1 ) <TEX> 0 0 0 1 1   // +Z face
( x0 y1 z0 ) ( x0 y0 z0 ) ( x1 y0 z0 ) <TEX> 0 0 0 1 1   // -Z face
}
```

Use texture `wbrick1_5` for walls, `sfloor4_2` for floors, `tlight02` for lift platforms, `metal1_1` for doors, `+0basebtn` for buttons.

### Smoke-testing a single scenario

For any scenario `ai_t<NN>_<name>`:

```sh
zig build run -- +map ai_t<NN>_<name> +set bot 1
```

Watch stdout for:
1. `AI-TEST t<NN>_<name>` appearing within ~2s (level loaded, worldspawn.message printed via `cl_parse.c:244`).
2. `<player> exited the level` followed by either `AI-TEST <NEXT>` or `AI-TEST DONE` within the per-scenario time budget (60s).

If you see the first but not the second, the bot got stuck — open `bot_debug 1` to log waypoints, or load the map without the bot and walk through it manually with `noclip` to confirm the layout is reachable.

---

### Task 1: Run script + terminal marker map (ai_done)

**Files:**
- Create: `scripts/run_ai_tests.sh`
- Create: `id1/maps/ai_done.map`
- Generated artefact: `id1/maps/ai_done.bsp`, `id1/maps/ai_done.lit`

- [ ] **Step 1: Author ai_done.map**

Write `id1/maps/ai_done.map` using the skeleton above with `<NAME>` = `DONE`. The room is a tiny 64×64×128 box (smaller than the skeleton's 768×768) because no bot ever needs to traverse it — it only exists to fire its `worldspawn.message`. Use this:

```
{
"classname" "worldspawn"
"wad" "gfx/base.wad"
"message" "AI-TEST DONE"
{
( 32 32 16 ) ( 32 32 0 ) ( 32 -32 0 ) sfloor4_2 0 0 0 1 1
( -32 -32 16 ) ( -32 -32 0 ) ( -32 32 0 ) sfloor4_2 0 0 0 1 1
( -32 32 16 ) ( -32 32 0 ) ( 32 32 0 ) sfloor4_2 0 0 0 1 1
( 32 -32 16 ) ( 32 -32 0 ) ( -32 -32 0 ) sfloor4_2 0 0 0 1 1
( 32 32 16 ) ( 32 -32 16 ) ( -32 -32 16 ) sfloor4_2 0 0 0 1 1
( -32 32 0 ) ( -32 -32 0 ) ( 32 -32 0 ) sfloor4_2 0 0 0 1 1
}
{
( 32 32 128 ) ( 32 32 16 ) ( 32 -32 16 ) wbrick1_5 0 0 0 1 1
( -48 -32 128 ) ( -48 -32 16 ) ( -48 32 16 ) wbrick1_5 0 0 0 1 1
( -32 32 128 ) ( -32 32 16 ) ( 32 32 16 ) wbrick1_5 0 0 0 1 1
( 32 -32 128 ) ( 32 -32 16 ) ( -32 -32 16 ) wbrick1_5 0 0 0 1 1
( 32 32 128 ) ( 32 -32 128 ) ( -32 -32 128 ) sky1 0 0 0 1 1
( -32 32 16 ) ( -32 -32 16 ) ( 32 -32 16 ) wbrick1_5 0 0 0 1 1
}
}
{
"classname" "info_player_start"
"origin" "0 0 32"
"angle" "0"
}
{
"classname" "light"
"origin" "0 0 100"
"light" "200"
}
```

- [ ] **Step 2: Compile ai_done.map**

Launch `zig build run -- +map start`. In console:

```
editor 1
editor_load ai_done
editor_compile_export
```

Expected output: `editor_compile_export: id1/maps/ai_done.bsp = <N> bytes (lit + vis'd BSP)`. Close engine.

- [ ] **Step 3: Verify ai_done loads and prints the marker**

```sh
zig build run -- +map ai_done
```

Expected: stdout contains a line `AI-TEST DONE`. Close engine.

- [ ] **Step 4: Author scripts/run_ai_tests.sh**

Create the file with this exact content (chmod +x after writing):

```sh
#!/usr/bin/env bash
set -u
LOG=$(mktemp -t ai-test.XXXXXX.log)

zig build run -- +map ai_t01_nav +set bot 1 > "$LOG" 2>&1 &
PID=$!

TIMEOUT_S=390
DEADLINE=$(( $(date +%s) + TIMEOUT_S ))

while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if grep -q "AI-TEST DONE" "$LOG"; then break; fi
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  sleep 1
done

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

echo "=== AI test markers seen ==="
grep -E "^AI-TEST " "$LOG" || echo "(none)"

EXPECTED="t01_nav t02_combat t03_stimulus t04_smoke t05_light t06_wander DONE"
RC=0
for tag in $EXPECTED; do
  if ! grep -q "^AI-TEST $tag" "$LOG"; then
    echo "MISS: AI-TEST $tag"
    RC=1
  fi
done

if [ $RC -eq 0 ]; then
  echo "ALL SCENARIOS PASSED"
else
  echo "Log: $LOG"
fi
exit $RC
```

Then: `chmod +x scripts/run_ai_tests.sh`

- [ ] **Step 5: Smoke-test the run script against the placeholder suite**

Since the scenarios don't exist yet, every scenario marker must be missing. Run:

```sh
./scripts/run_ai_tests.sh
```

Expected: 6 `MISS: AI-TEST t<NN>_<name>` lines, possibly an `AI-TEST DONE` miss too, exit code 1. Script must not crash.

(The engine will fail to load `+map ai_t01_nav` because that map doesn't exist; it'll print an error and quit. The script's timeout will catch this within seconds.)

- [ ] **Step 6: Commit**

```sh
git add scripts/run_ai_tests.sh id1/maps/ai_done.map id1/maps/ai_done.bsp id1/maps/ai_done.lit
git commit -m "$(cat <<'EOF'
test(ai): scaffold run script + terminal marker map

scripts/run_ai_tests.sh boots the engine on the first scenario with bot 1
and parses stdout for AI-TEST markers. id1/maps/ai_done is the terminator
that prints AI-TEST DONE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: ai_t01_nav (core nav: corridor + corner + locked door + lift)

**Files:**
- Create: `id1/maps/ai_t01_nav.map`
- Generated artefact: `id1/maps/ai_t01_nav.bsp`, `id1/maps/ai_t01_nav.lit`

**Layout (top-down, +X right, +Y up):**

```
   y=+384  +------------------+
           |  exit_pad        |   ← trigger_changelevel to ai_t02_combat
   y=+256  |  ··············  |
           |  ·  silver key·  |
   y=+128  |  ·  (item_key1) |
           |  ·  ........ |   ← inner wall (locked door at x=+200, requires silver key)
   y=  0   |spawn  ·      |
           |  ·    ·      |
           |  ·    ........|
   y=-128  |  ·    ·       |
           |  ·    · lift  |   ← func_door rising platform
   y=-256  |  ·    ·       |
           |  ·············|
   y=-384  +------------------+
        x=-384            x=+384
```

Bot must: walk +X from spawn (-300,0,32), find silver key item at top of room (south corridor), come back, unlock door, ride lift up, touch exit.

For a v1 minimum, we can collapse this into a much simpler **single-room** layout that still hits all four nav primitives:

```
   spawn at (-300, 0, 32) facing +X
   silver key at (0, +200, 32)                — straight walk + slight detour
   func_door (silver-key-locked) at x=+100, y=-32..+32, z=16..96
   lift (func_door MOVETYPE_PUSH, vertical) at (200, 0, 16..96 → 16..160)
   exit_pad: trigger_changelevel at (300, 0, 16..40), 64×64 footprint, map "ai_t02_combat"
```

Use that. Single 768×768 room from skeleton, add the entities and brushes below.

- [ ] **Step 1: Copy the skeleton**

```sh
cp id1/maps/ai_done.map id1/maps/ai_t01_nav.map
```

Then edit `id1/maps/ai_t01_nav.map`:
- Change `"message" "AI-TEST DONE"` → `"message" "AI-TEST t01_nav"`.
- Replace the whole worldspawn brush list with the full 768×768 skeleton from the "Shared .map skeleton" section above.

- [ ] **Step 2: Add the silver-key item entity**

Append to the file (outside worldspawn, before EOF):

```
{
"classname" "item_key1"
"origin" "0 200 32"
}
```

- [ ] **Step 3: Add the locked door brush + entity**

Append a `func_door` brush entity. The door is 16 thick (x=92..108), 64 wide (y=-32..+32), 80 tall (z=16..96), spawnflags 8 = "DOOR_SILVER_KEY":

```
{
"classname" "func_door"
"spawnflags" "8"
"angle" "-1"
"wait" "-1"
"message" "Silver key needed"
{
( 108 32 96 ) ( 108 32 16 ) ( 108 -32 16 ) metal1_1 0 0 0 1 1
( 92 -32 96 ) ( 92 -32 16 ) ( 92 32 16 ) metal1_1 0 0 0 1 1
( 92 32 96 ) ( 92 32 16 ) ( 108 32 16 ) metal1_1 0 0 0 1 1
( 108 -32 96 ) ( 108 -32 16 ) ( 92 -32 16 ) metal1_1 0 0 0 1 1
( 108 32 96 ) ( 108 -32 96 ) ( 92 -32 96 ) metal1_1 0 0 0 1 1
( 92 32 16 ) ( 92 -32 16 ) ( 108 -32 16 ) metal1_1 0 0 0 1 1
}
}
```

- [ ] **Step 4: Add the lift func_door (vertical riser)**

The lift is a horizontal pad (192..240, -32..+32, z=16..32) that rises 144 units when activated (target by a button or trigger_multiple on top):

```
{
"classname" "func_door"
"angle" "-2"
"lip" "0"
"wait" "3"
"speed" "100"
{
( 240 32 32 ) ( 240 32 16 ) ( 240 -32 16 ) tlight02 0 0 0 1 1
( 192 -32 32 ) ( 192 -32 16 ) ( 192 32 16 ) tlight02 0 0 0 1 1
( 192 32 32 ) ( 192 32 16 ) ( 240 32 16 ) tlight02 0 0 0 1 1
( 240 -32 32 ) ( 240 -32 16 ) ( 192 -32 16 ) tlight02 0 0 0 1 1
( 240 32 32 ) ( 240 -32 32 ) ( 192 -32 32 ) tlight02 0 0 0 1 1
( 192 32 16 ) ( 192 -32 16 ) ( 240 -32 16 ) tlight02 0 0 0 1 1
}
}
```

(`angle "-2"` is "down" in Quake's door convention, which for a lift means the brush moves _up_ to its start position. The `lip` and `wait` mean it stays up for 3s and then drops back. The player-bot's drive layer steps onto the brush, the touch handler triggers the door, and on the way up the bot rides it.)

- [ ] **Step 5: Add the exit trigger_changelevel**

Trigger spans (272..336, -32..+32, z=160..168) — a 64×64 pad at the top of the lift's travel:

```
{
"classname" "trigger_changelevel"
"map" "ai_t02_combat"
{
( 336 32 168 ) ( 336 32 160 ) ( 336 -32 160 ) trigger 0 0 0 1 1
( 272 -32 168 ) ( 272 -32 160 ) ( 272 32 160 ) trigger 0 0 0 1 1
( 272 32 168 ) ( 272 32 160 ) ( 336 32 160 ) trigger 0 0 0 1 1
( 336 -32 168 ) ( 336 -32 160 ) ( 272 -32 160 ) trigger 0 0 0 1 1
( 336 32 168 ) ( 336 -32 168 ) ( 272 -32 168 ) trigger 0 0 0 1 1
( 272 32 160 ) ( 272 -32 160 ) ( 336 -32 160 ) trigger 0 0 0 1 1
}
}
```

- [ ] **Step 6: Add a light above each landmark**

Append three more `light` entities so the bot's eyes (and ours) can see what's going on:

```
{
"classname" "light"
"origin" "0 200 180"
"light" "200"
}
{
"classname" "light"
"origin" "100 0 200"
"light" "250"
}
{
"classname" "light"
"origin" "300 0 200"
"light" "250"
}
```

- [ ] **Step 7: Compile**

```sh
zig build run -- +map start
```

Console:
```
editor 1
editor_load ai_t01_nav
editor_compile_export
```

Expected output `editor_compile_export: id1/maps/ai_t01_nav.bsp = <N> bytes`. If compile fails, read the error — common causes: degenerate brush (two coplanar faces), missing texture in `gfx/base.wad`, malformed entity block.

- [ ] **Step 8: Smoke-test load without bot**

```sh
zig build run -- +map ai_t01_nav
```

Expected: stdout shows `AI-TEST t01_nav`. Walk around with `noclip` (`~` then `noclip`) — verify the door is locked from spawn, picking up the key unlocks it, the lift rises when touched, and the exit pad fires changelevel to ai_t02_combat (which won't exist yet, so the engine will error out — fine).

Close engine.

- [ ] **Step 9: Smoke-test bot completion (when t02 exists, defer until Task 9)**

This step is deferred — `ai_t02_combat` doesn't exist yet, so `trigger_changelevel` will fail. For now, verify the bot reaches the exit pad by running:

```sh
zig build run -- +map ai_t01_nav +set bot 1
```

Watch the bot navigate. Expected: bot reaches `(300, 0, 160)` area within 30s and the engine errors on the missing `ai_t02_combat`. If the bot is stuck (oscillating, not finding the key, not riding the lift), reopen the .map and tune entity placement. Close engine.

- [ ] **Step 10: Commit**

```sh
git add id1/maps/ai_t01_nav.map id1/maps/ai_t01_nav.bsp id1/maps/ai_t01_nav.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t01_nav — key + door + lift + exit

Single-room scenario that exercises the player bot's item-pickup goal,
locked-door + key handling, lift ride, and trigger_changelevel exit.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: ai_t02_combat (1v1 + 1v3 with trigger_counter gates)

**Files:**
- Create: `id1/maps/ai_t02_combat.map`
- Generated artefact: `.bsp`, `.lit`

**Layout:**

Single 768×768×256 room split by a func_door wall into arena A (south half, y<0) and arena B (north half, y>0). Spawn in A's south. A contains 1 grunt. Killing it fires `trigger_counter` (count 1), which opens the door to B. B contains 2 grunts + 1 ogre. Killing all 3 fires `trigger_counter` (count 3), which opens the exit_pad's `trigger_changelevel` (target the changelevel itself via a `func_wall` cover that gets removed, or just unlock a door in front of the exit).

Simpler wiring: use two `trigger_counter`s, each fires a `trigger_relay` which `target`s a `func_door` (one revealing the path to arena B, one revealing the exit pad).

- [ ] **Step 1: Copy skeleton from t01_nav**

```sh
cp id1/maps/ai_t01_nav.map id1/maps/ai_t02_combat.map
```

Edit `id1/maps/ai_t02_combat.map`:
- Change worldspawn `message` → `"AI-TEST t02_combat"`.
- Keep the 768×768 hollow room skeleton.
- **Remove** the t01_nav-specific entities (key, locked-door brush entity, lift func_door brush entity, exit trigger_changelevel brush, the three positional lights). Keep the worldspawn brushes + info_player_start + the centre light.
- Move spawn south: change `info_player_start` origin from `"-300 0 32"` to `"0 -320 32"`, angle `"90"`.

- [ ] **Step 2: Add dividing wall between arenas**

Brush spans (-368..+368, -16..+16, z=16..256), with a 64-wide doorway at x=0 cut out. Easier: two brushes, one for each side of the doorway:

```
{
( -64 16 256 ) ( -64 16 16 ) ( -64 -16 16 ) wbrick1_5 0 0 0 1 1
( -368 -16 256 ) ( -368 -16 16 ) ( -368 16 16 ) wbrick1_5 0 0 0 1 1
( -368 16 256 ) ( -368 16 16 ) ( -64 16 16 ) wbrick1_5 0 0 0 1 1
( -64 -16 256 ) ( -64 -16 16 ) ( -368 -16 16 ) wbrick1_5 0 0 0 1 1
( -64 16 256 ) ( -64 -16 256 ) ( -368 -16 256 ) wbrick1_5 0 0 0 1 1
( -368 16 16 ) ( -368 -16 16 ) ( -64 -16 16 ) wbrick1_5 0 0 0 1 1
}
{
( 368 16 256 ) ( 368 16 16 ) ( 368 -16 16 ) wbrick1_5 0 0 0 1 1
( 64 -16 256 ) ( 64 -16 16 ) ( 64 16 16 ) wbrick1_5 0 0 0 1 1
( 64 16 256 ) ( 64 16 16 ) ( 368 16 16 ) wbrick1_5 0 0 0 1 1
( 368 -16 256 ) ( 368 -16 16 ) ( 64 -16 16 ) wbrick1_5 0 0 0 1 1
( 368 16 256 ) ( 368 -16 256 ) ( 64 -16 256 ) wbrick1_5 0 0 0 1 1
( 64 16 16 ) ( 64 -16 16 ) ( 368 -16 16 ) wbrick1_5 0 0 0 1 1
}
```

(Both brushes go inside the worldspawn block, alongside the floor/ceiling/walls.)

- [ ] **Step 3: Add the doorway gate (func_door, initially closed, targetname "gate_b")**

Append as a brush entity (outside worldspawn):

```
{
"classname" "func_door"
"targetname" "gate_b"
"angle" "-1"
"wait" "-1"
"speed" "100"
{
( 64 16 96 ) ( 64 16 16 ) ( 64 -16 16 ) metal1_1 0 0 0 1 1
( -64 -16 96 ) ( -64 -16 16 ) ( -64 16 16 ) metal1_1 0 0 0 1 1
( -64 16 96 ) ( -64 16 16 ) ( 64 16 16 ) metal1_1 0 0 0 1 1
( 64 -16 96 ) ( 64 -16 16 ) ( -64 -16 16 ) metal1_1 0 0 0 1 1
( 64 16 96 ) ( 64 -16 96 ) ( -64 -16 96 ) metal1_1 0 0 0 1 1
( -64 16 16 ) ( -64 -16 16 ) ( 64 -16 16 ) metal1_1 0 0 0 1 1
}
}
```

- [ ] **Step 4: Add arena A monster (1 grunt, fires "kill_a" on death)**

```
{
"classname" "monster_army"
"origin" "0 -200 32"
"angle" "90"
"target" "kill_a"
}
{
"classname" "trigger_counter"
"targetname" "kill_a"
"count" "1"
"target" "gate_b"
}
```

(`trigger_counter` fires its `target` after receiving `count` activations. monster's `target` on death sends one activation.)

- [ ] **Step 5: Add arena B monsters (2 grunts + 1 ogre, fire "kill_b" on death)**

```
{
"classname" "monster_army"
"origin" "-200 200 32"
"angle" "270"
"target" "kill_b"
}
{
"classname" "monster_army"
"origin" "200 200 32"
"angle" "270"
"target" "kill_b"
}
{
"classname" "monster_ogre"
"origin" "0 320 32"
"angle" "270"
"target" "kill_b"
}
{
"classname" "trigger_counter"
"targetname" "kill_b"
"count" "3"
"target" "exit_unlock"
}
```

- [ ] **Step 6: Add exit-blocking gate (initially closed, opens on kill_b)**

A wall in front of the exit pad, plus the exit pad itself:

```
{
"classname" "func_door"
"targetname" "exit_unlock"
"angle" "-1"
"wait" "-1"
"speed" "100"
{
( 320 352 96 ) ( 320 352 16 ) ( 320 288 16 ) metal1_1 0 0 0 1 1
( 256 288 96 ) ( 256 288 16 ) ( 256 352 16 ) metal1_1 0 0 0 1 1
( 256 352 96 ) ( 256 352 16 ) ( 320 352 16 ) metal1_1 0 0 0 1 1
( 320 288 96 ) ( 320 288 16 ) ( 256 288 16 ) metal1_1 0 0 0 1 1
( 320 352 96 ) ( 320 288 96 ) ( 256 288 96 ) metal1_1 0 0 0 1 1
( 256 352 16 ) ( 256 288 16 ) ( 320 288 16 ) metal1_1 0 0 0 1 1
}
}
{
"classname" "trigger_changelevel"
"map" "ai_t03_stimulus"
{
( 320 352 48 ) ( 320 352 16 ) ( 320 288 16 ) trigger 0 0 0 1 1
( 256 288 48 ) ( 256 288 16 ) ( 256 352 16 ) trigger 0 0 0 1 1
( 256 352 48 ) ( 256 352 16 ) ( 320 352 16 ) trigger 0 0 0 1 1
( 320 288 48 ) ( 320 288 16 ) ( 256 288 16 ) trigger 0 0 0 1 1
( 320 352 48 ) ( 320 288 48 ) ( 256 288 48 ) trigger 0 0 0 1 1
( 256 352 16 ) ( 256 288 16 ) ( 320 288 16 ) trigger 0 0 0 1 1
}
}
```

- [ ] **Step 7: Add lights**

```
{
"classname" "light"
"origin" "0 -200 200"
"light" "250"
}
{
"classname" "light"
"origin" "0 200 200"
"light" "250"
}
{
"classname" "light"
"origin" "288 320 100"
"light" "200"
}
```

- [ ] **Step 8: Give the bot a weapon at spawn**

The bot starts with the axe only. Give them shells + shotgun via an early item placement near spawn:

```
{
"classname" "item_shells"
"origin" "0 -300 32"
}
{
"classname" "item_shells"
"origin" "-32 -300 32"
}
{
"classname" "item_shells"
"origin" "32 -300 32"
}
```

(Quake players start with shotgun + 25 shells normally; the bot's `client.c` `PutClientInServer` should give the same. Verify by reading `client.c`'s init code if the bot ends up axe-only.)

- [ ] **Step 9: Compile**

```sh
zig build run -- +map start
```
```
editor 1
editor_load ai_t02_combat
editor_compile_export
```

- [ ] **Step 10: Smoke-test bot completes the scenario**

```sh
zig build run -- +map ai_t02_combat +set bot 1
```

Expected within 90s: `AI-TEST t02_combat` on load, `<player> exited the level` after all 4 monsters die, then engine errors on missing `ai_t03_stimulus`. Close engine.

- [ ] **Step 11: Commit**

```sh
git add id1/maps/ai_t02_combat.map id1/maps/ai_t02_combat.bsp id1/maps/ai_t02_combat.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t02_combat — 1v1 then 1v3 arenas

Bot must kill 1 grunt to unlock arena B, then 2 grunts + 1 ogre to
unlock the exit. trigger_counter gates the progression.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: ai_t03_stimulus (patrol + corridor, monster reacts to gunfire)

**Files:**
- Create: `id1/maps/ai_t03_stimulus.map`
- Generated artefact: `.bsp`, `.lit`

**Layout:**

A 768×768 room split into two halves by a partial inner wall with a doorway. Bot spawns south. North half has a `monster_army` patrolling a 4-corner `path_corner` square. Bot's path to exit (north-east corner) passes through the same corridor the monster patrols. The intent: the bot's combat-cascade goal triggers when it sees the patroller, OR the monster's FSM aggros from stimulus when the bot fires.

- [ ] **Step 1: Copy skeleton + change message**

```sh
cp id1/maps/ai_t01_nav.map id1/maps/ai_t03_stimulus.map
```

Edit `id1/maps/ai_t03_stimulus.map`:
- Worldspawn `message` → `"AI-TEST t03_stimulus"`.
- Keep the 768×768 hollow room skeleton.
- Remove all the t01_nav non-skeleton entities and brush entities.
- Set `info_player_start` to `origin "-300 -300 32" angle "45"`.

- [ ] **Step 2: Add internal partial wall (creates a doorway)**

The wall runs along y=0 from x=-368 to x=-32 (with a 64-wide gap at x=-32..+32 forming the doorway, plus an x=+32 to x=+368 segment east of the doorway). Add inside the worldspawn brush list:

```
{
( -32 16 256 ) ( -32 16 16 ) ( -32 -16 16 ) wbrick1_5 0 0 0 1 1
( -368 -16 256 ) ( -368 -16 16 ) ( -368 16 16 ) wbrick1_5 0 0 0 1 1
( -368 16 256 ) ( -368 16 16 ) ( -32 16 16 ) wbrick1_5 0 0 0 1 1
( -32 -16 256 ) ( -32 -16 16 ) ( -368 -16 16 ) wbrick1_5 0 0 0 1 1
( -32 16 256 ) ( -32 -16 256 ) ( -368 -16 256 ) wbrick1_5 0 0 0 1 1
( -368 16 16 ) ( -368 -16 16 ) ( -32 -16 16 ) wbrick1_5 0 0 0 1 1
}
{
( 368 16 256 ) ( 368 16 16 ) ( 368 -16 16 ) wbrick1_5 0 0 0 1 1
( 32 -16 256 ) ( 32 -16 16 ) ( 32 16 16 ) wbrick1_5 0 0 0 1 1
( 32 16 256 ) ( 32 16 16 ) ( 368 16 16 ) wbrick1_5 0 0 0 1 1
( 368 -16 256 ) ( 368 -16 16 ) ( 32 -16 16 ) wbrick1_5 0 0 0 1 1
( 368 16 256 ) ( 368 -16 256 ) ( 32 -16 256 ) wbrick1_5 0 0 0 1 1
( 32 16 16 ) ( 32 -16 16 ) ( 368 -16 16 ) wbrick1_5 0 0 0 1 1
}
```

- [ ] **Step 3: Add the patrolling monster + path_corners**

```
{
"classname" "monster_army"
"origin" "-200 200 32"
"angle" "0"
"target" "p1"
}
{
"classname" "path_corner"
"targetname" "p1"
"target" "p2"
"origin" "200 200 32"
}
{
"classname" "path_corner"
"targetname" "p2"
"target" "p3"
"origin" "200 320 32"
}
{
"classname" "path_corner"
"targetname" "p3"
"target" "p4"
"origin" "-200 320 32"
}
{
"classname" "path_corner"
"targetname" "p4"
"target" "p1"
"origin" "-200 200 32"
}
```

- [ ] **Step 4: Add exit trigger at north-east**

```
{
"classname" "trigger_changelevel"
"map" "ai_t04_smoke"
{
( 336 336 48 ) ( 336 336 16 ) ( 336 272 16 ) trigger 0 0 0 1 1
( 272 272 48 ) ( 272 272 16 ) ( 272 336 16 ) trigger 0 0 0 1 1
( 272 336 48 ) ( 272 336 16 ) ( 336 336 16 ) trigger 0 0 0 1 1
( 336 272 48 ) ( 336 272 16 ) ( 272 272 16 ) trigger 0 0 0 1 1
( 336 336 48 ) ( 336 272 48 ) ( 272 272 48 ) trigger 0 0 0 1 1
( 272 336 16 ) ( 272 272 16 ) ( 336 272 16 ) trigger 0 0 0 1 1
}
}
```

- [ ] **Step 5: Add lights and shells**

```
{
"classname" "light"
"origin" "0 200 200"
"light" "250"
}
{
"classname" "light"
"origin" "0 -200 200"
"light" "200"
}
{
"classname" "item_shells"
"origin" "-300 -300 32"
}
```

- [ ] **Step 6: Compile + smoke-test**

Same workflow as Task 2 / Task 3 steps. Run:

```sh
zig build run -- +map ai_t03_stimulus +set bot 1
```

Expected: bot crosses doorway, engages or evades the grunt, reaches NE exit pad within 90s. `<player> exited the level` appears in stdout. Engine then errors on missing ai_t04_smoke.

- [ ] **Step 7: Commit**

```sh
git add id1/maps/ai_t03_stimulus.map id1/maps/ai_t03_stimulus.bsp id1/maps/ai_t03_stimulus.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t03_stimulus — patrolling monster + bot transit

Bot crosses a corridor where a monster_army patrols 4 path_corners.
Stimulus bus / FSM aggro behaviour is observed but not asserted.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: ai_t04_smoke (smoke + wind LOS occlusion)

**Files:**
- Create: `id1/maps/ai_t04_smoke.map`
- Generated artefact: `.bsp`, `.lit`

**Layout:**

Single 768×768 open room. Spawn south. A `misc_smokegrenade` at room centre, a perpendicular `info_wind_source` blowing west-to-east so the smoke plume drifts laterally across the room. A `monster_army` at the north of the room watching south. The exit pad is at the north corner. The bot must walk through the smoke shadow (occlusion) to reach the exit without the monster spotting it.

- [ ] **Step 1: Copy skeleton**

```sh
cp id1/maps/ai_t01_nav.map id1/maps/ai_t04_smoke.map
```

Edit:
- Worldspawn `message` → `"AI-TEST t04_smoke"`.
- Keep the 768×768 hollow room.
- Remove all t01_nav entity/brush content except the centre light.
- `info_player_start` → `origin "0 -300 32" angle "90"`.

- [ ] **Step 2: Add smoke + wind sources**

```
{
"classname" "misc_smokegrenade"
"origin" "0 0 32"
"dmg" "0.5"
"distance" "256"
}
{
"classname" "info_wind_source"
"origin" "-300 0 64"
"velocity" "300 0 0"
}
```

(`dmg` = smoke density, `distance` = plume radius. These are the M4 entity fields per `sim_wind.c`.)

- [ ] **Step 3: Add the watching monster**

```
{
"classname" "monster_army"
"origin" "0 320 32"
"angle" "270"
}
```

- [ ] **Step 4: Add the exit pad at north**

```
{
"classname" "trigger_changelevel"
"map" "ai_t05_light"
{
( 32 352 48 ) ( 32 352 16 ) ( 32 288 16 ) trigger 0 0 0 1 1
( -32 288 48 ) ( -32 288 16 ) ( -32 352 16 ) trigger 0 0 0 1 1
( -32 352 48 ) ( -32 352 16 ) ( 32 352 16 ) trigger 0 0 0 1 1
( 32 288 48 ) ( 32 288 16 ) ( -32 288 16 ) trigger 0 0 0 1 1
( 32 352 48 ) ( 32 288 48 ) ( -32 288 48 ) trigger 0 0 0 1 1
( -32 352 16 ) ( -32 288 16 ) ( 32 288 16 ) trigger 0 0 0 1 1
}
}
```

(Exit is right next to the monster — the only way through is if smoke breaks LOS in time. If the bot dies repeatedly, widen the room or move the monster further away.)

- [ ] **Step 5: Compile + smoke-test + commit**

```sh
zig build run -- +map start
# editor 1; editor_load ai_t04_smoke; editor_compile_export
```

```sh
zig build run -- +map ai_t04_smoke +set bot 1
```

Expected within 60s: bot exits. If bot keeps dying to the monster, this scenario may need geometry tuning (a side corridor, more smoke density, or just trimming the monster's awareness radius). That tuning happens in Task 9.

```sh
git add id1/maps/ai_t04_smoke.map id1/maps/ai_t04_smoke.bsp id1/maps/ai_t04_smoke.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t04_smoke — smoke + wind occlusion

misc_smokegrenade plume drifts across the room driven by an
info_wind_source. Bot crosses behind the plume to reach the exit
under a watching grunt.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: ai_t05_light (bright vs dark corridor fork)

**Files:**
- Create: `id1/maps/ai_t05_light.map`
- Generated artefact: `.bsp`, `.lit`

**Layout:**

768×768 room split by a north-south wall into a bright west corridor and a dark east corridor. A monster watches the bright corridor from north. The exit pad is at the north end of the dark corridor (only).

- [ ] **Step 1: Copy skeleton**

```sh
cp id1/maps/ai_t01_nav.map id1/maps/ai_t05_light.map
```

Edit:
- `message` → `"AI-TEST t05_light"`.
- Keep 768×768 skeleton.
- Remove t01_nav non-skeleton entities and the centre light.
- `info_player_start` → `origin "0 -320 32" angle "90"`.

- [ ] **Step 2: Add the dividing N-S wall**

Wall along x=0 from y=-368 to y=+368, 16 thick (x=-8..+8), with a 32-wide gap at y=-320..-288 for the bot to choose corridor:

```
{
( 8 -288 256 ) ( 8 -288 16 ) ( 8 368 16 ) wbrick1_5 0 0 0 1 1
( -8 368 256 ) ( -8 368 16 ) ( -8 -288 16 ) wbrick1_5 0 0 0 1 1
( -8 -288 256 ) ( -8 -288 16 ) ( 8 -288 16 ) wbrick1_5 0 0 0 1 1
( 8 368 256 ) ( 8 368 16 ) ( -8 368 16 ) wbrick1_5 0 0 0 1 1
( 8 -288 256 ) ( 8 368 256 ) ( -8 368 256 ) wbrick1_5 0 0 0 1 1
( -8 -288 16 ) ( -8 368 16 ) ( 8 368 16 ) wbrick1_5 0 0 0 1 1
}
```

- [ ] **Step 3: Add bright-side light + monster**

```
{
"classname" "light"
"origin" "-192 0 200"
"light" "400"
}
{
"classname" "light"
"origin" "-192 200 200"
"light" "300"
}
{
"classname" "monster_army"
"origin" "-192 320 32"
"angle" "270"
}
```

Dark side gets no lights at all (or a single very-dim one).

```
{
"classname" "light"
"origin" "192 0 200"
"light" "60"
}
```

- [ ] **Step 4: Add exit pad at north end of dark corridor**

```
{
"classname" "trigger_changelevel"
"map" "ai_t06_wander"
{
( 224 352 48 ) ( 224 352 16 ) ( 224 288 16 ) trigger 0 0 0 1 1
( 160 288 48 ) ( 160 288 16 ) ( 160 352 16 ) trigger 0 0 0 1 1
( 160 352 48 ) ( 160 352 16 ) ( 224 352 16 ) trigger 0 0 0 1 1
( 224 288 48 ) ( 224 288 16 ) ( 160 288 16 ) trigger 0 0 0 1 1
( 224 352 48 ) ( 224 288 48 ) ( 160 288 48 ) trigger 0 0 0 1 1
( 160 352 16 ) ( 160 288 16 ) ( 224 288 16 ) trigger 0 0 0 1 1
}
}
```

(In the bright corridor's north end, put a `func_wall` blocking — the bot can't exit that way, so it must take the dark side. Use this brush entity at (-224..-160, 288..352, 16..48), texture wbrick1_5.)

Actually skip the func_wall: if the only `trigger_changelevel` is on the dark side, the bot's goal cascade will path to it because that's the only exit. We're not splitting the level into reachable regions — the bot just navigates to wherever the exit is. The bright corridor is decorative. If the bot takes the bright corridor and gets shot, the test (correctly) fails to progress.

- [ ] **Step 5: Compile + smoke-test + commit**

```sh
# editor compile as before
zig build run -- +map ai_t05_light +set bot 1
```

```sh
git add id1/maps/ai_t05_light.map id1/maps/ai_t05_light.bsp id1/maps/ai_t05_light.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t05_light — dark corridor evades monster

Bot picks the dim east corridor (only one with an exit). The bright
west corridor has a watching monster_army; if the bot strays into
it, M5 Light_TierAt should still afford concealment in the dark
fork.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: ai_t06_wander (navmesh gap forces stuck-recovery)

**Files:**
- Create: `id1/maps/ai_t06_wander.map`
- Generated artefact: `.bsp`, `.lit`

**Layout:**

Two rectangular rooms placed end-to-end with a 24u-tall step separating them — enough to skip a navmesh link (the bake skips edges with z-deltas > some threshold) but small enough that the bot can jump it. The bot spawns in room A, must reach the exit in room B, navmesh can't path there, so the bot enters BOT_STUCK → WANDER and eventually crosses.

- [ ] **Step 1: Author the .map**

We need a non-standard skeleton because this scenario has two distinct floor levels. Author the file from scratch:

```
{
"classname" "worldspawn"
"wad" "gfx/base.wad"
"message" "AI-TEST t06_wander"
// floor room A: -384..+0 in y, full x range, z=0..16
{
( 384 0 16 ) ( 384 0 0 ) ( 384 -384 0 ) sfloor4_2 0 0 0 1 1
( -384 -384 16 ) ( -384 -384 0 ) ( -384 0 0 ) sfloor4_2 0 0 0 1 1
( -384 0 16 ) ( -384 0 0 ) ( 384 0 0 ) sfloor4_2 0 0 0 1 1
( 384 -384 16 ) ( 384 -384 0 ) ( -384 -384 0 ) sfloor4_2 0 0 0 1 1
( 384 0 16 ) ( 384 -384 16 ) ( -384 -384 16 ) sfloor4_2 0 0 0 1 1
( -384 0 0 ) ( -384 -384 0 ) ( 384 -384 0 ) sfloor4_2 0 0 0 1 1
}
// floor room B: 0..+384 in y, full x range, z=40..56 (24u higher)
{
( 384 384 56 ) ( 384 384 40 ) ( 384 0 40 ) sfloor4_2 0 0 0 1 1
( -384 0 56 ) ( -384 0 40 ) ( -384 384 40 ) sfloor4_2 0 0 0 1 1
( -384 384 56 ) ( -384 384 40 ) ( 384 384 40 ) sfloor4_2 0 0 0 1 1
( 384 0 56 ) ( 384 0 40 ) ( -384 0 40 ) sfloor4_2 0 0 0 1 1
( 384 384 56 ) ( 384 0 56 ) ( -384 0 56 ) sfloor4_2 0 0 0 1 1
( -384 384 40 ) ( -384 0 40 ) ( 384 0 40 ) sfloor4_2 0 0 0 1 1
}
// sky band z=240..256
{
( 384 384 256 ) ( 384 384 240 ) ( 384 -384 240 ) sky1 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 240 ) ( -384 384 240 ) sky1 0 0 0 1 1
( -384 384 256 ) ( -384 384 240 ) ( 384 384 240 ) sky1 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 240 ) ( -384 -384 240 ) sky1 0 0 0 1 1
( 384 384 256 ) ( 384 -384 256 ) ( -384 -384 256 ) sky1 0 0 0 1 1
( -384 384 240 ) ( -384 -384 240 ) ( 384 -384 240 ) sky1 0 0 0 1 1
}
// +Y wall
{
( 384 384 256 ) ( 384 384 16 ) ( 384 368 16 ) wbrick1_5 0 0 0 1 1
( -384 368 256 ) ( -384 368 16 ) ( -384 384 16 ) wbrick1_5 0 0 0 1 1
( -384 384 256 ) ( -384 384 16 ) ( 384 384 16 ) wbrick1_5 0 0 0 1 1
( 384 368 256 ) ( 384 368 16 ) ( -384 368 16 ) wbrick1_5 0 0 0 1 1
( 384 384 256 ) ( 384 368 256 ) ( -384 368 256 ) wbrick1_5 0 0 0 1 1
( -384 384 16 ) ( -384 368 16 ) ( 384 368 16 ) wbrick1_5 0 0 0 1 1
}
// -Y wall
{
( 384 -368 256 ) ( 384 -368 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 16 ) ( -384 -368 16 ) wbrick1_5 0 0 0 1 1
( -384 -368 256 ) ( -384 -368 16 ) ( 384 -368 16 ) wbrick1_5 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 16 ) ( -384 -384 16 ) wbrick1_5 0 0 0 1 1
( 384 -368 256 ) ( 384 -384 256 ) ( -384 -384 256 ) wbrick1_5 0 0 0 1 1
( -384 -368 16 ) ( -384 -384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
}
// +X wall
{
( 384 384 256 ) ( 384 384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
( 368 -384 256 ) ( 368 -384 16 ) ( 368 384 16 ) wbrick1_5 0 0 0 1 1
( 368 384 256 ) ( 368 384 16 ) ( 384 384 16 ) wbrick1_5 0 0 0 1 1
( 384 -384 256 ) ( 384 -384 16 ) ( 368 -384 16 ) wbrick1_5 0 0 0 1 1
( 384 384 256 ) ( 384 -384 256 ) ( 368 -384 256 ) wbrick1_5 0 0 0 1 1
( 368 384 16 ) ( 368 -384 16 ) ( 384 -384 16 ) wbrick1_5 0 0 0 1 1
}
// -X wall
{
( -368 384 256 ) ( -368 384 16 ) ( -368 -384 16 ) wbrick1_5 0 0 0 1 1
( -384 -384 256 ) ( -384 -384 16 ) ( -384 384 16 ) wbrick1_5 0 0 0 1 1
( -384 384 256 ) ( -384 384 16 ) ( -368 384 16 ) wbrick1_5 0 0 0 1 1
( -368 -384 256 ) ( -368 -384 16 ) ( -384 -384 16 ) wbrick1_5 0 0 0 1 1
( -368 384 256 ) ( -368 -384 256 ) ( -384 -384 256 ) wbrick1_5 0 0 0 1 1
( -384 384 16 ) ( -384 -384 16 ) ( -368 -384 16 ) wbrick1_5 0 0 0 1 1
}
}
{
"classname" "info_player_start"
"origin" "0 -200 32"
"angle" "90"
}
{
"classname" "light"
"origin" "0 -200 200"
"light" "250"
}
{
"classname" "light"
"origin" "0 200 200"
"light" "250"
}
{
"classname" "trigger_changelevel"
"map" "ai_done"
{
( 32 352 88 ) ( 32 352 56 ) ( 32 288 56 ) trigger 0 0 0 1 1
( -32 288 88 ) ( -32 288 56 ) ( -32 352 56 ) trigger 0 0 0 1 1
( -32 352 88 ) ( -32 352 56 ) ( 32 352 56 ) trigger 0 0 0 1 1
( 32 288 88 ) ( 32 288 56 ) ( -32 288 56 ) trigger 0 0 0 1 1
( 32 352 88 ) ( 32 288 88 ) ( -32 288 88 ) trigger 0 0 0 1 1
( -32 352 56 ) ( -32 288 56 ) ( 32 288 56 ) trigger 0 0 0 1 1
}
}
```

- [ ] **Step 2: Compile + smoke-test + commit**

```sh
zig build run -- +map start
# editor 1; editor_load ai_t06_wander; editor_compile_export
```

```sh
zig build run -- +map ai_t06_wander +set bot 1
```

Expected within 120s: bot eventually crosses the 24u step (either jumping naturally or via WANDER recovery) and reaches the exit. `<player> exited the level`, then `AI-TEST DONE`. If the 24u step is too easy (bot crosses immediately on first pass, no WANDER), bump it to 48u or add a 32-unit-wide pit between rooms instead.

```sh
git add id1/maps/ai_t06_wander.map id1/maps/ai_t06_wander.bsp id1/maps/ai_t06_wander.lit
git commit -m "$(cat <<'EOF'
test(ai): scenario t06_wander — navmesh gap stress

Two rooms separated by a 24u step. Navmesh bake skips the edge,
forcing the bot into stuck recovery / WANDER to cross. Exits to
ai_done which prints AI-TEST DONE.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: ai_gym (dev convenience, hub-and-spoke)

**Files:**
- Create: `id1/maps/ai_gym.map`
- Generated artefact: `.bsp`, `.lit`

The gym is a 1024×1024 outer room with four 256×256 satellite rooms attached, each containing one of the t01_nav primitives (straight walk, L-corner, key+door, lift). Each satellite returns to the hub via a teleporter. No `trigger_changelevel` — the gym is run interactively, not as part of the suite.

This task is **optional** for the v1 deliverable: skip it if t01..t06 + ai_done are all working. If you want it, follow the same pattern (skeleton, internal brushes, entities, compile, smoke-test). Defer detailed authoring until t01..t06 reveal which primitives most need a dedicated debug room.

- [ ] **Step 1: Decide whether to build ai_gym now or defer**

If t01..t06 all pass within their time budgets in Task 9, skip the gym. If one or more scenarios needed multiple iteration rounds to tune, build a focused gym room for the primitive(s) that needed tuning, so future debugging is faster. Document the decision in a follow-up commit message.

---

### Task 9: Full suite validation

**Files:** none modified — running existing scripts and reading their output.

- [ ] **Step 1: Run the full suite once end-to-end**

```sh
./scripts/run_ai_tests.sh
```

Expected within 6.5 minutes:
- Console output ends with `ALL SCENARIOS PASSED` and exit code 0.
- `=== AI test markers seen ===` shows all 7 lines:
  ```
  AI-TEST t01_nav
  AI-TEST t02_combat
  AI-TEST t03_stimulus
  AI-TEST t04_smoke
  AI-TEST t05_light
  AI-TEST t06_wander
  AI-TEST DONE
  ```

If any scenario MISSes, read the log and identify whether the bot got stuck in nav, died in combat, failed key pickup, etc. Triage per scenario:
- **t01_nav MISS** — bot couldn't path to the key, the door, or the lift. Reopen and move entities closer to spawn, or use `bot_debug 1` to see the bot's waypoint list.
- **t02_combat MISS** — bot died in combat or `trigger_counter` wiring is broken. Verify with manual run, watch monster death triggers fire.
- **t03_stimulus MISS** — bot died to the patroller. Move spawn further from the patrol path, or give bot more shells.
- **t04_smoke MISS** — bot died to the watcher. Widen the room or make the smoke denser.
- **t05_light MISS** — bot got shot in the bright corridor. Verify only the dark side has a `trigger_changelevel`; the bot's goal selection should prefer the only exit.
- **t06_wander MISS** — bot couldn't cross the step. Reduce step height or replace with a small pit.

For each tuning fix, recompile via the editor (`editor 1; editor_load <path>; editor_compile_export`), re-run the suite, commit when the affected scenario passes.

- [ ] **Step 2: Intentional-failure test**

Temporarily remove the `trigger_changelevel` from `ai_t03_stimulus.map`, recompile, run `./scripts/run_ai_tests.sh`. Expected: `MISS: AI-TEST t04_smoke`, `MISS: AI-TEST t05_light`, `MISS: AI-TEST t06_wander`, `MISS: AI-TEST DONE`. Restore the trigger and recompile.

(This verifies the run script correctly detects partial-suite failures, not just full-suite success.)

- [ ] **Step 3: Final commit (only if any tuning fixes were made)**

```sh
git add id1/maps/ai_t*.map id1/maps/ai_t*.bsp id1/maps/ai_t*.lit
git commit -m "$(cat <<'EOF'
test(ai): suite tuning after full-run validation

[describe specific tunings made]

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Spec coverage check

- **Goal — both bot AIs running together:** ✓ Every scenario has monsters (sim_ai.c) and the player bot; t02..t05 specifically exercise the interaction.
- **Run all scenarios, don't short-circuit on failure:** ✓ Task 1 Step 4's `set -u` (not `-e`) and the EXPECTED loop.
- **No engine/DLL code changes:** ✓ Plan is map-content only.
- **Zero new entities:** ✓ Uses only worldspawn, info_player_start, light, item_key1, item_shells, func_door, trigger_changelevel, trigger_counter, monster_army, monster_ogre, path_corner, misc_smokegrenade, info_wind_source — all already implemented.
- **Six scenarios + done marker + optional gym:** ✓ Tasks 1, 2-7, 8.
- **`worldspawn.message` + `trigger_changelevel` exit print as signal:** ✓ Every scenario sets the message; every non-terminal scenario has a `trigger_changelevel`.
- **Compile via in-engine editor:** ✓ Every map task uses `editor_load` + `editor_compile_export`.
- **Bash run script with set -u, MISS reporting, exit code:** ✓ Task 1 Step 4.
- **Suite ordering: t01_nav → t02_combat → t03_stimulus → t04_smoke → t05_light → t06_wander → ai_done:** ✓ Each scenario's `trigger_changelevel.map` field chains to the next.

No gaps.

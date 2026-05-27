# AI test levels design

**Date:** 2026-05-27
**Status:** Draft

## Goal

A repeatable test rig for the project's two "bot AIs" running together:

- The **player bot** (`sdlquake/engine_src/bot.c`, see `docs/superpowers/specs/2026-05-26-player-bot-design.md`) — drives `usercmd` toward exits, fights monsters, fetches keys.
- The **monster brains** (`sdlquake/game/sim/sim_ai.c`, Phase 8 M2/M2.5) — `IDLE`/`SUSPICIOUS`/`SEARCHING`/`COMBAT` FSM running on `monster_army`, `monster_ogre`, etc.

Success = boot the engine on `+map ai_t01_nav` with `bot 1`, hands-off, and observe a sequence of `AI-TEST <name>` markers in stdout culminating in `AI-TEST DONE`. Each marker confirms one scenario was completed (player bot reached the scenario's `trigger_changelevel`, possibly after killing required monsters).

The test runs **all scenarios**: failures don't short-circuit the suite. If the bot gets stuck in `ai_t03_stimulus`, the run script will time out that scenario and report which markers were and weren't seen, rather than aborting.

## Architecture

Two artefacts:

1. **A `.map` suite** committed under `id1/maps/` — one gym map and six scenario maps, chained by `trigger_changelevel`.
2. **A bash run script** `scripts/run_ai_tests.sh` that launches `zig build run` with the first scenario, parses stdout for `AI-TEST <name>` markers, applies a per-scenario timeout, and prints a final pass/fail summary.

No engine or `game.dll` code changes. **Zero new entities.** The whole rig reuses what's already there:

- `cl_parse.c:244` already prints `worldspawn.message` to stdout on every level load.
- `client.c:255` (`spawn_trigger_changelevel`) already emits `"<netname> exited the level\n"` to all clients via `SV_BPrint`, which on the local client lands in `Con_Printf` → stdout.
- `monster_army`, `monster_ogre`, `func_door`, `item_key1`/`item_key2`, `trigger_changelevel`, `trigger_counter`, `trigger_once`, `path_corner`, `misc_smokegrenade`, `info_wind_source`, `light` already work in `game.dll`.

## Scenario suite

Each scenario is a single small map (~512–1024 units across), one floor, brick walls, minimal lighting. Each map's worldspawn carries `"message" "AI-TEST <name>"` and ends in a `trigger_changelevel` that links to the next scenario.

| Order | Map | worldspawn.message | Tests | Success path |
|---|---|---|---|---|
| 1 | `ai_t01_nav` | `AI-TEST t01_nav` | Core nav: straight, L-corner, locked door + key, lift | Player bot walks straight, turns L, grabs `item_key1`, opens `func_door`, takes lift up, touches exit |
| 2 | `ai_t02_combat` | `AI-TEST t02_combat` | 1v1 then 1v3 combat; monster FSM IDLE→COMBAT; weapon switching | Player bot kills `monster_army` × 1 in arena A; `trigger_counter` opens door to arena B; kills `monster_army` × 2 + `monster_ogre` × 1; touches exit |
| 3 | `ai_t03_stimulus` | `AI-TEST t03_stimulus` | Stimulus bus: monster on patrol around a corner; player bot's gunfire alerts it; SUSPICIOUS→SEARCHING transition | `monster_army` follows `path_corner` patrol; player bot enters in a different room, fires weapon (in COMBAT goal cascade, or as nuisance if no LOS), then transits a corridor that the alerted monster is now searching; success on exit |
| 4 | `ai_t04_smoke` | `AI-TEST t04_smoke` | M4 smoke + wind LOS occlusion in `Wind_PathOcclusion` | Open room with `misc_smokegrenade` in the middle and `info_wind_source` blowing perpendicular; `monster_army` on far side; bot crosses behind the smoke wall to exit |
| 5 | `ai_t05_light` | `AI-TEST t05_light` | M5 `Light_TierAt` thresholds; dark patch hides player | Two corridors from spawn to exit: a brightly lit one with a `monster_army` watching, and a dark unlit one; only the dark route is navmesh-reachable to the exit |
| 6 | `ai_t06_wander` | `AI-TEST t06_wander` | Player bot's WANDER + give-up timer (nav fallback) | Two rooms separated by a step the navmesh-bake doesn't bridge (intentional gap); bot stalls in `BOT_STUCK`, twists yaw, jumps, eventually crosses; exits the second room |
| 7 | `ai_done` | `AI-TEST DONE` | Terminal marker | Tiny 64-unit room with no exit. Loaded by the previous scenario's `trigger_changelevel` so its worldspawn.message fires; the test script sees `AI-TEST DONE` and tears down. |

The gym (`ai_gym`) is a separate hub-and-spoke map that exposes the same four nav rooms as `ai_t01_nav` (straight / corner / door+key / lift), each as a satellite reachable from a central hub, with each satellite teleporting back to the hub on completion. The gym is for visual iteration during development — not part of the suite, not chained with `trigger_changelevel`. Run with `+map ai_gym; bot 1` manually.

## Signal flow

Stdout when the suite runs cleanly (showing only `AI-TEST` and "exited" lines):

```
AI-TEST t01_nav
<player> exited the level
AI-TEST t02_combat
<player> exited the level
AI-TEST t03_stimulus
<player> exited the level
AI-TEST t04_smoke
<player> exited the level
AI-TEST t05_light
<player> exited the level
AI-TEST t06_wander
<player> exited the level
AI-TEST DONE
```

The "AI-TEST" markers come from `cl_parse.c:244` printing `cl.levelname` (which is the worldspawn.message) on each level load. The "exited the level" markers come from `client.c:255-256` (`SV_BPrint` from `trigger_changelevel`'s touch handler), arriving at the local client and being routed to `Con_Printf` like any other broadcast.

A scenario that doesn't complete will simply not produce its corresponding "exited" line, and the next `AI-TEST <name>` won't appear. The run script's timeout catches this.

## Combat scenarios — counting monster deaths

For `ai_t02_combat`, the success condition is "all monsters dead" not "touch exit". Quake's existing `monster_*` entities fire their `target` field on death (see `client.c` death handling). We wire:

```
monster_army { targetname "" ; target "deathcount_a" }
monster_army { target "deathcount_a" }
trigger_counter { targetname "deathcount_a" ; count 2 ; target "open_door_b" }
func_door     { targetname "open_door_b" ; ... }
```

When both arena-A grunts die, `trigger_counter` fires once, opening the door to arena B. Arena B has a similar counter targeting the exit's `trigger_changelevel`. This is stock Quake; no new entities. Verified at spec time: `spawn_trigger_counter` exists in `triggers.c:189` and is registered in `spawn.c:263`.

## Stimulus / smoke / light scenarios — what we're actually measuring

These scenarios are deliberately set up so that **failure** of the sim system produces a visible bot failure:

- **Stimulus (t03):** if `Sim_Stimulus_Sound` doesn't propagate the bot's gunfire, the monster stays IDLE and the bot walks past unmolested. PASS on exit. If the monster aggros and kills the bot, the scenario hangs (bot dead, no progress). The marker tells us "stimulus didn't break the level" but not "stimulus worked"; for that we'd need to grep additional debug prints. Acceptable for v1 — visual inspection on dev runs covers the positive case.
- **Smoke (t04):** if `Wind_PathOcclusion` doesn't kick in, the monster on the far side sees the bot crossing and kills it. Same observability tradeoff.
- **Light (t05):** if `Light_TierAt` thresholds aren't applied, the monster spots the bot in the "dark" corridor. Same.

The first pass of this rig is "does the suite stay playable to the end". A second pass (out of scope here, captured as follow-up) would add MCP polling for monster FSM state via `list_entities` to actively assert SUSPICIOUS / COMBAT transitions.

## Wander scenario — what counts as success

`ai_t06_wander` deliberately exercises the player bot's stuck-recovery (WANDER + give-up timer). Success criterion is unchanged: bot eventually touches the exit, producing the next `AI-TEST` marker. The implementation note for this scenario is that the geometry must be reachable in principle (the bot's drive layer can jump 32u steps; the trace-forward-and-strafe handles wall-scraping) but the navmesh bake must fail to link the two rooms — otherwise we're testing nav, not WANDER.

A practical recipe: place the two rooms 80u apart on a ledge with a 24u step in between; the navmesh bake skips edges over >48u drops (see `sim_nav.c`), so the rooms end up unlinked even though the bot can jump across.

## Run script

`scripts/run_ai_tests.sh`:

```sh
#!/usr/bin/env bash
set -u
LOG=$(mktemp -t ai-test.XXXXXX.log)
ZIG_OUT="${LOG%.log}.out"

zig build run -- +map ai_t01_nav +set bot 1 > "$LOG" 2>&1 &
PID=$!

# Total budget: 6 scenarios × 60s = 360s + 30s slack
TIMEOUT_S=390
DEADLINE=$(( $(date +%s) + TIMEOUT_S ))

while [ $(date +%s) -lt "$DEADLINE" ]; do
  if grep -q "AI-TEST DONE" "$LOG"; then break; fi
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  sleep 1
done

kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

echo "=== AI test markers seen ==="
grep -E "^AI-TEST " "$LOG"

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

- `+set bot 1` rather than `bot 1`: the bot cvar is archived, so a one-time `+set` is enough.
- Per-scenario timing isn't enforced; the rig is "did the whole run finish within budget". This matches the user's stated preference to run all tests rather than abort on first failure.
- The script is `set -u` not `set -e`: we want it to continue past missing markers and report all of them at the end.

## Compile path

No qbsp / light CLIs are installed (`which qbsp light` returns nothing; no Homebrew formula, no ericw-tools). The project does have `qbsp_lib` linked into the engine, used by `editor_compile_export` (F3 → Compile).

For this work: hand-author each `.map`, open it in the in-game editor, click Compile, commit the `.bsp` + `.lit` alongside. Same workflow as `m7_skeleton`. No new tooling.

(Aside, out of scope: a `compile_map <path>` console command that runs `qbsp_lib` headlessly would let `scripts/run_ai_tests.sh` rebuild maps from source. Capture as a follow-up if the round-trip becomes annoying.)

## File touch list

**New maps** (each as `.map` source + `.bsp` + `.lit` compiled artefact):

- `id1/maps/ai_gym.map` — dev gym, hub-and-spoke
- `id1/maps/ai_t01_nav.map` — straight + L + key+door + lift
- `id1/maps/ai_t02_combat.map` — 1v1 then 1v3 arenas
- `id1/maps/ai_t03_stimulus.map` — patrol + corridor
- `id1/maps/ai_t04_smoke.map` — open room with smoke + wind
- `id1/maps/ai_t05_light.map` — bright vs dark corridor fork
- `id1/maps/ai_t06_wander.map` — two rooms, navmesh gap
- `id1/maps/ai_done.map` — terminal marker, no exit

**New scripts:**

- `scripts/run_ai_tests.sh`

No `game.dll` or engine source changes are needed. `trigger_counter` is already present (`triggers.c:189`, registered `spawn.c:263`).

## Out of scope

- Active assertion of monster FSM transitions (SUSPICIOUS / SEARCHING / COMBAT). The current rig only observes "did the bot survive to the next scenario", not "did the monster brain react correctly". A follow-up could add MCP polling of `list_entities`.
- Automated `.map` → `.bsp` rebuild on save. Maps are checked-in artefacts; manual recompile via the editor.
- Demo recording for replay debugging.
- Multi-skill testing. `bot_skill` is held at its default; we don't sweep difficulty.
- Performance / regression timing of the suite. Pass/fail only.

## Testing the test

Smoke test once after each scenario map exists:

1. `zig build run -- +map ai_t<NN>_<name>; bot 1` — watch the bot complete the scenario, see `AI-TEST t<NN>_<name>` on load and `AI-TEST <next>` (or `AI-TEST DONE`) on exit.
2. After all six scenarios exist, run `scripts/run_ai_tests.sh` and confirm `ALL SCENARIOS PASSED`.
3. Intentionally break one scenario (e.g., wall up the exit), rerun the script, confirm it reports the right `MISS:` line.

No engine-side automated tests are added. The project has none and `CLAUDE.md` explicitly says build success + visual correctness is the verification method.

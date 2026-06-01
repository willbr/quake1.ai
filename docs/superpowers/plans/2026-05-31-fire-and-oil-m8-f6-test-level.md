# Fire & Oil — M8/F6 (Test Level + Tooling + Balance) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the final M8 stage — a dedicated `ai_t10_fire.map` showcase level that exercises every fire/oil system at once, zero-ABI headless-inspection count cvars, and a balance/perf pass — all DLL-side, `GAME_API_VERSION` stays **36**.

**Architecture:** Two tiny DLL additions (a `misc_oilslick` map-spawn oil seed in `flammables.c`; two read-only count cvars in `sim_fire.c`) plus a hand-authored `.map` built on the proven `m7_skeleton` box-room shell. Verification is the established MCP rig (give weapons → spray → ignite → screenshot + assert via `get_cvar`), not a unit suite.

**Tech Stack:** C (modern-C game DLL), Zig build, vendored in-process qbsp/light/vis (`zig build mapcompile`), Quake `.map` brush format, MCP HTTP/SSE rig (port 9876).

**Verification model:** This repo has no unit-test suite (per `CLAUDE.md`). Verification is: (1) `zig build game` + `zig build` succeed; (2) `zig build mapcompile -- id1 ai_t10_fire` produces `.bsp`+`.lit`; (3) launch `+map ai_t10_fire --mcp-http 9876` and drive the MCP rig to confirm each system + read the new count cvars; (4) a `profile` capture with many simultaneous fires stays within frame budget.

**Locked decisions:** see `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md` → "F6 — locked decisions (2026-05-31)". Single showcase room; zero-ABI count cvars (no `fire_query` MCP tool); control/area-denial balance; `AI-TEST t10_fire` marker but NOT added to `run_ai_tests.sh`'s gating list.

---

## File Structure

- `sdlquake/game/flammables.c` — ADD `oilslick_think` + `spawn_misc_oilslick` (map-spawn oil seed; deferred deposit + periodic re-pour). Peer of the existing `spawn_misc_oilbarrel`.
- `sdlquake/game/spawn.c` — ADD forward decl + dispatch-table entry for `misc_oilslick`.
- `sdlquake/game/sim/sim_fire.c` — ADD `fire_burning_count` + `fire_lit_oil_count` cvar registration (next to `fire_oil_count`) and per-tick writes (at the existing `fire_oil_count` write site, line ~629).
- `id1/maps/ai_t10_fire.map` — NEW showcase level (box room from `m7_skeleton` shell + fire/oil entities).
- `id1/maps/ai_t10_fire.bsp` / `.lit` — NEW compiled outputs (committed alongside the `.map`, per the ai_tNN convention).
- `CLAUDE.md`, the design spec, and `~/.claude/.../memory/m8-fire-staged-build.md` — docs.

These changes are independent: Tasks 1 and 2 are pure DLL additions; Task 3 authors the map (depends on Task 1's `misc_oilslick` classname existing); Task 4 verifies + balances; Task 5 documents.

---

## Task 1: `misc_oilslick` map-spawn oil seed

**Files:**
- Modify: `sdlquake/game/flammables.c` (add after `spawn_misc_oilbarrel`, ~line 122)
- Modify: `sdlquake/game/spawn.c` (forward decl ~line 110-112; dispatch entry ~line 233)

A point entity that deposits one oil patch at its origin shortly after level start (deferred so the sim/oil pool is fully live) and re-pours before the 60 s `OIL_TTL_SECS` so the showcase slick never fully evaporates between demos. `Fire_AddOil(origin, 0, 0)` lets the sim default radius/amount (`OIL_DEFAULT_RADIUS` 48 / `OIL_DEFAULT_AMOUNT` 1) — no need to expose the `#define`s.

- [ ] **Step 1: Add the spawn function to `flammables.c`**

Add immediately after `spawn_misc_oilbarrel`'s closing brace (~line 122):

```c
// ---------------------------------------------------------------------------
// misc_oilslick (F6) — a map-spawn oil seed for the ai_t10_fire showcase. Not a
// visible prop: it deposits an oil patch at its origin via Fire_AddOil, deferred
// one frame so the sim/oil pool is fully live at level start, then re-pours
// before the 60s OIL_TTL_SECS so a pre-placed slick stays available across
// repeated demos. Passing 0/0 lets Fire_AddOil default the radius/amount.
// ---------------------------------------------------------------------------
static void oilslick_think(edict_t *self) {
    Fire_AddOil(self->v.origin, 0.0f, 0.0f);
    self->v.nextthink = g->time + 40.0f;   // < OIL_TTL_SECS (60) so it never lapses
}

void spawn_misc_oilslick(edict_t *e) {
    g->self        = e;
    e->v.solid     = SOLID_NOT;
    e->v.movetype  = MOVETYPE_NONE;
    e->v.think     = oilslick_think;
    e->v.nextthink = g->time + 0.5f;       // after frame 1: sim is initialised
}
```

- [ ] **Step 2: Forward-declare + register in `spawn.c`**

After the existing `void spawn_misc_oilbarrel(edict_t *e);` (~line 110), add:

```c
void spawn_misc_oilslick(edict_t *e);
```

In the dispatch table, after the `{ "misc_oilbarrel", spawn_misc_oilbarrel },` line (~233), add:

```c
    { "misc_oilslick",                spawn_misc_oilslick                   },
```

- [ ] **Step 3: Build the DLL**

Run: `zig build game`
Expected: compiles clean (no warnings from `flammables.c` / `spawn.c`). `Fire_AddOil` is already declared in `sim.h`; `g`, `eng`, `edict_t`, `SOLID_NOT`, `MOVETYPE_NONE` are all already used in `flammables.c`.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/flammables.c sdlquake/game/spawn.c
git commit -m "feat(fire): misc_oilslick map-spawn oil seed for the F6 showcase

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Headless-inspection count cvars

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c` (register ~line 122; write ~line 629)

Two read-only cvars the DLL refreshes each `Fire_Frame` tick, read over MCP via the existing `get_cvar`. Closes the F5 gap where "is it burning?" had to be inferred from health deltas (the stale-read gotcha). No ABI bump.

- [ ] **Step 1: Register the cvars**

In the init block, immediately after the existing `eng->Cvar_Register("fire_oil_count", "0");` (line 122), add:

```c
    eng->Cvar_Register("fire_burning_count",  "0");  // Fire_Frame: live burning-edict count
    eng->Cvar_Register("fire_lit_oil_count",  "0");  // Fire_Frame: live lit oil-patch count
```

- [ ] **Step 2: Write the counts each tick**

At the end of `oil_frame()`, replace:

```c
    eng->Cvar_SetValue("fire_oil_count", (float)live);
}
```

with:

```c
    eng->Cvar_SetValue("fire_oil_count", (float)live);

    /* F6: headless-inspection counts (read via MCP get_cvar). Cheap full scans
       once per fire tick; co-located with the existing fire_oil_count write. */
    {
        int lit = 0, burning = 0;
        for (int i = 0; i < OIL_MAX_PATCHES; i++)
            if (s_oil[i].active && s_oil[i].lit) lit++;
        for (int n = 0; n < FIRE_MAX_BURNING; n++)
            if (s_burning[n].active) burning++;
        eng->Cvar_SetValue("fire_lit_oil_count", (float)lit);
        eng->Cvar_SetValue("fire_burning_count", (float)burning);
    }
}
```

- [ ] **Step 3: Build the DLL**

Run: `zig build game`
Expected: compiles clean. `s_oil`, `s_burning`, `OIL_MAX_PATCHES`, `FIRE_MAX_BURNING` are all in scope in `sim_fire.c`.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(fire): fire_burning_count + fire_lit_oil_count cvars for headless asserts (M8/F6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Author + compile `ai_t10_fire.map`

**Files:**
- Create: `id1/maps/ai_t10_fire.map`
- Create (compiled): `id1/maps/ai_t10_fire.bsp`, `id1/maps/ai_t10_fire.lit`

The box-room shell (floor / sky ceiling / 4 brick walls) is copied verbatim from `m7_skeleton.map`'s outer brushes (proven-good windings — interior corridor dropped). Interior is x:[-752,752] y:[-368,368] z:[16,240]. Player enters west looking east. Layout west→east: arsenal → demo slick → torches+crates → oil-trail→barrel → monster cluster.

- [ ] **Step 1: Write the map**

Create `id1/maps/ai_t10_fire.map` with exactly:

```
{
"classname" "worldspawn"
"wad" "gfx/base.wad"
"message" "AI-TEST t10_fire"
{
( 768 384 16 ) ( 768 384 0 ) ( 768 -384 0 ) sfloor4_2 0 0 0 1 1
( -768 -384 16 ) ( -768 -384 0 ) ( -768 384 0 ) sfloor4_2 0 0 0 1 1
( -768 384 16 ) ( -768 384 0 ) ( 768 384 0 ) sfloor4_2 0 0 0 1 1
( 768 -384 16 ) ( 768 -384 0 ) ( -768 -384 0 ) sfloor4_2 0 0 0 1 1
( 768 384 16 ) ( 768 -384 16 ) ( -768 -384 16 ) sfloor4_2 0 0 0 1 1
( -768 384 0 ) ( -768 -384 0 ) ( 768 -384 0 ) sfloor4_2 0 0 0 1 1
}
{
( 768 384 256 ) ( 768 384 240 ) ( 768 -384 240 ) sky1 0 0 0 1 1
( -768 -384 256 ) ( -768 -384 240 ) ( -768 384 240 ) sky1 0 0 0 1 1
( -768 384 256 ) ( -768 384 240 ) ( 768 384 240 ) sky1 0 0 0 1 1
( 768 -384 256 ) ( 768 -384 240 ) ( -768 -384 240 ) sky1 0 0 0 1 1
( 768 384 256 ) ( 768 -384 256 ) ( -768 -384 256 ) sky1 0 0 0 1 1
( -768 384 240 ) ( -768 -384 240 ) ( 768 -384 240 ) sky1 0 0 0 1 1
}
{
( 768 384 256 ) ( 768 384 16 ) ( 768 368 16 ) wbrick1_5 0 0 0 1 1
( -768 368 256 ) ( -768 368 16 ) ( -768 384 16 ) wbrick1_5 0 0 0 1 1
( -768 384 256 ) ( -768 384 16 ) ( 768 384 16 ) wbrick1_5 0 0 0 1 1
( 768 368 256 ) ( 768 368 16 ) ( -768 368 16 ) wbrick1_5 0 0 0 1 1
( 768 384 256 ) ( 768 368 256 ) ( -768 368 256 ) wbrick1_5 0 0 0 1 1
( -768 384 16 ) ( -768 368 16 ) ( 768 368 16 ) wbrick1_5 0 0 0 1 1
}
{
( 768 -368 256 ) ( 768 -368 16 ) ( 768 -384 16 ) wbrick1_5 0 0 0 1 1
( -768 -384 256 ) ( -768 -384 16 ) ( -768 -368 16 ) wbrick1_5 0 0 0 1 1
( -768 -368 256 ) ( -768 -368 16 ) ( 768 -368 16 ) wbrick1_5 0 0 0 1 1
( 768 -384 256 ) ( 768 -384 16 ) ( -768 -384 16 ) wbrick1_5 0 0 0 1 1
( 768 -368 256 ) ( 768 -384 256 ) ( -768 -384 256 ) wbrick1_5 0 0 0 1 1
( -768 -368 16 ) ( -768 -384 16 ) ( 768 -384 16 ) wbrick1_5 0 0 0 1 1
}
{
( 768 384 256 ) ( 768 384 16 ) ( 768 -384 16 ) wbrick1_5 0 0 0 1 1
( 752 -384 256 ) ( 752 -384 16 ) ( 752 384 16 ) wbrick1_5 0 0 0 1 1
( 752 384 256 ) ( 752 384 16 ) ( 768 384 16 ) wbrick1_5 0 0 0 1 1
( 768 -384 256 ) ( 768 -384 16 ) ( 752 -384 16 ) wbrick1_5 0 0 0 1 1
( 768 384 256 ) ( 768 -384 256 ) ( 752 -384 256 ) wbrick1_5 0 0 0 1 1
( 752 384 16 ) ( 752 -384 16 ) ( 768 -384 16 ) wbrick1_5 0 0 0 1 1
}
{
( -752 384 256 ) ( -752 384 16 ) ( -752 -384 16 ) wbrick1_5 0 0 0 1 1
( -768 -384 256 ) ( -768 -384 16 ) ( -768 384 16 ) wbrick1_5 0 0 0 1 1
( -768 384 256 ) ( -768 384 16 ) ( -752 384 16 ) wbrick1_5 0 0 0 1 1
( -752 -384 256 ) ( -752 -384 16 ) ( -768 -384 16 ) wbrick1_5 0 0 0 1 1
( -752 384 256 ) ( -752 -384 256 ) ( -768 -384 256 ) wbrick1_5 0 0 0 1 1
( -768 384 16 ) ( -768 -384 16 ) ( -752 -384 16 ) wbrick1_5 0 0 0 1 1
}
}
{
"classname" "info_player_start"
"origin" "-680 0 40"
"angle" "0"
}
{
"classname" "info_intermission"
"origin" "0 0 210"
"mangle" "25 0 0"
}
{
"classname" "light"
"origin" "-400 0 200"
"light" "150"
}
{
"classname" "light"
"origin" "0 0 200"
"light" "150"
}
{
"classname" "light"
"origin" "400 0 200"
"light" "150"
}
{
"classname" "light"
"origin" "640 0 200"
"light" "160"
}
{
"classname" "weapon_oilgun"
"origin" "-620 -72 30"
}
{
"classname" "weapon_flamethrower"
"origin" "-620 72 30"
}
{
"classname" "item_cells"
"origin" "-560 -48 30"
"spawnflags" "1"
}
{
"classname" "item_cells"
"origin" "-560 48 30"
"spawnflags" "1"
}
{
"classname" "item_cells"
"origin" "-560 0 30"
"spawnflags" "1"
}
{
"classname" "misc_oilslick"
"origin" "-360 0 18"
}
{
"classname" "light_torch_small_walltorch"
"origin" "-200 356 96"
"light" "200"
}
{
"classname" "light_torch_small_walltorch"
"origin" "200 356 96"
"light" "200"
}
{
"classname" "light_torch_small_walltorch"
"origin" "-200 -356 96"
"light" "200"
}
{
"classname" "light_torch_small_walltorch"
"origin" "200 -356 96"
"light" "200"
}
{
"classname" "misc_breakable"
"origin" "100 -220 24"
}
{
"classname" "misc_breakable"
"origin" "160 -220 24"
}
{
"classname" "misc_oilslick"
"origin" "250 0 18"
}
{
"classname" "misc_oilslick"
"origin" "306 0 18"
}
{
"classname" "misc_oilslick"
"origin" "362 0 18"
}
{
"classname" "misc_oilslick"
"origin" "418 0 18"
}
{
"classname" "misc_oilbarrel"
"origin" "490 0 24"
}
{
"classname" "monster_army"
"origin" "580 -32 40"
"angle" "180"
}
{
"classname" "monster_army"
"origin" "580 32 40"
"angle" "180"
}
{
"classname" "monster_army"
"origin" "660 -48 40"
"angle" "180"
}
{
"classname" "monster_ogre"
"origin" "700 0 40"
"angle" "180"
}
```

- [ ] **Step 2: Compile the map**

Run: `zig build mapcompile -- id1 ai_t10_fire`
Expected: prints qbsp/vis/light progress, exits 0, writes `id1/maps/ai_t10_fire.bsp` + `.lit`. If qbsp reports a **leak** or "brush with no normals", a wall winding is wrong — the 6 worldspawn brushes are copied verbatim from `m7_skeleton.map`; re-diff them against the source. (A leak also occurs if an entity is placed outside the room — keep every origin inside x:[-752,752] y:[-368,368] z:[16,240].)

- [ ] **Step 3: Visual smoke check**

Run: `zig build run -- +map ai_t10_fire` (or attach the MCP rig in Task 4). Confirm: the room loads, the console prints `AI-TEST t10_fire`, you spawn facing the arsenal-then-room, four wall torches are lit, the barrel + crates + 4 monsters are visible, and `fire_oil_count` is ≥1 within ~1 s (the slicks deposited). If a monster/barrel "fell out of level" prints in the console, nudge its origin onto open floor.

- [ ] **Step 4: Commit**

```bash
git add id1/maps/ai_t10_fire.map id1/maps/ai_t10_fire.bsp id1/maps/ai_t10_fire.lit
git commit -m "feat(fire): ai_t10_fire showcase test level (M8/F6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Runtime verification (MCP rig) + balance/perf pass

**Files:**
- Create (scratch, not committed): `/tmp/f6_verify.py` (clone the f5 MCP client plumbing: SSE thread + POST, `console_exec`/`get_cvar`/`set_cvar`/`wait_frames`/`screenshot`/`teleport`/`inspect_entity`/`list_entities`).

No code edits unless the balance pass finds a default clearly wrong (then a one-line cvar-default change in `sim_fire.c`/`weapons_fire.c`, separately committed).

- [ ] **Step 1: Launch a fresh instance** (hot-reload defers the DLL swap while a map is live — verify gotcha #1)

Run (background): `zig build run -- +map ai_t10_fire --mcp-http 9876`

- [ ] **Step 2: Drive the rig + assert via the new count cvars.** Confirm each system; rely on cvars that *change* across reads (health reads can be stale — gotcha #11); insert a `screenshot` between `impulse` calls (gotcha #2):
  1. **Slick present:** `get_cvar fire_oil_count` ≥ 1 at spawn (the seeds). Screenshot.
  2. **Ignite the demo slick:** teleport near (-360,0), `impulse 210` (ignite at crosshair) or `fire_oil_ignite 1`; `get_cvar fire_lit_oil_count` ≥ 1; screenshot shows flame.
  3. **Oil-trail → barrel:** stand west of the trail, light the (250,0) end; over a few seconds `fire_lit_oil_count` rises as the cascade races east; the barrel (`list_entities` → its id) explodes (`misc_oilbarrel`→`explo_box` classname flip + full-screen boom screenshot). The barrel's `T_RadiusDamage` re-lights its own spilled oil.
  4. **Monster panic + contact-spread:** after the barrel boom / flamethrower, `fire_burning_count` ≥ 1 and rises as fire spreads between the clustered grunts; monster health (via `inspect_entity`, trust changing reads) drops; the ogre (200 hp) burns longest. Screenshot the panic/flee + smoke.
  5. **Torch interactions:** `impulse 215` (toggle nearest torch) or Gust at a torch → it snuffs (model gone); flamethrower at an unlit torch → relights; a lit torch ignites oil within reach.
  6. **Breakable:** flamethrower a crate → `misc_breakable` burns down → breaks (puff + `ax1.wav`).
  7. **Gust self-rescue/extinguish:** stand in burning oil (player ignites) → `+gust`/`-gust` → player fire out, lit oil in the cone consumed (`fire_lit_oil_count` drops).

- [ ] **Step 3: Balance pass (control/area-denial).** With the cluster burning, confirm the current defaults read as "threatening, not instant":
  - grunt (30 hp) under `fire_dps` 8 → DOT = dps×FIRE_DMG_INTERVAL(0.5) = 4/tick at 2 Hz ⇒ ~7.5 s to kill a grunt by fire alone — i.e. fire *pressures + panics*, the kill comes from combat/boom. If it feels like fire alone trivially mass-kills, that's the "fast finisher" failure mode → do **not** raise `fire_dps`. If fire feels inert, nudge `fire_secs` (burn duration) up rather than dps.
  - Confirm panic-flee + smoke-LOS dominate the encounter feel over raw damage. Record the final numbers (likely unchanged).

- [ ] **Step 4: Perf pass.** Pour a long oil trail with `+pouroil` (or several `impulse 211`), light it so many patches + plumes are active, then `profile 120` (captures to `profiles/perf_<ts>.json` + summary). Confirm `Sim_Frame`/fire scope and `VID_Update` stay within frame budget (no red frames in the histogram). Note any hotspot — the per-tick `oil_frame` O(patches×edicts) scan and `Lightmap_AddDelta` churn are the known suspects (spatial buckets = documented follow-up; fix only if it actually blows the budget).

- [ ] **Step 5: Commit any balance tweak (only if Step 3 changed a default).** Otherwise no commit here.

```bash
# only if a default changed:
git add sdlquake/game/sim/sim_fire.c
git commit -m "balance(fire): <what + why> (M8/F6)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Docs + memory

**Files:**
- Modify: `CLAUDE.md` (append an F6 sentence to the `sim_fire.c` bullet; add `ai_t10_fire.map` to the "Reference data" / maps note; flip the M8 milestone table F6 row to ✅)
- Modify: `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md` (status line → "F6 implemented + verified + committed")
- Modify: `~/.claude/projects/-Users-wjbr-src-quake1-ai/memory/m8-fire-staged-build.md` (F6 paragraph: commits, what landed, review verdict, runtime verification, final balance numbers; mark M8 complete)

- [ ] **Step 1: Update `CLAUDE.md`** — in the F-stage paragraph on the `sim_fire.c` bullet, append F6: the `ai_t10_fire.map` showcase, the `misc_oilslick` seed entity, the `fire_burning_count`/`fire_lit_oil_count` cvars, and "no ABI bump (`GAME_API_VERSION` stays 36) — M8 complete". Note `ai_t10_fire` in the maps list.

- [ ] **Step 2: Flip the spec status line** to `F1–F6 implemented + verified + committed; M8 complete`. Read it back.

- [ ] **Step 3: Update the memory file** — add the F6 paragraph (mirror the F5 entry style: commits, landed features, review verdict, runtime-verified results, final balance numbers, any new verify gotchas), and update the description frontmatter + the "Next stage" line to "M8 complete".

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-05-30-fire-and-oil-design.md
git commit -m "docs(fire): record M8/F6 (showcase level, count cvars, balance) — M8 complete

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

(The memory file lives outside the repo — no `git add`.)

---

## Self-Review

**Spec coverage:** Test level (Task 3) ✓; zero-ABI count cvars / "tooling" (Task 2) ✓; balance + perf pass (Task 4) ✓; `AI-TEST t10_fire` marker ✓; not added to `run_ai_tests.sh` gating list ✓ (no edit to that file in any task); `misc_oilslick` seed (Task 1) ✓. All three locked decisions covered.

**Placeholder scan:** All code is verbatim; the map is complete; commands are exact (`zig build game`, `zig build mapcompile -- id1 ai_t10_fire`, `+map ai_t10_fire --mcp-http 9876`, `profile 120`). The only conditional is Task 4 Step 5 (commit *iff* a default changed) — intentional.

**Type/name consistency:** `spawn_misc_oilslick` (decl in spawn.c, def in flammables.c, classname `misc_oilslick` in table + map) — consistent. `Fire_AddOil(vec3_t, float, float)` matches sim_fire.c:208. Cvars `fire_burning_count`/`fire_lit_oil_count` consistent between register + write + Task 4 asserts. Classnames in the map (`weapon_oilgun`, `weapon_flamethrower`, `item_cells`, `misc_oilbarrel`, `misc_breakable`, `light_torch_small_walltorch`, `info_player_start`, `info_intermission`, `light`) all verified registered (DLL spawn table) or engine/compile-handled (`light*`).

**Risk note:** brush windings are the classic failure — mitigated by copying `m7_skeleton`'s proven shell verbatim. Monster contact-spread at 64u is borderline for standing monsters (bbox spacing) — the two front grunts are placed 64 apart so the demo triggers as they bunch/flee; the F5 verify already data-confirmed spread, so this is a showcase nicety, not the proof.

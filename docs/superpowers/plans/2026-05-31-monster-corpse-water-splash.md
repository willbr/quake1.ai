# Monster & Corpse Water-Entry Splash — Implementation Plan

## Goal

Give `FL_MONSTER` bodies (live monsters, corpses, thrown heads) a person-sized,
speed-scaled, per-liquid water-entry splash matching the player's, by extending the
existing `TE_WATERSPLASH` system in one engine function. No protocol or `game_api`
ABI change.

## Context

- **Design/spec:** `docs/superpowers/specs/2026-05-31-monster-corpse-water-splash-design.md`
  (read it for the *why*, the current-state survey, and edge-case rationale).
- **Single touched file:** `sdlquake/engine_src/sv_phys.c`.
- **Build:** `zig build run -- +map e1m1` (builds engine + game.dll and runs).
- **No test suite** — verification is build success + in-game behaviour, per
  `CLAUDE.md`.
- **Approach (approved):** Approach A — one new branch in `SV_CheckWaterTransition`
  keyed on `FL_MONSTER`, plus an `sv_bodysplash` kill-switch cvar.

### Key facts established during design (so the implementer needn't rediscover)

- `SV_CheckWaterTransition` (around `sv_phys.c:1305`) is the single chokepoint for
  air→liquid crossings. It already: plays `misc/h2ohit1.wav`; gates the visual
  splash behind `vmag >= 100`; finds the surface (4096u "air above" probe + 14-step
  binary search into `hi_z`); picks `kind` (0 water / 1 slime / 2 lava) from the
  contents entered; and emits `n_bursts` × `TE_WATERSPLASH` with a per-burst
  datagram-budget break. Rockets (`classname "missile"`) emit 4 bursts, grenades
  (`"grenade"`) 3, else 1.
- It runs for monsters and corpses (Step physics) and thrown heads (Toss physics).
  The **player does not** go through it; the player's splash is a separate game-side
  path and is out of scope here.
- `FL_MONSTER` is the same flag family as `FL_SWIM` / `FL_FLY` / `FL_ONGROUND`, which
  are already used inside `sv_phys.c` — so `FL_MONSTER` is in scope engine-side.
- Per-liquid tinting and the positional splash sound are already handled by the
  client when it receives `TE_WATERSPLASH` (`cl_tent.c:301`). Nothing client-side
  needs to change.

> **Tooling note for this session:** Bash/grep output was intermittently dropping or
> returning garbled content while this plan was written. Treat every concrete line
> number here as approximate — confirm by reading the file. Stage 1 begins with an
> explicit current-state check for exactly this reason.

---

## Stage 1: Add the `sv_bodysplash` cvar

**Goal:** A registered server cvar `sv_bodysplash` (float, default `1`) usable from
the console, with current state confirmed before editing.

**First — confirm current state (guards against partial/pre-existing work):**

1. `grep -rn bodysplash sdlquake/` — expect matches **only** in the design doc, none
   in `.c` files. If `sv_bodysplash` already exists in `sv_phys.c`, stop and
   reconcile against this plan (it may be partly implemented); skip whatever is
   already done.
2. Read `SV_CheckWaterTransition` in `sdlquake/engine_src/sv_phys.c` in full and
   confirm it matches the "Key facts" above before Stage 2.

**Changes (`sdlquake/engine_src/sv_phys.c`):**

- Declare the cvar next to the other server cvars at the top of the file (where
  `sv_gravity`, `sv_maxvelocity`, `sv_nostep` are declared). Match the short
  `cvar_t` initializer form used by `sv_nostep`:

  ```c
  cvar_t  sv_bodysplash = {"sv_bodysplash","1"};
  ```

- Register it wherever the sibling server cvars are registered. Find the site with
  `grep -n "Cvar_RegisterVariable (&sv_nostep" sdlquake/engine_src/*.c` (same place
  `sv_gravity`/`sv_nostep` are registered — `SV_Init` in `sv_main.c` in stock
  WinQuake; confirm in this tree). Add:

  ```c
  Cvar_RegisterVariable (&sv_bodysplash);
  ```

**Verification:**

- [ ] `zig build run -- +map e1m1` compiles and launches.
- [ ] At the console (`~`), typing `sv_bodysplash` prints `"1"`; `sv_bodysplash 0`
      then `sv_bodysplash` prints `"0"`. (No behavioural change yet.)

---

## Stage 2: Body branch in `SV_CheckWaterTransition`

**Goal:** Bodies emit a person-sized, speed-scaled, 2-burst, per-liquid splash on any
air→liquid entry; gibs and projectiles are unchanged.

**Changes (`sdlquake/engine_src/sv_phys.c`, inside `SV_CheckWaterTransition`, in the
`cont <= CONTENTS_WATER` → `watertype == CONTENTS_EMPTY` block):**

1. After `vmag` is computed, derive the body flag:

   ```c
   int is_body = ((int)ent->v.flags & FL_MONSTER) && sv_bodysplash.value;
   ```

2. Relax the speed gate so bodies are not subject to the `>= 100` threshold:

   ```c
   // was: if (vmag >= 100.0f)
   if (is_body || vmag >= 100.0f)
   ```

3. Keep the existing "air within 4096u above" guard and the binary search to the
   surface (`hi_z`) exactly as-is — both paths share them.

4. Replace the strength / burst-count selection (currently: clamp `vmag*0.03` to
   [8,16], then bump for `missile`/`grenade`) with a body-first branch. Declare the
   three values once, then choose:

   ```c
   int   strength;
   int   n_bursts;
   float offset_r;
   if (is_body)
   {
       // Person-sized, speed-scaled plunk. Ceiling 96 == the player's
       // fixed entry splash; floor 32 keeps slow wade-ins/slumps visible.
       strength = (int)(32.0f + vmag * 0.16f);
       if (strength > 96) strength = 96;
       if (strength < 32) strength = 32;
       n_bursts = 2;          // body-sized footprint, like the player's two bursts
       offset_r = 10.0f;
   }
   else
   {
       // Unchanged projectile path.
       strength = (int)(vmag * 0.03f);
       if (strength > 16) strength = 16;
       if (strength < 8)  strength = 8;
       n_bursts = 1;
       offset_r = 0.0f;
       if (ent->v.classname && strcmp (ent->v.classname, "missile") == 0) {
           n_bursts = 4; offset_r = 12.0f; strength = 16;
       } else if (ent->v.classname && strcmp (ent->v.classname, "grenade") == 0) {
           n_bursts = 3; offset_r = 10.0f; strength = 16;
       }
   }
   ```

5. Leave the `kind` selection and the `for (bi = 0; bi < n_bursts; bi++)` emit loop
   (with its `cursize >= MAX_DATAGRAM - 16` break and `n_bursts > 1` offset jitter)
   unchanged — they already consume `strength`, `n_bursts`, `offset_r`, and `kind`.

**Notes / gotchas:**

- `strength` max is 96 → fits in the single `MSG_WriteByte` already used. Fine.
- `is_body` short-circuits on `sv_bodysplash.value == 0`, so the kill-switch makes
  bodies fall straight back to the old behaviour (projectile path, `vmag >= 100`
  only). No separate code path needed for the off case.
- `FL_MONSTER` covers live monsters, corpses, and thrown heads (it is never cleared
  on death — see spec). Gibs (`classname "gib"`, no `FL_MONSTER`) and the player are
  unaffected.
- Do not touch the `SV_StartSound (ent, 0, "misc/h2ohit1.wav", ...)` call — the entry
  sound is intentionally kept (spec "Sound" edge case).

**Verification (build + in-game smoke test — required, not just compilation):**

- [ ] `zig build run -- +map e1m1` compiles and launches.
- [ ] Pick/seed a map with water, slime, and lava (e.g. fight near a pool, or use the
      MCP tools to position monsters / teleport). For each liquid, with
      `sv_bodysplash 1`:
  - [ ] A live monster (grunt and ogre) falling/walking into the liquid throws a
        clearly visible, correctly-tinted splash.
  - [ ] The **corpse** going under (kill it over/at the liquid) also splashes.
  - [ ] A slow wade-in produces a smaller — but still visible — splash (floor works).
- [ ] **Gibs unchanged:** gib a monster over water; gib splashes stay subtle (no
      person-sized burst).
- [ ] **Player unchanged:** the player's own entry splash looks as before.
- [ ] **Kill-switch:** `sv_bodysplash 0` → monster/corpse entries go back to
      near-invisible; `sv_bodysplash 1` restores the splash. (Same session, no
      restart needed.)
- [ ] No console errors; no perf complaints (transitions are rare; the binary search
      runs only on the crossing frame).

---

## Out of scope (do not implement)

- Unifying the player's fixed-96 plunk with this speed curve.
- Any AI/stimulus reaction to the splash.
- Gib splash tuning.
- Promoting the floor/ceiling/scale/burst constants to cvars (leave as named
  literals; only `sv_bodysplash` is a cvar).

## Done when

Both stages' verification checklists pass, and the change is committed to `master`.

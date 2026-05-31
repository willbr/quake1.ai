# Monster & Corpse Water-Entry Splash Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `FL_MONSTER` bodies (live monsters, corpses, thrown heads) a person-sized, speed-scaled, per-liquid water-entry splash matching the player's.

**Architecture:** Extend the one existing engine chokepoint, `SV_CheckWaterTransition` in `sdlquake/engine_src/sv_phys.c`, with a `FL_MONSTER` "body" branch that reuses the existing `TE_WATERSPLASH` emit path (surface-find, per-liquid `kind`, datagram budget). Add an `sv_bodysplash` kill-switch cvar. No protocol change, no `game_api` ABI bump, no client change.

**Tech Stack:** C (engine, compiled `-std=gnu89`), Zig build (`zig build run`). No unit-test harness exists for engine C in this repo — per `CLAUDE.md`, verification is **build success + in-game behaviour**. Steps therefore use build + console + visual smoke-test as the "test", not red-green-refactor.

---

## Context (read before starting)

- **Design/spec:** `docs/superpowers/specs/2026-05-31-monster-corpse-water-splash-design.md` — the *why*, the current-state survey, and edge-case rationale.
- **Build & run:** `zig build run -- +map e1m1` builds engine + game.dll and launches. Open the console with `~`.
- **This is an engine change** (`sdlquake/engine_src/`), not the hot-reloadable game DLL, so a full `zig build run` is required; `zig build game` will not pick it up.

### Confirmed facts (verified in-tree; don't re-derive)

- `SV_CheckWaterTransition` begins at `sdlquake/engine_src/sv_phys.c:1305`. The air→liquid case is the `if (cont <= CONTENTS_WATER)` → `if (ent->v.watertype == CONTENTS_EMPTY)` block (~lines 1325-1411). It already: plays `misc/h2ohit1.wav`; computes `vmag`; gates the visual splash behind `if (vmag >= 100.0f)`; probes for air within 4096u above; binary-searches the surface into `hi_z`; picks `kind` (`0` water / `1` slime / `2` lava) from `cont`; and emits `n_bursts` × `TE_WATERSPLASH` in a datagram-budgeted loop. Rockets (`classname "missile"`) → 4 bursts, grenades (`"grenade"`) → 3, else 1.
- It runs for monsters/corpses (`SV_Physics_Step`) and thrown heads (`SV_Physics_Toss`). The **player does not** go through it (`SV_Physics_Client`'s `MOVETYPE_WALK` path uses `SV_WalkMove`/`SV_CheckWater`). Player splash is a separate game-side path — out of scope.
- `FL_MONSTER` is defined engine-side: `sdlquake/engine_src/server.h:161` (`#define FL_MONSTER 32`); already used as `(int)ent->v.flags & FL_MONSTER` in `world.c:863`. In scope in `sv_phys.c`.
- `FL_MONSTER` is never cleared on death and is kept by thrown heads (`game/weapons.c:642`), so it identifies live monsters + corpses + heads, while excluding gibs (`classname "gib"`) and projectiles.
- Server cvars are declared in `sv_phys.c` (e.g. `sv_phys.c:76: cvar_t sv_gravity = {"sv_gravity","800",false,true};`) and registered in `SV_Init` (`sv_main.c:55: Cvar_RegisterVariable (&sv_gravity);`).
- `TE_WATERSPLASH` is `16` (`protocol.h:174`); the client parse at `cl_tent.c:301` already renders the per-`kind` tint **and** plays a positional `h2ohit`. Nothing client-side changes.

## File Structure

- **Modify only:** `sdlquake/engine_src/sv_phys.c`
  - cvar declaration (top, beside other `sv_` cvars)
  - the `FL_MONSTER` branch inside `SV_CheckWaterTransition`
- **Modify:** `sdlquake/engine_src/sv_main.c` — one `Cvar_RegisterVariable` line in `SV_Init`.

No new files. No headers change (`FL_MONSTER` already in `server.h`).

---

## Task 1: Add and register the `sv_bodysplash` cvar

**Files:**
- Modify: `sdlquake/engine_src/sv_phys.c` (cvar declaration near line 76)
- Modify: `sdlquake/engine_src/sv_main.c` (registration in `SV_Init`, near line 55)

- [ ] **Step 1: Confirm the feature isn't already present**

Run:
```bash
grep -rn "bodysplash" /Users/wjbr/src/quake1.ai/sdlquake/
```
Expected: **no matches** (the token currently lives only in the design doc under `docs/`). If anything prints from a `.c`/`.h` file, the feature is partly implemented — stop and reconcile against this plan before continuing.

- [ ] **Step 2: Declare the cvar**

In `sdlquake/engine_src/sv_phys.c`, immediately after the existing `sv_gravity` declaration (line ~76), add:

```c
cvar_t	sv_bodysplash = {"sv_bodysplash","1"};	// monster/corpse water-entry splash (0=off)
```

- [ ] **Step 3: Register the cvar**

In `sdlquake/engine_src/sv_main.c`, in `SV_Init`, immediately after `Cvar_RegisterVariable (&sv_gravity);` (line ~55), add:

```c
	Cvar_RegisterVariable (&sv_bodysplash);
```

If `sv_bodysplash` is declared `static` or not visible to `sv_main.c`, add `extern cvar_t sv_bodysplash;` beside the existing `extern cvar_t sv_gravity;` at `sv_main.c:44`. (Stock layout declares these non-static in `sv_phys.c` and `extern`s them in `sv_main.c`; match whatever the sibling cvars do.)

- [ ] **Step 4: Build**

Run:
```bash
zig build run -- +map e1m1
```
Expected: compiles and launches into `e1m1` with no new warnings/errors about `sv_bodysplash`.

- [ ] **Step 5: Verify the cvar exists at runtime**

In-game, press `~` and type:
```
sv_bodysplash
```
Expected console output: `"sv_bodysplash" is "1"`. Then `sv_bodysplash 0` followed by `sv_bodysplash` prints `"0"`. Set it back to `1`. (No behavioural change yet — that's Task 2.) Close the game.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/engine_src/sv_phys.c sdlquake/engine_src/sv_main.c
git commit -m "feat(engine): add sv_bodysplash cvar (monster/corpse water splash toggle)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Body branch in `SV_CheckWaterTransition`

**Files:**
- Modify: `sdlquake/engine_src/sv_phys.c` (`SV_CheckWaterTransition`, the `watertype == CONTENTS_EMPTY` block, ~lines 1340-1390)

The three edits below are inside the `if (ent->v.watertype == CONTENTS_EMPTY)` block, after the `SV_StartSound(... "misc/h2ohit1.wav" ...)` line. Apply them in order.

- [ ] **Step 1: Derive the body flag (insert)**

Find the `vmag` computation:

```c
			float vx = ent->v.velocity[0];
			float vy = ent->v.velocity[1];
			float vz = ent->v.velocity[2];
			float vmag = (float)sqrt(vx*vx + vy*vy + vz*vz);
			if (vmag >= 100.0f)
```

Insert one line so it reads:

```c
			float vx = ent->v.velocity[0];
			float vy = ent->v.velocity[1];
			float vz = ent->v.velocity[2];
			float vmag = (float)sqrt(vx*vx + vy*vy + vz*vz);
			// Live monsters, corpses, and thrown heads (all keep FL_MONSTER)
			// get a person-sized splash like the player; gibs/projectiles do not.
			int is_body = ((int)ent->v.flags & FL_MONSTER) && sv_bodysplash.value;
			if (is_body || vmag >= 100.0f)
```

(That single edit both adds `is_body` and relaxes the speed gate for bodies.)

- [ ] **Step 2: Branch the strength/burst selection (replace)**

Find the existing strength/burst block:

```c
					int strength = (int)(vmag * 0.03f);
					if (strength > 16) strength = 16;
					if (strength < 8)  strength = 8;
					int n_bursts = 1;
					float offset_r = 0.0f;
					if (ent->v.classname && strcmp (ent->v.classname, "missile") == 0) {
						n_bursts = 4;            // rocket
						offset_r = 12.0f;
						strength = 16;
					} else if (ent->v.classname && strcmp (ent->v.classname, "grenade") == 0) {
						n_bursts = 3;            // grenade
						offset_r = 10.0f;
						strength = 16;
					}
```

Replace it with:

```c
					int strength;
					int n_bursts;
					float offset_r;
					if (is_body) {
						// Person-sized, speed-scaled plunk. Ceiling 96 ==
						// the player's fixed entry splash; floor 32 keeps
						// slow wade-ins / corpse slumps visible. Hits full
						// strength around ~400 u/s entry speed.
						strength = (int)(32.0f + vmag * 0.16f);
						if (strength > 96) strength = 96;
						if (strength < 32) strength = 32;
						n_bursts = 2;            // body-sized footprint
						offset_r = 10.0f;
					} else {
						strength = (int)(vmag * 0.03f);
						if (strength > 16) strength = 16;
						if (strength < 8)  strength = 8;
						n_bursts = 1;
						offset_r = 0.0f;
						if (ent->v.classname && strcmp (ent->v.classname, "missile") == 0) {
							n_bursts = 4;            // rocket
							offset_r = 12.0f;
							strength = 16;
						} else if (ent->v.classname && strcmp (ent->v.classname, "grenade") == 0) {
							n_bursts = 3;            // grenade
							offset_r = 10.0f;
							strength = 16;
						}
					}
```

- [ ] **Step 3: Confirm the emit loop is untouched**

Verify the code after your replacement still reads (do **not** change it — it already consumes `strength`, `n_bursts`, `offset_r`, `kind`):

```c
					int kind = 0; // water
					if (cont == CONTENTS_SLIME) kind = 1;
					if (cont == CONTENTS_LAVA)  kind = 2;
					for (int bi = 0; bi < n_bursts; bi++) {
						if (sv.datagram.cursize >= MAX_DATAGRAM - 16) break;
						...
						MSG_WriteByte  (&sv.datagram, TE_WATERSPLASH);
						...
						MSG_WriteByte  (&sv.datagram, kind);
						MSG_WriteByte  (&sv.datagram, strength);
					}
```

Also confirm you did **not** alter the `SV_StartSound (ent, 0, "misc/h2ohit1.wav", 255, 1);` line above — the entry sound is intentionally kept.

- [ ] **Step 4: Build**

Run:
```bash
zig build run -- +map e1m1
```
Expected: compiles and launches, no errors. (`strength` max 96 fits the existing single `MSG_WriteByte`.)

- [ ] **Step 5: Smoke-test — live monsters in each liquid**

`e1m1` has water near the start. For slime/lava, either fight near a known pool or use the MCP tools / `noclip` + `impulse`-spawns to stage monsters over each liquid. With `sv_bodysplash 1`:

- [ ] A grunt and an ogre **falling** into water throw a clearly visible, blue, body-sized splash (two bursts).
- [ ] Same over **slime** → green-tinted splash; over **lava** → lava-tinted splash.
- [ ] A monster **walking slowly** into water still makes a smaller but visible splash (floor of 32 works — not "nothing").

Tip: the existing MCP `screenshot` tool + teleport (see the smoke-test rig) can stage and capture these.

- [ ] **Step 6: Smoke-test — corpses, gibs, player, kill-switch**

- [ ] Kill a monster so its **corpse drops into** water/slime/lava → corpse entry splashes.
- [ ] **Gib** a monster over water (rockets/SSG) → gib splashes stay small/subtle (no body-sized burst); this confirms gibs are excluded.
- [ ] The **player's own** entry splash looks unchanged.
- [ ] `sv_bodysplash 0` → monster/corpse entries revert to near-invisible; `sv_bodysplash 1` restores the splash (same session, no restart).
- [ ] No console errors; framerate steady (transitions are rare; the binary search runs only on the crossing frame).

- [ ] **Step 7: Commit**

```bash
git add sdlquake/engine_src/sv_phys.c
git commit -m "feat(engine): person-sized water splash for monsters & corpses

SV_CheckWaterTransition now gives FL_MONSTER bodies a speed-scaled
(32..96), 2-burst, per-liquid TE_WATERSPLASH on any air->liquid entry,
matching the player. Gibs/projectiles unchanged; gated by sv_bodysplash.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Out of scope (do not implement)

- Unifying the player's fixed-96 plunk with this speed curve.
- Any AI / stimulus reaction to the splash.
- Gib splash tuning.
- Promoting the floor/ceiling/scale/burst constants to cvars (leave as named literals; only `sv_bodysplash` is a cvar).

## Done when

Task 1 and Task 2 verification checkboxes pass and both commits are on `master`.

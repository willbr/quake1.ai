# Fire & Oil — M8 / F3 Weapons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add two player weapons — an **oil gun** (sprays oil patches, coats monsters, inert until lit) and a **flamethrower** (short-range cone that ignites oil + sets edicts burning) — built on the existing Phase 6 `weapon2` dispatch, drawing from shared `ammo_cells`.

**Architecture:** Both weapons run through the existing Phase 6 parallel roster (`self->v.weapon2 != 0` → `W_Attack_Phase6`), so the stock 8-weapon switching logic stays untouched. All logic lives in a new DLL-side file `weapons_fire.c`; the held-fire cadence is modeled on the lightning gun's self-perpetuating 10 Hz think loop. The weapons reuse existing `.mdl` view models (`v_light.mdl` flamethrower, `v_rock.mdl` oil gun) and call only the already-shipped fire/oil API (`Fire_PourOil`, `Fire_AddOil`, `Fire_IgniteMaybeCoated`, `Fire_LightOilNear`) plus existing engine pointers (`SV_Fire`, `SV_Decal`, `SV_Traceline`, `SV_StartSound`). **No `engine_api_t`/`entvars_t` change → `GAME_API_VERSION` stays 36, no ABI bump.**

**Tech Stack:** C (game DLL, hot-reloadable), Zig build (`zig build` full / `zig build game` DLL-only), MCP HTTP transport for runtime verification.

---

## Context the implementer needs

This project has **no unit-test framework**. Verification = build success + runtime observation (in-game / MCP / screenshot), exactly as F1 and F2 were verified. Each task ends with a build + a concrete runtime check + a commit.

**Process constraints (from project memory — follow exactly):**
- Commit straight to `master`. Never branch or use a worktree.
- Commit each task immediately when it's done and verified.
- End every commit message with: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`

**Build / iterate:**
- Task 1 adds a new `.c` file and edits `build.zig` → run **`zig build`** (full) and **restart** the game.
- Tasks 2–5 touch only `game.dll` sources (no ABI change) → run **`zig build game`**; if a `--hot-reload` instance is running it picks up the new DLL within ~1 s. A fresh `zig build run` also works.

**Verification rig (matches the F1/F2 pattern — see project memory `m8-fire-staged-build`, `smoke-test-rig`, `headless-bot-testing`):**
- Windowed instance with MCP: `zig build run -- --hot-reload +map e1m1 --mcp-http 9876`.
- MCP tools over HTTP `localhost:9876`: `console_exec`, `get_cvar`, `set_cvar`, `wait_frames`, `screenshot`, `teleport`, `get_player_state` (returns `"position"`), `console_tail`, `list_entities`. A one-shot SSE client pattern is in `/tmp/f2_dbg*.py` (reuse it).
- **Grant the new weapons for testing:** console `impulse 212` (added in Task 1) grants both fire weapons + 200 cells and selects the oil gun. Select oil gun = `impulse 40`, flamethrower = `impulse 41`.
- **Existing fire debug aids:** `impulse 211` deposits an oil patch at the crosshair; `fire_oil_count` cvar reports live oil-patch count (MCP-readable); `impulse 210` ignites the entity/oil under the crosshair; `sim_fire_debug 1` and `r_decals_debug 1` print diagnostics.

**Key facts established by code reconnaissance (cite when in doubt):**
- Impulse dispatch: `weapons.c::ImpulseCommands` (`weapons.c:1652`). Phase 6 select is `if (imp >= 30 && imp <= 39) Phase6_ChangeWeapon(imp);`. Impulses 40–99 are free; 210/211 are fire/oil debug, 212+ free.
- Phase 6 weapon selector lives in `self->v.weapon2`; the bitfield is `self->v.items2`; bits `IT2_*` are defined in `game_defs.h:90-103` (currently bits 0–9 used; 10+ free).
- `weapons_phase6.c`: `Phase6_ChangeWeapon` (`:833`), `W_Attack_Phase6` (`:734`), `W_SetCurrentAmmo_Phase6` (`:751`), `Phase6_CheatGiveAll` (`:861`). Declarations in `weapons_phase6.h`.
- `W_Attack` routes to Phase 6 when `weapon2 != 0` (`weapons.c:1438-1447`): `eng->MakeVectors(self->v.v_angle); ... W_FireGust(); W_Attack_Phase6(); return;`. `W_WeaponFrame` (`weapons.c:1684`) calls `W_Attack` each postthink, gated by `g->time < self->v.attack_finished` and `self->v.button0`.
- **Lightning held-fire reference** (`player.c:294-318` + `weapons.c:1485`): on press, `W_Attack` calls `player_light1(self)`, sets `attack_finished = g->time + 0.1`, plays `weapons/lstart.wav` once. `player_light1/2_think` ping-pong at `nextthink = g->time + 0.1` (10 Hz), each calls `W_FireLightning()` and re-arms `attack_finished = g->time + 0.2`; on `!button0` they bail to `player_run_think`. Loop sound `weapons/lhit.wav` is throttled via `self->v.t_width < g->time` (`weapons.c:956-959`). Per-tick cells drain: `self->v.currentammo = self->v.ammo_cells = self->v.ammo_cells - 1` (`weapons.c:961`).
- **Fire/oil API** (declared `sim/sim.h`, defined `sim/sim_fire.c`):
  - `int Fire_PourOil(edict_t *player)` — eye→2048 forward floor-trace (`SV_Traceline` nomonsters=1) + `Fire_AddOil` at the endpoint; returns 1 if it deposited. `Fire_AddOil` stamps a `DECAL_OIL` and **coats every `takedamage` edict within the patch radius** (`s_coated_until`), so spraying near a monster coats it.
  - `void Fire_IgniteMaybeCoated(edict_t *e, float base_secs, float dps, edict_t *igniter)` — sets `e` burning; if `e` is oil-coated, burns `OIL_COAT_BURN_SECS` (8 s) instead of `base_secs`. The burn applies DOT and emits `STIM_FIRE` (AI panic/avoid) automatically.
  - `int Fire_LightOilNear(const vec3_t pos, float reach)` — lights every unlit oil patch whose `(radius+reach)` sphere contains `pos`; each lit patch cascades.
- **Gust cone reference** (`abilities.c::gust_fire`, `:225-370`): walks `for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e))`, skips `client`/`MOVETYPE_PUSH`/`MOVETYPE_NONE`, computes `to = e->v.origin - eye`, `d = vlen(to)`, rejects `d > range || d < 1`, cone test `dot(to/d, forward) >= cos(cone_deg·π/180)`, LOS via `eng->SV_Traceline(eye, e->v.origin, 1, client)` accepting only `trace_fraction == 1 || trace_ent == e`.
- `void (*SV_Fire)(vec3_t origin, vec3_t dir, float count)` — spawns rising fire-blob particles. Existing call (`sim_fire.c:475`): `vec3_t up = {0,0,12}; eng->SV_Fire(org, up, 4.0f);`.
- Cvars register via `eng->Cvar_Register("name", "default")` (idempotent — the dedup fix in `hotreload.c` preserves existing values across re-registration, so registering in the per-level `W_Precache` is safe). `eng->Cvar_VariableValue("name")` reads.
- `player_run`, `W_BestWeapon`, `W_SetCurrentAmmo` are non-static (called cross-file) → safe to forward-`extern` from `weapons_fire.c`.
- Frame constants (`player.c`): `FR_SHOTATT1 = 113` (generic shooting stance), `FR_LIGHT1 = 105`. View models `v_light.mdl`/`v_rock.mdl` are precached every map (`world.c:187,201`) and exist on disk (`id1/progs/`).

---

## File structure

| File | Create/Modify | Responsibility |
|---|---|---|
| `sdlquake/game/weapons_fire.c` | **Create** | The F3 weapon module: cvar init, fire entrypoints, held-fire think loops, the flamethrower cone, the grant cheat, and (Task 5) pickup spawn functions. |
| `sdlquake/game/weapons_fire.h` | **Create** | Declarations consumed by `weapons.c`/`weapons_phase6.c`/`spawn.c`. |
| `sdlquake/game/game_defs.h` | Modify | Add `IT2_OILGUN`/`IT2_FLAMETHROWER` bits. |
| `build.zig` | Modify (`:418` list) | Compile `weapons_fire.c` into the game DLL. |
| `sdlquake/game/weapons.c` | Modify | `ImpulseCommands`: extend Phase 6 range to 41 + add impulse 212; `W_Precache`: `WeaponsFire_Init()` + new sound precache. |
| `sdlquake/game/weapons_phase6.c` | Modify | Add the two weapons to `Phase6_ChangeWeapon`, `W_Attack_Phase6`, `W_SetCurrentAmmo_Phase6`, `Phase6_CheatGiveAll`. |
| `sdlquake/game/items.c` | Modify (Task 5) | `weapon_touch` branches for the pickup classnames. |
| `sdlquake/game/spawn.c` | Modify (Task 5) | Register `weapon_oilgun`/`weapon_flamethrower` in `s_spawns[]`. |
| `CLAUDE.md`, spec, memory | Modify (Task 6) | Document F3. |

---

## Task 1: Scaffold — selection, view models, grant cheat (no firing yet)

Goal: after this task you can grant the weapons (`impulse 212`), select each (`impulse 40`/`41`), and see the correct `.mdl` view model with cells as ammo. Firing is a no-op stub.

**Files:**
- Create: `sdlquake/game/weapons_fire.h`
- Create: `sdlquake/game/weapons_fire.c`
- Modify: `sdlquake/game/game_defs.h` (after `IT2_WOLF_CHAINGUN`, `game_defs.h:103`)
- Modify: `build.zig` (game source list, `build.zig:431`)
- Modify: `sdlquake/game/weapons.c` (`ImpulseCommands` `:1660`; `W_Precache` `:289`)
- Modify: `sdlquake/game/weapons_phase6.c` (`Phase6_ChangeWeapon`, `W_Attack_Phase6`, `W_SetCurrentAmmo_Phase6`, `Phase6_CheatGiveAll`)

- [ ] **Step 1: Add the item bits to `game_defs.h`**

After the line `#define IT2_WOLF_CHAINGUN (1 << 9)` (`game_defs.h:103`), add:

```c
// M8 / F3 fire weapons (continue the items2 parallel roster).
#define IT2_OILGUN          (1 << 10)
#define IT2_FLAMETHROWER    (1 << 11)
```

- [ ] **Step 2: Create `sdlquake/game/weapons_fire.h`**

```c
#ifndef WEAPONS_FIRE_H
#define WEAPONS_FIRE_H

#include "game_api.h"

// M8 / F3 fire weapons: oil gun + flamethrower, dispatched through the Phase 6
// weapon2 selector (IT2_OILGUN / IT2_FLAMETHROWER). All logic is DLL-side and
// uses only existing engine + fire/oil API -- no ABI bump (GAME_API_VERSION 36).

void WeaponsFire_Init(void);        // register cvars; call once from W_Precache

void W_FireOilGun(void);            // dispatched from W_Attack_Phase6 (weapon2 == IT2_OILGUN)
void W_FireFlamethrower(void);      // dispatched from W_Attack_Phase6 (weapon2 == IT2_FLAMETHROWER)

void Fire_GiveWeapons(edict_t *self);   // impulse 212 cheat: grant both + cells, select oil gun

// World pickups (Task 5) -- registered in spawn.c's s_spawns[] table.
void spawn_weapon_oilgun(edict_t *e);
void spawn_weapon_flamethrower(edict_t *e);

#endif // WEAPONS_FIRE_H
```

- [ ] **Step 3: Create `sdlquake/game/weapons_fire.c` (scaffold)**

Write the file with cvar init, the grant cheat, and stub fire functions. The real firing arrives in Tasks 2–4.

```c
// M8 / F3 fire weapons -- oil gun + flamethrower.
// Built on the Phase 6 weapon2 selector; held-fire cadence modeled on the
// lightning gun (player_light1/2_think, weapons.c:1485 + player.c:294-318).
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "weapons_fire.h"
#include "weapons_phase6.h"
#include "sim/sim.h"

extern engine_api_t  *eng;
extern game_globals_t *g;

// Non-static game functions defined elsewhere (called cross-file already).
extern void  player_run(edict_t *self);
extern float W_BestWeapon(void);
extern void  W_SetCurrentAmmo(void);

#define FR_FIRE_BODY 113   // FR_SHOTATT1 -- generic shooting stance (player.c:39)

void WeaponsFire_Init(void) {
    // Flamethrower cone + ignition.
    eng->Cvar_Register("fire_flame_range", "220");   // cone length (units)
    eng->Cvar_Register("fire_flame_cone",  "25");    // cone half-angle (degrees)
    eng->Cvar_Register("fire_flame_secs",  "3");     // ignite duration on a direct hit
    eng->Cvar_Register("fire_flame_tick",  "0.1");   // think interval = fire/drain rate
    eng->Cvar_Register("fire_flame_cost",  "1");     // cells drained per tick
    // Oil gun.
    eng->Cvar_Register("fire_oilgun_tick", "0.12");  // think interval
    eng->Cvar_Register("fire_oilgun_cost", "1");     // cells drained per deposit
    // (Flamethrower ignite DPS reuses the existing fire_dps cvar.)
}

// impulse 212: grant both fire weapons, top up cells, select the oil gun.
void Fire_GiveWeapons(edict_t *self) {
    self->v.items2 = (float)((int)self->v.items2 | IT2_OILGUN | IT2_FLAMETHROWER);
    if (self->v.ammo_cells < 200) self->v.ammo_cells = 200;
    self->v.weapon  = 0;
    self->v.weapon2 = (float)IT2_OILGUN;
    W_SetCurrentAmmo();
    eng->Con_Print("fire: granted oil gun + flamethrower (200 cells)\n");
}

// --- Fire entrypoints (stubs until Tasks 2-4) ---------------------------------
void W_FireOilGun(void) {
    edict_t *self = g->self;
    self->v.attack_finished = g->time + 0.2f;
}

void W_FireFlamethrower(void) {
    edict_t *self = g->self;
    self->v.attack_finished = g->time + 0.2f;
}
```

- [ ] **Step 4: Register the new source file in `build.zig`**

In the game-DLL source list, immediately after the line `"sdlquake/game/player_phase6.c",` (`build.zig:432`), add:

```zig
            "sdlquake/game/weapons_fire.c",
```

- [ ] **Step 5: Wire selection + cvar init + sound precache in `weapons.c`**

In `weapons.c`, add the include near the other game includes at the top of the file (after the existing `#include` block, e.g. after the line that includes `"weapons_phase6.h"` if present, otherwise after `#include "game_defs.h"`):

```c
#include "weapons_fire.h"
```

In `ImpulseCommands` (`weapons.c:1660`), change the Phase 6 range from `<= 39` to `<= 41` and add the grant cheat. Replace:

```c
    if (imp >= 30 && imp <= 39) Phase6_ChangeWeapon(imp);   // Wolf3D + Doom1 roster
```

with:

```c
    if (imp >= 30 && imp <= 41) Phase6_ChangeWeapon(imp);   // Wolf3D + Doom1 roster + F3 fire weapons (40,41)
```

and, next to the other fire-debug impulses (after the `if (imp == 211) Fire_OilTraced(self);` line, `weapons.c:1663`), add:

```c
    if (imp == 212) Fire_GiveWeapons(self);    // M8/F3: grant fire weapons + cells
```

In `W_Precache` (`weapons.c:289`), register the cvars once and precache the one new sound. After the existing `eng->PrecacheSound("weapons/shotgn2.wav");` line and before `Phase6_PrecacheCommon();`, add:

```c
    eng->PrecacheSound("ambience/fire1.wav");   // flamethrower loop (M8/F3)
    WeaponsFire_Init();
```

(The oil-gun spray sound `misc/water1.wav`, the empty-click `weapons/guncock.wav`, and the flamethrower spin-up `weapons/lstart.wav` are already precached in `world.c`/`W_Precache`.)

- [ ] **Step 6: Add the two weapons to the Phase 6 dispatch in `weapons_phase6.c`**

In `Phase6_ChangeWeapon` (`weapons_phase6.c:833`), add two cases to the switch (after `case 39: flag = IT2_WOLF_CHAINGUN; break;`):

```c
        case 40: flag = IT2_OILGUN;       break;
        case 41: flag = IT2_FLAMETHROWER; break;
```

In `W_Attack_Phase6` (`weapons_phase6.c:734`), add two cases (after `case IT2_WOLF_CHAINGUN: W_FirePhase6_WolfChaingun(); break;`):

```c
        case IT2_OILGUN:       W_FireOilGun();       break;
        case IT2_FLAMETHROWER: W_FireFlamethrower(); break;
```

In `W_SetCurrentAmmo_Phase6` (`weapons_phase6.c:751`), add two cases before `default:` (these set the **`.mdl`** view models and cells ammo):

```c
        case IT2_OILGUN:
            self->v.weaponmodel = "progs/v_rock.mdl";    // grenade launcher model = oil sprayer
            self->v.currentammo = self->v.ammo_cells;
            break;
        case IT2_FLAMETHROWER:
            self->v.weaponmodel = "progs/v_light.mdl";   // lightning gun model = flamethrower
            self->v.currentammo = self->v.ammo_cells;
            break;
```

In `Phase6_CheatGiveAll` (`weapons_phase6.c:861`), add the two bits to the `items2` mask so `impulse 100` also grants them, and ensure cells are topped up. Change the `self->v.items2 = (float)( ... )` assignment to include:

```c
        IT2_WOLF_KNIFE | IT2_WOLF_PISTOL | IT2_WOLF_MACHINEGUN | IT2_WOLF_CHAINGUN |
        IT2_OILGUN | IT2_FLAMETHROWER
```

and after the existing ammo top-ups add:

```c
    if (self->v.ammo_cells   < 200) self->v.ammo_cells   = 200;
```

At the top of `weapons_phase6.c`, ensure `#include "weapons_fire.h"` is present (add it after the existing includes if missing — needed for `W_FireOilGun`/`W_FireFlamethrower`).

- [ ] **Step 7: Build (full) and restart**

Run: `zig build`
Expected: compiles clean (new `weapons_fire.c` linked into `game.dll`; no ABI change — `GAME_API_VERSION` stays 36).

If a previous instance is running, stop it. Start the verification instance:
`zig build run -- --hot-reload +map e1m1 --mcp-http 9876`
Expected: game launches to e1m1, MCP listening on 9876.

- [ ] **Step 8: Verify selection + view models via MCP**

Drive the running instance (reuse the `/tmp/f2_dbg*.py` SSE-client pattern, or issue these as `console_exec` calls):

1. `console_exec impulse 212` → console prints `fire: granted oil gun + flamethrower (200 cells)`; player is now holding the oil gun.
2. `wait_frames 3`, `screenshot` → the **grenade-launcher** view model (`v_rock.mdl`) is visible in the bottom-right; the HUD ammo count shows cells (200).
3. `console_exec impulse 41`, `wait_frames 3`, `screenshot` → the view model switches to the **lightning gun** (`v_light.mdl`) — this is the flamethrower.
4. `console_exec impulse 40`, `wait_frames 3` → back to the oil gun (`v_rock.mdl`).
5. Hold +attack briefly (`console_exec +attack`, `wait_frames 5`, `console_exec -attack`) → no crash, nothing fires yet (stub).

Expected: correct view models per selection, cells shown as ammo, no errors in `console_tail`. (Firing does nothing — that's Tasks 2–4.)

- [ ] **Step 9: Commit**

```bash
git add sdlquake/game/weapons_fire.c sdlquake/game/weapons_fire.h \
        sdlquake/game/game_defs.h build.zig sdlquake/game/weapons.c \
        sdlquake/game/weapons_phase6.c
git commit -m "feat(fire): M8/F3 scaffold -- oil gun + flamethrower selection, view models, grant cheat

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Oil gun firing

Goal: holding +attack with the oil gun selected paints oil on the floor (reusing the proven `Fire_PourOil` path), drains cells, and plays a spray sound. Sweeping the view lays a trail; monsters standing in the spray get oil-coated.

**Files:**
- Modify: `sdlquake/game/weapons_fire.c` (replace the `W_FireOilGun` stub; add helpers + think loop)

- [ ] **Step 1: Add the AI sound-stim helper + oil-gun fire code**

In `weapons_fire.c`, **replace** the stub `W_FireOilGun` (from Task 1, Step 3) with the following block. Place the helper `weaponsfire_sound_stim` and the static functions above `W_FireOilGun`:

```c
// Emit a STIM_SOUND so the immersive-sim AI hears the player firing
// (mirrors weapons.c::emit_weapon_sound, which is static to that file).
static void weaponsfire_sound_stim(edict_t *shooter, float intensity) {
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_SOUND;
    s.origin[0]    = shooter->v.origin[0];
    s.origin[1]    = shooter->v.origin[1];
    s.origin[2]    = shooter->v.origin[2];
    s.intensity    = intensity;
    s.source_edict = eng->ED_GetNum(shooter);
    Stim_Emit(&s);
}

// One oil-gun tick. Returns 0 when out of fuel (caller ends the loop).
static int OilGun_DoFire(edict_t *self) {
    int cost = (int)eng->Cvar_VariableValue("fire_oilgun_cost");
    if (self->v.ammo_cells < cost) {
        self->v.weapon2 = 0;                  // out of fuel: drop to a stock weapon
        self->v.weapon  = W_BestWeapon();
        W_SetCurrentAmmo();
        return 0;
    }

    // Deposit oil at the crosshair floor-trace. Fire_PourOil also coats any
    // monster standing in the fresh patch (Fire_AddOil's coat loop). Only spend
    // fuel when oil actually lands (aiming at the sky deposits nothing).
    if (Fire_PourOil(self)) {
        self->v.ammo_cells -= cost;
        self->v.currentammo = self->v.ammo_cells;
    }

    if (self->v.t_width < g->time) {          // throttle the looping spray sound
        eng->SV_StartSound(self, CHAN_WEAPON, "misc/water1.wav", 1, ATTN_NORM);
        self->v.t_width = g->time + 0.3f;
    }
    weaponsfire_sound_stim(self, 0.4f);
    return 1;
}

// Self-perpetuating held-fire loop (modeled on player_light1/2_think). Runs
// while +attack is held and the oil gun stays selected.
static void oilgun_think(edict_t *self) {
    g->self = self;
    if (!self->v.button0 || (int)self->v.weapon2 != IT2_OILGUN) {
        player_run(self);
        return;
    }
    self->v.frame = FR_FIRE_BODY;
    self->v.weaponframe++;
    if (self->v.weaponframe > 4) self->v.weaponframe = 1;
    if (!OilGun_DoFire(self)) {               // out of fuel -> stop
        player_run(self);
        return;
    }
    self->v.nextthink       = g->time + eng->Cvar_VariableValue("fire_oilgun_tick");
    self->v.think           = oilgun_think;
    self->v.attack_finished = g->time + 0.2f; // keep W_WeaponFrame from re-entering W_Attack
}

void W_FireOilGun(void) {
    edict_t *self = g->self;
    if (self->v.ammo_cells < 1) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
        self->v.attack_finished = g->time + 0.5f;
        return;
    }
    self->v.attack_finished = g->time + 0.1f; // hand off to the think loop
    oilgun_think(self);
}
```

- [ ] **Step 2: Build the DLL**

Run: `zig build game`
Expected: compiles clean. A `--hot-reload` instance reloads `game.dll` within ~1 s; otherwise run `zig build run -- --hot-reload +map e1m1 --mcp-http 9876`.

- [ ] **Step 3: Verify oil deposition via MCP**

1. `console_exec impulse 212` (grant + select oil gun), `wait_frames 2`.
2. Record baseline: `get_cvar fire_oil_count` (should be 0 on a fresh map) and player cells (`get_player_state` or watch the HUD).
3. Look down at the floor: `get_player_state` to read position; `teleport` to the same position with angles pitch ~25° (looking down-forward) if needed so the spray lands on visible floor.
4. `console_exec +attack`, `wait_frames 8`, `console_exec -attack`.
5. `wait_frames 2`, `get_cvar fire_oil_count` → **count > 0** (oil patches deposited).
6. `screenshot` → a dark oil decal stain is visible on the floor where you sprayed; the cells count dropped.
7. Sweep test: `+attack`, then `teleport` rotating the yaw a few degrees across two or three steps while held, `-attack` → `fire_oil_count` climbs further and the screenshot shows an elongated trail (merge logic keeps a stationary spray as one patch, a swept spray as a trail).

Expected: `fire_oil_count` increases, cells drain, oil decals render on the floor. If `fire_oil_count` stays 0, check `console_tail` and confirm `Fire_PourOil` is hitting a surface (aim at a floor, not a distant skybox).

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/weapons_fire.c
git commit -m "feat(fire): M8/F3 oil gun -- held-fire spray deposits + coats, drains cells

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Flamethrower firing — cone ignition

Goal: holding +attack with the flamethrower selected ignites every damageable edict in a short forward cone (with LOS) and lights oil patches the cone sweeps over. Burning monsters take DOT and panic/flee (existing F1 behavior); oil-coated targets flare longer.

**Files:**
- Modify: `sdlquake/game/weapons_fire.c` (replace the `W_FireFlamethrower` stub; add cone helper + think loop)

- [ ] **Step 1: Add the flamethrower fire code**

In `weapons_fire.c`, **replace** the stub `W_FireFlamethrower` (from Task 1, Step 3) with the following. The cone loop is adapted directly from `gust_fire` (`abilities.c:225-370`):

```c
// Small vector length helper (gust_fire uses its own vlen; keep this local).
static float wf_vlen(const vec3_t v) {
    return (float)sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}

// One flamethrower tick: ignite damageable edicts in a forward cone (with LOS)
// and light oil along the cone axis. Returns 0 when out of fuel.
static int Flamethrower_DoFire(edict_t *self) {
    int cost = (int)eng->Cvar_VariableValue("fire_flame_cost");
    if (self->v.ammo_cells < cost) {
        self->v.weapon2 = 0;
        self->v.weapon  = W_BestWeapon();
        W_SetCurrentAmmo();
        return 0;
    }
    self->v.ammo_cells -= cost;
    self->v.currentammo = self->v.ammo_cells;

    if (self->v.t_width < g->time) {          // throttled flame loop sound
        eng->SV_StartSound(self, CHAN_WEAPON, "ambience/fire1.wav", 1, ATTN_NORM);
        self->v.t_width = g->time + 0.5f;
    }
    weaponsfire_sound_stim(self, 0.7f);

    float range    = eng->Cvar_VariableValue("fire_flame_range");
    float cone_cos = (float)cos(eng->Cvar_VariableValue("fire_flame_cone") * 3.14159265f / 180.0f);
    float secs     = eng->Cvar_VariableValue("fire_flame_secs");
    float dps      = eng->Cvar_VariableValue("fire_dps");

    eng->MakeVectors(self->v.v_angle);
    vec3_t eye = { self->v.origin[0],
                   self->v.origin[1],
                   self->v.origin[2] + self->v.view_ofs[2] };
    vec3_t fwd = { g->v_forward[0], g->v_forward[1], g->v_forward[2] };

    // Ignite damageable edicts inside the cone (LOS-gated). Mirrors gust_fire.
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == self || !e->v.takedamage) continue;

        vec3_t to = { e->v.origin[0] - eye[0],
                      e->v.origin[1] - eye[1],
                      e->v.origin[2] - eye[2] };
        float d = wf_vlen(to);
        if (d > range || d < 1.0f) continue;

        vec3_t dirn = { to[0]/d, to[1]/d, to[2]/d };
        if (dirn[0]*fwd[0] + dirn[1]*fwd[1] + dirn[2]*fwd[2] < cone_cos) continue;

        // Don't ignite through walls.
        eng->SV_Traceline(eye, e->v.origin, 1, self);
        if (g->trace_fraction != 1.0f && g->trace_ent != e) continue;

        Fire_IgniteMaybeCoated(e, secs, dps, self);   // oil-coated targets burn longer
    }

    // Light oil patches the cone sweeps over (sample down the axis).
    for (int i = 1; i <= 4; i++) {
        float t = (float)i / 4.0f * range;
        vec3_t p = { eye[0] + fwd[0]*t, eye[1] + fwd[1]*t, eye[2] + fwd[2]*t };
        Fire_LightOilNear(p, 32.0f);
    }
    return 1;
}

static void flamethrower_think(edict_t *self) {
    g->self = self;
    if (!self->v.button0 || (int)self->v.weapon2 != IT2_FLAMETHROWER) {
        player_run(self);
        return;
    }
    self->v.effects = (float)((int)self->v.effects | EF_MUZZLEFLASH);   // muzzle glow
    self->v.frame = FR_FIRE_BODY;
    self->v.weaponframe++;
    if (self->v.weaponframe > 4) self->v.weaponframe = 1;
    if (!Flamethrower_DoFire(self)) {
        player_run(self);
        return;
    }
    self->v.nextthink       = g->time + eng->Cvar_VariableValue("fire_flame_tick");
    self->v.think           = flamethrower_think;
    self->v.attack_finished = g->time + 0.2f;
}

void W_FireFlamethrower(void) {
    edict_t *self = g->self;
    if (self->v.ammo_cells < 1) {
        eng->SV_StartSound(self, CHAN_WEAPON, "weapons/guncock.wav", 1, ATTN_NORM);
        self->v.attack_finished = g->time + 0.5f;
        return;
    }
    eng->SV_StartSound(self, CHAN_AUTO, "weapons/lstart.wav", 1, ATTN_NORM);   // spin-up, once
    self->v.attack_finished = g->time + 0.1f;
    flamethrower_think(self);
}
```

- [ ] **Step 2: Build the DLL**

Run: `zig build game`
Expected: compiles clean; hot-reload picks it up (or `zig build run -- --hot-reload +map e1m1 --mcp-http 9876`).

- [ ] **Step 3: Verify ignition via MCP**

Test A — ignite a monster:
1. `console_exec impulse 212`, then `console_exec impulse 41` (select flamethrower).
2. Find a monster: `list_entities` (e1m1 has a few; e.g. a dog/grunt) and note its number + position; or `console_exec impulse 9` won't help — instead `teleport` to face a known monster, or spawn one if a spawn impulse exists. Simplest on e1m1: walk/teleport to the first grunt near the start.
3. `set_cvar fire_noflee 1` (freeze the burning monster so it stays in frame for observation), `set_cvar sim_fire_debug 1`.
4. Aim at the monster (`teleport` with angles toward it), `console_exec +attack`, `wait_frames 6`, `console_exec -attack`.
5. `inspect_entity <num>` across a few `wait_frames` → its `health` decreases over time (burn DOT); `screenshot` → the monster is on fire (flame plume + muzzle glow).
6. `set_cvar fire_noflee 0` → confirm a burning monster panics/flees (existing F1 behavior).

Test B — light an oil trail:
1. With the oil gun (`impulse 40`), spray a short trail on the floor (`+attack`/`-attack`); confirm `fire_oil_count > 0`.
2. Switch to flamethrower (`impulse 41`), aim at the near end of the trail, `+attack` briefly.
3. `screenshot` + watch `fire_oil_count` → the oil ignites and **cascades** down the trail (patches light in sequence), then the patches burn out (count returns toward 0 after `OIL_BURN_SECS`).

Expected: cone ignites monsters in front (not through walls — test by putting a wall between you and the monster: it should NOT ignite), DOT applied, oil lights + cascades. Reset `fire_noflee 0`, `sim_fire_debug 0` when done.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/weapons_fire.c
git commit -m "feat(fire): M8/F3 flamethrower -- LOS-gated cone ignites edicts + lights oil

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Flamethrower visible flame stream

Goal: the flamethrower emits a visible flame from the muzzle along the cone each tick (not just the muzzle-flash glow), so it reads as a flamethrower even when it isn't igniting anything.

**Files:**
- Modify: `sdlquake/game/weapons_fire.c` (add an `SV_Fire` sample loop to `Flamethrower_DoFire`)

- [ ] **Step 1: Spawn flame blobs down the cone axis**

In `Flamethrower_DoFire` (Task 3), **after** the oil-lighting sample loop and **before** `return 1;`, add a flame-stream loop. `SV_Fire(origin, dir, count)` spawns rising fire-blob particles (existing use: `vec3_t up={0,0,12}; eng->SV_Fire(org, up, 4.0f);`, `sim_fire.c:475`). Sample the near half of the cone so the flame hugs the muzzle:

```c
    // Visible flame stream: fire-blobs along the near cone axis each tick.
    {
        vec3_t up = { 0.0f, 0.0f, 12.0f };
        float reach = range * 0.6f;          // flame visibly reaches ~60% of cone length
        for (int i = 1; i <= 3; i++) {
            float t = (float)i / 3.0f * reach;
            vec3_t fp = { eye[0] + fwd[0]*t,
                          eye[1] + fwd[1]*t,
                          eye[2] + fwd[2]*t };
            eng->SV_Fire(fp, up, 3.0f);
        }
    }
```

- [ ] **Step 2: Build the DLL**

Run: `zig build game`
Expected: compiles clean; hot-reload picks it up.

- [ ] **Step 3: Verify the flame is visible**

1. `console_exec impulse 212`, `console_exec impulse 41`.
2. Aim at an open area (not a monster, not oil) so the only thing you see is the weapon's own flame: `console_exec +attack`, `wait_frames 4`, `screenshot`, `wait_frames 4`, `screenshot`, `console_exec -attack`.
3. Inspect the screenshots → a stream of orange→grey fire blobs extends forward from the muzzle while held, fading when released.

Expected: a clearly visible flame jet. If it's too short/long or too sparse/dense, that's a tuning matter — note it for the user (the stream length follows `fire_flame_range`; blob density is the `i <= 3` / `count 3.0f` constants).

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/weapons_fire.c
git commit -m "feat(fire): M8/F3 flamethrower visible flame jet (SV_Fire down the cone)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: World pickups

Goal: `weapon_oilgun` / `weapon_flamethrower` entities can be placed in a map; touching one grants + selects the weapon. (No id1 map places them yet — the F6 test level will — so this is verified by manually spawning/placing one. It's the spec's F3 "pickups" deliverable and is cleanly separable.)

**Files:**
- Modify: `sdlquake/game/weapons_fire.c` (add the two `spawn_weapon_*` functions)
- Modify: `sdlquake/game/items.c` (`weapon_touch` branches)
- Modify: `sdlquake/game/spawn.c` (`s_spawns[]` registration)

- [ ] **Step 1: Add pickup spawn functions to `weapons_fire.c`**

Append to `weapons_fire.c` (these mirror `spawn_weapon_lightning`, `items.c:413`; `StartItem`/`SUB_*` are non-static game funcs — forward-`extern` them):

```c
extern void StartItem(edict_t *e);

// Reuse existing world models (no new precache asset): grenade-launcher box for
// the oil gun, lightning box for the flamethrower.
void spawn_weapon_oilgun(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_rock.mdl");
    eng->SV_SetModel(e, "progs/g_rock.mdl");
    e->v.weapon  = 0;
    e->v.netname = "Oil Gun";
    e->v.touch   = weapon_touch_fire;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}

void spawn_weapon_flamethrower(edict_t *e) {
    g->self = e;
    eng->PrecacheModel("progs/g_light.mdl");
    eng->SV_SetModel(e, "progs/g_light.mdl");
    e->v.weapon  = 0;
    e->v.netname = "Flamethrower";
    e->v.touch   = weapon_touch_fire;
    vec3_t wmin = {-16,-16,0}, wmax = {16,16,56};
    eng->SV_SetSize(e, wmin, wmax);
    StartItem(e);
}
```

These reference `weapon_touch_fire` (a dedicated touch handler) — declare it `extern` near the top of `weapons_fire.c` with the other externs:

```c
extern void weapon_touch_fire(edict_t *self, edict_t *other);
```

- [ ] **Step 2: Add the dedicated touch handler to `items.c`**

The stock `weapon_touch` (`items.c:278`) is `static` and keyed to `IT_*` weapons; the fire weapons use `items2`/`weapon2`, so give them their own non-static handler. Add to `items.c` (after `weapon_touch`, around `items.c:351`):

```c
// M8 / F3: touch handler for the fire-weapon pickups (items2/weapon2 roster).
void weapon_touch_fire(edict_t *self, edict_t *other) {
    g->self = self; g->other = other;
    if (!((int)other->v.flags & FL_CLIENT)) return;
    if (other->v.health <= 0) return;

    int flag = 0;
    const char *cn = self->v.classname;
    if (cn && strcmp(cn, "weapon_oilgun") == 0)            flag = IT2_OILGUN;
    else if (cn && strcmp(cn, "weapon_flamethrower") == 0) flag = IT2_FLAMETHROWER;
    else { eng->Host_Error("weapon_touch_fire: unknown classname"); return; }

    other->v.items2 = (float)((int)other->v.items2 | flag);
    other->v.ammo_cells += 30;
    if (other->v.ammo_cells > 100) other->v.ammo_cells = 100;

    eng->SV_SPrint(other, 0, "You got the ");
    eng->SV_SPrint(other, 0, self->v.netname);
    eng->SV_SPrint(other, 0, "\n");
    eng->SV_StartSound(other, CHAN_ITEM, "weapons/pkup.wav", 1, ATTN_NORM);
    eng->SV_StuffCmd(other, "bf\n");

    edict_t *stemp = g->self;
    g->self = other;
    g->self->v.weapon  = 0;
    g->self->v.weapon2 = (float)flag;
    W_SetCurrentAmmo();
    g->self = stemp;

    self->v.model = NULL;
    self->v.solid = SOLID_NOT;
    if (g->deathmatch == 1) { self->v.nextthink = g->time + 30; self->v.think = SUB_regen; }

    g->activator = other;
    SUB_UseTargets();
}
```

Add `#include "weapons_fire.h"` near the top of `items.c` if not already present (so the declaration matches). `IT2_OILGUN`/`IT2_FLAMETHROWER` come from `game_defs.h` (already included by `items.c`).

- [ ] **Step 3: Register the classnames in `spawn.c`**

In `spawn.c`, add `#include "weapons_fire.h"` near the includes (for the `spawn_weapon_*` declarations), then add two entries to the `s_spawns[]` table (after the `{ "weapon_lightning", spawn_weapon_lightning },` line, `spawn.c:301`):

```c
    { "weapon_oilgun",                    spawn_weapon_oilgun                   },
    { "weapon_flamethrower",              spawn_weapon_flamethrower             },
```

- [ ] **Step 4: Build the DLL**

Run: `zig build game`
Expected: compiles clean (no ABI change). Confirm `g_rock.mdl` and `g_light.mdl` exist: `ls id1/progs/g_rock.mdl id1/progs/g_light.mdl` — both should be present (lightning + grenade-launcher world models). If `g_rock.mdl` is absent, fall back to `g_light.mdl` for both.

- [ ] **Step 5: Verify a pickup grants the weapon**

There's no map placing these yet, so spawn one in front of the player. Use the in-game editor to place a `weapon_oilgun`, OR add a one-off console/impulse spawn if available, OR (simplest) place one via the editor's entity browser at the player's feet and save/reload. Then:
1. Start fresh without the cheat (`map e1m1`, do **not** `impulse 212`).
2. Walk onto the pickup → console prints `You got the Oil Gun`, pickup sound plays, the oil gun becomes active (`v_rock.mdl` shows, cells ammo).
3. `+attack` → it sprays (Task 2 behavior), confirming the grant path works end-to-end.

Expected: touch grants `items2` bit + selects the weapon. If you cannot place an entity easily, document that the pickup is wired (spawn fn + touch + s_spawns registered + builds) and will be exercised by the F6 test level, and verify the grant logic instead via `impulse 212` (which exercises the same `items2`/`weapon2`/`W_SetCurrentAmmo` path).

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/weapons_fire.c sdlquake/game/items.c sdlquake/game/spawn.c
git commit -m "feat(fire): M8/F3 weapon_oilgun + weapon_flamethrower world pickups

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Documentation + memory

Goal: record F3 in CLAUDE.md, mark the stage done in the spec, and update project memory — the same closeout F1/F2 got.

**Files:**
- Modify: `CLAUDE.md` (the `sim_fire.c` / weapons module entry)
- Modify: `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md` (status line + F3 staged-milestone row)
- Modify: `~/.claude/projects/-Users-wjbr-src-quake1-ai/memory/m8-fire-staged-build.md` (memory; outside the repo)

- [ ] **Step 1: Document F3 in CLAUDE.md**

In `CLAUDE.md`, append a short **F3** paragraph to the `sim_fire.c` module-map entry (after the F2 prose), covering: the two weapons live behind the Phase 6 `weapon2` dispatch in `weapons_fire.c`; oil gun = `v_rock.mdl` sprays via `Fire_PourOil` (coats monsters), flamethrower = `v_light.mdl` LOS-gated cone via `Fire_IgniteMaybeCoated` + `Fire_LightOilNear` + visible `SV_Fire` jet; both held-fire 10 Hz (lightning-modeled), drain shared `ammo_cells`; select `impulse 40`/`41`, grant `impulse 212`, pickups `weapon_oilgun`/`weapon_flamethrower`; cvars `fire_flame_range`/`fire_flame_cone`/`fire_flame_secs`/`fire_flame_tick`/`fire_flame_cost`/`fire_oilgun_tick`/`fire_oilgun_cost`; **no ABI bump (GAME_API_VERSION stays 36)** — all DLL-side. Read the paragraph back after writing (user reviews docs inline).

- [ ] **Step 2: Update the spec status + staged table**

In `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md`:
- Change the `**Status:**` line (`:4`) to note F3 implemented (e.g. `F1+F2+F3 implemented 2026-05-30; F4–F6 not started`).
- Optionally mark the **F3 Weapons** row in the staged-milestones table (`:267`) as done.

- [ ] **Step 3: Update project memory**

In `~/.claude/projects/-Users-wjbr-src-quake1-ai/memory/m8-fire-staged-build.md`, add an **F3 (weapons) landed** paragraph (same style as F1/F2): the `weapon2`/`weapons_fire.c` approach, `.mdl` reuse, lightning-modeled held-fire, shared cells, `impulse 212`/`40`/`41`, the cvars, no ABI bump, and the commit hashes. Update the `description:` front-matter to mention F3. Add a one-line pointer review if `MEMORY.md` needs it (it already lists the file).

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-05-30-fire-and-oil-design.md
git commit -m "docs(fire): document M8/F3 weapons (oil gun + flamethrower)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

(The memory file lives outside the repo and is not committed.)

---

## Self-review notes (author)

- **Spec coverage:** Oil gun (Task 2) ✓; flamethrower cone + DOT-via-ignite (Task 3) ✓; visible flame (Task 4) ✓; impulse selection 40/41 (Task 1) ✓; cheat grant — impulse 212 + `Phase6_CheatGiveAll` (Task 1) ✓; `weapon_*` pickups (Task 5) ✓; shared `ammo_cells` fuel — Locked decision A (Tasks 1–3) ✓; new `IT2_*` bits (Task 1) ✓. Spec items deliberately **out of F3 scope** (deferred per the staged table): player-burns + oil barrels + relightable torches + breakable props (F4); Gust-extinguish + contact-spread tuning (F5); test level `ai_t10_fire.map` + `fire_query` MCP tool (F6). The spec's "direct DOT inside the cone" is delivered as burn-DOT via `Fire_IgniteMaybeCoated` (the target ignites and takes damage over time); immediate per-tick contact damage is a tuning add-on, not built, to keep F3 within the existing documented API.
- **No placeholders:** every code step shows complete code; every verify step names exact impulses/cvars/MCP calls and the expected observation.
- **Type/name consistency:** `IT2_OILGUN`/`IT2_FLAMETHROWER`, `W_FireOilGun`/`W_FireFlamethrower`, `OilGun_DoFire`/`Flamethrower_DoFire`, `oilgun_think`/`flamethrower_think`, `weaponsfire_sound_stim`, `Fire_GiveWeapons`, `weapon_touch_fire`, `spawn_weapon_oilgun`/`spawn_weapon_flamethrower` are used identically across tasks. Cvar names are consistent between registration (Task 1) and reads (Tasks 2–4). Reused API signatures (`Fire_PourOil`, `Fire_IgniteMaybeCoated`, `Fire_LightOilNear`, `SV_Fire`, `SV_StartSound`) match the reconnaissance.
- **Risk to flag at execution:** view-model frame cycling assumes `v_rock.mdl`/`v_light.mdl` have ≥4 view frames (true — the grenade launcher and lightning gun animate them); the verify screenshots will catch any out-of-range frame as a visual glitch. `g_rock.mdl` existence is checked in Task 5, Step 4 with a `g_light.mdl` fallback.

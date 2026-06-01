# M8 / F4 Flammables Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the four F4 "flammables" interactions to the M8 fire/oil layer — oil barrels (trail→barrel→boom), (re)lightable torches, breakable wooden props, and player-burns — all DLL-side with no engine ABI change.

**Architecture:** A new `sdlquake/game/flammables.c` houses the F4 entity logic (oil barrel, breakable props, torch extinguish/relight helpers, debug spawn impulses). The four stock flame/torch entities in `misc.c` are de-static'd into live edicts so their flame model can be hidden (extinguish) and restored (relight); the latent M5 Gust-extinguish finally becomes visible as a result. The oil barrel reuses the existing `barrel_explode` (its `T_RadiusDamage` already auto-lights any oil in the blast via `combat.c:478`), so it only needs to spill oil first. Breakables are flammable for free — they have `takedamage`+`health`+`th_die`, so the existing fire DOT burns them down to their death think. The flamethrower relights extinguished torches in its cone; lit torches ignite nearby oil; the flamethrower's backdraft ignites the player point-blank.

**Tech Stack:** C (game DLL, compiled modern C — C99 declarations OK, matching `weapons_fire.c`), Zig build, existing `engine_api_t` (GAME_API_VERSION **stays 36** — no bump), existing `sim/sim_fire.c` fire/oil API, MCP HTTP rig for verification.

---

## Background facts the implementer must know

- **GAME_API_VERSION is 36** (`game_api.h:7`). F4 adds NO ABI bump. Everything uses already-exposed engine calls (`SV_SetModel`, `SV_Particle`, `SV_StartSound`, `Lightmap_AddDelta` via `Light_AddOverride`, `MakeVectors`, `SV_Traceline`, `ED_Alloc`) and DLL-side helpers (`Fire_AddOil`, `Fire_IgniteMaybeCoated`, `T_RadiusDamage`, `barrel_explode`, `ThrowGib`, `BecomeExplosion`, `Stim_Emit`).
- **Game `.c` files compile as modern C** (not gnu89). C99 `for (edict_t *e = ...)` and mid-block declarations are fine — match `weapons_fire.c` style.
- **`SV_MakeStatic` frees the edict** (`hotreload.c:833` calls `ED_Free`). That is why the 4 flame entities currently have no live edict, and why the M5 Gust loop (`abilities.c:327`) scanning live edicts for `light_torch`/`light_flame` never matches anything on real maps. Dropping `SV_MakeStatic` makes them live and makes both extinguish *and* relight possible.
- **`T_RadiusDamage` auto-lights oil** (`combat.c:478` → `Fire_LightOilNear(inflictor->v.origin, damage)`), with a comment anticipating exactly this exploding-prop case. So the oil barrel only needs to *deposit* oil before exploding.
- **The fire DOT and burning-oil ignite have no `FL_CLIENT` filter** (`sim_fire.c:553`, `:454-461`), so the player already takes DOT and ignites when standing in burning oil, and `Fire_Frame` already sets `EF_DIMLIGHT` on any burning edict (`sim_fire.c:563`). Player-burn feedback (red flash via `T_Damage`, glow via `EF_DIMLIGHT`, pain grunts via the player pain chain) is therefore mostly free — F4 only adds the flamethrower backdraft self-ignite.
- **Oil patches** are not edicts (`oil_patch_t` pool in `sim_fire.c`). `Fire_AddOil(origin, radius, amount)` deposits one; `radius<=0`→`OIL_DEFAULT_RADIUS` (48), `amount<=0`→`OIL_DEFAULT_AMOUNT` (1). A persistent `DECAL_OIL` stain is stamped at deposit.
- **`STIM_PROP_BROKEN`** already exists in `stim_kind_t` (`sim.h:36`) and the AI already reacts to it (`sim_ai.c`, 768-unit reference). F4 is the first emitter.

## Verification rig (READ — these gotchas cost real time in F1–F3)

1. **Hot-reload DEFERS the `game.dll` swap while a map is loaded.** `zig build game` does NOT take effect in a running map. To test new DLL code, **restart the instance**: kill the PID on the MCP port, then relaunch `zig build run -- +map e1m1 --mcp-http 9876` (loads the new DLL at startup). The full `zig build run` rebuilds engine+game.
2. **MCP `wait_frames` does not reliably advance player impulse processing** between back-to-back `console_exec impulse` calls. Insert a `screenshot` call between impulses to force real frame processing.
3. `inspect_entity` takes `{"id":N}` (edict index) and returns `health`/`takedamage`; `list_entities` items carry an `id`. `get_player_state` returns `position` (not `origin`) and has no `ammo_cells` key (read cells off a HUD screenshot).
4. Debug hooks added by this plan (all MCP-friendly via `console_exec`): `impulse 213` spawn oil barrel ahead, `impulse 214` spawn breakable ahead, `impulse 215` toggle nearest torch (extinguish↔relight). Plus the existing `impulse 210` ignite-at-crosshair, `impulse 211` deposit oil, `impulse 212` grant fire weapons, and cvars `fire_oil_count` / `fire_dps` / `fire_secs`.

---

### Task 1: Oil barrel (`misc_oilbarrel`) + flammables.c scaffold

**Files:**
- Create: `sdlquake/game/flammables.h`
- Create: `sdlquake/game/flammables.c`
- Modify: `sdlquake/game/misc.c` (promote `barrel_explode` from `static` to non-static)
- Modify: `sdlquake/game/spawn.c` (forward decl + table entry for `misc_oilbarrel`)
- Modify: `sdlquake/game/weapons.c` (call `Flammables_Init()` in `W_Precache`; impulse 213 dispatch)
- Modify: `build.zig` (add `flammables.c` to the game source list)

- [ ] **Step 1: Create `sdlquake/game/flammables.h`**

```c
// M8 / F4 flammables -- oil barrels, breakable props, (re)lightable torches.
#ifndef FLAMMABLES_H
#define FLAMMABLES_H

#include "game_types.h"

void Flammables_Init(void);   // precache F4 assets + register F4 cvars

// World-pickup / map spawn functions (registered in spawn.c).
void spawn_misc_oilbarrel(edict_t *e);
void spawn_func_breakable(edict_t *e);
void spawn_misc_breakable(edict_t *e);

// Torch interaction (live-edict flame entities). Both self-check classname +
// current lit state and no-op if the edict is not an (extinguished/lit) torch.
void Torch_Extinguish(edict_t *t, edict_t *source);   // hide flame, darken, stim
void Torch_Relight(edict_t *t, edict_t *source);       // restore flame, brighten, stim

// Debug impulse hooks (wired in weapons.c ImpulseCommands).
void Flammables_DebugSpawnBarrel(edict_t *player);     // impulse 213
void Flammables_DebugSpawnBreakable(edict_t *player);  // impulse 214
void Flammables_DebugToggleNearestTorch(edict_t *player); // impulse 215

#endif // FLAMMABLES_H
```

- [ ] **Step 2: Create `sdlquake/game/flammables.c` with the scaffold + oil barrel**

```c
// M8 / F4 flammables -- oil barrels, breakable props, (re)lightable torches.
// All DLL-side; no engine ABI change (GAME_API_VERSION stays 36).
#include <string.h>
#include <math.h>

#include "game_defs.h"
#include "game_api.h"
#include "game_types.h"
#include "flammables.h"
#include "sim/sim.h"

extern engine_api_t   *eng;
extern game_globals_t *g;

// Defined in misc.c (de-static'd by this task) and combat.c.
extern void barrel_explode(edict_t *self);
extern void T_RadiusDamage(edict_t *inflictor, edict_t *attacker, float damage, edict_t *ignore);

void Flammables_Init(void) {
    // Reused world models -- precache so map placement and debug spawns work.
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    eng->PrecacheSound("weapons/ax1.wav");   // breakable "crack" placeholder
}

// ---------------------------------------------------------------------------
// Oil barrel: a misc_explobox whose death spills oil first. barrel_explode's
// T_RadiusDamage(160) then lights the spill (combat.c:478) -> burning pool.
// ---------------------------------------------------------------------------
static void oilbarrel_explode(edict_t *self) {
    g->self = self;
    vec3_t c = { self->v.origin[0], self->v.origin[1], self->v.origin[2] };
    Fire_AddOil(c, 72.0f, 2.0f);                          // central pool
    for (int i = 0; i < 6; i++) {                         // surrounding ring
        float a = (float)i / 6.0f * 6.2831853f;
        vec3_t p = { c[0] + (float)cos(a) * 80.0f,
                     c[1] + (float)sin(a) * 80.0f,
                     c[2] };
        Fire_AddOil(p, 48.0f, 1.0f);
    }
    barrel_explode(self);   // flips takedamage/classname, T_RadiusDamage(160)
                            // (lights the oil), boom sound, TE_EXPLOSION, BecomeExplosion
}

void spawn_misc_oilbarrel(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->SV_SetModel(e, "maps/b_explob.bsp");
    eng->PrecacheSound("weapons/r_exp3.wav");
    e->v.health     = 20;
    e->v.th_die     = oilbarrel_explode;
    e->v.takedamage = DAMAGE_AIM;
    e->v.origin[2] += 2;
    float oldz = e->v.origin[2];
    eng->SV_DropToFloor(e);
    if (oldz - e->v.origin[2] > 250) {
        eng->Con_DPrintf("item fell out of level\n");
        eng->ED_Free(e);
    }
}

// ---------------------------------------------------------------------------
// Debug spawn (impulse 213): drop an oil barrel ~96u in front of the player.
// ---------------------------------------------------------------------------
void Flammables_DebugSpawnBarrel(edict_t *player) {
    eng->MakeVectors(player->v.v_angle);
    edict_t *e = eng->ED_Alloc();
    e->v.origin[0] = player->v.origin[0] + g->v_forward[0] * 96.0f;
    e->v.origin[1] = player->v.origin[1] + g->v_forward[1] * 96.0f;
    e->v.origin[2] = player->v.origin[2];
    e->v.classname = "misc_oilbarrel";
    spawn_misc_oilbarrel(e);
    eng->Con_Print("fire: spawned oil barrel ahead\n");
}
```

> NOTE: `func_breakable`/`misc_breakable`/`Torch_*`/the other debug hooks are added in later tasks. To keep this task building, the `.h` declares them now but they are defined in Tasks 2–3. **C allows declared-but-undefined functions as long as nothing calls them yet** — spawn.c only references `spawn_misc_oilbarrel` this task, and weapons.c only calls `Flammables_Init` + `Flammables_DebugSpawnBarrel`. Do NOT add spawn.c/impulse wiring for breakables/torches until their tasks.

- [ ] **Step 3: Promote `barrel_explode` to non-static in `misc.c`**

In `sdlquake/game/misc.c`, change the definition (currently `static void barrel_explode(edict_t *self)`, around line 226):

```c
void barrel_explode(edict_t *self) {
```

(Remove the `static`. No other change to its body. It is now externally linkable for `oilbarrel_explode`.)

- [ ] **Step 4: Register `misc_oilbarrel` in `spawn.c`**

Add a forward declaration next to the other `spawn_misc_*` decls (near `spawn.c:108`):

```c
void spawn_misc_oilbarrel(edict_t *e);
```

Add a table entry in `s_spawns[]` next to `misc_explobox` (near `spawn.c:228`):

```c
    { "misc_oilbarrel",               spawn_misc_oilbarrel                  },
```

- [ ] **Step 5: Wire `Flammables_Init` + impulse 213 in `weapons.c`**

Add the include near the top of `sdlquake/game/weapons.c` (with the other game includes; `weapons_fire.h` is already included there):

```c
#include "flammables.h"
```

In `W_Precache` (after the existing `WeaponsFire_Init();` at `weapons.c:304`):

```c
    Flammables_Init();
```

In `ImpulseCommands` (after the `if (imp == 212) ...` line at `weapons.c:1667`):

```c
    if (imp == 213) Flammables_DebugSpawnBarrel(self);   // M8/F4 debug: oil barrel ahead
```

- [ ] **Step 6: Add `flammables.c` to `build.zig`**

In `build.zig`, in the game source list (next to `"sdlquake/game/weapons_fire.c",`, ~line 432):

```zig
        "sdlquake/game/flammables.c",
```

- [ ] **Step 7: Build**

Run: `zig build game`
Expected: compiles clean (game.dll rebuilt). If `barrel_explode` undefined-reference: confirm Step 3 removed `static`. If `Fire_AddOil` undefined: confirm `#include "sim/sim.h"`.

- [ ] **Step 8: Verify in-game (restart the instance — see rig note 1)**

Kill any instance on 9876, then `zig build run -- +map e1m1 --mcp-http 9876`. Via MCP:
- `console_exec "impulse 213"` → expect console "fire: spawned oil barrel ahead"; `screenshot` should show a barrel ~96u ahead.
- Aim at it, `console_exec "impulse 210"` (ignite at crosshair) or shoot it; OR `set_cvar fire_oil_count` read before/after.
- Better end-to-end: `console_exec "impulse 213"`, `screenshot`, then fire a rocket / `impulse 210` at the barrel → expect explosion + a `fire_oil_count` jump (oil spilled) + the spilled oil visibly burning (orange `SV_Fire` plume) in a follow-up `screenshot`.
Expected: barrel explodes, leaves a ring of burning oil. Note the `fire_oil_count` delta in your report.

- [ ] **Step 9: Commit**

```bash
git add sdlquake/game/flammables.c sdlquake/game/flammables.h sdlquake/game/misc.c sdlquake/game/spawn.c sdlquake/game/weapons.c build.zig
git commit -m "$(cat <<'EOF'
feat(fire): M8/F4 oil barrel (misc_oilbarrel) + flammables.c scaffold

misc_oilbarrel mirrors misc_explobox but spills a ring of oil before
exploding; barrel_explode's T_RadiusDamage(160) auto-lights the spill
(combat.c:478) into a burning pool -> trail->barrel->boom loop. New
flammables.c hub + Flammables_Init precache; debug impulse 213 drops a
barrel ahead of the player. No ABI change (GAME_API_VERSION stays 36).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Breakable props (`func_breakable` brush + `misc_breakable` point)

**Files:**
- Modify: `sdlquake/game/flammables.c` (add `breakable_die`, both spawn fns, debug spawn)
- Modify: `sdlquake/game/spawn.c` (forward decls + table entries)
- Modify: `sdlquake/game/weapons.c` (impulse 214 dispatch)

- [ ] **Step 1: Add breakable logic to `flammables.c`**

Add after the oil-barrel section:

```c
// ---------------------------------------------------------------------------
// Breakable props. Flammability is automatic: takedamage + health + th_die
// means the fire DOT (sim_fire.c) burns them down to breakable_die. They also
// break to bullets/axe. health is tuned so fire_dps(8) consumes one in ~3s.
// ---------------------------------------------------------------------------
static void breakable_die(edict_t *self) {
    g->self = self;
    self->v.takedamage = DAMAGE_NO;
    self->v.solid      = SOLID_NOT;

    // The AI already understands this stimulus (sim_ai.c, 768u reference).
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_PROP_BROKEN;
    s.origin[0]    = self->v.origin[0];
    s.origin[1]    = self->v.origin[1];
    s.origin[2]    = self->v.origin[2];
    s.intensity    = 0.8f;
    s.source_edict = eng->ED_GetNum(self);
    Stim_Emit(&s);

    // Splinter puff (brown particle band) + a wood-ish crack. No wood-chunk
    // model ships in shareware, so particles stand in for debris (MVP).
    vec3_t up = { 0.0f, 0.0f, 0.0f };
    eng->SV_Particle(self->v.origin, up, 116, 64);
    eng->SV_StartSound(self, CHAN_BODY, "weapons/ax1.wav", 1, ATTN_NORM);

    eng->ED_Free(self);
}

// Brush form (map-authored crates/walls): model comes from the .map ("*N").
void spawn_func_breakable(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BSP;
    e->v.movetype = MOVETYPE_PUSH;
    eng->SV_SetModel(e, e->v.model);     // brush model from the map
    if (e->v.health <= 0) e->v.health = 40;
    e->v.takedamage = DAMAGE_AIM;
    e->v.th_die     = breakable_die;
}

// Point form (spawn-anywhere debug/test): reuse the box bmodel.
void spawn_misc_breakable(edict_t *e) {
    g->self = e;
    e->v.solid    = SOLID_BBOX;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("maps/b_explob.bsp");
    eng->SV_SetModel(e, "maps/b_explob.bsp");
    if (e->v.health <= 0) e->v.health = 25;   // ~3s under fire_dps 8
    e->v.takedamage = DAMAGE_AIM;
    e->v.th_die     = breakable_die;
    e->v.origin[2] += 2;
    eng->SV_DropToFloor(e);
}

// Debug spawn (impulse 214): drop a breakable ~80u in front of the player.
void Flammables_DebugSpawnBreakable(edict_t *player) {
    eng->MakeVectors(player->v.v_angle);
    edict_t *e = eng->ED_Alloc();
    e->v.origin[0] = player->v.origin[0] + g->v_forward[0] * 80.0f;
    e->v.origin[1] = player->v.origin[1] + g->v_forward[1] * 80.0f;
    e->v.origin[2] = player->v.origin[2];
    e->v.classname = "misc_breakable";
    e->v.health    = 25;
    spawn_misc_breakable(e);
    eng->Con_Print("fire: spawned breakable ahead\n");
}
```

- [ ] **Step 2: Register both classnames in `spawn.c`**

Forward decls (near the other flammables decl from Task 1):

```c
void spawn_func_breakable(edict_t *e);
void spawn_misc_breakable(edict_t *e);
```

Table entries in `s_spawns[]` (near the `misc_oilbarrel` entry):

```c
    { "func_breakable",               spawn_func_breakable                  },
    { "misc_breakable",               spawn_misc_breakable                  },
```

- [ ] **Step 3: Wire impulse 214 in `weapons.c`**

In `ImpulseCommands` after the `impulse 213` line:

```c
    if (imp == 214) Flammables_DebugSpawnBreakable(self);   // M8/F4 debug: breakable ahead
```

- [ ] **Step 4: Build**

Run: `zig build game`
Expected: clean.

- [ ] **Step 5: Verify in-game (restart the instance)**

Relaunch on 9876. Via MCP:
- `console_exec "impulse 214"` → "fire: spawned breakable ahead"; `screenshot` shows a box ahead.
- Find its edict id via `list_entities` (classname `misc_breakable`); read `health` via `inspect_entity {"id":N}` (expect 25).
- Ignite it: aim at it + `console_exec "impulse 210"` (or set `fire_ignite_num N`). `wait_frames` ~25; re-`inspect_entity` → health should drop each tick; after ~3s the entity is freed (inspect errors / gone from `list_entities`).
- A burning box should glow (`EF_DIMLIGHT`) and throw a flame plume; on break, a brown particle puff + `ax1.wav`.
Expected: breakable catches fire, burns down over ~3s, vanishes in a puff. (Headless: `STIM_PROP_BROKEN` is emitted — verify via AI reaction or a console note if you add one for debugging.)

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/flammables.c sdlquake/game/spawn.c sdlquake/game/weapons.c
git commit -m "$(cat <<'EOF'
feat(fire): M8/F4 breakable props (func_breakable + misc_breakable)

func_breakable (brush, map-authored) and misc_breakable (point, box
bmodel, spawn-anywhere) both get takedamage+health+th_die, so the
existing fire DOT burns them down to breakable_die with no extra wiring.
On death: STIM_PROP_BROKEN (AI already reacts), a brown splinter puff,
and ax1.wav. Debug impulse 214 drops one ahead of the player. First
emitter of the long-defined STIM_PROP_BROKEN.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: De-static torches into live edicts + extinguish/relight + Gust wire

**Files:**
- Modify: `sdlquake/game/misc.c` (drop `SV_MakeStatic` on the 4 flame entities; make them live, non-solid)
- Modify: `sdlquake/game/flammables.c` (add `is_flammable_light`, `Torch_Extinguish`, `Torch_Relight`, `Flammables_DebugToggleNearestTorch`)
- Modify: `sdlquake/game/abilities.c` (replace the inline Gust extinguish block with `Torch_Extinguish`)
- Modify: `sdlquake/game/weapons.c` (impulse 215 dispatch)

- [ ] **Step 1: De-static the 4 flame entities in `misc.c`**

For each of `spawn_light_torch_small_walltorch`, `spawn_light_flame_large_yellow`, `spawn_light_flame_small_yellow`, `spawn_light_flame_small_white` (`misc.c:113-140`): **remove the `eng->SV_MakeStatic(e);` call** and instead make the entity a live, non-blocking prop. Example for `spawn_light_torch_small_walltorch`:

```c
void spawn_light_torch_small_walltorch(edict_t *e) {
    e->v.solid    = SOLID_NOT;
    e->v.movetype = MOVETYPE_NONE;
    eng->PrecacheModel("progs/flame.mdl");
    eng->SV_SetModel(e, "progs/flame.mdl");   // SV_SetModel links the edict
    fire_ambient(e);
    // NOT SV_MakeStatic: stays a live edict so the flame can be hidden
    // (Gust) and restored (fire). "lit" state == (modelindex != 0).
}
```

Apply the same change to the other three (preserve `e->v.frame = 1;` in `spawn_light_flame_large_yellow`). `fire_ambient`'s baked `SV_AmbientSound` is kept as-is.

> KNOWN MVP LIMITATION (document, do not fix here): the fire crackle is a *baked* ambient sound (`SV_AmbientSound` writes the signon buffer); it cannot be stopped at runtime without engine work, so an extinguished torch still crackles. The visible flame + lightmap toggle is the headline. Leave the audio.
>
> KNOWN RISK (document): live torches consume edict slots (MAX_EDICTS 600). Episode-1 maps have ample headroom; very torch-dense maps could approach the cap. If a specific map errors with no free edicts, that is the mitigation point (follow-up).

- [ ] **Step 2: Add torch helpers to `flammables.c`**

Add near the top (after the externs):

```c
static int is_flammable_light(const char *cn) {
    return cn && (strncmp(cn, "light_torch", 11) == 0 ||
                  strncmp(cn, "light_flame", 11) == 0);
}

// Emit the "lights changed" stimulus the AI sense filter already consumes.
static void torch_light_stim(edict_t *t, edict_t *source) {
    stimulus_t s;
    memset(&s, 0, sizeof(s));
    s.kind         = STIM_LIGHT_CHANGE;
    s.origin[0]    = t->v.origin[0];
    s.origin[1]    = t->v.origin[1];
    s.origin[2]    = t->v.origin[2];
    s.intensity    = 0.6f;
    s.source_edict = source ? eng->ED_GetNum(source) : -1;
    Stim_Emit(&s);
}
```

Add the public helpers:

```c
// Hide the flame, darken the room (AI + renderer via Light_AddOverride),
// stim. No-op unless this is a currently-lit flammable light.
void Torch_Extinguish(edict_t *t, edict_t *source) {
    if (!t || t->free || !is_flammable_light(t->v.classname)) return;
    if ((int)t->v.modelindex == 0) return;     // already out
    t->v.modelindex = 0;                        // model string preserved
    Light_AddOverride(t->v.origin, 192.0f, -80.0f);
    torch_light_stim(t, source);
}

// Restore the flame (re-resolve modelindex from the preserved model string),
// brighten (cancels a prior -80), stim. No-op unless currently extinguished.
void Torch_Relight(edict_t *t, edict_t *source) {
    if (!t || t->free || !is_flammable_light(t->v.classname)) return;
    if ((int)t->v.modelindex != 0) return;     // already lit
    eng->SV_SetModel(t, t->v.model);            // model string never cleared
    Light_AddOverride(t->v.origin, 192.0f, 80.0f);
    torch_light_stim(t, source);
}

// Debug (impulse 215): toggle the nearest flammable light to the player.
void Flammables_DebugToggleNearestTorch(edict_t *player) {
    edict_t *best = NULL;
    float bestd = 1e18f;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (!is_flammable_light(e->v.classname)) continue;
        float dx = e->v.origin[0] - player->v.origin[0];
        float dy = e->v.origin[1] - player->v.origin[1];
        float dz = e->v.origin[2] - player->v.origin[2];
        float d = dx*dx + dy*dy + dz*dz;
        if (d < bestd) { bestd = d; best = e; }
    }
    if (!best) { eng->Con_Print("fire: no torch found\n"); return; }
    if ((int)best->v.modelindex != 0) {
        Torch_Extinguish(best, player);
        eng->Con_Print("fire: extinguished nearest torch\n");
    } else {
        Torch_Relight(best, player);
        eng->Con_Print("fire: relit nearest torch\n");
    }
}
```

- [ ] **Step 3: Replace the inline Gust extinguish with `Torch_Extinguish` in `abilities.c`**

Add the include near the top of `sdlquake/game/abilities.c`:

```c
#include "flammables.h"
```

In the Gust flammable-light loop (`abilities.c:327-362`), replace the body from the `Light_AddOverride(le->v.origin, 192.0f, -80.0f);` call through the end of the inline `Stim_Emit(&ls);` block with a single call (keep the classname/distance/cone gating above it unchanged):

```c
        Torch_Extinguish(le, client);
```

(This removes the now-duplicated `Light_AddOverride` + the local `stimulus_t ls` block. The visible result: Gust now hides the flame model in addition to darkening — the latent M5 mechanic becomes visible because torches are live edicts.)

- [ ] **Step 4: Wire impulse 215 in `weapons.c`**

In `ImpulseCommands` after the `impulse 214` line:

```c
    if (imp == 215) Flammables_DebugToggleNearestTorch(self);   // M8/F4 debug: toggle nearest torch
```

- [ ] **Step 5: Build**

Run: `zig build game`
Expected: clean. (`abilities.c` may now have an unused variable warning if the old `ls` declaration wasn't fully removed — remove it.)

- [ ] **Step 6: Verify in-game (restart the instance)**

Relaunch on 9876, `+map e1m1` (e1m1 has wall torches). Via MCP:
- Teleport near a torch (find one via `list_entities` classname `light_torch_small_walltorch` / `light_flame_*` — they now appear in the list because they're live edicts; confirm they DO appear, which itself proves de-static worked).
- `console_exec "impulse 215"` → "fire: extinguished nearest torch"; `screenshot` → the flame model is gone and the area is darker.
- `console_exec "impulse 215"` again → "fire: relit nearest torch"; `screenshot` → flame back, area brighter.
- Also confirm Gust snuffs visibly: face a torch, `console_exec "+gust"` then `-gust` (with energy; grant via the abilities cheat if needed), `screenshot` → flame gone.
Expected: torches appear as live edicts; toggle hides/restores the flame + lightmap; Gust visibly snuffs.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/game/misc.c sdlquake/game/flammables.c sdlquake/game/abilities.c sdlquake/game/weapons.c
git commit -m "$(cat <<'EOF'
feat(fire): M8/F4 de-static torches into live (re)lightable edicts

The 4 light_torch/light_flame entities drop SV_MakeStatic (which frees
the edict) and become live, SOLID_NOT props, so their flame model can be
hidden and restored. Torch_Extinguish/Torch_Relight toggle modelindex
(model string preserved -> re-resolve on relight) + a +/-80 lightmap
override + STIM_LIGHT_CHANGE. Gust now calls Torch_Extinguish, finally
making the latent M5 Gust-snuff visible. Debug impulse 215 toggles the
nearest torch. "lit" state == (modelindex != 0).

Known MVP limits (documented): baked ambient crackle persists when out;
live torches consume edict slots (fine for episode 1).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Fire relights torches + lit torch ignites oil

**Files:**
- Modify: `sdlquake/game/weapons_fire.c` (flamethrower cone relights extinguished torches)
- Modify: `sdlquake/game/sim/sim_fire.c` (lit torch ignites nearby oil in `oil_frame`)

- [ ] **Step 1: Flamethrower cone relights extinguished torches**

Add the include near the top of `sdlquake/game/weapons_fire.c`:

```c
#include "flammables.h"
```

In `Flamethrower_DoFire` (`weapons_fire.c`), the cone loop currently skips non-damageable edicts (`if (e == self || !e->v.takedamage) continue;`). Change it so torches are not skipped, and relight extinguished ones that pass the cone+LOS test. Replace that loop's filtering/action with:

```c
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == self) continue;

        vec3_t to = { e->v.origin[0] - eye[0],
                      e->v.origin[1] - eye[1],
                      e->v.origin[2] - eye[2] };
        float d = wf_vlen(to);
        if (d > range || d < 1.0f) continue;

        vec3_t dirn = { to[0]/d, to[1]/d, to[2]/d };
        if (dirn[0]*fwd[0] + dirn[1]*fwd[1] + dirn[2]*fwd[2] < cone_cos) continue;

        eng->SV_Traceline(eye, e->v.origin, 1, self);
        if (g->trace_fraction != 1.0f && g->trace_ent != e) continue;

        if (e->v.takedamage)
            Fire_IgniteMaybeCoated(e, secs, dps, self);   // oil-coated targets burn longer
        else
            Torch_Relight(e, self);                        // no-op unless an extinguished torch
    }
```

(The only change vs. the existing loop is the early filter and the `else Torch_Relight(...)` branch. The oil-axis-light and visible-jet blocks below this loop are unchanged.)

- [ ] **Step 2: Lit torch ignites nearby oil in `oil_frame`**

In `sdlquake/game/sim/sim_fire.c`, add a constant near the other oil defines (~line 39):

```c
#define TORCH_OIL_REACH      40.0f     // a lit torch lights oil within radius+this
```

In `oil_frame`, the unlit-patch branch loops edicts looking for a burning one standing in the patch (`sim_fire.c:431-437`). Extend that loop so a *lit torch* near the patch also lights it. Inside the loop, after the existing burning-edict check (which `break`s on a hit), add a torch check (`<string.h>` is already included in sim_fire.c):

```c
                const char *cn = e->v.classname;
                int lit_torch = cn &&
                    (strncmp(cn, "light_torch", 11) == 0 ||
                     strncmp(cn, "light_flame", 11) == 0) &&
                    (int)e->v.modelindex != 0;
                if (lit_torch) {
                    float tdx = e->v.origin[0] - o->origin[0];
                    float tdy = e->v.origin[1] - o->origin[1];
                    float tdz = e->v.origin[2] - o->origin[2];
                    float reach = o->radius + TORCH_OIL_REACH;
                    if (tdx*tdx + tdy*tdy + tdz*tdz <= reach*reach) {
                        oil_light_patch(o);
                        break;
                    }
                }
```

(Place this inside the same `for (edict_t *e = ...)` loop that already does the burning-edict test, so the patch is lit by either a burning edict or a nearby lit torch. Do not add a second edict loop.)

> NOTE: the burning-edict check at the top of that loop does `if (en < 0 || en >= FIRE_MAX_BURNING || !Fire_IsBurning(en)) continue;` — that `continue` would skip the torch check. Restructure so the `Fire_IsBurning` test does NOT `continue` past the torch test: compute the torch check first, OR fold both tests before any `continue`. Simplest: move the torch check above the `Fire_IsBurning` early-continue, then keep the burning-edict logic below it.

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: clean.

- [ ] **Step 4: Verify in-game (restart the instance)**

Relaunch on 9876, `+map e1m1`. Via MCP:
- **Relight via fire:** `impulse 215` (extinguish nearest torch), `screenshot` (flame gone). Grant fire weapons `impulse 212`, select flamethrower `impulse 41` (insert a `screenshot` between impulses — rig note 2). Face the extinguished torch, `console_exec "+attack"`, `wait_frames` ~10, `console_exec "-attack"`, `screenshot` → flame restored.
- **Lit torch ignites oil:** stand under a lit torch, `console_exec "impulse 211"` (deposit oil at crosshair — aim at the floor below the torch) or `impulse 213`-style; read `fire_oil_count`; within a tick the patch near the lit torch should auto-light (its plume appears) without any manual ignite.
Expected: flamethrower relights an extinguished torch; oil deposited under a lit torch ignites on its own.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/weapons_fire.c sdlquake/game/sim/sim_fire.c
git commit -m "$(cat <<'EOF'
feat(fire): M8/F4 fire relights torches + lit torches ignite oil

Flamethrower cone now relights extinguished torches it sweeps (LOS-gated;
Torch_Relight no-ops on non-torches). oil_frame lights any unlit patch
within radius+TORCH_OIL_REACH of a lit torch (modelindex!=0), folded into
the existing burning-edict scan loop -- closes the (re)light loop both
directions and makes lit torches passive ignition sources.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Player-burns (flamethrower backdraft self-ignite)

**Files:**
- Modify: `sdlquake/game/weapons_fire.c` (register `fire_flame_backdraft` cvar; backdraft self-ignite in `Flamethrower_DoFire`)

- [ ] **Step 1: Register the backdraft cvar**

In `WeaponsFire_Init` (`weapons_fire.c:23`), add next to the other flame cvars:

```c
    eng->Cvar_Register("fire_flame_backdraft", "40");  // wall within Nu point-blank ignites you; 0=off
```

- [ ] **Step 2: Add the backdraft self-ignite to `Flamethrower_DoFire`**

In `Flamethrower_DoFire`, after the visible-jet block (just before `return 1;`), add:

```c
    // Backdraft: spraying point-blank into a wall ignites the player.
    {
        float bd = eng->Cvar_VariableValue("fire_flame_backdraft");
        if (bd > 0.0f) {
            vec3_t wall = { eye[0] + fwd[0]*bd, eye[1] + fwd[1]*bd, eye[2] + fwd[2]*bd };
            eng->SV_Traceline(eye, wall, 1, self);
            if (g->trace_fraction < 1.0f)              // solid within bd units
                Fire_IgniteMaybeCoated(self, secs, dps, self);
        }
    }
```

> The "standing in burning oil ignites + damages the player" path already works (no `FL_CLIENT` filter in `sim_fire.c:454-461`/`:553`), and `EF_DIMLIGHT` glow + the vanilla red `T_Damage` flash + pain grunts come for free. F4 only adds backdraft. Do NOT add a Gust-puts-you-out path — that is F5.

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: clean.

- [ ] **Step 4: Verify in-game (restart the instance)**

Relaunch on 9876, `+map e1m1`. Via MCP:
- **Backdraft:** grant + select flamethrower (`impulse 212`, `screenshot`, `impulse 41`). Face a wall point-blank (teleport against one). `console_exec "+attack"`, `wait_frames` ~6, `console_exec "-attack"`. `get_player_state` health should be dropping; `screenshot` should show the red damage flash + the player on fire (HUD/cshift). `Fire_IsBurning` on the player edict (id 1) is true (inspect health decreasing).
- **Burning oil burns player (already works — confirm):** `impulse 211` deposit oil at your feet, `impulse 210` to light it (aim down), stand in it; `get_player_state` health drops each tick.
Expected: flamethrower backdraft ignites the player; standing in burning oil damages + ignites the player. Note health deltas in your report.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/weapons_fire.c
git commit -m "$(cat <<'EOF'
feat(fire): M8/F4 player-burns -- flamethrower backdraft self-ignite

Spraying the flamethrower point-blank into a wall (solid within
fire_flame_backdraft units, default 40, 0=off) ignites the player. The
standing-in-burning-oil ignite/DOT path already covered the player (no
FL_CLIENT filter), with EF_DIMLIGHT glow + vanilla T_Damage red flash for
free. Gust-puts-you-out is deferred to F5.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Documentation

**Files:**
- Modify: `CLAUDE.md` (append F4 to the sim_fire.c / fire bullet)
- Modify: `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md` (status line + F4 row ✅)

- [ ] **Step 1: Update `CLAUDE.md`**

Append an F4 paragraph to the fire bullet in the sim-module-map section (after the F3 paragraph), summarizing: `misc_oilbarrel` (spills oil ring → `barrel_explode` auto-lights it); `func_breakable`/`misc_breakable` (flammable via takedamage+health+th_die → `breakable_die` emits STIM_PROP_BROKEN); torches de-static'd into live edicts with `Torch_Extinguish`/`Torch_Relight` (modelindex toggle + ±80 lightmap override; Gust now visibly snuffs; flamethrower relights; lit torches ignite oil within radius+`TORCH_OIL_REACH`); player backdraft self-ignite (`fire_flame_backdraft`); debug impulses 213/214/215; new file `flammables.c`; no ABI bump (GAME_API_VERSION stays 36). Note the two documented MVP limits (baked-ambient crackle persists; live torches cost edicts).

- [ ] **Step 2: Update the spec**

In `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md`:
- Change the status line (line 4) to: `**Status:** F1+F2+F3+F4 implemented 2026-05-30 (F4 = oil barrels, (re)lightable torches, breakable props, player backdraft); F5–F6 not started`
- Mark the **F4 Flammables** row in the staged-milestones table (line 268) with ✅.

- [ ] **Step 3: Read back both files** (per the user's "open written docs" preference — Read them so they render inline for review).

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-05-30-fire-and-oil-design.md
git commit -m "$(cat <<'EOF'
docs: M8/F4 flammables (oil barrel, relightable torches, breakables, player-burn)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Self-review checklist (run before dispatching Task 1)

- **Spec coverage:** F4 row = `misc_oilbarrel` (Task 1) ✓, (re)lightable torches (Tasks 3+4) ✓, breakable props (Task 2) ✓, player-burns (Task 5) ✓. Verify-by "Trail → barrel → boom" (Task 1 verify) ✓, "light a torch" (Tasks 3/4 verify) ✓, "burn a crate" (Task 2 verify) ✓, "burn self" (Task 5 verify) ✓.
- **No ABI bump:** every engine call used (`SV_SetModel`, `SV_Particle`, `SV_StartSound`, `MakeVectors`, `SV_Traceline`, `ED_Alloc`, `Lightmap_AddDelta` via `Light_AddOverride`) exists at version 36. ✓
- **Type/name consistency:** `Torch_Extinguish`/`Torch_Relight`/`is_flammable_light`/`Flammables_DebugToggleNearestTorch` spelled identically across `.h`, `flammables.c`, `abilities.c`, `weapons_fire.c`. `oilbarrel_explode` calls non-static `barrel_explode`. `TORCH_OIL_REACH` defined in `sim_fire.c` only. ✓
- **Build-order safety:** Task 1 declares all `.h` symbols but only defines/uses `spawn_misc_oilbarrel` + `Flammables_Init` + `Flammables_DebugSpawnBarrel`; later tasks define the rest before wiring their callers. Each task builds clean and commits. ✓
- **Off-limits:** none of the F1–F3 sites the user previously accepted are being reverted; `barrel_explode` is reused, not rewritten. ✓

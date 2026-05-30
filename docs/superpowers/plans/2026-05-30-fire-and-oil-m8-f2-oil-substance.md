# M8 F2: Oil Substance — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add flammable oil as a world substance — a bounded pool of floor "oil patches" you can deposit; patches persist, can be ignited (by a burning edict standing in them, or a debug trigger), do area damage-over-time while lit, ignite edicts standing in them, **cascade** fire to nearby patches (the oil-trail effect), and **coat** edicts so they ignite hotter/longer. Builds directly on F1's burn registry. No weapons yet (F3).

**Architecture:** Extends `sim_fire.c` (the F1 module) with a fixed `oil_patch_t s_oil[]` pool (NOT edicts — keeps the 600-edict budget free) and a per-coated-edict side-array. A new `oil_frame()` step inside `Fire_Frame` ticks the pool at the existing 10 Hz: ignites scheduled/contacted patches, applies area DOT via F1's `Fire_Ignite` + `T_Damage`, schedules cascade, renders particles, feeds smoke. Lit oil reuses F1's `SV_Fire` flame plume (rising `pt_fireblob`); unlit oil is sparse dark particles. **No room-lighting** — `Lightmap_AddDelta`/M5 reveal is an F5 concern. No engine ABI change — `GAME_API_VERSION` stays at **34** (F1 already bumped it 33→34 for `SV_Fire`; F2 adds no new ABI entries).

> **Reconciliation note (2026-05-30, post-F1):** this plan was first drafted before the F1 visible-flame work landed. Two corrections were folded in afterward: (1) lit-oil flame now uses `eng->SV_Fire` (the rising `pt_fireblob` plume) instead of `eng->SV_Particle` — the latter is the falling `pt_grav` debris cone that F1 proved *cannot* read as flame; (2) the `fire_crand` particle-scatter helper is re-added (F1's cleanup dropped it as unused, F2 needs it again). Engine-API signatures used here were re-verified against the landed `game_api.h`.

**Tech Stack:** C (game DLL), Zig build (`zig build game`), hot-reloadable `game.dll`, MCP rig for verification (`scripts/mcp_call.py`, port 9876).

---

## Verification model (read first)

Same as F1: **no unit-test harness** (CLAUDE.md). Hard gate per task = **`zig build game` compiles clean**. Behavioral checks use the MCP rig and are mostly deferred to a controller-run interactive pass — note them, don't block on running the GUI yourself.

New headless-friendly hooks this stage adds (so F2 is MCP-verifiable without aiming):
- `fire_oil_num <N>` cvar — deposit an oil patch at edict N's origin once (mirrors F1's `fire_ignite_num`).
- `fire_oil_ignite 1` cvar — light the oil patch nearest the player once.
- `fire_oil_count` cvar — Fire_Frame writes the live active-patch count each tick; read it with the MCP `get_cvar` tool to assert deposit / cascade / expiry.
- `impulse 211` — deposit oil at the player's crosshair trace endpoint (manual play).

Reusable end-to-end test path: `fire_oil_num 8` (oil under monster 8) → `fire_ignite_num 8` (torch the monster) → its burn ignites the oil → patch damages/ignites neighbors.

Commit after every task, straight to master.

---

## Where F2 plugs into the existing `sim_fire.c`

F1 left `sim_fire.c` with: constants block, `fire_burn_t s_burning[FIRE_MAX_BURNING]`, `s_next_tick`, helpers `fire_crand()` / `fire_find_edict()` / `fire_clear_slot()`, the `Fire_Ignite/Extinguish/IsBurning/GetIgniterOrigin/NearestHazard/IgniteTraced` API, and `Fire_Frame()` (cvar ignite hook + per-edict burn loop). F2 adds an oil pool + an `oil_frame()` step. `Fire_AddOil` is declared in `sim.h` for future callers (F3 weapons, F4 barrels).

## File structure

| File | Change | Responsibility |
|---|---|---|
| `sdlquake/game/sim/sim_fire.c` | Modify | Oil pool, `Fire_AddOil`, `oil_frame()` tick (ignite/DOT/cascade/coat/render), oil cvars + debug deposit |
| `sdlquake/game/sim/sim.h` | Modify | Declare `Fire_AddOil` |
| `sdlquake/game/weapons.c` | Modify | `impulse 211` → deposit oil at crosshair |

---

## Task 1: Oil-patch pool + `Fire_AddOil` + debug deposit + unlit-oil render

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`
- Modify: `sdlquake/game/sim/sim.h`
- Modify: `sdlquake/game/weapons.c`

- [ ] **Step 1: Declare `Fire_AddOil` in `sim.h`**

In `sdlquake/game/sim/sim.h`, in the "Fire & oil (Phase 8 / M8)" block (added in F1), add after the `Fire_IgniteTraced` declaration:

```c
// Deposit a patch of flammable oil on the floor at `origin`. radius<=0 and
// amount<=0 use defaults. Patches persist until ignited or they time out.
void Fire_AddOil(const vec3_t origin, float radius, float amount);
```

- [ ] **Step 2: Add oil constants + the pool to `sim_fire.c`**

In `sdlquake/game/sim/sim_fire.c`, add after the existing F1 `#define FIRE_SMOKE_RADIUS ...` line:

```c
// --- Oil substance (F2) ---------------------------------------------------
#define OIL_MAX_PATCHES      256
#define OIL_DEFAULT_RADIUS   48.0f
#define OIL_DEFAULT_AMOUNT   1.0f
#define OIL_MERGE_DIST       40.0f     // deposit within this of an unlit patch merges in
#define OIL_TTL_SECS         60.0f     // unlit oil evaporates after this (generous)
#define OIL_BURN_SECS        4.0f      // how long a lit patch burns before the oil is spent
#define OIL_DMG_INTERVAL     0.5f      // seconds between area-DOT applications
#define OIL_PATCH_DPS        6.0f      // damage/sec to edicts standing in burning oil
#define OIL_IGNITE_SECS      3.0f      // burn duration handed to edicts a patch ignites
#define OIL_CASCADE_RADIUS   80.0f     // a lit patch schedules unlit patches within this
#define OIL_CASCADE_DELAY    0.35f     // delay before a scheduled neighbour catches
```

Add the patch struct + pool after the `fire_burn_t` struct / `s_burning` declaration:

```c
typedef struct {
    int    active;
    int    lit;
    vec3_t origin;
    float  radius;
    float  amount;
    float  deposit_time;
    float  ignite_at;       // >0: scheduled cascade ignition time; 0: not scheduled
    float  burn_until;      // when lit: expiry time
    float  next_dmg_time;   // when lit: next area-DOT application
} oil_patch_t;

static oil_patch_t s_oil[OIL_MAX_PATCHES];
```

- [ ] **Step 2b: Restore the `fire_crand` particle-scatter helper**

F1's final cleanup removed `fire_crand` as unused; F2's oil render (this task's unlit sheen + Task 2's flame) needs it again. Add it next to `fire_find_edict` near the top of `sim_fire.c`:

```c
// Centered random in [-1,1], for scattering oil/flame particles across a
// patch. (F1 had this; its cleanup dropped it as unused -- F2 brings it back.)
static float fire_crand(void) { return eng->Random() * 2.0f - 1.0f; }
```

- [ ] **Step 3: Clear the oil pool in `Fire_LevelInit`**

In `Fire_LevelInit`, add `memset(s_oil, 0, sizeof(s_oil));` alongside the existing `memset(s_burning, ...)`:

```c
void Fire_LevelInit(void) {
    memset(s_burning, 0, sizeof(s_burning));
    memset(s_oil, 0, sizeof(s_oil));
    s_next_tick = 0.0f;
}
```

(Also add the same `memset(s_oil, ...)` to `Fire_Init` next to its `memset(s_burning, ...)` so a hot-reload starts clean.)

- [ ] **Step 4: Register the oil cvars in `Fire_Init`**

In `Fire_Init`, after the existing `eng->Cvar_Register("fire_ignite_num", "-1");` line, add:

```c
    eng->Cvar_Register("fire_oil_num",    "-1");   // deposit oil at edict N (test hook)
    eng->Cvar_Register("fire_oil_ignite", "0");    // light nearest oil to player (test hook)
    eng->Cvar_Register("fire_oil_count",  "0");    // Fire_Frame writes live patch count
```

- [ ] **Step 5: Implement `Fire_AddOil` (merge / allocate / recycle)**

Add this function to `sim_fire.c` (place it after `Fire_NearestHazard`, before `Fire_IgniteTraced`):

```c
void Fire_AddOil(const vec3_t origin, float radius, float amount) {
    if (radius <= 0.0f) radius = OIL_DEFAULT_RADIUS;
    if (amount <= 0.0f) amount = OIL_DEFAULT_AMOUNT;

    // Merge into a nearby UNLIT patch so dense deposits don't thrash the pool.
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *o = &s_oil[i];
        if (!o->active || o->lit) continue;
        float dx = origin[0] - o->origin[0];
        float dy = origin[1] - o->origin[1];
        float dz = origin[2] - o->origin[2];
        if (dx*dx + dy*dy + dz*dz <= OIL_MERGE_DIST * OIL_MERGE_DIST) {
            o->amount += amount;
            if (radius > o->radius) o->radius = radius;
            o->deposit_time = g->time;
            return;
        }
    }

    // Allocate a free slot; if none, recycle the oldest patch (logged, never silent).
    int slot = -1;
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        if (!s_oil[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        float oldest = 1e30f;
        for (int i = 0; i < OIL_MAX_PATCHES; i++) {
            if (s_oil[i].deposit_time < oldest) { oldest = s_oil[i].deposit_time; slot = i; }
        }
        eng->Con_DPrintf("sim_fire: oil pool full, recycling oldest patch\n");
    }

    oil_patch_t *o = &s_oil[slot];
    memset(o, 0, sizeof(*o));
    o->active       = 1;
    o->origin[0]    = origin[0];
    o->origin[1]    = origin[1];
    o->origin[2]    = origin[2];
    o->radius       = radius;
    o->amount       = amount;
    o->deposit_time = g->time;
}
```

- [ ] **Step 6: Add `oil_frame()` with deposit hooks, unlit render, and TTL expiry**

Add this function just above `Fire_Frame` in `sim_fire.c`. (Lit-patch behavior and cascade are added in Tasks 2-3; this task establishes the tick, the test hooks, unlit visuals, and TTL evaporation.)

```c
// Find the player edict (number 1) or NULL.
static edict_t *oil_find_player(void) {
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e))
        if (eng->ED_GetNum(e) == 1) return e;
    return 0;
}

static void oil_frame(void) {
    // Test hook: deposit oil at edict N's origin once.
    {
        int req = (int)eng->Cvar_VariableValue("fire_oil_num");
        if (req >= 0) {
            edict_t *e = fire_find_edict(req);
            if (e && !e->free) Fire_AddOil(e->v.origin, OIL_DEFAULT_RADIUS, OIL_DEFAULT_AMOUNT);
            eng->Cvar_SetValue("fire_oil_num", -1.0f);
        }
    }

    // Test hook: light the unlit oil patch nearest the player once.
    if (eng->Cvar_VariableValue("fire_oil_ignite") != 0.0f) {
        eng->Cvar_SetValue("fire_oil_ignite", 0.0f);
        edict_t *p = oil_find_player();
        if (p) {
            int best = -1; float best2 = 1e30f;
            for (int i = 0; i < OIL_MAX_PATCHES; i++) {
                if (!s_oil[i].active || s_oil[i].lit) continue;
                float dx = s_oil[i].origin[0] - p->v.origin[0];
                float dy = s_oil[i].origin[1] - p->v.origin[1];
                float dz = s_oil[i].origin[2] - p->v.origin[2];
                float d2 = dx*dx + dy*dy + dz*dz;
                if (d2 < best2) { best2 = d2; best = i; }
            }
            if (best >= 0) oil_light_patch(&s_oil[best]);
        }
    }

    int live = 0;
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *o = &s_oil[i];
        if (!o->active) continue;

        // Unlit oil evaporates after its TTL.
        if (!o->lit && g->time - o->deposit_time > OIL_TTL_SECS) {
            o->active = 0;
            continue;
        }

        live++;

        if (!o->lit) {
            // Sparse dark sheen so the player can see where oil lies (decal is a
            // later-stage upgrade; particles are the MVP per the spec).
            vec3_t org = { o->origin[0] + fire_crand() * o->radius * 0.6f,
                           o->origin[1] + fire_crand() * o->radius * 0.6f,
                           o->origin[2] + 2.0f };
            vec3_t dir = { 0, 0, 0 };
            eng->SV_Smoke(org, dir, 4.0f, 1.0f);   // palette 4 = dark grey
            continue;
        }

        // Lit-patch behaviour is added in Task 2; cascade in Task 3.
    }

    eng->Cvar_SetValue("fire_oil_count", (float)live);
}
```

> Note: `oil_light_patch` is referenced here but defined in Task 2. This task will not compile until Task 2 adds it — so **implement Task 1 and Task 2 together before the first build**, OR temporarily stub `oil_light_patch` (see Step 7). To keep tasks independently committable, Step 7 adds a minimal stub now and Task 2 replaces it.

- [ ] **Step 7: Add a minimal `oil_light_patch` stub (replaced in Task 2)**

Add above `oil_frame()`:

```c
// Light an oil patch. Full behaviour (cascade scheduling, timers) is filled
// in by Task 2/3; the minimal form just flips it lit with a burn window.
static void oil_light_patch(oil_patch_t *o) {
    if (!o->active || o->lit) return;
    o->lit           = 1;
    o->burn_until    = g->time + OIL_BURN_SECS;
    o->next_dmg_time = g->time + OIL_DMG_INTERVAL;
    o->ignite_at     = 0.0f;
}
```

- [ ] **Step 8: Call `oil_frame()` from `Fire_Frame`**

In `Fire_Frame`, after the per-edict burn `for` loop closes (and before `Fire_Frame`'s closing brace), add:

```c
    oil_frame();
}
```

- [ ] **Step 9: Bind `impulse 211` to a crosshair oil deposit in `weapons.c`**

First add the debug deposit function to `sim_fire.c` (after `Fire_IgniteTraced`):

```c
void Fire_OilTraced(edict_t *player) {
    if (!player) return;
    eng->MakeVectors(player->v.v_angle);
    vec3_t src = { player->v.origin[0],
                   player->v.origin[1],
                   player->v.origin[2] + player->v.view_ofs[2] };
    vec3_t end = { src[0] + g->v_forward[0] * 2048.0f,
                   src[1] + g->v_forward[1] * 2048.0f,
                   src[2] + g->v_forward[2] * 2048.0f };
    eng->SV_Traceline(src, end, 1, player);   // nomonsters: hit the floor
    if (g->trace_fraction < 1.0f) {
        Fire_AddOil(g->trace_endpos, OIL_DEFAULT_RADIUS, OIL_DEFAULT_AMOUNT);
        eng->Con_Print("fire: oil deposited\n");
    }
}
```

Declare it in `sim.h` (in the fire block, after `Fire_AddOil`):

```c
void Fire_OilTraced(edict_t *player);   // debug: deposit oil at crosshair
```

In `weapons.c` `ImpulseCommands`, after the `imp == 210` line added in F1:

```c
    if (imp == 210) Fire_IgniteTraced(self);
    if (imp == 211) Fire_OilTraced(self);   // debug: deposit oil at crosshair
```

- [ ] **Step 10: Build**

Run: `zig build game`
Expected: clean compile.

- [ ] **Step 11 (interactive, deferred):** `fire_oil_num 8` → dark oil sheen appears under monster 8; `get_cvar fire_oil_count` → `1`; wait 60s → patch evaporates, count → 0. Note for the controller.

- [ ] **Step 12: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c sdlquake/game/sim/sim.h sdlquake/game/weapons.c
git commit -m "feat(fire): M8/F2 oil-patch pool, Fire_AddOil, deposit hooks + unlit render

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Lit-patch behaviour — ignite by burning edict, area DOT, fire visuals, smoke, expiry

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`

- [ ] **Step 1: Replace the `oil_light_patch` stub with the full version**

Replace the Task-1 stub (the cascade scan is added in Task 3; this version is complete except for that):

```c
static void oil_light_patch(oil_patch_t *o) {
    if (!o->active || o->lit) return;
    o->lit           = 1;
    o->burn_until    = g->time + OIL_BURN_SECS;
    o->next_dmg_time = g->time + OIL_DMG_INTERVAL;
    o->ignite_at     = 0.0f;
    // (Task 3 adds: schedule nearby unlit patches to cascade.)
}
```

- [ ] **Step 2: Replace the `if (!o->lit) { ... continue; }` block's trailing comment with lit-patch behaviour**

In `oil_frame()`, replace the `// Lit-patch behaviour is added in Task 2; cascade in Task 3.` line (which sits after the unlit `continue`) with the lit-patch body:

```c
        // --- Lit patch ---
        if (g->time >= o->burn_until) {
            o->active = 0;   // oil consumed
            continue;
        }

        // Area damage + contact ignition for edicts standing in the patch.
        if (g->time >= o->next_dmg_time) {
            o->next_dmg_time = g->time + OIL_DMG_INTERVAL;
            for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
                if (!e->v.takedamage) continue;
                float dx = e->v.origin[0] - o->origin[0];
                float dy = e->v.origin[1] - o->origin[1];
                if (dx*dx + dy*dy > o->radius * o->radius) continue;
                T_Damage(e, g->world, g->world, OIL_PATCH_DPS * OIL_DMG_INTERVAL);
                Fire_Ignite(e, OIL_IGNITE_SECS,
                            eng->Cvar_VariableValue("fire_dps"), g->world);
            }
        }

        // Flame plume across the patch + smoke into the wind grid. MUST use
        // SV_Fire (rising pt_fireblob -- the F1 look), NOT SV_Particle: the
        // svc_particle/pt_grav path falls and sprays a debris cone, so it can
        // never read as flame. This is the exact lesson F1 paid for -- see how
        // Fire_Frame spawns its plume with eng->SV_Fire in sim_fire.c. SV_Fire
        // takes no colour (the orange->grey ramp3 is internal); count drives
        // how many blobs per call.
        for (int p = 0; p < 3; p++) {
            vec3_t org = { o->origin[0] + fire_crand() * o->radius * 0.7f,
                           o->origin[1] + fire_crand() * o->radius * 0.7f,
                           o->origin[2] + 4.0f };
            vec3_t up  = { 0.0f, 0.0f, 12.0f };
            eng->SV_Fire(org, up, 4.0f);
        }
        Wind_AddSmoke(o->origin, FIRE_SMOKE_AMOUNT, o->radius);

        // Fire stimulus so AI registers/avoids the burning oil.
        {
            stimulus_t st;
            memset(&st, 0, sizeof(st));
            st.kind         = STIM_FIRE;
            st.origin[0]    = o->origin[0];
            st.origin[1]    = o->origin[1];
            st.origin[2]    = o->origin[2];
            st.intensity    = 0.8f;
            st.source_edict = -1;   // world-sourced; no self-react concern
            Stim_Emit(&st);
        }
```

- [ ] **Step 3: Let a burning edict standing in an unlit patch ignite it**

In `oil_frame()`, inside the `if (!o->lit)` block, BEFORE the unlit-render `SV_Smoke` call, add a contact check:

```c
        if (!o->lit) {
            // A burning edict standing in unlit oil sets it alight (F1 burning
            // edicts -> oil contact ignition).
            for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
                int en = eng->ED_GetNum(e);
                if (en < 0 || en >= FIRE_MAX_BURNING || !Fire_IsBurning(en)) continue;
                float dx = e->v.origin[0] - o->origin[0];
                float dy = e->v.origin[1] - o->origin[1];
                if (dx*dx + dy*dy <= o->radius * o->radius) { oil_light_patch(o); break; }
            }
            if (o->lit) {
                // Lit this tick — fall through to lit handling next tick.
                live++;
                continue;
            }
            // Sparse dark sheen ... (existing unlit render below)
```

> Keep the existing unlit `SV_Smoke` dark-sheen render after this check, then the existing `continue;`. (The `live++` already happened above the `if (!o->lit)`; do not double-count — see Step 4.)

- [ ] **Step 4: Fix the `live` count so a just-lit patch isn't double-counted**

The `live++` in Step 3's just-lit branch duplicates the `live++` already done before the `if (!o->lit)` block in Task 1. Remove the extra one: in Step 3's `if (o->lit) { ... }` block, delete the `live++;` line (the patch was already counted). Final just-lit branch:

```c
            if (o->lit) {
                continue;   // already counted; handle as lit next tick
            }
```

- [ ] **Step 5: Build**

Run: `zig build game`
Expected: clean compile.

- [ ] **Step 6 (interactive, deferred):** `fire_oil_num 8` then `fire_ignite_num 8` → monster 8 burns, walks into/over its oil, the patch lights (flame particles), and the patch damages anything standing in it. Or `fire_oil_num 8` + `fire_oil_ignite 1` → nearest patch lights directly. Note for the controller.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(fire): M8/F2 lit oil patches — area DOT, contact ignition, flame+smoke

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Cascade — a lit patch ignites nearby oil after a short delay

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`

- [ ] **Step 1: Schedule neighbours when a patch lights**

Replace `oil_light_patch` with the cascading version:

```c
static void oil_light_patch(oil_patch_t *o) {
    if (!o->active || o->lit) return;
    o->lit           = 1;
    o->burn_until    = g->time + OIL_BURN_SECS;
    o->next_dmg_time = g->time + OIL_DMG_INTERVAL;
    o->ignite_at     = 0.0f;

    // Schedule nearby UNLIT patches to catch after a short delay -> fire races
    // down an oil trail. Discrete patch-to-patch, no fluid solver.
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *n = &s_oil[i];
        if (n == o || !n->active || n->lit || n->ignite_at > 0.0f) continue;
        float dx = n->origin[0] - o->origin[0];
        float dy = n->origin[1] - o->origin[1];
        float dz = n->origin[2] - o->origin[2];
        if (dx*dx + dy*dy + dz*dz <= OIL_CASCADE_RADIUS * OIL_CASCADE_RADIUS)
            n->ignite_at = g->time + OIL_CASCADE_DELAY;
    }
}
```

- [ ] **Step 2: Fire scheduled patches when their delay elapses**

In `oil_frame()`, inside the `if (!o->lit)` block, add the scheduled-ignition check BEFORE the burning-edict contact scan from Task 2 Step 3:

```c
        if (!o->lit) {
            // Cascade: a scheduled neighbour catches when its delay elapses.
            if (o->ignite_at > 0.0f && g->time >= o->ignite_at) {
                oil_light_patch(o);
                continue;   // already counted; handle as lit next tick
            }
            // (Task 2 Step 3 burning-edict contact scan follows)
            for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
                ...
```

(Leave the rest of the unlit block — the burning-edict scan and the dark-sheen render — unchanged.)

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: clean compile.

- [ ] **Step 4 (interactive, deferred):** deposit a line of patches with repeated `impulse 211` (or several `fire_oil_num` on spread-out edicts), `fire_oil_ignite 1` one end → watch `fire_oil_count` patches light in sequence and the flame visibly travel the trail. Note for the controller.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(fire): M8/F2 oil cascade — fire races along connected patches

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Coating — oil deposited onto an edict makes it ignite hotter/longer

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`

- [ ] **Step 1: Add the coated side-array + constants**

Add to the oil constants block:

```c
#define OIL_COAT_SECS        8.0f      // how long an edict stays oil-coated
#define OIL_COAT_BURN_SECS   8.0f      // a coated edict burns this long (vs OIL_IGNITE_SECS)
```

Add after `s_oil`:

```c
// Coated edicts (by edict number): g->time < value => still oil-coated.
static float s_coated_until[FIRE_MAX_BURNING];
```

Clear it in BOTH `Fire_Init` and `Fire_LevelInit` next to the `memset(s_oil, ...)`:

```c
    memset(s_coated_until, 0, sizeof(s_coated_until));
```

- [ ] **Step 2: Coat edicts inside a fresh deposit**

In `Fire_AddOil`, after a NEW patch is allocated and initialized (at the very end of the function, after setting `o->deposit_time`), add:

```c
    // Coat edicts standing in the fresh oil so a later spark ignites them
    // instantly and they burn longer/hotter.
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (!e->v.takedamage) continue;
        int en = eng->ED_GetNum(e);
        if (en < 0 || en >= FIRE_MAX_BURNING) continue;
        float dx = e->v.origin[0] - o->origin[0];
        float dy = e->v.origin[1] - o->origin[1];
        if (dx*dx + dy*dy <= o->radius * o->radius)
            s_coated_until[en] = g->time + OIL_COAT_SECS;
    }
```

(Note: the merge branch returns early and does not coat — only fresh deposits coat, which is fine; a merge just tops up amount.)

- [ ] **Step 3: Add a coated-aware ignite helper and use it for patch contact**

Add this helper after `Fire_AddOil`:

```c
// Ignite `e`, but if it is oil-coated, burn it longer/hotter. Used wherever a
// fire source touches an edict (oil patch contact; later, weapons/explosions).
void Fire_IgniteMaybeCoated(edict_t *e, float base_secs, float dps, edict_t *igniter) {
    if (!e) return;
    int en = eng->ED_GetNum(e);
    float secs = base_secs;
    if (en >= 0 && en < FIRE_MAX_BURNING && g->time < s_coated_until[en])
        secs = OIL_COAT_BURN_SECS;
    Fire_Ignite(e, secs, dps, igniter);
}
```

Declare it in `sim.h` (fire block, after `Fire_OilTraced`):

```c
void Fire_IgniteMaybeCoated(edict_t *e, float base_secs, float dps, edict_t *igniter);
```

In `oil_frame()`'s lit-patch area-DOT loop (Task 2 Step 2), replace the direct `Fire_Ignite(e, OIL_IGNITE_SECS, ...)` call with the coated-aware version:

```c
                T_Damage(e, g->world, g->world, OIL_PATCH_DPS * OIL_DMG_INTERVAL);
                Fire_IgniteMaybeCoated(e, OIL_IGNITE_SECS,
                                       eng->Cvar_VariableValue("fire_dps"), g->world);
```

- [ ] **Step 4: Build**

Run: `zig build game`
Expected: clean compile.

- [ ] **Step 5 (interactive, deferred):** `fire_oil_num 12` (coat the ogre) then expose it to fire (`fire_oil_num 12` puts oil under it; light that patch with `fire_oil_ignite 1`) → the ogre ignites and burns ~8 s (vs the ~3 s a non-coated contact gives). Note for the controller.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c sdlquake/game/sim/sim.h
git commit -m "feat(fire): M8/F2 oil coating — coated edicts ignite hotter/longer

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (F2 row: "Oil-patch pool, deposit/coat, cascade, ignite-on-contact"):**
- Oil-patch pool → Task 1 (`s_oil`, `Fire_AddOil`). ✓
- Deposit → Task 1 (`Fire_AddOil` + `fire_oil_num` cvar + `impulse 211`). ✓
- Coat → Task 4 (`s_coated_until`, `Fire_IgniteMaybeCoated`). ✓
- Cascade → Task 3 (`oil_light_patch` neighbour scheduling + `oil_frame` delay check). ✓
- Ignite-on-contact → Task 2 (burning edict in unlit patch lights it; lit patch ignites edicts in radius). ✓
- Persistence/TTL + merge/recycle (spec System 2) → Task 1. ✓
- Smoke (spec System 7, present from F1) → Task 2 (`Wind_AddSmoke`). ✓
- Particle render, no decal/lighting (spec System 8 MVP; light deferred to F5) → Tasks 1-2. ✓
- No ABI bump → confirmed (all in `sim_fire.c`/`sim.h`/`weapons.c`). ✓

**Deferred to later stages (correctly, per milestone table):** room-lighting / M5 reveal from oil fire (F5), oil decal rendering (polish), weapon-driven deposit (F3 oil gun), barrels/torches/props (F4), Gust-extinguish of oil (F5).

**Placeholder scan:** none — every step has complete code. The one forward-reference (`oil_light_patch` used in Task 1, defined as a stub in Task 1 Step 7, completed in Task 2/3) is called out explicitly with the stub provided so each task compiles.

**Type/name consistency:** `oil_patch_t`/`s_oil`/`Fire_AddOil`/`oil_frame`/`oil_light_patch`/`oil_find_player`/`Fire_OilTraced`/`Fire_IgniteMaybeCoated`/`s_coated_until` are defined before use across tasks; cvars `fire_oil_num`/`fire_oil_ignite`/`fire_oil_count` are registered in Task 1 and read in Tasks 1-3; constants (`OIL_*`) are introduced in Task 1 (Task 4 adds the two `OIL_COAT_*`). `Fire_Ignite`/`Fire_IsBurning`/`fire_find_edict`/`fire_crand`/`FIRE_MAX_BURNING`/`FIRE_SMOKE_AMOUNT` are F1 symbols reused.

**Carry-forward note (from F1):** `STIM_FIRE` now also emitted per lit oil patch at 10 Hz — the F1 stim-ring-crowding note is now live; if many patches burn at once, revisit throttling (already flagged in the spec Risks and a code comment).

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-30-fire-and-oil-m8-f2-oil-substance.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task + two-stage review (same as F1).
2. **Inline Execution** — execute here with checkpoints.

Which approach?

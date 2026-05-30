# M8 F1: Burn Primitive — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the core damage-over-time "burning" mechanic — any edict can be set on fire, takes periodic damage, shows flame + dynamic light + smoke, emits a `STIM_FIRE` stimulus, and (if it's an AI monster) panics and flees while non-burning monsters avoid the fire. Triggerable for testing via a debug impulse and an MCP-settable cvar. No weapons or oil yet (those are F2/F3).

**Architecture:** A new DLL-side sim module `sim_fire.c` (peer to `sim_light.c`/`sim_wind.c`) owns a fixed-size **burn registry** keyed by edict number. `Fire_Frame()` ticks at 10 Hz inside `Sim_Frame`, applying `T_Damage`, spawning particles, setting `EF_DIMLIGHT`, injecting smoke into the M4 wind grid, and emitting `STIM_FIRE`. The AI module (`sim_ai.c` + `ai.c`) reads the registry to drive panic/avoidance. All state is DLL-side — **no `GAME_API_VERSION` bump** (stays 33).

**Tech Stack:** C (gnu89 for engine, modern C for game DLL), Zig build (`zig build game`), hot-reloadable `game.dll`, MCP server for headless verification.

---

## Verification model (read first)

This repo has **no unit-test harness** — per `CLAUDE.md`, verification is **build success + in-game/MCP behavioral checks**. So in every task:

- A **"confirm it fails / is absent"** step means: build and observe the behavior is *not* there yet (or the symbol doesn't exist).
- A **"verify"** step is the equivalent of a passing test: a concrete command plus the exact observation expected.

**Build command (fast game-only rebuild):** `zig build game` — expected: no errors, `zig-out/bin/game.dll` (or `.dylib`) updated.

**Runtime rig (two terminals):**
- Terminal 1: `zig build run -- --hot-reload +map ai_t02_combat` (a map with monsters; `e1m1` also works).
- Manual ignition: aim at a monster, type `impulse 210` in the console → that monster ignites.
- Headless/MCP ignition: add `--mcp-http 8080` to the run command, then drive it with the MCP tools `list_entities` (find a monster's edict number + read its `health`), `set_cvar` (`fire_ignite_num <N>` ignites edict N), and `screenshot`. `scripts/mcp_call.py` is the one-shot CLI; tool names are in `sdlquake/mcp/mcp_server.c`.

Commit after every task (per repo convention: commit straight to `master`).

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `sdlquake/game/sim/sim_fire.c` | **Create** | Burn registry, 10 Hz tick (DOT + visuals + smoke + STIM_FIRE), ignite/extinguish/query API, debug triggers |
| `sdlquake/game/sim/sim.h` | Modify | Add `STIM_FIRE`; add `int burning` to `ai_brain_t`; declare `Fire_*` API |
| `sdlquake/game/sim/sim_main.c` | Modify | Call `Fire_Init` / `Fire_LevelInit` / `Fire_Frame` |
| `sdlquake/game/sim/sim_ai.c` | Modify | Mirror burn flag onto brains; panic-flee + avoid in `behavior_tick`; `STIM_FIRE` sense case |
| `sdlquake/game/ai.c` | Modify | Suppress vanilla move (3 hooks) + attacks (`CheckAnyAttack`) while burning |
| `sdlquake/game/weapons.c` | Modify | `impulse 210` → `Fire_IgniteTraced` (manual test trigger) |
| `build.zig` | Modify | Add `sim_fire.c` to the game DLL source list |

---

## Task 1: Scaffold `sim_fire.c` module, ABI-free header additions, build + lifecycle wiring

**Files:**
- Create: `sdlquake/game/sim/sim_fire.c`
- Modify: `sdlquake/game/sim/sim.h`
- Modify: `sdlquake/game/sim/sim_main.c`
- Modify: `build.zig:464`

- [ ] **Step 1: Add `STIM_FIRE`, the brain `burning` flag, and the `Fire_*` API to `sim.h`**

In `sdlquake/game/sim/sim.h`, add `STIM_FIRE` to the stimulus enum (after `STIM_PROP_BROKEN`):

```c
typedef enum {
    STIM_NONE = 0,
    STIM_SOUND,
    STIM_SIGHT_ENTITY,
    STIM_SMOKE,
    STIM_LIGHT_CHANGE,
    STIM_CORPSE,
    STIM_PROP_BROKEN,
    STIM_FIRE,            // Phase 8 / M8 — active fire / burning entity
} stim_kind_t;
```

In the same file, add a field to `ai_brain_t` (just after the `walking` field, before the closing `}`):

```c
    // 1 while this monster is on fire (mirrored from the fire registry each
    // AI tick). Drives panic-flee in behavior_tick and suppresses vanilla
    // movement/attacks in ai.c. Phase 8 / M8.
    int          burning;
```

And add the fire API block right before the `// Arena (test)` section:

```c
// ---------------------------------------------------------------------------
// Fire & oil (Phase 8 / M8)
// ---------------------------------------------------------------------------
void Fire_Init(void);
void Fire_LevelInit(void);
void Fire_Frame(void);

// Set an edict on fire for `seconds`, doing `dps` damage/second, attributed
// to `igniter` (NULL = world). Extends the burn if already alight.
void Fire_Ignite(edict_t *e, float seconds, float dps, edict_t *igniter);
void Fire_Extinguish(edict_t *e);

int  Fire_IsBurning(int edict_num);                          // 1 if alight
int  Fire_GetIgniterOrigin(int edict_num, vec3_t out);       // 1 if igniter known
// Nearest active fire within `radius` of `pos`; writes its origin to `out`.
// Returns 1 if one was found. Used by AI to flee/avoid.
int  Fire_NearestHazard(const vec3_t pos, float radius, vec3_t out);

// Debug trigger: trace from the player's view and ignite whatever's hit.
void Fire_IgniteTraced(edict_t *player);
```

- [ ] **Step 2: Create `sim_fire.c` with stubs + the registry skeleton**

Create `sdlquake/game/sim/sim_fire.c`:

```c
// sim_fire.c -- Burning damage-over-time + fire FX (Phase 8 / M8, stage F1).
//
// A fixed registry keyed by edict number holds which edicts are on fire.
// Fire_Frame() ticks at 10 Hz: applies T_Damage, spawns flame particles,
// sets EF_DIMLIGHT, injects smoke into the wind grid, and emits STIM_FIRE.
// All state is DLL-side; nothing touches entvars_t or the engine ABI.

#include "sim.h"
#include "../game_defs.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

extern void T_Damage(edict_t *targ, edict_t *inflictor, edict_t *attacker, float damage);

#define FIRE_MAX_BURNING     SIM_MAX_BRAINS    // one slot per possible edict
#define FIRE_TICK_HZ         10.0f
#define FIRE_DMG_INTERVAL    0.5f              // seconds between damage applications
#define FIRE_HAZARD_RADIUS   64.0f             // burning edict's danger radius (unused until F2 oil, kept for clarity)
#define FIRE_AI_AVOID_RADIUS 160.0f            // non-burning monsters flee fire within this
#define FIRE_SMOKE_AMOUNT    0.12f
#define FIRE_SMOKE_RADIUS    40.0f

typedef struct {
    int   active;
    float burn_until;
    float dps;
    int   igniter_edict;     // -1 = world / unknown
    float next_dmg_time;
} fire_burn_t;

static fire_burn_t s_burning[FIRE_MAX_BURNING];   // indexed by edict number
static float       s_next_tick;

static float fire_crand(void) { return 2.0f * (eng->Random() - 0.5f); }

static edict_t *fire_find_edict(int num) {
    if (num < 0) return 0;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e))
        if (eng->ED_GetNum(e) == num) return e;
    return 0;
}

void Fire_Init(void) {
    memset(s_burning, 0, sizeof(s_burning));
    s_next_tick = 0.0f;
    eng->Cvar_Register("sim_fire_debug",  "0");
    eng->Cvar_Register("fire_dps",        "8");
    eng->Cvar_Register("fire_secs",       "5");
    eng->Cvar_Register("fire_ignite_num", "-1");   // MCP/console test hook
}

void Fire_LevelInit(void) {
    memset(s_burning, 0, sizeof(s_burning));
    s_next_tick = 0.0f;
}

void Fire_Ignite(edict_t *e, float seconds, float dps, edict_t *igniter) { (void)e; (void)seconds; (void)dps; (void)igniter; }
void Fire_Extinguish(edict_t *e) { (void)e; }
int  Fire_IsBurning(int edict_num) { (void)edict_num; return 0; }
int  Fire_GetIgniterOrigin(int edict_num, vec3_t out) { (void)edict_num; (void)out; return 0; }
int  Fire_NearestHazard(const vec3_t pos, float radius, vec3_t out) { (void)pos; (void)radius; (void)out; return 0; }
void Fire_IgniteTraced(edict_t *player) { (void)player; }

void Fire_Frame(void) {
    if (g->time < s_next_tick) return;
    s_next_tick = g->time + (1.0f / FIRE_TICK_HZ);
    // Tick body added in Task 2.
}
```

- [ ] **Step 3: Register `sim_fire.c` in the build**

In `build.zig`, after the `sim_retrofit.c` line (`build.zig:464`), add `sim_fire.c`:

```
            "sdlquake/game/sim/sim_retrofit.c",
            "sdlquake/game/sim/sim_fire.c",
```

- [ ] **Step 4: Wire lifecycle into `sim_main.c`**

In `sdlquake/game/sim/sim_main.c`, add `Fire_Init()` to `Sim_Init` (after `Light_Init();`):

```c
    Light_Init();
    Fire_Init();
    Sim_Arena_Init();
```

Add `Fire_LevelInit()` to `Sim_LevelInit` (after `Light_LevelInit();`):

```c
    Light_LevelInit();
    Fire_LevelInit();
    Sim_Retrofit_LevelInit();
```

Add the frame tick to `Sim_Frame` — **before** `Sim_AI` so the AI sees fresh fire state this tick:

```c
    SIM_PERF("Sim_Retrofit") Sim_Retrofit_Frame();
    SIM_PERF("Fire")         Fire_Frame();
    SIM_PERF("Sim_AI")       Sim_AI_Frame();
    SIM_PERF("Wind")         Wind_Frame();
```

- [ ] **Step 5: Build and verify it compiles**

Run: `zig build game`
Expected: builds with no errors; `sim_fire.c` is compiled (it appears in the source list now). The stubs link cleanly.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c sdlquake/game/sim/sim.h sdlquake/game/sim/sim_main.c build.zig
git commit -m "feat(fire): M8/F1 scaffold sim_fire module + lifecycle wiring

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Burn registry + DOT tick + ignition triggers

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`
- Modify: `sdlquake/game/weapons.c:1650` (`ImpulseCommands`)

- [ ] **Step 1: Confirm ignition does nothing yet**

Run: `zig build run -- --hot-reload +map ai_t02_combat`, aim at a monster, type `impulse 210` in console.
Expected: nothing happens (the impulse is unbound; `Fire_IgniteTraced` is a stub). This is the "test fails first" baseline.

- [ ] **Step 2: Implement the ignite/extinguish/query API in `sim_fire.c`**

Replace the stub bodies for `Fire_Ignite`, `Fire_Extinguish`, `Fire_IsBurning`, `Fire_GetIgniterOrigin`, `Fire_NearestHazard`:

```c
static void fire_clear_slot(int n, edict_t *e) {
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    s_burning[n].active = 0;
    if (e) e->v.effects = (float)((int)e->v.effects & ~EF_DIMLIGHT);
}

void Fire_Ignite(edict_t *e, float seconds, float dps, edict_t *igniter) {
    if (!e || e->free) return;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    fire_burn_t *f = &s_burning[n];
    float until = g->time + seconds;
    if (!f->active || until > f->burn_until) f->burn_until = until;
    f->active        = 1;
    f->dps           = dps;
    f->igniter_edict = (igniter && !igniter->free) ? eng->ED_GetNum(igniter) : -1;
    if (f->next_dmg_time < g->time) f->next_dmg_time = g->time + FIRE_DMG_INTERVAL;
}

void Fire_Extinguish(edict_t *e) {
    if (!e) return;
    fire_clear_slot(eng->ED_GetNum(e), e);
}

int Fire_IsBurning(int edict_num) {
    if (edict_num < 0 || edict_num >= FIRE_MAX_BURNING) return 0;
    return s_burning[edict_num].active;
}

int Fire_GetIgniterOrigin(int edict_num, vec3_t out) {
    if (edict_num < 0 || edict_num >= FIRE_MAX_BURNING) return 0;
    if (!s_burning[edict_num].active) return 0;
    edict_t *ig = fire_find_edict(s_burning[edict_num].igniter_edict);
    if (!ig) return 0;
    out[0] = ig->v.origin[0]; out[1] = ig->v.origin[1]; out[2] = ig->v.origin[2];
    return 1;
}

int Fire_NearestHazard(const vec3_t pos, float radius, vec3_t out) {
    float best2 = radius * radius;
    int found = 0;
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        int n = eng->ED_GetNum(e);
        if (n < 0 || n >= FIRE_MAX_BURNING || !s_burning[n].active) continue;
        float dx = pos[0] - e->v.origin[0];
        float dy = pos[1] - e->v.origin[1];
        float dz = pos[2] - e->v.origin[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best2) {
            best2 = d2;
            out[0] = e->v.origin[0]; out[1] = e->v.origin[1]; out[2] = e->v.origin[2];
            found = 1;
        }
    }
    return found;
}
```

- [ ] **Step 3: Implement the debug ignite trigger `Fire_IgniteTraced`**

Replace the `Fire_IgniteTraced` stub:

```c
void Fire_IgniteTraced(edict_t *player) {
    if (!player) return;
    float dps  = eng->Cvar_VariableValue("fire_dps");
    float secs = eng->Cvar_VariableValue("fire_secs");
    eng->MakeVectors(player->v.v_angle);
    vec3_t src = { player->v.origin[0],
                   player->v.origin[1],
                   player->v.origin[2] + player->v.view_ofs[2] };
    vec3_t end = { src[0] + g->v_forward[0] * 2048.0f,
                   src[1] + g->v_forward[1] * 2048.0f,
                   src[2] + g->v_forward[2] * 2048.0f };
    eng->SV_Traceline(src, end, 0, player);
    edict_t *t = g->trace_ent;
    if (t && t != g->world && t->v.takedamage) {
        Fire_Ignite(t, secs, dps, player);
        eng->Con_Print("fire: ignited entity under crosshair\n");
    } else {
        eng->Con_Print("fire: no flammable entity under crosshair\n");
    }
}
```

- [ ] **Step 4: Implement the tick body (cvar poll + expiry + DOT) in `Fire_Frame`**

Replace the `Fire_Frame` body (keep the `s_next_tick` guard at the top):

```c
void Fire_Frame(void) {
    if (g->time < s_next_tick) return;
    s_next_tick = g->time + (1.0f / FIRE_TICK_HZ);

    // MCP/console test hook: `fire_ignite_num <N>` ignites edict N once.
    {
        int req = (int)eng->Cvar_VariableValue("fire_ignite_num");
        if (req >= 0) {
            edict_t *e = fire_find_edict(req);
            if (e && !e->free && e->v.takedamage)
                Fire_Ignite(e, eng->Cvar_VariableValue("fire_secs"),
                               eng->Cvar_VariableValue("fire_dps"), g->world);
            eng->Cvar_SetValue("fire_ignite_num", -1.0f);
        }
    }

    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        int n = eng->ED_GetNum(e);
        if (n < 0 || n >= FIRE_MAX_BURNING) continue;
        fire_burn_t *f = &s_burning[n];
        if (!f->active) continue;

        int   con      = eng->SV_PointContents(e->v.origin);
        int   inwater  = (con == CONTENT_WATER || con == CONTENT_SLIME);
        int   dead     = (e->v.health <= 0.0f || e->v.deadflag != DEAD_NO);
        if (e->free || g->time >= f->burn_until || dead || inwater) {
            fire_clear_slot(n, e->free ? 0 : e);
            continue;
        }

        // Damage in discrete chunks so armor/save math stays meaningful.
        if (g->time >= f->next_dmg_time && e->v.takedamage) {
            f->next_dmg_time = g->time + FIRE_DMG_INTERVAL;
            edict_t *attacker = fire_find_edict(f->igniter_edict);
            if (!attacker || attacker->free) attacker = g->world;
            T_Damage(e, g->world, attacker, f->dps * FIRE_DMG_INTERVAL);
        }
        // Visuals + STIM_FIRE added in Tasks 3 and 4.
    }
}
```

- [ ] **Step 5: Bind `impulse 210` to `Fire_IgniteTraced` in `weapons.c`**

In `sdlquake/game/weapons.c`, add an extern declaration near the top of the file (with the other `extern` game-function decls), e.g. just below the includes:

```c
extern void Fire_IgniteTraced(edict_t *player);
```

In `ImpulseCommands` (`weapons.c:1650`), add the debug impulse (after the `imp == 100` line):

```c
    if (imp == 100) Phase6_CheatGiveAll();
    if (imp == 210) Fire_IgniteTraced(self);   // debug: ignite entity under crosshair
    if (imp == 255) QuadCheat();
```

- [ ] **Step 6: Build**

Run: `zig build game`
Expected: no errors.

- [ ] **Step 7: Verify DOT applies (manual)**

Run: `zig build run -- --hot-reload +map ai_t02_combat`. Aim at a monster, `impulse 210`.
Expected: console prints `fire: ignited entity under crosshair`; over the next ~5 s the monster loses health each half-second (watch it flinch / die if low HP). A 30-HP soldier should die within the burn window at default `fire_dps 8` (≈40 total damage).

- [ ] **Step 8: Verify DOT applies (headless/MCP, optional)**

Run terminal 1: `zig build run -- --hot-reload --mcp-http 8080 +map ai_t02_combat`.
Using the MCP tools (via `scripts/mcp_call.py` against port 8080):
1. `list_entities` → note a monster's edict number `N` and its `health`.
2. `set_cvar` `fire_ignite_num` = `N`.
3. wait ~2 s, `list_entities` again → that entity's `health` has dropped.
Expected: health decreases, confirming the registry + DOT work without manual aiming.

- [ ] **Step 9: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c sdlquake/game/weapons.c
git commit -m "feat(fire): M8/F1 burn registry, DOT tick, ignite triggers

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Fire visuals — flame particles, dynamic light, smoke

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c` (inside the `Fire_Frame` per-edict loop)

- [ ] **Step 1: Confirm there's no flame yet**

Run the rig, ignite a monster (`impulse 210`).
Expected: it takes damage but there is **no** visible flame, no light, no smoke. Baseline.

- [ ] **Step 2: Add particles + `EF_DIMLIGHT` + smoke to the tick**

In `Fire_Frame`, replace the `// Visuals + STIM_FIRE added in Tasks 3 and 4.` comment with the visual block (this runs every tick for any still-burning edict):

```c
        // Dynamic light: a real-time dlight follows the moving fire. (Static
        // oil-patch fires will use Lightmap_AddDelta in F2; moving burning
        // edicts use EF_DIMLIGHT to avoid lightmap-delta accumulation.)
        e->v.effects = (float)((int)e->v.effects | EF_DIMLIGHT);

        // Flame particles — small upward jet in the Quake fire/orange ramp.
        for (int p = 0; p < 3; p++) {
            vec3_t org = { e->v.origin[0] + fire_crand() * 8.0f,
                           e->v.origin[1] + fire_crand() * 8.0f,
                           e->v.origin[2] + 8.0f + eng->Random() * 24.0f };
            vec3_t dir = { fire_crand() * 8.0f,
                           fire_crand() * 8.0f,
                           24.0f + eng->Random() * 24.0f };
            float  color = 109.0f + (float)((int)(eng->Random() * 3.0f)); // 109..111
            eng->SV_Particle(org, dir, color, 1.0f);
        }

        // Feed the M4 wind/smoke grid so fire throws up a smoke screen.
        Wind_AddSmoke(e->v.origin, FIRE_SMOKE_AMOUNT, FIRE_SMOKE_RADIUS);
```

> Note: `Wind_AddSmoke` is declared in `sim.h` (DLL-internal), so it's called directly, not through `eng`. `EF_DIMLIGHT` is from `game_defs.h` (already included).

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: no errors.

- [ ] **Step 4: Verify visuals**

Run the rig, ignite a monster (`impulse 210`).
Expected: orange flame particles jet upward from the monster, it casts a flickering dynamic light on nearby surfaces, and faint smoke rises (visible against a lit wall; toggle `r_part`/wind debug if needed). On extinguish/death the dlight stops.

- [ ] **Step 5: Capture a screenshot for the record (optional)**

Via MCP `screenshot`, or in-game console `screenshot`. Confirm the flame reads clearly.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(fire): M8/F1 flame particles, EF_DIMLIGHT, smoke injection

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Emit `STIM_FIRE` and teach the sense filter about it

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c` (`Fire_Frame` loop)
- Modify: `sdlquake/game/sim/sim_ai.c` (`sense_intensity`)

- [ ] **Step 1: Emit `STIM_FIRE` each tick from every burning edict**

In `Fire_Frame`, after the `Wind_AddSmoke(...)` line (still inside the per-edict loop), add:

```c
        // Broadcast a fire stimulus so distant AI can register the threat.
        {
            stimulus_t st;
            memset(&st, 0, sizeof(st));
            st.kind          = STIM_FIRE;
            st.origin[0]     = e->v.origin[0];
            st.origin[1]     = e->v.origin[1];
            st.origin[2]     = e->v.origin[2];
            st.intensity     = 0.8f;
            st.source_edict  = n;
            Stim_Emit(&st);
        }
```

> `stimulus_t`, `Stim_Emit`, and `STIM_FIRE` are all in `sim.h` (already included).

- [ ] **Step 2: Add a `STIM_FIRE` case to the sense filter**

In `sdlquake/game/sim/sim_ai.c`, in `sense_intensity`'s `switch (s->kind)`, add a case alongside the others (e.g. after `STIM_SMOKE`):

```c
        case STIM_SMOKE:
            ref = b->sense_sight_range; break;
        case STIM_FIRE:
            ref = 512.0f; break;     // fire is alarming at medium range
        default:
            return 0.0f;
```

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: no errors (no `STIM_FIRE` unhandled-enum warnings).

- [ ] **Step 4: Verify the stimulus raises alert**

Run the rig with `+map ai_t02_combat`. Open the dev overlay (F12) to view the AI panel. Ignite a monster near another idle monster (or stand near one and ignite yourself isn't possible yet — instead ignite a monster close to a second one).
Expected: a nearby non-targeting monster's `alert_level` rises after the fire starts (it "notices" the fire), visible in the AI panel. (Full flee behavior lands in Task 5 — here we only confirm the stim flows into the sense filter.)

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c sdlquake/game/sim/sim_ai.c
git commit -m "feat(fire): M8/F1 emit STIM_FIRE + sense-filter case

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: AI panic-flee when burning, avoid fire when not

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c` (mirror flag in `Sim_AI_Frame`; `flee_from` helper; override in `behavior_tick`)

- [ ] **Step 1: Mirror the burn flag onto each brain every AI tick**

In `sdlquake/game/sim/sim_ai.c`, in `Sim_AI_Frame`, after the dead-skip block and before `sense_tick(b, e);` (around line 484), add:

```c
        // Phase 8 / M8 — mirror the fire registry so ai.c hooks and the
        // behavior override can read b->burning cheaply (it's already loaded).
        b->burning = Fire_IsBurning(b->edict_num);

        sense_tick(b, e);
```

- [ ] **Step 2: Add the `flee_from` helper**

In `sim_ai.c`, add this just above `static void behavior_tick(...)`:

```c
// Run directly away from `threat` (2D), projecting a target point well past
// the monster so walk_and_track has somewhere to head. Phase 8 / M8.
static void flee_from(ai_brain_t *b, edict_t *e, const vec3_t threat, float dist) {
    float dx = e->v.origin[0] - threat[0];
    float dy = e->v.origin[1] - threat[1];
    float len = (float)sqrt(dx*dx + dy*dy);
    if (len < 1.0f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }  // threat on top of us
    vec3_t target = { e->v.origin[0] + (dx / len) * 256.0f,
                      e->v.origin[1] + (dy / len) * 256.0f,
                      e->v.origin[2] };
    walk_and_track(b, e, target, dist);
}
```

- [ ] **Step 3: Add the fire override at the top of `behavior_tick`**

In `behavior_tick`, immediately after the opening `{` and before `switch (b->state) {`, insert:

```c
    // Phase 8 / M8 — fire overrides normal behavior.
    if (b->burning) {
        // Panic: run away from whoever set us alight (fall back to the last
        // known threat position), suppress everything else this tick.
        vec3_t away;
        if (!Fire_GetIgniterOrigin(b->edict_num, away)) {
            away[0] = b->last_known_pos[0];
            away[1] = b->last_known_pos[1];
            away[2] = b->last_known_pos[2];
        }
        flee_from(b, e, away, 10.0f);
        return;
    } else {
        // Not burning: give active fire a wide berth.
        vec3_t hazard;
        if (Fire_NearestHazard(e->v.origin, FIRE_AI_AVOID_RADIUS, hazard)) {
            flee_from(b, e, hazard, 8.0f);
            return;
        }
    }
```

- [ ] **Step 4: Build**

Run: `zig build game`
Expected: no errors.

- [ ] **Step 5: Verify panic + avoid**

Run the rig with `+map ai_t02_combat`. Get a monster into combat (let it see you), then `impulse 210` it.
Expected: the burning monster turns and **runs away** from you instead of attacking; a second, non-burning monster within ~160 units keeps its distance from the burning one. (Vanilla attack/chase may still leak until Task 6 — note whether it does.)

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim_ai.c
git commit -m "feat(fire): M8/F1 AI panic-flee while burning + avoid active fire

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Suppress vanilla movement + attacks while burning

**Files:**
- Modify: `sdlquake/game/ai.c` (three early-return hooks + `CheckAnyAttack`)

- [ ] **Step 1: Confirm the leak (test-first)**

From Task 5's verify: a burning monster may still chase via vanilla `ai_run` or loose an attack mid-animation. Confirm whether it does.
Expected (baseline): some residual chase/attack while burning.

- [ ] **Step 2: Add `b->burning` to the three vanilla-movement early-returns**

In `sdlquake/game/ai.c`, the identical guard appears in `ai_walk` (~308), `ai_stand` (~328), and `ai_run` (~442). Update **all three** (use a replace-all on the exact block):

Find:
```c
        if (b && (b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING ||
                  (b->state == AI_IDLE && b->patrol_route_id >= 0)))
            return;
```
Replace with:
```c
        if (b && (b->burning ||
                  b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING ||
                  (b->state == AI_IDLE && b->patrol_route_id >= 0)))
            return;
```

- [ ] **Step 3: Block attacks while burning in `CheckAnyAttack`**

In `ai.c`, in `CheckAnyAttack` (line ~384), add a guard as the first statements (after the existing `if (!enemy_vis) return 0;`):

```c
static int CheckAnyAttack(void) {
    if (!enemy_vis) return 0;
    {
        ai_brain_t *b = Sim_AI_GetBrain(g->self);
        if (b && b->burning) return 0;   // panicking, on fire — no attacks
    }
    edict_t    *self = g->self;
```

- [ ] **Step 4: Build**

Run: `zig build game`
Expected: no errors.

- [ ] **Step 5: Verify full suppression**

Run the rig, get a monster into combat, `impulse 210` it.
Expected: the burning monster **only** flees — no vanilla chase, no melee/projectile attacks — until the fire goes out (if it survives) or it dies. When the fire expires (default 5 s) a survivor resumes normal combat.

- [ ] **Step 6: Verify the burn lifecycle end-to-end (regression sweep)**

Run the rig and confirm all F1 behaviors together:
1. Ignite a monster → flame + light + smoke appear, health ticks down (Tasks 2–3).
2. It flees and doesn't attack (Tasks 5–6).
3. Drive it into water → fire goes out immediately (`CONTENT_WATER` expiry, Task 2).
4. Let a low-HP monster burn to death → dlight clears, no lingering fire on the corpse.
5. `changelevel` / reload the map → no stale fire (`Fire_LevelInit` clears the registry).
Expected: all five hold.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/game/ai.c
git commit -m "feat(fire): M8/F1 suppress vanilla move + attacks while burning

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (F1 row of the M8 spec):** "Burn registry + 10 Hz DOT tick + fire particles/light/smoke + STIM_FIRE emission. MCP-ignite a monster → it takes DOT, panics, others avoid."
- Burn registry → Task 1 (struct) + Task 2 (Ignite/Extinguish/query). ✓
- 10 Hz DOT tick → Task 2 (`Fire_Frame` + `FIRE_DMG_INTERVAL`). ✓
- Fire particles → Task 3. ✓
- Light → Task 3 (`EF_DIMLIGHT`; spec assigns `Lightmap_AddDelta` to F2 static patches, so EF_DIMLIGHT-only here is consistent). ✓
- Smoke → Task 3 (`Wind_AddSmoke`). ✓
- `STIM_FIRE` emission → Task 4. ✓
- MCP-ignite → Task 2 (`fire_ignite_num` cvar) + impulse 210. ✓
- Panics → Tasks 5–6. ✓
- Others avoid → Task 5 (`Fire_NearestHazard` + flee). ✓
- No ABI bump → confirmed: all additions are in `sim.h`/`sim_fire.c`/DLL-side; `GAME_API_VERSION` untouched. ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code; tunables are real cvars with defaults.

**Type/name consistency:** `Fire_Ignite/Extinguish/IsBurning/GetIgniterOrigin/NearestHazard/IgniteTraced/Init/LevelInit/Frame` declared in Task 1 match every call site (Tasks 2–6). `fire_burn_t`, `s_burning`, `fire_clear_slot`, `fire_find_edict`, `fire_crand` are all defined before use. `b->burning` added in Task 1, set in Task 5, read in Tasks 5–6. `STIM_FIRE` added in Task 1, emitted in Task 4, handled in Task 4's sense case.

**Known limitation (carried forward):** AI "avoid" is flee-if-near (`FIRE_AI_AVOID_RADIUS`), not A* re-routing — exactly as the spec scoped for MVP. Static-patch fire→lightmap (the "fire reveals you" sense effect) arrives with oil patches in F2.

---

## Execution handoff

Plan complete and saved to `docs/superpowers/plans/2026-05-30-fire-and-oil-m8-f1-burn-primitive.md`. Two execution options:

1. **Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — execute tasks in this session using executing-plans, batch execution with checkpoints.

Which approach?

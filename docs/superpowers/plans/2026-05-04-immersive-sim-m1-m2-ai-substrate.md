# Immersive-Sim AI Substrate (M1 + M2 + M2.5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the AI substrate from the immersive-sim spec — stimulus bus, 4-state FSM, BSP-baked navmesh — so monsters investigate noise, hunt to last-known position, and give up. End state: in a procedural arena, fire a shot, watch a grunt patrol → suspect → search a path to the noise → time out → resume patrol.

**Architecture:** All new code lives inside the hot-reloadable `game.dll` under `sdlquake/game/sim/`. Modules communicate through `sim.h` types and a central stimulus ring buffer; they never `#include` each other directly. No `engine_api_t` changes — `SV_Traceline`, `SV_WalkMove`, `ED_GetNum`, etc. are already exposed.

**Tech Stack:** C (gnu99 already configured for game DLL via `-std=c11`), Zig 0.16 build, Dear ImGui for debug overlays, no test framework — verification is `zig build` + in-game observation via imgui panels and a new `sim_*` console-command family.

**Verification model:** This codebase has no xUnit test suite. Each task ends with a concrete verification step: "build succeeds", "type `sim_dump_stims` in console, see 3 lines printed", "fire shotgun, imgui AI panel shows alert level rise on grunt #3". Treat verification steps as non-negotiable acceptance criteria — don't skip them.

**Workflow note:** All commits go directly to `master` (per project convention; no PRs/branches). Use `zig build run -Dnative_game=true -- +map start` to launch in single-player on the start map. `zig build game -Dnative_game=true` rebuilds only the DLL (hot-reload picks it up live).

---

## File Structure

**Created:**
- `sdlquake/game/sim/sim.h` — public types, bus API, AI brain struct, navmesh handle
- `sdlquake/game/sim/sim_stimulus.c` — ring buffer, `Stim_Emit`, `Stim_QueryNear`
- `sdlquake/game/sim/sim_ai.c` — brain side-table, sense filter, FSM, patrol routes
- `sdlquake/game/sim/sim_nav.c` — BSP walkable extraction, navmesh bake, A*, disk cache
- `sdlquake/game/sim/sim_arena.c` — `sim_arena` console command spawning a test arena
- `sdlquake/engine/imgui_ai_panel.c` — imgui overlay reading sim state via a small read-only API

**Modified:**
- `sdlquake/game/game_main.c` — call `Sim_Init` / `Sim_Frame` / `Sim_LevelInit`
- `sdlquake/game/spawn.c` — register `info_patrol_node` classname
- `sdlquake/game/combat.c` — emit `STIM_CORPSE` + `STIM_SOUND` on death; `STIM_SOUND` on damage
- `sdlquake/game/weapons.c` — emit `STIM_SOUND` on each weapon fire path
- `sdlquake/game/ai.c` — replace monster `ai_run`/`ai_stand` calls' default behavior with `Sim_AI_Tick` (gated by FSM state)
- `sdlquake/engine/imgui_layer.c` — add the "AI" panel to the panel list
- `build.zig` — add the new `.c` files to the game DLL module

**No engine ABI change in this plan.** `engine_api_t` and `GAME_API_VERSION` are untouched.

---

## Conventions used by every task

- All new public symbols are prefixed `Sim_*`. Private statics may use `s_` prefix.
- All new cvars start with `sim_`.
- All new console commands start with `sim_`.
- Side-table indexing: use `eng->ED_GetNum(e)` to key into `ai_brain_t brains[MAX_EDICTS]`. `MAX_EDICTS` is 600 in stock Quake (`quakedef.h`); use the same constant by adding `#define SIM_MAX_BRAINS 600` to `sim.h`.
- Time source: `g->time` (game-globals time, frame-synced) — *not* `eng->Sys_FloatTime`, which is wall-clock.
- LOS check: `eng->SV_Traceline(a, b, /*nomonsters=*/1, /*skip=*/NULL)`; success = `g->trace_fraction == 1.0f`.

---

## Phase A — Foundations

### Task 1: Create `sim.h` with shared types

**Files:**
- Create: `sdlquake/game/sim/sim.h`

- [ ] **Step 1:** Create directory `sdlquake/game/sim/`.

```sh
mkdir sdlquake/game/sim
```

- [ ] **Step 2:** Create `sdlquake/game/sim/sim.h` with full content:

```c
// sim.h -- Shared types and inter-module API for the immersive-sim layer.
// All sim/*.c files include only this header (and game_api.h / game_types.h).

#ifndef SIM_H
#define SIM_H

#include "game_api.h"
#include "game_types.h"

#define SIM_MAX_BRAINS         600    // matches engine MAX_EDICTS
#define SIM_STIM_RING_SIZE     512
#define SIM_STIM_MAX_AGE_S     5.0f
#define SIM_AI_TICK_HZ         10.0f

// ---------------------------------------------------------------------------
// Stimulus bus
// ---------------------------------------------------------------------------
typedef enum {
    STIM_NONE = 0,
    STIM_SOUND,
    STIM_SIGHT_ENTITY,
    STIM_SMOKE,
    STIM_LIGHT_CHANGE,
    STIM_CORPSE,
    STIM_PROP_BROKEN,
} stim_kind_t;

typedef struct {
    stim_kind_t kind;
    vec3_t      origin;
    float       intensity;     // 0..1 at the source
    float       time;          // g->time at emission
    int         source_edict;  // edict number (-1 = world)
    int         flags;
} stimulus_t;

void Stim_Init(void);
void Stim_LevelInit(void);                 // clears bus on map change
void Stim_Emit(const stimulus_t *s);
int  Stim_QueryNear(const vec3_t pos,
                    float radius,
                    float since_time,
                    stimulus_t *out,
                    int max_out);

// ---------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------
typedef enum {
    AI_IDLE = 0,
    AI_SUSPICIOUS,
    AI_SEARCHING,
    AI_COMBAT,
} ai_state_t;

typedef struct {
    int          in_use;             // 0 if this slot is unused
    int          edict_num;          // engine entity number
    ai_state_t   state;
    float        state_entered_time;
    float        alert_level;        // 0..1
    vec3_t       last_known_pos;
    int          target_edict;       // -1 if none
    int          patrol_route_id;    // -1 if none
    int          patrol_node_idx;
    float        sense_sight_range;  // base 1024
    float        sense_hearing_mult; // base 1.0
    float        next_tick_time;
} ai_brain_t;

void        Sim_AI_Init(void);
void        Sim_AI_LevelInit(void);
void        Sim_AI_Frame(void);                              // called once/frame
ai_brain_t *Sim_AI_GetBrain(edict_t *e);                     // returns NULL if not registered
ai_brain_t *Sim_AI_RegisterMonster(edict_t *e);              // idempotent
void        Sim_AI_UnregisterByEdictNum(int edict_num);

// Read-only iterator for the imgui overlay (returns NULL when done).
ai_brain_t *Sim_AI_IterFirst(void);
ai_brain_t *Sim_AI_IterNext(ai_brain_t *prev);

// ---------------------------------------------------------------------------
// Patrol routes
// ---------------------------------------------------------------------------
void Sim_Patrol_RegisterNode(edict_t *e);   // called from spawn dispatch
void Sim_Patrol_Resolve(void);              // links targets at level start

// ---------------------------------------------------------------------------
// Navmesh
// ---------------------------------------------------------------------------
typedef struct sim_navmesh_s sim_navmesh_t;

void           Sim_Nav_Init(void);
void           Sim_Nav_LevelInit(const char *mapname);  // kicks off bake or load
int            Sim_Nav_IsReady(void);                   // 0 while baking
sim_navmesh_t *Sim_Nav_Get(void);                       // NULL if not ready

// Path query: fills out[] with up to max_out vec3 waypoints.
// Returns the number of waypoints written, or 0 if no path.
int Sim_Nav_PathTo(const vec3_t from,
                   const vec3_t to,
                   vec3_t *out,
                   int max_out);

// ---------------------------------------------------------------------------
// Top-level lifecycle
// ---------------------------------------------------------------------------
void Sim_Init(void);                       // once on DLL init
void Sim_LevelInit(const char *mapname);   // on each map load
void Sim_Frame(void);                      // each game start_frame

#endif // SIM_H
```

- [ ] **Step 3:** Verify compile of just this header is wired correctly later (no build step yet — header isn't included anywhere).

- [ ] **Step 4:** Commit.

```sh
git add sdlquake/game/sim/sim.h
git commit -m "sim: add shared header with stimulus, AI, navmesh APIs"
```

---

### Task 2: Create `sim_stimulus.c` ring buffer

**Files:**
- Create: `sdlquake/game/sim/sim_stimulus.c`

- [ ] **Step 1:** Create the file with full content:

```c
// sim_stimulus.c -- Central event bus. All systems emit; AI consumes.

#include "sim.h"
#include <math.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static stimulus_t s_ring[SIM_STIM_RING_SIZE];
static int        s_head;        // next write index
static int        s_count;       // number of valid entries (<= ring size)

void Stim_Init(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_head  = 0;
    s_count = 0;
}

void Stim_LevelInit(void) {
    Stim_Init();
}

void Stim_Emit(const stimulus_t *s) {
    if (!s) return;
    s_ring[s_head] = *s;
    s_ring[s_head].time = g->time;
    s_head = (s_head + 1) % SIM_STIM_RING_SIZE;
    if (s_count < SIM_STIM_RING_SIZE) s_count++;
}

static float vec_dist(const vec3_t a, const vec3_t b) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return (float)sqrt(dx*dx + dy*dy + dz*dz);
}

int Stim_QueryNear(const vec3_t pos,
                   float radius,
                   float since_time,
                   stimulus_t *out,
                   int max_out)
{
    if (!out || max_out <= 0) return 0;

    int written = 0;
    for (int i = 0; i < s_count && written < max_out; i++) {
        // Walk newest-to-oldest so the caller sees most-recent first.
        int idx = (s_head - 1 - i + SIM_STIM_RING_SIZE) % SIM_STIM_RING_SIZE;
        const stimulus_t *s = &s_ring[idx];
        if (s->time < since_time) continue;
        if (g->time - s->time > SIM_STIM_MAX_AGE_S) continue;
        if (vec_dist(s->origin, pos) > radius) continue;
        out[written++] = *s;
    }
    return written;
}
```

- [ ] **Step 2:** Commit.

```sh
git add sdlquake/game/sim/sim_stimulus.c
git commit -m "sim: implement stimulus ring buffer (Stim_Emit/QueryNear)"
```

---

### Task 3: Add sim files to `build.zig`

**Files:**
- Modify: `build.zig` (around line 197 — the game DLL `.files` array)

- [ ] **Step 1:** In `build.zig`, locate the `game_mod.addCSourceFiles({ .files = &.{ ... } })` block (currently ends with `"sdlquake/game/monster_oldone.c"`). Add the four new entries before the closing `}`:

```zig
            "sdlquake/game/monster_oldone.c",
            // Immersive-sim systems (M1 + M2 + M2.5)
            "sdlquake/game/sim/sim_stimulus.c",
            "sdlquake/game/sim/sim_ai.c",
            "sdlquake/game/sim/sim_nav.c",
            "sdlquake/game/sim/sim_arena.c",
```

- [ ] **Step 2:** The files don't all exist yet — to keep the build green, create empty placeholder `.c` files now.

```sh
printf '#include "sim.h"\n' > sdlquake/game/sim/sim_ai.c
printf '#include "sim.h"\n' > sdlquake/game/sim/sim_nav.c
printf '#include "sim.h"\n' > sdlquake/game/sim/sim_arena.c
```

- [ ] **Step 3:** Verify build.

```sh
zig build -Dnative_game=true 2>&1 | tail -20
```

Expected: build succeeds. If it fails because `sim.h` references types from `game_types.h` that aren't visible in this include path, double-check `game_mod.addIncludePath(b.path("sdlquake/game"))` is present (it is, at build.zig line 162).

- [ ] **Step 4:** Commit.

```sh
git add build.zig sdlquake/game/sim/
git commit -m "sim: wire stimulus + placeholder modules into build"
```

---

### Task 4: Top-level `Sim_*` lifecycle in `game_main.c`

**Files:**
- Modify: `sdlquake/game/game_main.c`

Implement `Sim_Init`, `Sim_LevelInit`, `Sim_Frame` as one-line dispatchers in a new `sim/sim_main.c`, and call them from `game_init` / `game_start_frame` / map-load.

- [ ] **Step 1:** Create `sdlquake/game/sim/sim_main.c`:

```c
// sim_main.c -- Top-level lifecycle dispatcher for the sim layer.

#include "sim.h"

void Sim_Init(void) {
    Stim_Init();
    Sim_AI_Init();
    Sim_Nav_Init();
}

void Sim_LevelInit(const char *mapname) {
    Stim_LevelInit();
    Sim_AI_LevelInit();
    Sim_Nav_LevelInit(mapname);
    Sim_Patrol_Resolve();
}

void Sim_Frame(void) {
    Sim_AI_Frame();
}
```

- [ ] **Step 2:** Add `sim_main.c` to `build.zig` next to the other sim files:

```zig
            "sdlquake/game/sim/sim_main.c",
            "sdlquake/game/sim/sim_stimulus.c",
            ...
```

- [ ] **Step 3:** Modify `sdlquake/game/game_main.c` — at the top, add include:

```c
#include "sim/sim.h"
```

- [ ] **Step 4:** In `game_init`, after the existing `g = globals;`, add:

```c
    Sim_Init();
```

- [ ] **Step 5:** Modify `game_start_frame` (currently `static void game_start_frame(void) { StartFrame(); }`) to:

```c
static void game_start_frame(void) { Sim_Frame(); StartFrame(); }
```

- [ ] **Step 6:** Hook level init. There is no dedicated "level loaded" entry point in `game_api_t`. Use a latch on `g->mapname` inside `game_entity_spawn` — the engine calls this for every entity during `SV_SpawnServer`, *before* any `StartFrame`, so triggering on the first spawn of a new map runs LevelInit *before* patrol nodes register.

Open `sdlquake/game/spawn.c`. Find the body of `game_entity_spawn` (top of file). Add as the first lines of the function body:

```c
    static const char *s_last_mapname = (const char *)1;  // sentinel
    if (g->mapname != s_last_mapname) {
        s_last_mapname = g->mapname;
        Sim_LevelInit(g->mapname ? g->mapname : "");
    }
```

Add `#include "sim/sim.h"` at the top of `spawn.c` if not already present.

The latch fires exactly once per map load (on the first entity spawned, typically `worldspawn`), clearing sim state *before* `info_patrol_node` and monster entities spawn.

- [ ] **Step 7:** Stub the empty `sim_ai.c`, `sim_nav.c`, `sim_arena.c` files with empty function bodies so linking succeeds. Replace each with:

`sim_ai.c`:
```c
#include "sim.h"
void        Sim_AI_Init(void) {}
void        Sim_AI_LevelInit(void) {}
void        Sim_AI_Frame(void) {}
ai_brain_t *Sim_AI_GetBrain(edict_t *e) { (void)e; return 0; }
ai_brain_t *Sim_AI_RegisterMonster(edict_t *e) { (void)e; return 0; }
void        Sim_AI_UnregisterByEdictNum(int n) { (void)n; }
ai_brain_t *Sim_AI_IterFirst(void) { return 0; }
ai_brain_t *Sim_AI_IterNext(ai_brain_t *p) { (void)p; return 0; }
void        Sim_Patrol_RegisterNode(edict_t *e) { (void)e; }
void        Sim_Patrol_Resolve(void) {}
```

`sim_nav.c`:
```c
#include "sim.h"
void           Sim_Nav_Init(void) {}
void           Sim_Nav_LevelInit(const char *m) { (void)m; }
int            Sim_Nav_IsReady(void) { return 0; }
sim_navmesh_t *Sim_Nav_Get(void) { return 0; }
int Sim_Nav_PathTo(const vec3_t a, const vec3_t b, vec3_t *o, int n) {
    (void)a; (void)b; (void)o; (void)n; return 0;
}
```

`sim_arena.c`:
```c
#include "sim.h"
// commands wired in Task 17
```

- [ ] **Step 8:** Build.

```sh
zig build -Dnative_game=true 2>&1 | tail -10
```

Expected: success.

- [ ] **Step 9:** Run.

```sh
zig build run -Dnative_game=true -- +map start
```

Expected: game launches, no crash, plays normally. The sim layer is silently active but has no visible effect yet.

- [ ] **Step 10:** Commit.

```sh
git add build.zig sdlquake/game/sim/sim_main.c sdlquake/game/sim/sim_ai.c sdlquake/game/sim/sim_nav.c sdlquake/game/sim/sim_arena.c sdlquake/game/game_main.c sdlquake/game/spawn.c
git commit -m "sim: wire Sim_Init/LevelInit/Frame lifecycle into game DLL"
```

---

## Phase B — M1: Stimuli + sense filter (no FSM)

### Task 5: Emit `STIM_SOUND` on each weapon fire path

**Files:**
- Modify: `sdlquake/game/weapons.c`

Each weapon emits a stim with intensity matching its real-world loudness.

- [ ] **Step 1:** At the top of `weapons.c`, add:

```c
#include "sim/sim.h"
```

- [ ] **Step 2:** Define a small helper near the top of the file (after the includes):

```c
static void emit_weapon_sound(edict_t *shooter, float intensity) {
    stimulus_t s = {0};
    s.kind          = STIM_SOUND;
    s.origin[0]     = shooter->v.origin[0];
    s.origin[1]     = shooter->v.origin[1];
    s.origin[2]     = shooter->v.origin[2];
    s.intensity     = intensity;
    s.source_edict  = eng->ED_GetNum(shooter);
    Stim_Emit(&s);
}
```

- [ ] **Step 3:** Add an `emit_weapon_sound` call as the FIRST statement (after the local declarations) of each of these functions:

| Function | Intensity |
|---|---|
| `W_FireAxe` | 0.15 (melee swing) |
| `W_FireShotgun` | 0.7 |
| `W_FireSuperShotgun` | 0.85 |
| `W_FireRocket` | 0.9 |
| `W_FireGrenade` | 0.8 |
| `W_FireLightning` | 0.6 |
| `W_FireSpikes` (the public one at line ~621) | 0.55 |

For each, find the function with `grep -n 'W_Fire' sdlquake/game/weapons.c` and add `emit_weapon_sound(self, X);` as the first line where `self` is the global player edict. (Quake's weapons.c uses `self` as a file-scope alias for `g->self` — confirm by reading the top of the file. If it does not exist, use `g->self` directly.)

- [ ] **Step 4:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5:** Add a temporary debug `Con_Print` inside `Stim_Emit` to confirm emissions during testing — at the end of `Stim_Emit` in `sim_stimulus.c`:

```c
    extern engine_api_t *eng;
    char buf[128];
    snprintf(buf, sizeof(buf),
        "stim emit kind=%d origin=%.0f,%.0f,%.0f int=%.2f t=%.2f\n",
        s->kind, s->origin[0], s->origin[1], s->origin[2],
        s->intensity, g->time);
    eng->Con_Print(buf);
```

(Add `#include <stdio.h>` to `sim_stimulus.c`.)

- [ ] **Step 6:** Run, fire each weapon. Verify console prints one `stim emit kind=1` line per shot with sensible intensity.

```sh
zig build run -Dnative_game=true -- +map start
# in game: impulse 1..7 to switch weapons, click to fire each
```

- [ ] **Step 7:** Remove the debug `Con_Print` from `Stim_Emit`. We'll get visibility back via the imgui panel in Task 9.

- [ ] **Step 8:** Commit.

```sh
git add sdlquake/game/weapons.c sdlquake/game/sim/sim_stimulus.c
git commit -m "sim: emit STIM_SOUND on each weapon fire path"
```

---

### Task 6: Emit `STIM_CORPSE` and `STIM_SOUND` on monster death

**Files:**
- Modify: `sdlquake/game/combat.c`

`Killed` (combat.c:56) is the kill-resolution function for any entity that runs out of health. We emit two stims: a corpse marker and a death-noise sound.

- [ ] **Step 1:** At top of `combat.c`, add:

```c
#include "sim/sim.h"
```

- [ ] **Step 2:** Inside `Killed`, after the existing `g->self = targ;` (line ~59) and before any other logic, add:

```c
    // Emit death stims for the AI sense filter.
    {
        stimulus_t s = {0};
        s.origin[0] = targ->v.origin[0];
        s.origin[1] = targ->v.origin[1];
        s.origin[2] = targ->v.origin[2];
        s.source_edict = eng->ED_GetNum(targ);

        s.kind = STIM_CORPSE;
        s.intensity = 1.0f;
        Stim_Emit(&s);

        s.kind = STIM_SOUND;
        s.intensity = 0.5f;   // gurgling death rattle / body thud
        Stim_Emit(&s);
    }
```

- [ ] **Step 3:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 4:** Verify in-game later (Task 9 wires up visibility). For now, no behavioral change is visible.

- [ ] **Step 5:** Commit.

```sh
git add sdlquake/game/combat.c
git commit -m "sim: emit STIM_CORPSE + STIM_SOUND on monster death"
```

---

### Task 7: Brain side-table + monster registration

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c`

Replace the empty stubs from Task 4 with a real brain table.

- [ ] **Step 1:** Replace the entire contents of `sdlquake/game/sim/sim_ai.c` with:

```c
// sim_ai.c -- AI brain side-table, sense filter, FSM.
// State for each monster lives in s_brains[edict_num], not in edict_t.

#include "sim.h"
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static ai_brain_t s_brains[SIM_MAX_BRAINS];

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_AI_Init(void) {
    memset(s_brains, 0, sizeof(s_brains));
}

void Sim_AI_LevelInit(void) {
    memset(s_brains, 0, sizeof(s_brains));
    extern void Sim_Patrol_LevelInit_(void);
    Sim_Patrol_LevelInit_();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
ai_brain_t *Sim_AI_GetBrain(edict_t *e) {
    if (!e) return 0;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= SIM_MAX_BRAINS) return 0;
    if (!s_brains[n].in_use) return 0;
    return &s_brains[n];
}

ai_brain_t *Sim_AI_RegisterMonster(edict_t *e) {
    if (!e) return 0;
    int n = eng->ED_GetNum(e);
    if (n < 0 || n >= SIM_MAX_BRAINS) return 0;
    ai_brain_t *b = &s_brains[n];
    if (b->in_use) return b;
    memset(b, 0, sizeof(*b));
    b->in_use             = 1;
    b->edict_num          = n;
    b->state              = AI_IDLE;
    b->state_entered_time = g->time;
    b->target_edict       = -1;
    b->patrol_route_id    = -1;
    b->sense_sight_range  = 1024.0f;
    b->sense_hearing_mult = 1.0f;
    b->next_tick_time     = g->time;
    return b;
}

void Sim_AI_UnregisterByEdictNum(int n) {
    if (n < 0 || n >= SIM_MAX_BRAINS) return;
    s_brains[n].in_use = 0;
}

// ---------------------------------------------------------------------------
// Iteration (for the imgui panel)
// ---------------------------------------------------------------------------
ai_brain_t *Sim_AI_IterFirst(void) {
    for (int i = 0; i < SIM_MAX_BRAINS; i++)
        if (s_brains[i].in_use) return &s_brains[i];
    return 0;
}

ai_brain_t *Sim_AI_IterNext(ai_brain_t *prev) {
    if (!prev) return 0;
    int i = (int)(prev - s_brains) + 1;
    for (; i < SIM_MAX_BRAINS; i++)
        if (s_brains[i].in_use) return &s_brains[i];
    return 0;
}

// ---------------------------------------------------------------------------
// Sense filter and FSM are added in later tasks. For now a no-op tick.
// ---------------------------------------------------------------------------
void Sim_AI_Frame(void) {}

// Stubs for patrol routes, filled in Task 14.
void Sim_Patrol_RegisterNode(edict_t *e) { (void)e; }
void Sim_Patrol_Resolve(void) {}
```

- [ ] **Step 2:** Build and run.

```sh
zig build -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 3:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c
git commit -m "sim: brain side-table with register/lookup/iter API"
```

---

### Task 8: Auto-register monsters on first think

**Files:**
- Modify: `sdlquake/game/monsters.c`

We need a hook that runs once per monster, when it's "alive in the world". The cleanest place is `walkmonster_start_go` (and `flymonster_start_go`, `swimmonster_start_go`), which are called from each monster's spawn function after a short delay.

- [ ] **Step 1:** Find the entry points:

```sh
grep -n "walkmonster_start_go\|flymonster_start_go\|swimmonster_start_go" sdlquake/game/monsters.c
```

- [ ] **Step 2:** Add `#include "sim/sim.h"` to the top of `monsters.c` if absent.

- [ ] **Step 3:** In each of `walkmonster_start_go`, `flymonster_start_go`, `swimmonster_start_go`, add as the first statement of the function body:

```c
    Sim_AI_RegisterMonster(g->self);
```

- [ ] **Step 4:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5:** Run, load `start.bsp`, immediately quit. We'll verify registration via the imgui panel in Task 9.

- [ ] **Step 6:** Commit.

```sh
git add sdlquake/game/monsters.c
git commit -m "sim: auto-register monsters on spawn-go"
```

---

### Task 9: imgui AI panel (read-only overlay)

**Files:**
- Create: `sdlquake/engine/imgui_ai_panel.c`
- Modify: `sdlquake/engine/imgui_layer.c`
- Modify: `build.zig`

The panel reads sim state via the `Sim_AI_IterFirst/Next` API. Because the sim lives in `game.dll`, we need a minimal cross-module hook: `imgui_ai_panel.c` resolves the iter functions by name from the loaded DLL through the existing hot-reload mechanism.

But there's a simpler path: *don't* go through the DLL boundary. Move the brain table into a small piece of state shared via `imgui_support.h` — i.e., the engine's `imgui_support.c` exposes a getter that the game DLL writes into each frame. This avoids importing DLL symbols.

- [ ] **Step 1:** Add to `sdlquake/engine/imgui_support.h` (find with `grep -n 'IMGUI_SUPPORT_H' sdlquake/engine/imgui_support.h`):

```c
// AI dev panel shared state (written by game DLL each frame, read by imgui layer).
#define IMGUI_AI_MAX_ROWS 64
typedef struct {
    int          edict_num;
    int          state;          // ai_state_t value
    float        alert_level;
    float        last_known_pos[3];
    int          target_edict;
} imgui_ai_row_t;

void   ImguiSupport_AI_Clear(void);
void   ImguiSupport_AI_Push(const imgui_ai_row_t *row);
int    ImguiSupport_AI_Count(void);
const  imgui_ai_row_t *ImguiSupport_AI_Row(int i);
```

- [ ] **Step 2:** Implement those in `sdlquake/engine/imgui_support.c` (find with `grep -n 'ImguiSupport_GetCvarList' sdlquake/engine/imgui_support.c` to find the file). Append at the bottom:

```c
static imgui_ai_row_t s_ai_rows[IMGUI_AI_MAX_ROWS];
static int            s_ai_count;

void ImguiSupport_AI_Clear(void) { s_ai_count = 0; }

void ImguiSupport_AI_Push(const imgui_ai_row_t *row) {
    if (s_ai_count >= IMGUI_AI_MAX_ROWS) return;
    s_ai_rows[s_ai_count++] = *row;
}

int ImguiSupport_AI_Count(void) { return s_ai_count; }

const imgui_ai_row_t *ImguiSupport_AI_Row(int i) {
    if (i < 0 || i >= s_ai_count) return 0;
    return &s_ai_rows[i];
}
```

- [ ] **Step 3:** The DLL needs to call these. The engine API doesn't expose `ImguiSupport_AI_*` yet. Add three new entries to `engine_api_t` in `sdlquake/game/game_api.h` — bumping `GAME_API_VERSION`.

```c
#define GAME_API_VERSION 5
```

In `engine_api_t`, before the closing brace, add:

```c
    // imgui dev panels (no-op if imgui inactive)
    void  (*ImguiAI_Clear)(void);
    void  (*ImguiAI_Push)(int edict_num, int state, float alert_level,
                          const vec3_t last_known_pos, int target_edict);
    int   (*ImguiAI_Active)(void);   // returns 1 if the panel is visible
```

- [ ] **Step 4:** Implement these on the engine side. Find the `engine_api_t` instantiation:

```sh
grep -rn "Cvar_VariableValue *=" sdlquake/engine/ | head -5
```

This is in `sdlquake/engine/sv_bridge.c` (or `hotreload.c`). Open that file. Find the table that initializes `engine_api_t` (look for `Con_Print` assignment). Append three new entries:

```c
    .ImguiAI_Clear  = ImguiSupport_AI_Clear,
    .ImguiAI_Push   = imgui_ai_push_shim,
    .ImguiAI_Active = imgui_ai_active_shim,
```

Above the table, define the two shims:

```c
static void imgui_ai_push_shim(int edict_num, int state, float alert_level,
                               const vec3_t last_known_pos, int target_edict)
{
    imgui_ai_row_t r;
    r.edict_num = edict_num;
    r.state = state;
    r.alert_level = alert_level;
    r.last_known_pos[0] = last_known_pos[0];
    r.last_known_pos[1] = last_known_pos[1];
    r.last_known_pos[2] = last_known_pos[2];
    r.target_edict = target_edict;
    ImguiSupport_AI_Push(&r);
}

extern int imgui_ai_panel_open;  // defined in imgui_layer.c
static int imgui_ai_active_shim(void) { return imgui_ai_panel_open; }
```

Add `#include "imgui_support.h"` to that file's includes.

- [ ] **Step 5:** Add the AI panel rendering. Open `sdlquake/engine/imgui_layer.c` and add a new panel function near `draw_perf`:

```c
int imgui_ai_panel_open = 1;

static const char *ai_state_name(int s) {
    switch (s) {
        case 0: return "idle";
        case 1: return "suspect";
        case 2: return "search";
        case 3: return "combat";
        default: return "?";
    }
}

static void draw_ai(void)
{
    if (!imgui_ai_panel_open) return;
    IG_SetNextWindowSize(420, 280, IG_Cond_Once);
    IG_SetNextWindowPos(10, 80, IG_Cond_Once);
    if (!IG_Begin("AI", &imgui_ai_panel_open, IG_WF_None)) { IG_End(); return; }

    int n = ImguiSupport_AI_Count();
    char buf[160];
    snprintf(buf, sizeof(buf), "%d brains", n);
    IG_TextUnformatted(buf);

    if (IG_BeginTable("##ai", 4,
            IG_TF_Borders | IG_TF_RowBg | IG_TF_ScrollY, 0, -1))
    {
        IG_TableSetupColumn("ent",     IG_TCF_WidthFixed, 40);
        IG_TableSetupColumn("state",   IG_TCF_WidthFixed, 70);
        IG_TableSetupColumn("alert",   IG_TCF_WidthFixed, 60);
        IG_TableSetupColumn("target",  IG_TCF_WidthFixed, 60);
        IG_TableHeadersRow();
        for (int i = 0; i < n; i++) {
            const imgui_ai_row_t *r = ImguiSupport_AI_Row(i);
            if (!r) continue;
            IG_TableNextRow();
            IG_TableSetColumnIndex(0);
            snprintf(buf, sizeof(buf), "%d", r->edict_num);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(1);
            IG_TextUnformatted(ai_state_name(r->state));
            IG_TableSetColumnIndex(2);
            snprintf(buf, sizeof(buf), "%.2f", r->alert_level);
            IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(3);
            snprintf(buf, sizeof(buf), "%d", r->target_edict);
            IG_TextUnformatted(buf);
        }
        IG_EndTable();
    }
    IG_End();
}
```

Add `#include "imgui_support.h"` at the top if not already present.

- [ ] **Step 6:** Wire `draw_ai` into the panel render list. Find the function in `imgui_layer.c` that calls `draw_perf` — it'll be something like `ImguiLayer_Render` or `imgui_layer_frame`. Add `draw_ai();` next to `draw_perf();`.

- [ ] **Step 7:** In the game DLL, push rows each frame. In `sdlquake/game/sim/sim_ai.c`'s `Sim_AI_Frame`:

```c
void Sim_AI_Frame(void) {
    if (!eng->ImguiAI_Active || !eng->ImguiAI_Active()) return;
    eng->ImguiAI_Clear();
    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        eng->ImguiAI_Push(b->edict_num, (int)b->state, b->alert_level,
                          b->last_known_pos, b->target_edict);
    }
}
```

- [ ] **Step 8:** Build (engine + DLL — engine ABI changed).

```sh
zig build -Dnative_game=true 2>&1 | tail -10
```

Expected: success. Note: bumped `GAME_API_VERSION` requires a clean rebuild of the DLL; this is automatic since `game_api.h` changed.

- [ ] **Step 9:** Run.

```sh
zig build run -Dnative_game=true -- +map start
# in game: press F12 to open imgui dev overlay
```

Expected: AI panel visible, lists brains for each monster on `start.bsp`. Alert levels are all 0.0 (sense filter not yet implemented).

- [ ] **Step 10:** Commit.

```sh
git add sdlquake/game/game_api.h sdlquake/game/sim/sim_ai.c sdlquake/engine/imgui_support.c sdlquake/engine/imgui_support.h sdlquake/engine/imgui_layer.c sdlquake/engine/sv_bridge.c sdlquake/engine/hotreload.c
git commit -m "sim: imgui AI panel shows live brain table"
```

(The two engine files modified depend on which one holds the `engine_api_t` initializer — adjust the `git add` to match what you actually edited.)

---

### Task 10: Sense filter — accumulate alert from stimuli

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c`

Implement the per-tick filter described in the spec, but for M1 it only *logs* alerts (no FSM transitions yet — those land in Task 12).

- [ ] **Step 1:** In `sim_ai.c`, replace `Sim_AI_Frame` (still just push to imgui — extend it) and add the filter:

```c
static float falloff(float distance, float ref_radius) {
    // Linear falloff to zero at 2× ref radius. Cheap and tunable.
    float f = 1.0f - (distance / (2.0f * ref_radius));
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

static int los_clear(const vec3_t a, const vec3_t b) {
    eng->SV_Traceline(a, b, /*nomonsters=*/1, /*skip=*/0);
    return g->trace_fraction == 1.0f;
}

static float sense_intensity(ai_brain_t *b, edict_t *e, const stimulus_t *s) {
    float d = 0;
    {
        float dx = s->origin[0] - e->v.origin[0];
        float dy = s->origin[1] - e->v.origin[1];
        float dz = s->origin[2] - e->v.origin[2];
        d = (float)sqrt(dx*dx + dy*dy + dz*dz);
    }
    float ref = 0;
    float los = 1.0f;
    switch (s->kind) {
        case STIM_SOUND:
            ref = 1024.0f * b->sense_hearing_mult; break;
        case STIM_SIGHT_ENTITY:
            ref = b->sense_sight_range;
            los = los_clear(e->v.origin, s->origin) ? 1.0f : 0.0f;
            break;
        case STIM_CORPSE:
            ref = 512.0f;
            los = los_clear(e->v.origin, s->origin) ? 1.0f : 0.0f;
            break;
        case STIM_PROP_BROKEN:
            ref = 768.0f; break;
        case STIM_LIGHT_CHANGE:
            ref = 384.0f; break;
        case STIM_SMOKE:
            ref = b->sense_sight_range; break;
        default:
            return 0.0f;
    }
    return s->intensity * falloff(d, ref) * los;
}

static void sense_tick(ai_brain_t *b, edict_t *e) {
    stimulus_t recents[16];
    float since = b->next_tick_time - 1.0f;     // 1s lookback overlap
    int n = Stim_QueryNear(e->v.origin,
                           b->sense_sight_range * 2.0f,
                           since, recents, 16);

    for (int i = 0; i < n; i++) {
        // Don't react to your own emissions.
        if (recents[i].source_edict == b->edict_num) continue;
        float eff = sense_intensity(b, e, &recents[i]);
        b->alert_level += eff * 0.5f;            // 0.5 = aggregate scale
    }

    // Decay
    b->alert_level *= 0.95f;
    if (b->alert_level < 0)   b->alert_level = 0;
    if (b->alert_level > 1.0f) b->alert_level = 1.0f;
}
```

- [ ] **Step 2:** Update `Sim_AI_Frame` to drive the filter at 10Hz per brain:

```c
void Sim_AI_Frame(void) {
    float now = g->time;

    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (now < b->next_tick_time) continue;
        // Look up the edict for this brain.
        // Engine doesn't expose ED_AtNum, so iterate via ED_Next.
        // Walk from world (edict 0).
        edict_t *e = 0;
        edict_t *world = g->world;
        for (edict_t *cur = eng->ED_Next(world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == b->edict_num) { e = cur; break; }
        }
        if (!e) { b->in_use = 0; continue; }
        if (e->v.health <= 0) {
            b->state = AI_IDLE;
            b->alert_level = 0;
            continue;
        }
        sense_tick(b, e);
        b->next_tick_time = now + (1.0f / SIM_AI_TICK_HZ);
    }

    if (eng->ImguiAI_Active && eng->ImguiAI_Active()) {
        eng->ImguiAI_Clear();
        for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
            eng->ImguiAI_Push(b->edict_num, (int)b->state, b->alert_level,
                              b->last_known_pos, b->target_edict);
        }
    }
}
```

Add to top of file: `#include <math.h>`.

- [ ] **Step 3:** The `ED_Next` walk above is O(N²) per frame. Acceptable for M1 (N ~30). Flag for optimization later.

- [ ] **Step 4:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# in game: F12 to open AI panel.
# walk close to a zombie (don't attack), fire shotgun into the air.
# Expected: alert_level on nearby zombies rises to ~0.3-0.6, decays over a few seconds.
```

- [ ] **Step 6:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c
git commit -m "sim: sense filter accumulates alert level (M1 complete)"
```

---

## Phase C — M2: FSM live + procedural arena

### Task 11: 4-state FSM transitions

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c`

- [ ] **Step 1:** Add transition logic at the bottom of `sense_tick`:

```c
    // Track strongest sight stim this tick for COMBAT/SEARCHING bookkeeping.
    int  saw_player_full = 0;
    vec3_t player_pos = {0, 0, 0};

    for (int i = 0; i < n; i++) {
        if (recents[i].source_edict == b->edict_num) continue;
        if (recents[i].kind != STIM_SIGHT_ENTITY) continue;
        if (recents[i].source_edict != 1) continue;   // edict 1 = player in single-player
        float eff = sense_intensity(b, e, &recents[i]);
        if (eff > 0.7f) {
            saw_player_full = 1;
            player_pos[0] = recents[i].origin[0];
            player_pos[1] = recents[i].origin[1];
            player_pos[2] = recents[i].origin[2];
        }
    }

    // Update last_known_pos when the strongest stim is sight of player.
    if (saw_player_full) {
        b->last_known_pos[0] = player_pos[0];
        b->last_known_pos[1] = player_pos[1];
        b->last_known_pos[2] = player_pos[2];
        b->target_edict = 1;
    }

    // FSM transitions
    float since_state = g->time - b->state_entered_time;
    ai_state_t prev = b->state;

    switch (b->state) {
    case AI_IDLE:
        if (b->alert_level > 0.25f) b->state = AI_SUSPICIOUS;
        break;
    case AI_SUSPICIOUS:
        if (saw_player_full || b->alert_level > 0.6f) b->state = AI_SEARCHING;
        else if (n == 0 && since_state > 8.0f && b->alert_level < 0.05f) b->state = AI_IDLE;
        break;
    case AI_SEARCHING:
        if (saw_player_full && los_clear(e->v.origin, b->last_known_pos))
            b->state = AI_COMBAT;
        else if (n == 0 && since_state > 20.0f) b->state = AI_IDLE;
        break;
    case AI_COMBAT:
        if (!saw_player_full && since_state > 3.0f) b->state = AI_SEARCHING;
        break;
    }

    if (b->state != prev) b->state_entered_time = g->time;
```

(Move the closing brace of `sense_tick` to after this block.)

- [ ] **Step 2:** Emit `STIM_SIGHT_ENTITY` for the player. The simplest place: in `world.c`'s `StartFrame` (which already runs every server tick) emit a sight stim from the player's current position. The sense filter does its own LOS check, so we don't need per-monster trace here.

Add `#include "sim/sim.h"` to the top of `world.c` if not already present. Then add to the body of `StartFrame`:

```c
    if (g->world && g->time > 0) {
        // Find the player edict (entity 1 in single-player).
        edict_t *player = 0;
        for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
            if (eng->ED_GetNum(cur) == 1) { player = cur; break; }
        }
        if (player && player->v.health > 0) {
            stimulus_t s = {0};
            s.kind = STIM_SIGHT_ENTITY;
            s.origin[0] = player->v.origin[0];
            s.origin[1] = player->v.origin[1];
            s.origin[2] = player->v.origin[2];
            s.intensity = 1.0f;
            s.source_edict = 1;
            Stim_Emit(&s);
        }
    }
```

- [ ] **Step 3:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 4:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# In game, F12 to open AI panel. Walk into LOS of a monster.
# Expected: state transitions IDLE → SUSPICIOUS → SEARCHING → COMBAT.
# Walk back behind a wall: stays COMBAT for 3s, then drops to SEARCHING.
```

- [ ] **Step 5:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c sdlquake/game/world.c
git commit -m "sim: 4-state FSM with sight-of-player stim"
```

---

### Task 12: SUSPICIOUS / SEARCHING behavior (stand-and-sweep fallback)

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c`

For M2 (no navmesh yet), SEARCHING walks toward `last_known_pos` using `SV_WalkMove` directly — no pathing — and gives up after 20s. SUSPICIOUS just turns to face the stim and slow-walks 96 units toward it.

- [ ] **Step 1:** Add to `sim_ai.c`, after `sense_tick`:

```c
static void face_point(edict_t *e, const vec3_t target) {
    float dx = target[0] - e->v.origin[0];
    float dy = target[1] - e->v.origin[1];
    vec3_t v = {dx, dy, 0};
    e->v.ideal_yaw = eng->VectorToYaw(v);
    eng->SV_ChangeYaw(e);
}

static void behavior_tick(ai_brain_t *b, edict_t *e) {
    switch (b->state) {
    case AI_IDLE:
        // Fall through to vanilla Quake AI (handled elsewhere).
        break;
    case AI_SUSPICIOUS:
    case AI_SEARCHING:
        face_point(e, b->last_known_pos);
        // Slow walk toward last_known_pos. SV_WalkMove takes yaw + dist.
        eng->SV_WalkMove(e, e->v.angles[1], 8.0f);
        break;
    case AI_COMBAT:
        // Fall through to vanilla Quake combat AI.
        break;
    }
}
```

- [ ] **Step 2:** Call `behavior_tick(b, e)` at the end of the `for (ai_brain_t *b ...)` loop body in `Sim_AI_Frame`, *after* `sense_tick` returns.

- [ ] **Step 3:** Gate vanilla Quake monster movement so it doesn't fight us. In `sdlquake/game/ai.c`, find `ai_stand`, `ai_walk`, and `ai_run`. At the top of each, add:

```c
    {
        ai_brain_t *b = Sim_AI_GetBrain(g->self);
        if (b && (b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING))
            return;   // sim layer is driving movement this tick
    }
```

Add `#include "sim/sim.h"` at the top of `ai.c`.

- [ ] **Step 4:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

- [ ] **Step 5:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# Fire a shot from cover near a zombie (out of LOS). Watch on AI panel:
# zombie alert rises, state → SUSPICIOUS, then → SEARCHING, walks toward you, gives up.
```

- [ ] **Step 6:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c sdlquake/game/ai.c
git commit -m "sim: SUSPICIOUS/SEARCHING behavior (stand-and-sweep)"
```

---

### Task 13: `info_patrol_node` entity classname

**Files:**
- Modify: `sdlquake/game/spawn.c`
- Modify: `sdlquake/game/sim/sim_ai.c`

A patrol node is just an info-style entity with `target` / `targetname`. It does not render and does not collide — just sits in the world graph.

- [ ] **Step 1:** In `sdlquake/game/sim/sim_ai.c`, add (after the brain functions):

```c
// ---------------------------------------------------------------------------
// Patrol routes — cyclic linked list of info_patrol_node entities.
// Resolution happens once at level-init via target/targetname matching.
// ---------------------------------------------------------------------------
#define SIM_MAX_PATROL_NODES 256
static edict_t *s_patrol_nodes[SIM_MAX_PATROL_NODES];
static int      s_patrol_count;

void Sim_Patrol_RegisterNode(edict_t *e) {
    if (s_patrol_count >= SIM_MAX_PATROL_NODES) return;
    s_patrol_nodes[s_patrol_count++] = e;
}

void Sim_Patrol_LevelInit_(void) {
    s_patrol_count = 0;
    for (int i = 0; i < SIM_MAX_PATROL_NODES; i++) s_patrol_nodes[i] = 0;
}

void Sim_Patrol_Resolve(void) {
    // Reserved for a future graph-build pass (e.g. precomputing patrol cycles
    // for the retrofit phase). For M2, nodes are looked up by targetname via
    // ED_Find when needed, and registration happens at spawn-time.
}

edict_t *Sim_Patrol_FindByTargetname(const char *name) {
    if (!name || !*name) return 0;
    for (int i = 0; i < s_patrol_count; i++) {
        edict_t *e = s_patrol_nodes[i];
        if (!e) continue;
        // Use ED_Find for string-field comparison — handles string_t indirection.
        // ED_Find walks all entities though; keep linear search here on the cached array.
        // For now, defer to ED_Find for correctness:
    }
    edict_t *r = eng->ED_Find(g->world, "targetname", name);
    return r;
}
```

- [ ] **Step 2:** Add `Sim_Patrol_FindByTargetname` to `sim.h`:

```c
edict_t *Sim_Patrol_FindByTargetname(const char *name);
```

- [ ] **Step 3:** Add the spawn function. In `spawn.c`, add a new spawn handler before the `s_spawns[]` table:

```c
void spawn_info_patrol_node(edict_t *e) {
    e->v.solid    = SOLID_NOT;
    e->v.movetype = MOVETYPE_NONE;
    eng->SV_SetSize(e, (vec3_t){-8, -8, -8}, (vec3_t){8, 8, 8});
    Sim_Patrol_RegisterNode(e);
}
```

Add to the `s_spawns[]` table (in alphabetical position):

```c
    { "info_patrol_node", spawn_info_patrol_node },
```

Add `#include "sim/sim.h"` to `spawn.c` if not present.

- [ ] **Step 4:** Patrol-route *behavior* (walking the route) lands in Task 15. For now we just have placement and resolution.

- [ ] **Step 5:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 6:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c sdlquake/game/sim/sim.h sdlquake/game/spawn.c
git commit -m "sim: info_patrol_node spawn func + registration table"
```

---

### Task 14: Patrol behavior in IDLE state

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c`

Monsters with `b->patrol_route_id >= 0` walk a chain of nodes; arriving within 32 units advances to the next node.

- [ ] **Step 1:** Replace the IDLE branch in `behavior_tick`:

```c
    case AI_IDLE: {
        if (b->patrol_route_id < 0) break;
        // Look up current node.
        char buf[32];
        snprintf(buf, sizeof(buf), "patrol_%d_%d", b->patrol_route_id, b->patrol_node_idx);
        edict_t *node = Sim_Patrol_FindByTargetname(buf);
        if (!node) break;
        float dx = node->v.origin[0] - e->v.origin[0];
        float dy = node->v.origin[1] - e->v.origin[1];
        float dz = node->v.origin[2] - e->v.origin[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < 32*32) {
            b->patrol_node_idx++;
            // Wrap if next node not found.
            snprintf(buf, sizeof(buf), "patrol_%d_%d", b->patrol_route_id, b->patrol_node_idx);
            if (!Sim_Patrol_FindByTargetname(buf)) b->patrol_node_idx = 0;
            break;
        }
        face_point(e, node->v.origin);
        eng->SV_WalkMove(e, e->v.angles[1], 12.0f);
        return;     // suppress vanilla AI walk this tick
    }
```

For this to suppress vanilla AI, gate `ai_walk` / `ai_stand` in `ai.c` to also bail when `b->state == AI_IDLE && b->patrol_route_id >= 0`. Update Task 12's gate:

```c
    {
        ai_brain_t *b = Sim_AI_GetBrain(g->self);
        if (b && (b->state == AI_SUSPICIOUS || b->state == AI_SEARCHING ||
                  (b->state == AI_IDLE && b->patrol_route_id >= 0)))
            return;
    }
```

- [ ] **Step 2:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

- [ ] **Step 3:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c sdlquake/game/ai.c
git commit -m "sim: patrol-route IDLE behavior (walk to next node, wrap)"
```

---

### Task 15: `sim_arena` console command (procedural test arena)

**Files:**
- Modify: `sdlquake/game/sim/sim_arena.c`
- Modify: `sdlquake/game/sim/sim.h`

Spawn a 1024-unit cube of patrol nodes with two soldier-grunts assigned to a route. Triggered by `sim_arena` console command. The arena spawns *at the current map's player position*, so it's a debug overlay on whatever map you're on.

- [ ] **Step 1:** Add to `sim.h`:

```c
void Sim_Arena_Init(void);    // registers console command
void Sim_Arena_Spawn(void);   // invoked by command
```

- [ ] **Step 2:** Replace `sim_arena.c` contents with:

```c
// sim_arena.c -- Procedural test arena spawned via the `sim_arena` console command.

#include "sim.h"
#include <stdio.h>
#include <string.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

static void spawn_patrol_node(int route, int idx, vec3_t pos) {
    edict_t *e = eng->ED_Alloc();
    e->v.classname = (string_t)0;   // unused
    e->v.origin[0] = pos[0];
    e->v.origin[1] = pos[1];
    e->v.origin[2] = pos[2];
    e->v.solid    = SOLID_NOT;
    e->v.movetype = MOVETYPE_NONE;
    char buf[32];
    snprintf(buf, sizeof(buf), "patrol_%d_%d", route, idx);
    // Set targetname via ED_Find won't work; if string_t requires engine alloc,
    // skip name and store the (route,idx) inside a per-node side-table.
    // For arena: keep a static array.
    static struct { int route, idx; edict_t *e; } s_arena_nodes[64];
    static int s_arena_n;
    if (s_arena_n < 64) {
        s_arena_nodes[s_arena_n].route = route;
        s_arena_nodes[s_arena_n].idx   = idx;
        s_arena_nodes[s_arena_n].e     = e;
        s_arena_n++;
    }
    Sim_Patrol_RegisterNode(e);
    eng->SV_SetOrigin(e, pos);
}

void Sim_Arena_Spawn(void) {
    // Find player.
    edict_t *player = 0;
    for (edict_t *cur = eng->ED_Next(g->world); cur; cur = eng->ED_Next(cur)) {
        if (eng->ED_GetNum(cur) == 1) { player = cur; break; }
    }
    if (!player) { eng->Con_Print("sim_arena: no player\n"); return; }

    vec3_t base = { player->v.origin[0], player->v.origin[1], player->v.origin[2] };

    // 4 patrol nodes in a 512-unit square.
    vec3_t nodes[4] = {
        { base[0] + 256, base[1] + 256, base[2] },
        { base[0] - 256, base[1] + 256, base[2] },
        { base[0] - 256, base[1] - 256, base[2] },
        { base[0] + 256, base[1] - 256, base[2] },
    };
    for (int i = 0; i < 4; i++) spawn_patrol_node(/*route=*/0, i, nodes[i]);

    // Spawn 2 soldiers and assign to route 0.
    for (int i = 0; i < 2; i++) {
        edict_t *m = eng->ED_Alloc();
        eng->SV_SetOrigin(m, nodes[i*2]);
        // Use the existing classname dispatch.
        extern void game_entity_spawn(edict_t *e, const char *classname);
        game_entity_spawn(m, "monster_army");
        // Register and assign route.
        ai_brain_t *b = Sim_AI_RegisterMonster(m);
        if (b) {
            b->patrol_route_id  = 0;
            b->patrol_node_idx  = i*2;
        }
    }
    eng->Con_Print("sim_arena: spawned\n");
}

// Console command registration is engine-side. The engine api doesn't currently
// expose Cmd_AddCommand. For M2, trigger via a cvar latch:
// `sim_arena_go 1` in console runs Spawn once and resets to 0.

void Sim_Arena_Init(void) {
    eng->Cvar_SetValue("sim_arena_go", 0.0f);
}

// Called from Sim_AI_Frame's tail (added in next step) to poll the latch.
void Sim_Arena_Poll(void) {
    if (eng->Cvar_VariableValue("sim_arena_go") > 0.5f) {
        eng->Cvar_SetValue("sim_arena_go", 0.0f);
        Sim_Arena_Spawn();
    }
}
```

- [ ] **Step 3:** Add to `sim.h`:

```c
void Sim_Arena_Poll(void);
```

- [ ] **Step 4:** In `sim_main.c`, add `Sim_Arena_Init()` to `Sim_Init` and `Sim_Arena_Poll()` to `Sim_Frame`:

```c
void Sim_Init(void) {
    Stim_Init();
    Sim_AI_Init();
    Sim_Nav_Init();
    Sim_Arena_Init();
}

void Sim_Frame(void) {
    Sim_AI_Frame();
    Sim_Arena_Poll();
}
```

- [ ] **Step 5:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success. If `string_t` casts complain, replace `e->v.classname = (string_t)0;` with `e->v.classname = 0;` matching the existing pattern in `spawn.c`.

- [ ] **Step 6:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# In game console: sim_arena_go 1
# Expected: two soldier-grunts appear near you and start patrolling a square.
# Open AI panel (F12). State = idle, patrol_route_id = 0.
# Fire a shot. Their state → SUSPICIOUS, then → SEARCHING, walk toward you.
```

- [ ] **Step 7:** Commit.

```sh
git add sdlquake/game/sim/sim_arena.c sdlquake/game/sim/sim.h sdlquake/game/sim/sim_main.c
git commit -m "sim: sim_arena_go cvar spawns a 4-node patrol arena (M2 complete)"
```

---

## Phase D — M2.5: Navmesh bake + A*

### Task 16: BSP walkable-surface extraction

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c`

We need access to BSP face data. Quake's BSP file format is documented in `Quake-master/WinQuake/model.h`. Faces have a plane normal and a list of vertex indices. The engine has already loaded these into memory; the loaded model lives at `cl.worldmodel` (engine side). Game DLL doesn't have direct access — but it can load the BSP file from disk independently, since Quake's `id1/maps/<name>.bsp` is just a file.

- [ ] **Step 1:** Add a BSP reader to `sim_nav.c`. Replace contents with:

```c
// sim_nav.c -- BSP walkable-surface extraction, navmesh bake, A* pathfinding.

#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// BSP file structures (from Quake-master/WinQuake/bspfile.h).
// We re-declare locally to avoid including the engine header.
// ---------------------------------------------------------------------------
#define BSPVERSION 29

typedef struct { int fileofs, filelen; } bsp_lump_t;
typedef struct {
    int        version;
    bsp_lump_t entities, planes, miptex, vertexes, visibility, nodes,
               texinfo, faces, lighting, clipnodes, leafs, marksurfaces,
               edges, surfedges, models;
} bsp_header_t;

typedef struct { float point[3]; } bsp_vertex_t;
typedef struct { unsigned short v[2]; } bsp_edge_t;
typedef struct {
    short  planenum;
    short  side;
    int    firstedge;
    short  numedges;
    short  texinfo;
    unsigned char styles[4];
    int    lightofs;
} bsp_face_t;

typedef struct {
    float normal[3];
    float dist;
    int   type;
} bsp_plane_t;

// ---------------------------------------------------------------------------
// Walkable points
// ---------------------------------------------------------------------------
typedef struct {
    vec3_t pos;
} nav_point_t;

typedef struct {
    int   from, to;
    float weight;
} nav_edge_t;

struct sim_navmesh_s {
    nav_point_t *points;
    int          point_count;
    nav_edge_t  *edges;
    int          edge_count;
    int         *adj_offsets;     // CSR: offsets into adj
    int         *adj;             // CSR: edge indices
};

static sim_navmesh_t *s_mesh;
static int            s_ready;

// ---------------------------------------------------------------------------
// Step 1: read the BSP, extract walkable face centers.
// ---------------------------------------------------------------------------
static int read_file(const char *path, void **out_data, int *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);
    *out_data = buf;
    *out_size = (int)sz;
    return 1;
}

static int extract_walkable_points(const char *bsp_path,
                                   nav_point_t **out_points,
                                   int *out_count)
{
    void *raw;
    int   size;
    if (!read_file(bsp_path, &raw, &size)) return 0;

    bsp_header_t *h = raw;
    if (h->version != BSPVERSION) { free(raw); return 0; }

    bsp_vertex_t *verts = (bsp_vertex_t *)((char *)raw + h->vertexes.fileofs);
    int nverts = h->vertexes.filelen / sizeof(bsp_vertex_t);

    bsp_edge_t *edges = (bsp_edge_t *)((char *)raw + h->edges.fileofs);
    int *surfedges   = (int *)((char *)raw + h->surfedges.fileofs);

    bsp_face_t  *faces  = (bsp_face_t  *)((char *)raw + h->faces.fileofs);
    int nfaces = h->faces.filelen / sizeof(bsp_face_t);

    bsp_plane_t *planes = (bsp_plane_t *)((char *)raw + h->planes.fileofs);

    // First pass: count walkable faces.
    int cap = 1024, n = 0;
    nav_point_t *pts = malloc(sizeof(nav_point_t) * cap);

    for (int fi = 0; fi < nfaces; fi++) {
        const bsp_face_t *f = &faces[fi];
        const bsp_plane_t *p = &planes[f->planenum];
        // Plane normal flips with side.
        float nx = p->normal[0], ny = p->normal[1], nz = p->normal[2];
        if (f->side) { nx = -nx; ny = -ny; nz = -nz; }
        if (nz < 0.7f) continue;     // not walkable

        // Compute face centroid.
        float cx = 0, cy = 0, cz = 0;
        int   nv = 0;
        for (int e = 0; e < f->numedges; e++) {
            int   se   = surfedges[f->firstedge + e];
            int   vidx = (se >= 0) ? edges[ se].v[0] : edges[-se].v[1];
            if (vidx < 0 || vidx >= nverts) continue;
            cx += verts[vidx].point[0];
            cy += verts[vidx].point[1];
            cz += verts[vidx].point[2];
            nv++;
        }
        if (nv == 0) continue;
        cx /= nv; cy /= nv; cz /= nv;

        if (n == cap) { cap *= 2; pts = realloc(pts, sizeof(nav_point_t) * cap); }
        pts[n].pos[0] = cx;
        pts[n].pos[1] = cy;
        pts[n].pos[2] = cz + 16.0f;   // lift to standing height
        n++;
    }

    free(raw);
    *out_points = pts;
    *out_count  = n;
    return 1;
}

// Stubs for path query — implemented in Task 19.
int Sim_Nav_PathTo(const vec3_t a, const vec3_t b, vec3_t *o, int n) {
    (void)a; (void)b; (void)o; (void)n;
    return 0;
}

void           Sim_Nav_Init(void) { s_mesh = 0; s_ready = 0; }
int            Sim_Nav_IsReady(void) { return s_ready; }
sim_navmesh_t *Sim_Nav_Get(void) { return s_mesh; }

// LevelInit triggers bake — implemented in Task 18.
void Sim_Nav_LevelInit(const char *mapname) {
    (void)mapname;
}
```

- [ ] **Step 2:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -10
```

Expected: success. May warn about unused `extract_walkable_points`; ignore.

- [ ] **Step 3:** Commit.

```sh
git add sdlquake/game/sim/sim_nav.c
git commit -m "sim: BSP walkable-face extraction (face centroids, normal-up)"
```

---

### Task 17: Edge generation via `walkmove` probes

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c`

Connect every pair of walkable points within ~96 units if `SV_WalkMove` succeeds between them. Builds a CSR adjacency for fast A*.

- [ ] **Step 1:** Add to `sim_nav.c` after `extract_walkable_points`:

```c
// Need an entity to probe with. Allocate once, reuse, free at end.
static edict_t *alloc_probe(void) {
    edict_t *e = eng->ED_Alloc();
    e->v.movetype = MOVETYPE_STEP;
    e->v.solid    = SOLID_SLIDEBOX;
    eng->SV_SetSize(e, (vec3_t){-16, -16, -24}, (vec3_t){16, 16, 32});
    return e;
}

static int build_edges(sim_navmesh_t *m) {
    int   cap = 4096, n = 0;
    nav_edge_t *e = malloc(sizeof(nav_edge_t) * cap);

    edict_t *probe = alloc_probe();

    for (int i = 0; i < m->point_count; i++) {
        eng->SV_SetOrigin(probe, m->points[i].pos);
        if (!eng->SV_DropToFloor(probe)) continue;

        for (int j = 0; j < m->point_count; j++) {
            if (i == j) continue;
            float dx = m->points[i].pos[0] - m->points[j].pos[0];
            float dy = m->points[i].pos[1] - m->points[j].pos[1];
            float dz = m->points[i].pos[2] - m->points[j].pos[2];
            float d  = (float)sqrt(dx*dx + dy*dy + dz*dz);
            if (d > 96.0f) continue;
            // Probe walkmove from i to j.
            eng->SV_SetOrigin(probe, m->points[i].pos);
            float yaw = (float)(atan2(-dy, -dx) * 180.0 / 3.14159265358979);
            int ok = eng->SV_WalkMove(probe, yaw, d);
            if (!ok) continue;

            if (n == cap) { cap *= 2; e = realloc(e, sizeof(nav_edge_t) * cap); }
            e[n].from   = i;
            e[n].to     = j;
            e[n].weight = d;
            n++;
        }
    }

    eng->ED_Free(probe);
    m->edges      = e;
    m->edge_count = n;
    return 1;
}

static void build_adjacency(sim_navmesh_t *m) {
    m->adj_offsets = calloc(m->point_count + 1, sizeof(int));
    for (int i = 0; i < m->edge_count; i++) m->adj_offsets[m->edges[i].from + 1]++;
    for (int i = 1; i <= m->point_count; i++) m->adj_offsets[i] += m->adj_offsets[i-1];

    int *cursor = calloc(m->point_count, sizeof(int));
    m->adj = malloc(sizeof(int) * m->edge_count);
    for (int i = 0; i < m->edge_count; i++) {
        int from = m->edges[i].from;
        m->adj[m->adj_offsets[from] + cursor[from]++] = i;
    }
    free(cursor);
}
```

- [ ] **Step 2:** Build. Expect warnings (unused functions). They go away in Task 18.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

- [ ] **Step 3:** Commit.

```sh
git add sdlquake/game/sim/sim_nav.c
git commit -m "sim: navmesh edge-build via walkmove probes + CSR adjacency"
```

---

### Task 18: Bake driver + disk cache

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c`
- Modify: `sdlquake/game/sim/sim.h`

Wire the pieces together: `Sim_Nav_LevelInit` triggers bake on the main thread (synchronous for v1 — a worker thread is "open question for the implementation plan" in the spec, defer). Cache the result on disk keyed by mapname + BSP file size.

- [ ] **Step 1:** Replace `Sim_Nav_LevelInit` and add helpers:

```c
static void free_mesh(sim_navmesh_t *m) {
    if (!m) return;
    free(m->points);
    free(m->edges);
    free(m->adj_offsets);
    free(m->adj);
    free(m);
}

static int save_mesh(const char *path, const sim_navmesh_t *m) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int magic = 0x4E41564D;     // 'NAVM'
    int ver   = 1;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);
    fwrite(&m->point_count, 4, 1, f);
    fwrite(&m->edge_count,  4, 1, f);
    fwrite(m->points, sizeof(nav_point_t), m->point_count, f);
    fwrite(m->edges,  sizeof(nav_edge_t),  m->edge_count, f);
    fclose(f);
    return 1;
}

static sim_navmesh_t *load_mesh(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int magic, ver, np, ne;
    fread(&magic, 4, 1, f); fread(&ver, 4, 1, f);
    fread(&np, 4, 1, f);    fread(&ne, 4, 1, f);
    if (magic != 0x4E41564D || ver != 1) { fclose(f); return 0; }
    sim_navmesh_t *m = calloc(1, sizeof(*m));
    m->point_count = np;
    m->edge_count  = ne;
    m->points = malloc(sizeof(nav_point_t) * np);
    m->edges  = malloc(sizeof(nav_edge_t)  * ne);
    fread(m->points, sizeof(nav_point_t), np, f);
    fread(m->edges,  sizeof(nav_edge_t),  ne, f);
    fclose(f);
    build_adjacency(m);
    return m;
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

void Sim_Nav_LevelInit(const char *mapname) {
    if (s_mesh) { free_mesh(s_mesh); s_mesh = 0; }
    s_ready = 0;
    if (!mapname || !*mapname) return;

    char bsp_path[256];
    char nav_path[256];
    snprintf(bsp_path, sizeof(bsp_path), "id1/maps/%s.bsp", mapname);
    long bsp_sz = file_size(bsp_path);
    if (bsp_sz < 0) {
        eng->Con_Print("sim_nav: bsp not found\n");
        return;
    }
    snprintf(nav_path, sizeof(nav_path),
             "id1/cache/navmesh/%s-%ld.nav", mapname, bsp_sz);

    // Try cache.
    s_mesh = load_mesh(nav_path);
    if (s_mesh) {
        char buf[160];
        snprintf(buf, sizeof(buf), "sim_nav: loaded %d pts %d edges from cache\n",
                 s_mesh->point_count, s_mesh->edge_count);
        eng->Con_Print(buf);
        s_ready = 1;
        return;
    }

    // Bake from scratch.
    eng->Con_Print("sim_nav: baking...\n");
    s_mesh = calloc(1, sizeof(*s_mesh));
    if (!extract_walkable_points(bsp_path, &s_mesh->points, &s_mesh->point_count)) {
        eng->Con_Print("sim_nav: extract failed\n");
        free_mesh(s_mesh); s_mesh = 0; return;
    }
    build_edges(s_mesh);
    build_adjacency(s_mesh);

    // Make cache directory and save (best-effort).
    {
        // Cross-platform mkdir of id1/cache/navmesh — use system().
        // Fine for v1; replace with platform-portable mkdir later.
        system("mkdir -p id1/cache/navmesh 2>/dev/null || mkdir id1\\cache\\navmesh 2>nul");
    }
    save_mesh(nav_path, s_mesh);

    char buf[160];
    snprintf(buf, sizeof(buf), "sim_nav: baked %d pts %d edges\n",
             s_mesh->point_count, s_mesh->edge_count);
    eng->Con_Print(buf);
    s_ready = 1;
}
```

- [ ] **Step 2:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 3:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# Console output should show "sim_nav: baked N pts M edges" on first run.
# Quit and re-run; should show "sim_nav: loaded N pts M edges from cache".
ls id1/cache/navmesh/
```

Expected: `start-<size>.nav` exists.

- [ ] **Step 4:** Commit.

```sh
git add sdlquake/game/sim/sim_nav.c
git commit -m "sim: bake driver + disk cache (mapname + bsp-size keyed)"
```

---

### Task 19: A* path query

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c`

- [ ] **Step 1:** Replace the stub `Sim_Nav_PathTo` with:

```c
// ---------------------------------------------------------------------------
// A* — small open-set, no priority queue. O(N²) for v1; fine for ~1k nodes.
// ---------------------------------------------------------------------------
static int nearest_point(const sim_navmesh_t *m, const vec3_t pos) {
    int   best = -1;
    float best_d2 = 1e18f;
    for (int i = 0; i < m->point_count; i++) {
        float dx = m->points[i].pos[0] - pos[0];
        float dy = m->points[i].pos[1] - pos[1];
        float dz = m->points[i].pos[2] - pos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static float dist3(const vec3_t a, const vec3_t b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)sqrt(dx*dx + dy*dy + dz*dz);
}

int Sim_Nav_PathTo(const vec3_t from, const vec3_t to, vec3_t *out, int max_out) {
    if (!s_mesh || !s_ready) return 0;
    int start = nearest_point(s_mesh, from);
    int goal  = nearest_point(s_mesh, to);
    if (start < 0 || goal < 0) return 0;
    if (start == goal) {
        if (max_out >= 1) {
            out[0][0] = s_mesh->points[goal].pos[0];
            out[0][1] = s_mesh->points[goal].pos[1];
            out[0][2] = s_mesh->points[goal].pos[2];
        }
        return 1;
    }

    int N = s_mesh->point_count;
    float *gscore = malloc(sizeof(float) * N);
    float *fscore = malloc(sizeof(float) * N);
    int   *came   = malloc(sizeof(int)   * N);
    char  *open   = calloc(N, 1);
    char  *closed = calloc(N, 1);
    for (int i = 0; i < N; i++) { gscore[i] = 1e18f; fscore[i] = 1e18f; came[i] = -1; }

    gscore[start] = 0;
    fscore[start] = dist3(s_mesh->points[start].pos, s_mesh->points[goal].pos);
    open[start] = 1;

    int found = 0;
    while (1) {
        int   cur  = -1;
        float bf = 1e18f;
        for (int i = 0; i < N; i++) {
            if (!open[i]) continue;
            if (fscore[i] < bf) { bf = fscore[i]; cur = i; }
        }
        if (cur < 0) break;
        if (cur == goal) { found = 1; break; }
        open[cur]   = 0;
        closed[cur] = 1;

        int o0 = s_mesh->adj_offsets[cur];
        int o1 = s_mesh->adj_offsets[cur + 1];
        for (int k = o0; k < o1; k++) {
            const nav_edge_t *e = &s_mesh->edges[s_mesh->adj[k]];
            int nb = e->to;
            if (closed[nb]) continue;
            float tentative = gscore[cur] + e->weight;
            if (tentative < gscore[nb]) {
                came[nb]   = cur;
                gscore[nb] = tentative;
                fscore[nb] = tentative + dist3(s_mesh->points[nb].pos, s_mesh->points[goal].pos);
                open[nb]   = 1;
            }
        }
    }

    int written = 0;
    if (found) {
        // Reconstruct path: walk came[] backwards into a temp buffer, reverse.
        int  tmp[1024];
        int  tn = 0;
        int  c  = goal;
        while (c != -1 && tn < 1024) { tmp[tn++] = c; c = came[c]; }
        // Reverse and copy to out.
        for (int i = tn - 1; i >= 0 && written < max_out; i--) {
            out[written][0] = s_mesh->points[tmp[i]].pos[0];
            out[written][1] = s_mesh->points[tmp[i]].pos[1];
            out[written][2] = s_mesh->points[tmp[i]].pos[2];
            written++;
        }
    }

    free(gscore); free(fscore); free(came); free(open); free(closed);
    return written;
}
```

- [ ] **Step 2:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 3:** Commit.

```sh
git add sdlquake/game/sim/sim_nav.c
git commit -m "sim: A* path query over the baked navmesh"
```

---

### Task 20: SEARCHING follows the navmesh path

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c`
- Modify: `sdlquake/game/sim/sim.h`

Add a per-brain path buffer. SEARCHING re-plans every 2 seconds and walks the next waypoint.

- [ ] **Step 1:** Extend `ai_brain_t` in `sim.h`:

```c
    // Navmesh path being walked (SEARCHING)
    vec3_t      path_pts[32];
    int         path_len;
    int         path_idx;
    float       path_replan_time;
```

- [ ] **Step 2:** Update `behavior_tick` in `sim_ai.c` — replace the SEARCHING branch:

```c
    case AI_SUSPICIOUS:
        face_point(e, b->last_known_pos);
        eng->SV_WalkMove(e, e->v.angles[1], 8.0f);
        break;
    case AI_SEARCHING: {
        // Replan if no path or every 2 seconds.
        if (b->path_len == 0 || g->time > b->path_replan_time) {
            b->path_len = Sim_Nav_PathTo(e->v.origin, b->last_known_pos,
                                         b->path_pts, 32);
            b->path_idx = 0;
            b->path_replan_time = g->time + 2.0f;
            if (b->path_len == 0) {
                // Fallback: stand-and-sweep at last_known_pos.
                face_point(e, b->last_known_pos);
                eng->SV_WalkMove(e, e->v.angles[1], 8.0f);
                break;
            }
        }
        if (b->path_idx >= b->path_len) {
            // Reached the destination — sweep until state times out.
            face_point(e, b->last_known_pos);
            break;
        }
        const float *next = b->path_pts[b->path_idx];
        float dx = next[0] - e->v.origin[0];
        float dy = next[1] - e->v.origin[1];
        if (dx*dx + dy*dy < 32*32) { b->path_idx++; break; }
        face_point(e, (vec3_t){ next[0], next[1], next[2] });
        eng->SV_WalkMove(e, e->v.angles[1], 12.0f);
    } break;
```

- [ ] **Step 3:** Reset path on state transition. In `sense_tick`'s FSM block, when `b->state` changes to `AI_SEARCHING`, set `b->path_len = 0;`. Find the FSM `switch` and add inside the `if (b->state != prev)` clause:

```c
    if (b->state != prev) {
        b->state_entered_time = g->time;
        if (b->state == AI_SEARCHING) {
            b->path_len = 0;
            b->path_idx = 0;
        }
    }
```

- [ ] **Step 4:** Build.

```sh
zig build game -Dnative_game=true 2>&1 | tail -5
```

Expected: success.

- [ ] **Step 5:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# console: sim_arena_go 1
# Move to a corner of the room, fire a shot, hide behind a wall before the grunts arrive.
# Expected: grunts navigate AROUND obstacles to your last known position.
# Without the navmesh, they'd get stuck against geometry.
```

- [ ] **Step 6:** Commit.

```sh
git add sdlquake/game/sim/sim_ai.c sdlquake/game/sim/sim.h
git commit -m "sim: SEARCHING walks navmesh path with 2s replan"
```

---

### Task 21: Debug-render navmesh + AI path in imgui

**Files:**
- Modify: `sdlquake/engine/imgui_support.h`
- Modify: `sdlquake/engine/imgui_support.c`
- Modify: `sdlquake/game/game_api.h`
- Modify: `sdlquake/engine/imgui_layer.c`
- Modify: `sdlquake/engine/sv_bridge.c` (or wherever the API table lives)
- Modify: `sdlquake/game/sim/sim_ai.c`
- Modify: `sdlquake/game/sim/sim_nav.c`

Add a "show navmesh" toggle that draws the points and edges as a 2D top-down minimap in the AI panel. Skip 3D in-world rendering for v1 — too much engine work — the minimap is enough to verify correctness.

- [ ] **Step 1:** Add to `imgui_support.h`:

```c
#define IMGUI_NAV_MAX_POINTS  4096
#define IMGUI_NAV_MAX_EDGES   16384
typedef struct { float x, y; }              imgui_nav_point_t;
typedef struct { unsigned short a, b; }     imgui_nav_edge_t;
typedef struct {
    int   has_path;
    int   path_len;
    float path_xy[64];   // up to 32 (x,y) pairs
} imgui_nav_active_t;

void ImguiSupport_Nav_Set(const imgui_nav_point_t *pts, int np,
                          const imgui_nav_edge_t *eds, int ne);
void ImguiSupport_Nav_SetPath(const imgui_nav_active_t *p);
int  ImguiSupport_Nav_Count(int *out_np, int *out_ne);
const imgui_nav_point_t  *ImguiSupport_Nav_Points(void);
const imgui_nav_edge_t   *ImguiSupport_Nav_Edges(void);
const imgui_nav_active_t *ImguiSupport_Nav_Path(void);
```

- [ ] **Step 2:** Implement in `imgui_support.c` (append):

```c
static imgui_nav_point_t  s_nav_pts[IMGUI_NAV_MAX_POINTS];
static imgui_nav_edge_t   s_nav_eds[IMGUI_NAV_MAX_EDGES];
static int                s_nav_np, s_nav_ne;
static imgui_nav_active_t s_nav_path;

void ImguiSupport_Nav_Set(const imgui_nav_point_t *pts, int np,
                          const imgui_nav_edge_t *eds, int ne) {
    if (np > IMGUI_NAV_MAX_POINTS) np = IMGUI_NAV_MAX_POINTS;
    if (ne > IMGUI_NAV_MAX_EDGES)  ne = IMGUI_NAV_MAX_EDGES;
    memcpy(s_nav_pts, pts, sizeof(imgui_nav_point_t) * np);
    memcpy(s_nav_eds, eds, sizeof(imgui_nav_edge_t)  * ne);
    s_nav_np = np;
    s_nav_ne = ne;
}

void ImguiSupport_Nav_SetPath(const imgui_nav_active_t *p) {
    s_nav_path = *p;
}

int ImguiSupport_Nav_Count(int *out_np, int *out_ne) {
    if (out_np) *out_np = s_nav_np;
    if (out_ne) *out_ne = s_nav_ne;
    return s_nav_np > 0;
}

const imgui_nav_point_t  *ImguiSupport_Nav_Points(void) { return s_nav_pts; }
const imgui_nav_edge_t   *ImguiSupport_Nav_Edges(void)  { return s_nav_eds; }
const imgui_nav_active_t *ImguiSupport_Nav_Path(void)   { return &s_nav_path; }
```

- [ ] **Step 3:** Add to `engine_api_t` in `game_api.h` (and bump version to 6):

```c
#define GAME_API_VERSION 6
...
    void  (*ImguiNav_Set)(const void *pts_xy, int np,
                          const void *edges_ushort_pairs, int ne);
    void  (*ImguiNav_SetPath)(const void *path_xy_floats, int n);
```

(Use `void*` to keep the engine-side imgui_support types out of `game_api.h`. Cast on both sides.)

- [ ] **Step 4:** Wire shims in the engine API table file (same place you added `ImguiAI_Push`):

```c
static void imgui_nav_set_shim(const void *pts_xy, int np,
                               const void *edges_ushort_pairs, int ne) {
    ImguiSupport_Nav_Set((const imgui_nav_point_t *)pts_xy, np,
                         (const imgui_nav_edge_t  *)edges_ushort_pairs, ne);
}

static void imgui_nav_setpath_shim(const void *path_xy_floats, int n) {
    imgui_nav_active_t p = {0};
    p.has_path = (n > 0);
    p.path_len = (n > 32) ? 32 : n;
    memcpy(p.path_xy, path_xy_floats, sizeof(float) * 2 * p.path_len);
    ImguiSupport_Nav_SetPath(&p);
}

// In the engine_api_t initializer:
    .ImguiNav_Set     = imgui_nav_set_shim,
    .ImguiNav_SetPath = imgui_nav_setpath_shim,
```

- [ ] **Step 5:** Push navmesh once on bake — at the end of `Sim_Nav_LevelInit`'s success path (both cache-load and fresh-bake paths), add:

```c
    {
        // Push to imgui as 2D xy-only.
        // 1 KB stack guard: if more than 4096 points, truncate.
        int np = s_mesh->point_count;
        if (np > 4096) np = 4096;
        float *xy = malloc(sizeof(float) * 2 * np);
        for (int i = 0; i < np; i++) {
            xy[2*i+0] = s_mesh->points[i].pos[0];
            xy[2*i+1] = s_mesh->points[i].pos[1];
        }
        unsigned short *eds = malloc(sizeof(unsigned short) * 2 * s_mesh->edge_count);
        int ne = 0;
        for (int i = 0; i < s_mesh->edge_count; i++) {
            int a = s_mesh->edges[i].from;
            int b = s_mesh->edges[i].to;
            if (a < 65536 && b < 65536) {
                eds[2*ne+0] = (unsigned short)a;
                eds[2*ne+1] = (unsigned short)b;
                ne++;
            }
        }
        eng->ImguiNav_Set(xy, np, eds, ne);
        free(xy); free(eds);
    }
```

- [ ] **Step 6:** Push the active path each frame — in `Sim_AI_Frame`, after the iter-push block:

```c
    // Push the path of the first SEARCHING brain (single-target debug).
    for (ai_brain_t *b = Sim_AI_IterFirst(); b; b = Sim_AI_IterNext(b)) {
        if (b->state == AI_SEARCHING && b->path_len > 0) {
            float xy[64];
            int n = b->path_len > 32 ? 32 : b->path_len;
            for (int i = 0; i < n; i++) {
                xy[2*i+0] = b->path_pts[i][0];
                xy[2*i+1] = b->path_pts[i][1];
            }
            eng->ImguiNav_SetPath(xy, n);
            return;
        }
    }
    // No active path.
    {
        float xy[2] = {0, 0};
        eng->ImguiNav_SetPath(xy, 0);
    }
```

- [ ] **Step 7:** Render in `imgui_layer.c`. In `draw_ai`, after the brain table, add:

```c
    int np, ne;
    ImguiSupport_Nav_Count(&np, &ne);
    snprintf(buf, sizeof(buf), "nav: %d pts %d edges", np, ne);
    IG_TextUnformatted(buf);

    // Minimap canvas: 256×256 pixels. Compute world AABB, fit in.
    if (np > 0) {
        const imgui_nav_point_t *pts = ImguiSupport_Nav_Points();
        float min_x = pts[0].x, max_x = pts[0].x, min_y = pts[0].y, max_y = pts[0].y;
        for (int i = 1; i < np; i++) {
            if (pts[i].x < min_x) min_x = pts[i].x;
            if (pts[i].x > max_x) max_x = pts[i].x;
            if (pts[i].y < min_y) min_y = pts[i].y;
            if (pts[i].y > max_y) max_y = pts[i].y;
        }
        float W = max_x - min_x, H = max_y - min_y;
        if (W < 1) W = 1; if (H < 1) H = 1;

        IG_BeginCanvas("nav_canvas", 256, 256);
        const imgui_nav_edge_t *eds = ImguiSupport_Nav_Edges();
        for (int i = 0; i < ne; i++) {
            float ax = (pts[eds[i].a].x - min_x) / W * 256.0f;
            float ay = (pts[eds[i].a].y - min_y) / H * 256.0f;
            float bx = (pts[eds[i].b].x - min_x) / W * 256.0f;
            float by = (pts[eds[i].b].y - min_y) / H * 256.0f;
            IG_CanvasLine(ax, ay, bx, by, 0xFF888888);
        }
        const imgui_nav_active_t *p = ImguiSupport_Nav_Path();
        if (p->has_path) {
            for (int i = 0; i + 1 < p->path_len; i++) {
                float ax = (p->path_xy[2*i+0] - min_x) / W * 256.0f;
                float ay = (p->path_xy[2*i+1] - min_y) / H * 256.0f;
                float bx = (p->path_xy[2*(i+1)+0] - min_x) / W * 256.0f;
                float by = (p->path_xy[2*(i+1)+1] - min_y) / H * 256.0f;
                IG_CanvasLine(ax, ay, bx, by, 0xFF00FF00);
            }
        }
        IG_EndCanvas();
    }
```

If `IG_BeginCanvas` / `IG_CanvasLine` / `IG_EndCanvas` don't exist on the bridge, add them — find existing canvas/line drawing in `imgui_bridge.cpp`. If not present, fall back to drawing as a list: `IG_TextUnformatted("(navmesh canvas not available)")` and verify only via the printed point/edge counts.

- [ ] **Step 8:** Build.

```sh
zig build -Dnative_game=true 2>&1 | tail -10
```

Expected: success. If canvas helpers are missing, accept the textual count as M2.5 verification.

- [ ] **Step 9:** Verify.

```sh
zig build run -Dnative_game=true -- +map start
# F12 to open AI panel.
# sim_arena_go 1
# Walk away from grunts, fire a shot, retreat behind a wall.
# Expected: green polyline appears in the navmesh canvas as a grunt searches.
```

- [ ] **Step 10:** Commit.

```sh
git add sdlquake/game/game_api.h sdlquake/engine/imgui_support.h sdlquake/engine/imgui_support.c sdlquake/engine/imgui_layer.c sdlquake/engine/sv_bridge.c sdlquake/game/sim/sim_ai.c sdlquake/game/sim/sim_nav.c
git commit -m "sim: navmesh + active path debug-render in imgui AI panel"
```

---

### Task 22: End-to-end demo verification

**Files:** None modified — manual verification + commit a docs note.

- [ ] **Step 1:** Run.

```sh
zig build run -Dnative_game=true -- +map start
```

- [ ] **Step 2:** Walk through this sequence and confirm each behavior:

| Action | Expected |
|---|---|
| Open AI panel (F12) | List of grunts/zombies on `start.bsp` with `state=idle` |
| `sim_arena_go 1` in console | Two soldiers spawn, start patrolling a square. AI panel rows show `patrol_route_id=0` |
| Stand in LOS of a soldier | State → `combat`, soldier turns to face you |
| Step behind cover | After 3s, state → `search`, alert decays slowly |
| Fire shotgun while hidden | Other soldier alert spikes; state → `suspect` then `search` |
| Wait 20s without LOS | All soldiers return to `idle`, resume patrol |
| Check `id1/cache/navmesh/start-*.nav` | File exists |
| Restart game | Console prints `sim_nav: loaded N pts M edges from cache` |

- [ ] **Step 3:** If any step fails, debug and re-run. Common issues:
  - **Patrol routes don't move:** `Sim_Patrol_FindByTargetname` falling back to `ED_Find` may not return the arena's procedurally-generated nodes if `targetname` isn't set correctly. Set it via a side-table instead — see Task 15.
  - **Navmesh has 0 edges:** the probe entity may be free'd while still in use. Move `eng->ED_Free(probe)` to the absolute end of bake, after `build_adjacency`.
  - **Game crashes on `sim_arena_go`:** likely classname dispatch — verify `monster_army` is registered in `s_spawns[]`.

- [ ] **Step 4:** Add a one-paragraph entry to `docs/superpowers/specs/2026-05-04-immersive-sim-systems-design.md` (M1/M2/M2.5 → "Done"). Find the "Build order" section and edit each milestone heading to add `✅ Done` next to M1, M2, M2.5.

```sh
# Manual edit — open file in your editor.
```

- [ ] **Step 5:** Commit the docs change and tag the milestone.

```sh
git add docs/superpowers/specs/2026-05-04-immersive-sim-systems-design.md
git commit -m "docs: mark M1/M2/M2.5 (immersive-sim AI substrate) complete"
git tag immersive-sim-m2.5
```

---

## Out of scope for this plan (future plans)

- M3: Blink + Gust + `CONTENTS_GRATE`
- M4: Wind/smoke field + LOS coupling
- M5: Light tier + torch extinguish
- M6: Retrofit pass on id1 maps
- M7: Bespoke mini-level

Each gets its own plan, dependent on this one being shipped.

## Open issues for the implementer to resolve in-flight

1. **Worker thread for navmesh bake.** The spec calls for it; this plan does it synchronously. If first-load on a large map exceeds 5s, lift the bake into a thread (use `SDL_CreateThread`).
2. **`string_t` indirection in classname / targetname.** The exact pattern depends on whether the codebase uses raw `char*` or QC-VM-style string offsets. Read existing accesses in `spawn.c` before committing to `Sim_Patrol_FindByTargetname`'s implementation.
3. **`monster_army` classname.** Confirmed to exist (`monster_soldier.c`). If you need a quieter test monster, use `monster_zombie` (slower, less aggressive).
4. **Grunt sound effects on alert.** Out of scope here — vanilla pain/sight sounds will play when COMBAT enters. If you want a "huh, what's that?" bark in SUSPICIOUS, use `eng->SV_StartSound` in the FSM transition handler.

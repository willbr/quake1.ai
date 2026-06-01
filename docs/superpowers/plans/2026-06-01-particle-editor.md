# Particle Editor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a data-driven particle-effect editor as a new editor mode (sibling to the Phase 7 map editor), with a generic emitter runtime, `.pcl` persistence, and an `engine_api` hook so gameplay can spawn authored effects by name.

**Architecture:** A new `editor_mode_t` vtable in the editor shell hosts Map mode (today's code, renamed) and Particle mode (new). A new engine renderer module `r_emitter.c` holds a name-keyed effect-def registry + a live-emitter instance pool; emitters spawn a new `pt_emitter` particle into the existing `free_particles` pool, integrated by `R_DrawParticles` and drawn by a new `D_DrawEmitterParticle`. Effects persist as Quake KV-block `.pcl` files parsed with `COM_Parse`. A new `engine_api` fn `SpawnParticleEffect(name, org, dir)` (ABI 36→37) lets the game DLL fire them.

**Tech Stack:** C (gnu89 for engine, modern C for engine/editor glue), Zig build, Dear ImGui via the `IG_*` bridge, SDL3.

**Spec:** `docs/superpowers/specs/2026-06-01-particle-editor-design.md`

---

## Verification cadence (read first)

This repo has **no unit-test suite** (per `CLAUDE.md`); verification is **build-clean + in-game behavior**. So each task's "test" steps are:

- **Compile gate:** `zig build` must succeed (builds engine **and** `game.dll`). This is the primary per-task gate.
- **Runtime check:** where a task produces observable behavior, run it and confirm (console command output, `r_emitter_active` cvar value, or a visible effect via the smoke-test rig).
- **Commit** after each task that builds clean.

The smoke-test rig (from project memory): launch `zig build run -- +map m7_skeleton`, MCP-teleport to `(380,0,40)` facing east, screenshot. Use it for the visual checks in Slices 2 and 4.

**Commit message footer for every commit in this plan:**
```
Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## File structure

**New files**
- `sdlquake/engine/editor/editor_mode.h` — `editor_mode_t` vtable type.
- `sdlquake/engine_src/r_emitter.h` — emitter data model + public runtime API.
- `sdlquake/engine_src/r_emitter.c` — registry, live-emitter pool, spawn, update, `.pcl` I/O.
- `sdlquake/engine/editor/edit_particle.c` — Particle editor mode (ImGui panels + preview).
- `id1/particles/campfire.pcl`, `id1/particles/spark_burst.pcl` — seed presets.

**Modified files**
- `sdlquake/engine_src/d_iface.h` — add `pt_emitter`; add `short def;` to `particle_t`.
- `sdlquake/engine_src/r_part.c` — extend `wind_drag_k[]`; add `pt_emitter` physics case; add `pt_emitter` draw dispatch; call `R_UpdateEmitters` + `R_EmitterInit`.
- `sdlquake/engine_src/d_part.c` — add `D_DrawEmitterParticle`.
- `sdlquake/engine_src/r_main.c` — register `r_emitter_active` cvar (with the other particle cvars).
- `sdlquake/engine/editor/editor.c` — mode registry + dispatchers; register Map + Particle modes.
- `sdlquake/engine/editor/editor_internal.h` — declare renamed `MapMode_*` functions + `editor_active_mode_idx`/setter.
- `sdlquake/engine/editor/editor_ui.c` — rename `Editor_DrawUI` → `MapMode_DrawUI`.
- `sdlquake/engine/editor/render_wire.c` — rename `Editor_RenderScene` → `MapMode_RenderScene`.
- `sdlquake/engine/imgui_bridge.h` / `imgui_bridge.cpp` — add `IG_ColorSwatch`.
- `sdlquake/game/game_api.h` — add `SpawnParticleEffect`; bump `GAME_API_VERSION` 36→37.
- `sdlquake/engine/hotreload.c` — extern-declare + wire `SpawnParticleEffect` into `engine_funcs`.
- `sdlquake/game/sim/sim_fire.c` (or another DLL site) — debug hook proving the ABI route.
- `build.zig` — add `r_emitter.c` (engine list) and `edit_particle.c` (editor list).

---

# Slice 1 — Editor mode seam

Goal: introduce `editor_mode_t`, wrap today's map editor as `map_mode`, add a mode switcher. **Map editor must behave identically.** No particles yet.

### Task 1.1: Define the `editor_mode_t` vtable

**Files:**
- Create: `sdlquake/engine/editor/editor_mode.h`

- [ ] **Step 1: Create the header**

```c
// editor_mode.h -- vtable describing one editor mode (Map, Particle, future
// Model/Texture). The shell (editor.c) holds a table of these and dispatches
// mode-specific behavior to the active one. Shared infrastructure (free-fly
// camera, open/close, look-mode) stays in the shell, NOT in the vtable.

#ifndef EDITOR_MODE_H
#define EDITOR_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct editor_mode_s {
    const char *name;                 // "Map", "Particle" -- shown in the switcher

    void (*enter)(void);              // optional; called when this mode becomes active
    void (*exit)(void);               // optional; called when leaving this mode

    void (*draw_ui)(void);            // ImGui panels for this mode (required)
    void (*render_scene)(void);       // 3D overlay for this mode (optional)
    int  (*process_event)(void *ev);  // SDL_Event*; return 1 if consumed (optional)

    // Per-mode policy queries. NULL => shell uses the documented default.
    int  (*hide_transient_fx)(void);  // Map=1 (hide), Particle=0 (show). default 1
    int  (*should_draw_player)(void); // default: shell's free-fly logic
} editor_mode_t;

#ifdef __cplusplus
}
#endif

#endif // EDITOR_MODE_H
```

- [ ] **Step 2: Compile gate** — `zig build` (header unused yet; just confirms it parses when included later). Actually no `.c` includes it yet, so this step is a no-op; proceed.

- [ ] **Step 3: Commit**
```bash
git add sdlquake/engine/editor/editor_mode.h
git commit -m "feat(editor): editor_mode_t vtable for multi-mode shell"
```

### Task 1.2: Rename the map-specific entry-point bodies to `MapMode_*`

These four functions are the map editor's implementations of mode-specific behavior. Rename them so the shell can register them in `map_mode` and re-expose thin dispatchers under the original public names.

**Files:**
- Modify: `sdlquake/engine/editor/editor_ui.c:2612` (`Editor_DrawUI` → `MapMode_DrawUI`)
- Modify: `sdlquake/engine/editor/render_wire.c:1293` (`Editor_RenderScene` → `MapMode_RenderScene`)
- Modify: `sdlquake/engine/editor/editor.c:2161` (`Editor_HideTransientFX` → `MapMode_HideTransientFX`), `:2155` (`Editor_ShouldDrawPlayer` → `MapMode_ShouldDrawPlayer`), `:2491` (`Editor_ProcessEvent` → `MapMode_ProcessEvent`)
- Modify: `sdlquake/engine/editor/editor_internal.h` (declare the renamed functions)

- [ ] **Step 1: Rename in `editor_ui.c`** — change the definition at line 2612 from
```c
void Editor_DrawUI(void)
```
to
```c
void MapMode_DrawUI(void)
```

- [ ] **Step 2: Rename in `render_wire.c`** — change the definition at line 1293 from
```c
void Editor_RenderScene(void)
```
to
```c
void MapMode_RenderScene(void)
```

- [ ] **Step 3: Rename the three in `editor.c`** — change these definitions:
```c
int Editor_ShouldDrawPlayer(void)   ->  int MapMode_ShouldDrawPlayer(void)
int Editor_HideTransientFX(void)    ->  int MapMode_HideTransientFX(void)
int Editor_ProcessEvent(void *evp)  ->  int MapMode_ProcessEvent(void *evp)
```
(Rename the definitions only. The public `Editor_*` names are re-created as dispatchers in Task 1.4.)

- [ ] **Step 4: Declare the renamed functions** in `editor_internal.h` (add near the other internal decls):
```c
// Map-mode implementations of the mode-specific entry points. Registered in
// map_mode (editor.c); the public Editor_* names are dispatchers.
void MapMode_DrawUI(void);
void MapMode_RenderScene(void);
int  MapMode_ShouldDrawPlayer(void);
int  MapMode_HideTransientFX(void);
int  MapMode_ProcessEvent(void *ev);
```

- [ ] **Step 5: Compile gate** — `zig build`. Expected: **fails to link** with undefined references to `Editor_DrawUI`, `Editor_RenderScene`, `Editor_ShouldDrawPlayer`, `Editor_HideTransientFX`, `Editor_ProcessEvent` (called from engine code). This is expected — Task 1.4 adds the dispatchers. Do NOT commit yet; proceed to 1.3 + 1.4, which restore those symbols.

### Task 1.3: Mode registry + active-mode state in the shell

**Files:**
- Modify: `sdlquake/engine/editor/editor.c` (top, near other statics — after the `#include`s)
- Modify: `sdlquake/engine/editor/editor_internal.h`

- [ ] **Step 1: Include the vtable header** at the top of `editor.c` (with the other editor includes):
```c
#include "editor_mode.h"
```

- [ ] **Step 2: Declare the two mode tables forward** and add registry state near the top-of-file statics in `editor.c`:
```c
// ---- Editor mode registry (Slice 1) -------------------------------------
// Shared infrastructure (camera, open/close, look-mode) stays in the shell;
// only mode-specific behavior is dispatched through these vtables.
static const editor_mode_t map_mode;        // defined below
static const editor_mode_t particle_mode;   // defined in edit_particle.c (Slice 4)

static const editor_mode_t *s_modes[] = { &map_mode, &particle_mode };
#define EDITOR_NUM_MODES ((int)(sizeof(s_modes)/sizeof(s_modes[0])))
static int s_active_mode = 0;

const editor_mode_t *Editor_ActiveMode(void) { return s_modes[s_active_mode]; }
int  Editor_ActiveModeIdx(void) { return s_active_mode; }
void Editor_SetMode(int idx)
{
    if (idx < 0 || idx >= EDITOR_NUM_MODES || idx == s_active_mode) return;
    if (s_modes[s_active_mode]->exit) s_modes[s_active_mode]->exit();
    s_active_mode = idx;
    if (s_modes[s_active_mode]->enter) s_modes[s_active_mode]->enter();
}
```

> NOTE: `particle_mode` is referenced here but defined in Slice 4. To keep Slice 1 self-contained and buildable, **temporarily** define a stub `particle_mode` at the bottom of `editor.c` in this task; Slice 4 Task 4.6 deletes the stub and moves the real definition to `edit_particle.c`.

- [ ] **Step 3: Add the temporary `particle_mode` stub** at the bottom of `editor.c`:
```c
// TEMPORARY stub -- replaced by the real definition in edit_particle.c (Slice 4).
static void particle_stub_draw_ui(void) { /* empty until Slice 4 */ }
static const editor_mode_t particle_mode = {
    .name = "Particle",
    .draw_ui = particle_stub_draw_ui,
};
```

- [ ] **Step 4: Declare the registry accessors** in `editor_internal.h`:
```c
struct editor_mode_s;
const struct editor_mode_s *Editor_ActiveMode(void);
int  Editor_ActiveModeIdx(void);
void Editor_SetMode(int idx);
```

- [ ] **Step 5: Compile gate** — still expected to fail on the missing `Editor_*` dispatchers until Task 1.4. Proceed.

### Task 1.4: Define `map_mode` + the public dispatchers

**Files:**
- Modify: `sdlquake/engine/editor/editor.c`

- [ ] **Step 1: Define `map_mode`** (near the registry, after the `MapMode_*` decls are visible via `editor_internal.h`):
```c
static const editor_mode_t map_mode = {
    .name               = "Map",
    .draw_ui            = MapMode_DrawUI,
    .render_scene       = MapMode_RenderScene,
    .process_event      = MapMode_ProcessEvent,
    .hide_transient_fx  = MapMode_HideTransientFX,
    .should_draw_player = MapMode_ShouldDrawPlayer,
};
```

- [ ] **Step 2: Add the public dispatchers** (replacing the old public names). Put these in `editor.c`:
```c
// Public engine-boundary entry points -> dispatch to the active mode.
// The mode switcher is drawn here so it's available in every mode.
void Editor_DrawUI(void)
{
    const editor_mode_t *m = Editor_ActiveMode();

    // Mode switcher: a small always-present window.
    IG_SetNextWindowPos(8.0f, 8.0f, IG_Cond_FirstUseEver);
    IG_SetNextWindowSize(180.0f, 0.0f, IG_Cond_FirstUseEver);
    if (IG_Begin("Editor Mode", NULL, IG_WF_None)) {
        for (int i = 0; i < EDITOR_NUM_MODES; i++) {
            if (i) IG_SameLine(0, -1);
            if (IG_RadioButton(s_modes[i]->name, i == s_active_mode))
                Editor_SetMode(i);
        }
    }
    IG_End();

    if (m->draw_ui) m->draw_ui();
}

void Editor_RenderScene(void)
{
    const editor_mode_t *m = Editor_ActiveMode();
    if (m->render_scene) m->render_scene();
}

int Editor_ProcessEvent(void *evp)
{
    const editor_mode_t *m = Editor_ActiveMode();
    if (m->process_event) return m->process_event(evp);
    return 0;
}

int Editor_HideTransientFX(void)
{
    const editor_mode_t *m = Editor_ActiveMode();
    if (m->hide_transient_fx) return m->hide_transient_fx();
    return 1; // default: hide (matches old map-only behavior)
}

int Editor_ShouldDrawPlayer(void)
{
    const editor_mode_t *m = Editor_ActiveMode();
    if (m->should_draw_player) return m->should_draw_player();
    return 0;
}
```

> IMPORTANT: `Editor_RenderScene` was previously defined in `render_wire.c` and is called even when the editor is **closed** (per `editor.h`: "Always runs ... so saved edits stay visible during play"). Verify the renamed `MapMode_RenderScene` still does its own open/closed gating internally (it does — the old body checked `s_open`/cull). The dispatcher adds no gating, so behavior is preserved when Map mode is active. When Particle mode is active and the editor is closed, `Editor_RenderScene` calls `particle_mode.render_scene` (NULL in Slice 1/stub) → no-op, which is correct.

- [ ] **Step 3: Compile gate** — `zig build`. Expected: **success** (all `Editor_*` symbols restored).

- [ ] **Step 4: Runtime check** — `zig build run -- +map start`. Press F2 (open editor). Confirm: (a) a small "Editor Mode" window with `Map`/`Particle` radio buttons appears; (b) Map is selected; (c) all existing map panels (Editor, Brushes, etc.) work exactly as before; (d) clicking `Particle` makes the map panels disappear (stub draws nothing) and clicking `Map` brings them back. Switch back to Map.

- [ ] **Step 5: Commit**
```bash
git add sdlquake/engine/editor/editor.c sdlquake/engine/editor/editor_ui.c \
        sdlquake/engine/editor/render_wire.c sdlquake/engine/editor/editor_internal.h \
        sdlquake/engine/editor/editor_mode.h
git commit -m "feat(editor): mode seam -- Map mode wrapped, Particle mode stub, switcher UI"
```

---

# Slice 2 — Emitter runtime core

Goal: `pt_emitter` particles, the `r_emitter.c` registry + live pool + per-frame update, integration & draw, a `particle_spawn` console command and `r_emitter_active` cvar. Drive it with one hardcoded def to prove particles render and obey the def.

### Task 2.1: Add `pt_emitter` type and `particle_t.def` field

**Files:**
- Modify: `sdlquake/engine_src/d_iface.h`

- [ ] **Step 1: Add `pt_emitter` to `ptype_t`** (the enum at line 34-40). Change the last line of the enum from:
```c
	pt_fireblob	// R_AddFire plume: pt_fire physics, ADSR size billboard in D_DrawFireParticle
} ptype_t;
```
to:
```c
	pt_fireblob,	// R_AddFire plume: pt_fire physics, ADSR size billboard in D_DrawFireParticle
	pt_emitter	// data-driven emitter particle: physics + size + color from emitter_def_t (r_emitter.c)
} ptype_t;
```

- [ ] **Step 2: Add `short def;` to `particle_t`** (struct at line 57-74). After the `byte flags;` line, before the closing brace, add:
```c
	// pt_emitter only: index into r_emitter.c's effect-def table (s_defs).
	// -1 / any other type: unused. Engine-internal field; not part of the
	// game ABI, and the id386 asm path (d_ifacea.h) is dormant in the SDL
	// build, so growing this struct is safe.
	short		def;
```

- [ ] **Step 3: Compile gate** — `zig build`. Expected: success (new enum value + field; nothing reads them yet, and `wind_drag_k[]` still sizes fine because `pt_emitter` isn't indexed until Task 2.3).

- [ ] **Step 4: Commit**
```bash
git add sdlquake/engine_src/d_iface.h
git commit -m "feat(particles): add pt_emitter type + particle_t.def field"
```

### Task 2.2: Emitter data model + public API header

**Files:**
- Create: `sdlquake/engine_src/r_emitter.h`

- [ ] **Step 1: Create the header**

```c
// r_emitter.h -- data-driven particle effects (the "particle editor" runtime).
//
// An emitter_def_t describes ONE effect as data. The registry (s_defs in
// r_emitter.c) holds up to EMIT_MAX_DEFS of them, loaded from id1/particles/
// *.pcl at startup and mutated live by the editor. Spawning an effect either
// bursts particles immediately or parks a live-emitter instance that emits at
// a rate until it expires. Particles spawned are type pt_emitter, carrying a
// `def` index; R_DrawParticles integrates them and D_DrawEmitterParticle draws
// them, both reading the def.

#ifndef R_EMITTER_H
#define R_EMITTER_H

#include "mathlib.h"   // vec3_t
#include "zone.h"      // byte (via quakedef include chain); see note below

// NOTE: r_emitter.h is included by engine_src C files that already include
// quakedef.h, so byte/vec3_t are available. The two #includes above are
// belt-and-suspenders for standalone inclusion order; if they cause trouble in
// gnu89, drop them -- callers include quakedef.h first.

typedef enum { EMIT_BURST = 0, EMIT_CONTINUOUS = 1 } emit_mode_t;
typedef enum { SHAPE_POINT = 0, SHAPE_SPHERE = 1, SHAPE_CONE = 2, SHAPE_BOX = 3 } emit_shape_t;
typedef enum { DIR_ALONG_SHAPE = 0, DIR_INHERIT = 1, DIR_UP = 2 } emit_dirmode_t;
typedef enum { STYLE_DOT = 0, STYLE_BLOB = 1, STYLE_SMOKE = 2 } emit_style_t;

#define EMIT_MAX_RAMP   8
#define EMIT_MAX_DEFS   128
#define EMIT_MAX_LIVE   64
#define EMIT_NAME_LEN   32

typedef struct {
    int             used;               // 0 = free registry slot
    char            name[EMIT_NAME_LEN];// lookup key == .pcl filename stem

    int             mode;               // emit_mode_t
    int             count;              // burst total
    float           rate;               // particles/sec (continuous)
    float           duration;           // sec; 0 = loop until stopped

    int             shape;              // emit_shape_t
    vec3_t          origin_offset;
    float           shape_size;         // sphere radius / box half-extent
    float           cone_angle;         // half-angle, deg (SHAPE_CONE)

    float           speed, speed_jitter;
    int             dir_mode;           // emit_dirmode_t
    float           spread;             // deg
    float           radial_bias;        // outward-from-origin velocity add

    float           gravity_scale;      // 1.0 ~= a classic falling particle; <0 rises
    float           drag;               // per-sec linear velocity decay

    float           life_min, life_max; // sec

    int             style;              // emit_style_t
    float           size_start, size_peak, size_end;  // blob/smoke size scale

    int             ramp_count;
    float           ramp_frac[EMIT_MAX_RAMP];   // 0..1 ascending
    byte            ramp_pal [EMIT_MAX_RAMP];   // palette index per stop
} emitter_def_t;

// ---- lifecycle ----------------------------------------------------------
void  R_EmitterInit (void);            // called from R_InitParticles
void  R_UpdateEmitters (void);         // called each frame before R_DrawParticles

// ---- registry access ----------------------------------------------------
int             R_EmitterCount (void);              // number of used slots
emitter_def_t  *R_EmitterGetDef (int idx);          // NULL if idx invalid/unused
int             R_EmitterFind (const char *name);   // slot idx or -1
int             R_EmitterNew  (const char *name);   // alloc slot with defaults, idx or -1
void            R_EmitterDelete (int idx);

// ---- spawning ------------------------------------------------------------
// Returns a live-instance handle (>=0) for continuous effects, or -1 for
// bursts / on failure. Handle is used only to stop a looping preview.
int   R_SpawnEffectIdx (int idx, vec3_t org, vec3_t dir);
void  R_SpawnParticleEffectByName (const char *name, vec3_t org, vec3_t dir); // engine_api hook
void  R_EmitterStopHandle (int handle);
void  R_EmitterStopAll (void);

// ---- color/size sampling (shared by r_part.c integrate + d_part.c draw) --
byte  R_EmitterRampColor (const emitter_def_t *d, float t);  // t in [0,1]
float R_EmitterSizeEnv  (const emitter_def_t *d, float t);   // size scale at age t

// ---- persistence ---------------------------------------------------------
void  R_EmitterLoadAll (void);         // scan id1/particles/*.pcl into s_defs
int   R_EmitterSave (int idx);         // write s_defs[idx] -> id1/particles/<name>.pcl; 1 ok

#endif // R_EMITTER_H
```

- [ ] **Step 2: Compile gate** — `zig build` (header not yet included anywhere; no-op). Proceed.

- [ ] **Step 3: Commit**
```bash
git add sdlquake/engine_src/r_emitter.h
git commit -m "feat(emitter): emitter_def_t data model + runtime API header"
```

### Task 2.3: Extend `wind_drag_k[]` for `pt_emitter`

**Files:**
- Modify: `sdlquake/engine_src/r_part.c:221` (the `wind_drag_k[]` initializer)

- [ ] **Step 1: Add the `pt_emitter` entry.** In the designated-initializer array (ends at line 236 with `[pt_slowgrav] = 2.5f,`), add a line before the closing `};`:
```c
    [pt_emitter]  = 0.5f,   // data-driven FX lean in wind moderately (per-effect tuning lives in the def, not here)
};
```
This is **required**: `R_DrawParticles` indexes `wind_drag_k[p->type]` (line ~1988); without this entry, a `pt_emitter` particle reads past the array.

- [ ] **Step 2: Compile gate** — `zig build`. Expected: success.

- [ ] **Step 3: Commit**
```bash
git add sdlquake/engine_src/r_part.c
git commit -m "fix(particles): add pt_emitter to wind_drag_k table"
```

### Task 2.4: Emitter runtime implementation

**Files:**
- Create: `sdlquake/engine_src/r_emitter.c`

This is the core module. It depends on engine globals already used by `r_part.c`: `free_particles`/`active_particles`, `cl.time`, `host_basepal`, `SMOKE_GAMEPLAY_RESERVE`. It allocates no particles itself beyond pulling from the free list (same etiquette as `R_AddFire`).

- [ ] **Step 1: Write the module**

```c
// r_emitter.c -- data-driven particle effects runtime. See r_emitter.h and
// docs/superpowers/specs/2026-06-01-particle-editor-design.md.

#include "quakedef.h"
#include "r_emitter.h"

// Shared particle pool (defined in r_part.c).
extern particle_t *active_particles, *free_particles;

// Pool reserve so authored FX can't starve gameplay particles (mirror r_part.c).
#ifndef SMOKE_GAMEPLAY_RESERVE
#define SMOKE_GAMEPLAY_RESERVE 2048
#endif

cvar_t r_emitter_active = { "r_emitter_active", "0" }; // reports live-instance count

// ---- registry -----------------------------------------------------------
static emitter_def_t s_defs[EMIT_MAX_DEFS];

typedef struct {
    int   used;
    int   def_idx;
    vec3_t org, dir;
    float expire;     // cl.time at which to stop (ignored if def->duration==0)
    float accum;      // fractional particle carry
} live_emitter_t;

static live_emitter_t s_live[EMIT_MAX_LIVE];

int R_EmitterCount(void)
{
    int n = 0, i;
    for (i = 0; i < EMIT_MAX_DEFS; i++) if (s_defs[i].used) n++;
    return n;
}

emitter_def_t *R_EmitterGetDef(int idx)
{
    if (idx < 0 || idx >= EMIT_MAX_DEFS || !s_defs[idx].used) return NULL;
    return &s_defs[idx];
}

int R_EmitterFind(const char *name)
{
    int i;
    if (!name || !name[0]) return -1;
    for (i = 0; i < EMIT_MAX_DEFS; i++)
        if (s_defs[i].used && !Q_strcmp(s_defs[i].name, name)) return i;
    return -1;
}

static void emitter_set_defaults(emitter_def_t *d, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->used = 1;
    Q_strncpy(d->name, name, EMIT_NAME_LEN - 1);
    d->mode        = EMIT_BURST;
    d->count       = 32;
    d->rate        = 20.0f;
    d->duration    = 1.0f;
    d->shape       = SHAPE_POINT;
    d->shape_size  = 8.0f;
    d->cone_angle  = 15.0f;
    d->speed       = 60.0f;
    d->speed_jitter= 10.0f;
    d->dir_mode    = DIR_ALONG_SHAPE;
    d->spread      = 20.0f;
    d->gravity_scale = 1.0f;
    d->drag        = 0.0f;
    d->life_min    = 0.6f;
    d->life_max    = 1.0f;
    d->style       = STYLE_DOT;
    d->size_start  = 1.0f;
    d->size_peak   = 4.0f;
    d->size_end    = 0.0f;
    d->ramp_count  = 2;
    d->ramp_frac[0]= 0.0f; d->ramp_pal[0] = 0x6f; // ramp1[0] orange-ish
    d->ramp_frac[1]= 1.0f; d->ramp_pal[1] = 0x61; // dark ember
}

int R_EmitterNew(const char *name)
{
    int i;
    if (R_EmitterFind(name) >= 0) return -1; // name clash
    for (i = 0; i < EMIT_MAX_DEFS; i++) {
        if (!s_defs[i].used) { emitter_set_defaults(&s_defs[i], name); return i; }
    }
    Con_Printf("R_EmitterNew: registry full (%d)\n", EMIT_MAX_DEFS);
    return -1;
}

void R_EmitterDelete(int idx)
{
    int i;
    if (idx < 0 || idx >= EMIT_MAX_DEFS) return;
    // Stop any live instances of this def first (reload-safety; see spec).
    for (i = 0; i < EMIT_MAX_LIVE; i++)
        if (s_live[i].used && s_live[i].def_idx == idx) s_live[i].used = 0;
    s_defs[idx].used = 0;
}

// ---- sampling -----------------------------------------------------------
byte R_EmitterRampColor(const emitter_def_t *d, float t)
{
    int i;
    if (d->ramp_count <= 0) return 0;
    if (t <= d->ramp_frac[0]) return d->ramp_pal[0];
    for (i = 1; i < d->ramp_count; i++) {
        if (t <= d->ramp_frac[i]) {
            // Nearest stop (palette indices don't interpolate meaningfully).
            float mid = 0.5f * (d->ramp_frac[i-1] + d->ramp_frac[i]);
            return (t < mid) ? d->ramp_pal[i-1] : d->ramp_pal[i];
        }
    }
    return d->ramp_pal[d->ramp_count - 1];
}

float R_EmitterSizeEnv(const emitter_def_t *d, float t)
{
    if (t < 0) t = 0; else if (t > 1) t = 1;
    if (t < 0.5f) {
        float a = t / 0.5f;
        return d->size_start + (d->size_peak - d->size_start) * a;
    } else {
        float a = (t - 0.5f) / 0.5f;
        return d->size_peak + (d->size_end - d->size_peak) * a;
    }
}

// ---- spawn one particle from a def --------------------------------------
static float frand(void) { return (rand() & 0x7fff) / 32767.0f; }       // [0,1]
static float frand_s(void) { return frand() * 2.0f - 1.0f; }            // [-1,1]

static void spawn_one(int def_idx, const vec3_t base_org, const vec3_t base_dir)
{
    emitter_def_t *d = &s_defs[def_idx];
    particle_t *p;
    vec3_t org, vel, sdir;
    int j;

    if (!free_particles) return;
    p = free_particles;
    free_particles = p->next;
    p->next = active_particles;
    active_particles = p;
    p->flags = 0;
    p->type  = pt_emitter;
    p->def   = (short)def_idx;

    // origin: base + offset + shape scatter
    for (j = 0; j < 3; j++) org[j] = base_org[j] + d->origin_offset[j];
    VectorCopy(base_dir, sdir);
    switch (d->shape) {
    case SHAPE_SPHERE:
        for (j = 0; j < 3; j++) org[j] += frand_s() * d->shape_size;
        break;
    case SHAPE_BOX:
        for (j = 0; j < 3; j++) org[j] += frand_s() * d->shape_size;
        break;
    case SHAPE_CONE:
        // scatter direction within cone_angle around base_dir
        for (j = 0; j < 3; j++) sdir[j] = base_dir[j] + frand_s() * (d->cone_angle / 90.0f);
        VectorNormalize(sdir);
        break;
    case SHAPE_POINT:
    default: break;
    }

    // velocity direction
    {
        vec3_t vdir;
        if (d->dir_mode == DIR_UP)          { vdir[0]=0; vdir[1]=0; vdir[2]=1; }
        else if (d->dir_mode == DIR_INHERIT){ VectorCopy(base_dir, vdir); }
        else                                { VectorCopy(sdir, vdir); } // ALONG_SHAPE
        // spread jitter
        for (j = 0; j < 3; j++) vdir[j] += frand_s() * (d->spread / 90.0f);
        VectorNormalize(vdir);
        float spd = d->speed + frand_s() * d->speed_jitter;
        for (j = 0; j < 3; j++) vel[j] = vdir[j] * spd;
        // radial bias: push outward from base_org
        if (d->radial_bias != 0.0f) {
            vec3_t rad; for (j=0;j<3;j++) rad[j] = org[j] - base_org[j];
            VectorNormalize(rad);
            for (j=0;j<3;j++) vel[j] += rad[j] * d->radial_bias;
        }
    }

    VectorCopy(org, p->org);
    VectorCopy(vel, p->vel);
    {
        float life = d->life_min + frand() * (d->life_max - d->life_min);
        if (life < 0.05f) life = 0.05f;
        p->birth = cl.time;
        p->die   = cl.time + life;
    }
    p->ramp  = 0;
    p->color = R_EmitterRampColor(d, 0.0f);
}

// Reserve-aware burst: like R_AddFire's guard.
static void burst(int def_idx, const vec3_t org, const vec3_t dir, int n)
{
    int need = SMOKE_GAMEPLAY_RESERVE + n, i;
    particle_t *probe = free_particles;
    while (need > 0 && probe) { probe = probe->next; need--; }
    if (need > 0) return; // pool too low; drop the burst
    for (i = 0; i < n; i++) spawn_one(def_idx, org, dir);
}

// ---- public spawn --------------------------------------------------------
int R_SpawnEffectIdx(int idx, vec3_t org, vec3_t dir)
{
    emitter_def_t *d = R_EmitterGetDef(idx);
    int i;
    if (!d) return -1;

    if (d->mode == EMIT_BURST) {
        burst(idx, org, dir, d->count);
        return -1;
    }
    // continuous: register a live instance
    for (i = 0; i < EMIT_MAX_LIVE; i++) {
        if (!s_live[i].used) {
            s_live[i].used = 1;
            s_live[i].def_idx = idx;
            VectorCopy(org, s_live[i].org);
            VectorCopy(dir, s_live[i].dir);
            s_live[i].accum = 0.0f;
            s_live[i].expire = (d->duration > 0.0f) ? (cl.time + d->duration) : 0.0f;
            return i;
        }
    }
    return -1; // live pool full
}

void R_SpawnParticleEffectByName(const char *name, vec3_t org, vec3_t dir)
{
    int idx = R_EmitterFind(name);
    if (idx < 0) {
        static int warned = 0;
        if (!warned) { Con_DPrintf("SpawnParticleEffect: no effect '%s'\n", name); warned = 1; }
        return;
    }
    R_SpawnEffectIdx(idx, org, dir);
}

void R_EmitterStopHandle(int handle)
{
    if (handle >= 0 && handle < EMIT_MAX_LIVE) s_live[handle].used = 0;
}

void R_EmitterStopAll(void)
{
    int i;
    for (i = 0; i < EMIT_MAX_LIVE; i++) s_live[i].used = 0;
}

// ---- per-frame update ----------------------------------------------------
void R_UpdateEmitters(void)
{
    float dt = cl.time - cl.oldtime;
    int i, live = 0;
    if (dt <= 0.0f) { Cvar_SetValue("r_emitter_active", 0); return; }

    for (i = 0; i < EMIT_MAX_LIVE; i++) {
        live_emitter_t *e = &s_live[i];
        emitter_def_t *d;
        if (!e->used) continue;
        d = R_EmitterGetDef(e->def_idx);
        if (!d || d->mode != EMIT_CONTINUOUS) { e->used = 0; continue; }
        if (e->expire != 0.0f && cl.time > e->expire) { e->used = 0; continue; }

        e->accum += d->rate * dt;
        int want = (int)e->accum;
        if (want > 0) {
            e->accum -= want;
            // honor reserve once for the whole tick's worth
            burst(e->def_idx, e->org, e->dir, want);
        }
        live++;
    }
    Cvar_SetValue("r_emitter_active", (float)live);
}
```

- [ ] **Step 2: Stub the persistence functions** at the end of `r_emitter.c` for now (Slice 3 fills them in). Add:
```c
// Persistence -- implemented in Slice 3. Stubs so the module links.
void R_EmitterLoadAll(void) {}
int  R_EmitterSave(int idx) { (void)idx; return 0; }
void R_EmitterInit(void) { R_EmitterLoadAll(); }
```

- [ ] **Step 3: Add `r_emitter.c` to the build.** In `build.zig`, the engine_src list (around line 56-59), add `"r_emitter.c"` next to `"r_part.c"`:
```zig
        "r_efrag.c", "r_light.c", "r_livelight.c", "r_lut.c", "r_main.c", "r_misc.c", "r_part.c",
        "r_emitter.c",
        "r_decals.c",
```

- [ ] **Step 4: Compile gate** — `zig build`. Expected: success (module compiles + links; nothing calls it yet beyond the cvar). If `Q_strncpy`/`Q_strcmp`/`VectorNormalize` signatures differ, adjust to the engine's actual decls in `common.h`/`mathlib.h`.

- [ ] **Step 5: Commit**
```bash
git add sdlquake/engine_src/r_emitter.c build.zig
git commit -m "feat(emitter): registry, live-emitter pool, spawn + per-frame update"
```

### Task 2.5: Integrate `pt_emitter` physics in `R_DrawParticles`

**Files:**
- Modify: `sdlquake/engine_src/r_part.c` (the `switch (p->type)` at ~line 2120; include header)

- [ ] **Step 1: Include the header** near the top of `r_part.c` (with other includes):
```c
#include "r_emitter.h"
```

- [ ] **Step 2: Add the `pt_emitter` case** to the physics switch. Insert before `case pt_grav:` (line ~2237):
```c
		case pt_emitter:
		{
			// Data-driven: gravity + drag from the def. Color/size are sampled
			// at draw time (D_DrawEmitterParticle), NOT here -- no ramp walk.
			emitter_def_t *d = R_EmitterGetDef(p->def);
			if (d) {
				// grav here is frametime*sv_gravity*0.05; pt_grav uses grav*20
				// for "full" gravity, so scale matches: 1.0 ~= a falling particle.
				p->vel[2] -= grav * 20.0f * d->gravity_scale;
				if (d->drag > 0.0f) {
					float k = 1.0f - d->drag * frametime;
					if (k < 0.0f) k = 0.0f;
					p->vel[0] *= k; p->vel[1] *= k; p->vel[2] *= k;
				}
			}
			break;
		}
```

- [ ] **Step 3: Add the draw dispatch.** In the draw block (line ~1970-1975), add a `pt_emitter` branch before the final `else`:
```c
		else if (p->type == pt_smoke)
			D_DrawSmokeParticle (p);
		else if (p->type == pt_fireblob)
			D_DrawFireParticle (p);
		else if (p->type == pt_emitter)
			D_DrawEmitterParticle (p);
		else
			D_DrawParticle (p);
```

- [ ] **Step 4: Forward-declare `D_DrawEmitterParticle`** near the top of `r_part.c` (next to the existing `void D_DrawFireParticle (particle_t *p);` at line 251):
```c
void D_DrawEmitterParticle (particle_t *p);  // data-driven emitter draw (d_part.c)
```

- [ ] **Step 5: Hook `R_UpdateEmitters` + `R_EmitterInit`.**
  - In `r_main.c`, just before the `R_DrawParticles ()` call at line 1200, add:
    ```c
    R_UpdateEmitters ();
    ```
    Final shape:
    ```c
    			R_UpdateEmitters ();
    			PERF_SCOPE("R_DrawParticles") R_DrawParticles ();
    ```
  - In `r_part.c`'s `R_InitParticles` (ends line 278), add before the closing brace:
    ```c
    	R_EmitterInit ();
    ```

- [ ] **Step 6: Register the cvar.** In `r_main.c` near the other particle cvar registrations (line ~262-278), add:
```c
	Cvar_RegisterVariable (&r_emitter_active);
```
and add an extern near the top of `r_main.c` (with other extern cvars):
```c
extern cvar_t r_emitter_active;
```

- [ ] **Step 7: Compile gate** — `zig build`. Expected: success. (`D_DrawEmitterParticle` is forward-declared; it's defined in Task 2.6, which must land before linking succeeds — so this step's link will FAIL with an undefined `D_DrawEmitterParticle`. Proceed to 2.6, then build.)

### Task 2.6: `D_DrawEmitterParticle`

**Files:**
- Modify: `sdlquake/engine_src/d_part.c` (add the function; it can call the `static` `draw_fire_blob`/`draw_smoke_blob` already in this file)

- [ ] **Step 1: Include the header** at the top of `d_part.c` (after the existing includes):
```c
#include "r_emitter.h"
```

- [ ] **Step 2: Add `D_DrawEmitterParticle`** at the end of `d_part.c` (after `D_DrawSmokeCells`). It mirrors `D_DrawFireParticle`'s projection, then dispatches by style:
```c
/*
==============
D_DrawEmitterParticle

Data-driven emitter particle (pt_emitter). Samples the def's palette ramp and
size envelope at the particle's normalized age, then draws via the style's
existing rasteriser:
  STYLE_DOT   -> classic distance-scaled splat (set color, reuse D_DrawParticle)
  STYLE_BLOB  -> solid ellipse (draw_fire_blob), size from the envelope
  STYLE_SMOKE -> fog-density ellipse (draw_smoke_blob), size from the envelope
==============
*/
void D_DrawEmitterParticle (particle_t *pparticle)
{
	emitter_def_t *d = R_EmitterGetDef(pparticle->def);
	if (!d) return;

	// normalized age
	float life = pparticle->die - pparticle->birth;
	float t    = (life > 0.001f) ? (cl.time - pparticle->birth) / life : 1.0f;
	if (t < 0) t = 0; else if (t > 1) t = 1;

	// color from ramp (write into the particle so DOT path picks it up)
	pparticle->color = R_EmitterRampColor(d, t);

	if (d->style == STYLE_DOT) {
		D_DrawParticle(pparticle);
		return;
	}

	// project for blob/smoke
	vec3_t local, transformed;
	VectorSubtract (pparticle->org, r_origin, local);
	transformed[0] = DotProduct(local, r_pright);
	transformed[1] = DotProduct(local, r_pup);
	transformed[2] = DotProduct(local, r_ppn);
	if (transformed[2] < PARTICLE_Z_CLIP) return;

	float zi  = 1.0f / transformed[2];
	int   u   = (int)(xcenter + zi * transformed[0] + 0.5f);
	int   v   = (int)(ycenter - zi * transformed[1] + 0.5f);
	int   izi = (int)(zi * 0x8000);

	float ramp_eff = R_EmitterSizeEnv(d, t);
	int pix = ((int)(izi * ramp_eff)) >> d_pix_shift;
	if (pix < 1) pix = 1;
	if (pix > d_pix_max * 3) pix = d_pix_max * 3;
	int rows = pix << d_y_aspect_shift;

	if (d->style == STYLE_SMOKE) {
		// density: hold then fade, like D_DrawSmokeParticle
		float density = (t < 0.5f) ? 1.0f : (1.0f - ((t-0.5f)/0.5f)*((t-0.5f)/0.5f));
		draw_smoke_blob(u, v, pix, rows, izi, density, 0u, (byte)pparticle->color);
	} else { // STYLE_BLOB
		draw_fire_blob(u, v, pix, rows, izi, (byte)pparticle->color);
	}
}
```

> NOTE: `draw_fire_blob` and `draw_smoke_blob` are `static` functions earlier in `d_part.c`, so `D_DrawEmitterParticle` must be defined **in this file** (it is). `xcenter`, `ycenter`, `d_pix_shift`, `d_pix_max`, `d_y_aspect_shift`, `r_pright/r_pup/r_ppn`, `PARTICLE_Z_CLIP` are all already in scope here (used by `D_DrawFireParticle`).

- [ ] **Step 3: Compile gate** — `zig build`. Expected: **success** now (both 2.5 and 2.6 landed).

- [ ] **Step 4: Add the `particle_spawn` console command** to drive a hardcoded test def. In `r_emitter.c`, add a command handler and register it in `R_EmitterInit`:
```c
// Console: particle_spawn <name> [x y z]   (defaults to a trace from the eye)
static void R_ParticleSpawn_f(void)
{
	vec3_t org, dir = {0,0,1};
	int idx;
	if (Cmd_Argc() < 2) { Con_Printf("usage: particle_spawn <name> [x y z]\n"); return; }
	idx = R_EmitterFind(Cmd_Argv(1));
	if (idx < 0) { Con_Printf("no effect '%s' (have %d)\n", Cmd_Argv(1), R_EmitterCount()); return; }
	if (Cmd_Argc() >= 5) {
		org[0]=atof(Cmd_Argv(2)); org[1]=atof(Cmd_Argv(3)); org[2]=atof(Cmd_Argv(4));
	} else {
		// trace forward from the client view
		vec3_t fwd, right, up, end;
		trace_t tr;
		AngleVectors(r_refdef.viewangles, fwd, right, up);
		VectorMA(r_refdef.vieworg, 2048, fwd, end);
		memset(&tr, 0, sizeof(tr));
		if (cl.worldmodel) {
			tr = SV_Move(r_refdef.vieworg, vec3_origin, vec3_origin, end, 1, NULL);
			VectorCopy(tr.endpos, org);
		} else VectorCopy(r_refdef.vieworg, org);
		VectorCopy(fwd, dir);
	}
	R_SpawnEffectIdx(idx, org, dir);
}
```
And in `R_EmitterInit`, register the command **and** create a hardcoded test def so there's something to spawn before Slice 3:
```c
void R_EmitterInit(void)
{
	Cvar_RegisterVariable(&r_emitter_active);   // (move here if not already registered in r_main.c; keep ONE registration)
	Cmd_AddCommand("particle_spawn", R_ParticleSpawn_f);
	R_EmitterLoadAll();
	// Bootstrap test def if none loaded yet (Slice 3 replaces with real .pcl files).
	if (R_EmitterCount() == 0) {
		int i = R_EmitterNew("test_fountain");
		if (i >= 0) {
			emitter_def_t *d = R_EmitterGetDef(i);
			d->mode = EMIT_CONTINUOUS; d->rate = 60; d->duration = 0;
			d->shape = SHAPE_POINT; d->speed = 120; d->speed_jitter = 30;
			d->dir_mode = DIR_UP; d->spread = 25;
			d->gravity_scale = 1.0f; d->life_min = 0.8f; d->life_max = 1.4f;
			d->style = STYLE_BLOB; d->size_start = 1; d->size_peak = 5; d->size_end = 0;
			d->ramp_count = 3;
			d->ramp_frac[0]=0; d->ramp_pal[0]=0x6f;
			d->ramp_frac[1]=0.5f; d->ramp_pal[1]=0x6b;
			d->ramp_frac[2]=1; d->ramp_pal[2]=0x61;
		}
	}
}
```
> If `r_emitter_active` is already registered in `r_main.c` (Task 2.5 Step 6), do NOT register it again here — pick ONE site. Recommended: keep it in `r_main.c` with the other particle cvars and delete the `Cvar_RegisterVariable` line above.

> `SV_Move`/`AngleVectors`/`r_refdef`/`vec3_origin` are engine globals available to `r_emitter.c` via `quakedef.h`. If `SV_Move`'s signature differs, use the same trace helper `r_part.c` uses (`R_TraceParticle` / `TraceLine`); adjust to whatever is in scope.

- [ ] **Step 5: Compile gate** — `zig build`. Expected: success.

- [ ] **Step 6: Runtime check** — `zig build run -- +map start`. Open console (`` ` ``), type `particle_spawn test_fountain`. Expected: a continuous fountain of growing/shrinking orange→ember blobs rising and falling in front of you. Type `r_emitter_active` → expect `"1"`. Run `particle_spawn test_fountain` a few more times → count rises. (Each continuous spawn parks a new instance; that's expected for now.)

- [ ] **Step 7: Commit**
```bash
git add sdlquake/engine_src/r_part.c sdlquake/engine_src/d_part.c \
        sdlquake/engine_src/r_emitter.c sdlquake/engine_src/r_main.c
git commit -m "feat(emitter): integrate + draw pt_emitter; particle_spawn cmd; bootstrap def"
```

---

# Slice 3 — `.pcl` persistence

Goal: load `id1/particles/*.pcl` at startup, save from the registry, `particle_reload` command with reload-safety. Replace the bootstrap def with real seed files.

### Task 3.1: `.pcl` parser (load)

**Files:**
- Modify: `sdlquake/engine_src/r_emitter.c` (replace the `R_EmitterLoadAll` stub)

- [ ] **Step 1: Implement a single-file loader + directory scan.** Replace the stub `void R_EmitterLoadAll(void) {}` with:
```c
// ---- parse helpers -------------------------------------------------------
static int parse_enum(const char *s,
                      const char * const *names, int n, int fallback)
{
	int i; for (i = 0; i < n; i++) if (!Q_strcmp(s, names[i])) return i;
	return fallback;
}
static const char *EMIT_MODE_NAMES[]  = { "burst", "continuous" };
static const char *EMIT_SHAPE_NAMES[] = { "point", "sphere", "cone", "box" };
static const char *EMIT_DIR_NAMES[]   = { "along_shape", "inherit", "up" };
static const char *EMIT_STYLE_NAMES[] = { "dot", "blob", "smoke" };

// "0.0:111 0.4:107 1.0:8" -> ramp stops
static void parse_ramp(emitter_def_t *d, const char *s)
{
	int n = 0;
	d->ramp_count = 0;
	while (*s && n < EMIT_MAX_RAMP) {
		while (*s == ' ' || *s == '\t') s++;
		if (!*s) break;
		float frac = (float)atof(s);
		const char *colon = strchr(s, ':');
		if (!colon) break;
		int pal = atoi(colon + 1);
		d->ramp_frac[n] = frac;
		d->ramp_pal[n]  = (byte)(pal & 0xff);
		n++;
		// advance past this token
		while (*s && *s != ' ' && *s != '\t') s++;
	}
	d->ramp_count = n;
	if (n == 0) { d->ramp_count = 1; d->ramp_frac[0]=0; d->ramp_pal[0]=15; }
}

// Parse one "particle_effect { "k" "v" ... }" block into a new registry slot.
static void emitter_load_file(const char *relpath)
{
	char *buf = (char *)COM_LoadTempFile((char *)relpath);
	char *data, key[128];
	int idx; emitter_def_t *d;
	if (!buf) return;
	data = buf;

	data = COM_Parse(data); if (!data) return;          // "particle_effect"
	data = COM_Parse(data); if (!data || com_token[0] != '{') return; // "{"

	// Name defaults to the file stem; overridden by a "name" key.
	{
		char stem[EMIT_NAME_LEN]; const char *base = relpath, *p2;
		for (p2 = relpath; *p2; p2++) if (*p2=='/'||*p2=='\\') base = p2+1;
		Q_strncpy(stem, base, EMIT_NAME_LEN-1);
		{ char *dot = strrchr(stem, '.'); if (dot) *dot = 0; }
		idx = R_EmitterFind(stem);
		if (idx < 0) idx = R_EmitterNew(stem);
		if (idx < 0) return;
		d = R_EmitterGetDef(idx);
	}

	while (1) {
		data = COM_Parse(data);
		if (!data) break;
		if (com_token[0] == '}') break;
		Q_strncpy(key, com_token, sizeof(key)-1);
		data = COM_Parse(data);
		if (!data) break;
		// com_token now holds the value string
		if      (!Q_strcmp(key,"name"))        Q_strncpy(d->name, com_token, EMIT_NAME_LEN-1);
		else if (!Q_strcmp(key,"emission"))    d->mode = parse_enum(com_token, EMIT_MODE_NAMES, 2, EMIT_BURST);
		else if (!Q_strcmp(key,"count"))       d->count = atoi(com_token);
		else if (!Q_strcmp(key,"rate"))        d->rate = atof(com_token);
		else if (!Q_strcmp(key,"duration"))    d->duration = atof(com_token);
		else if (!Q_strcmp(key,"shape"))       d->shape = parse_enum(com_token, EMIT_SHAPE_NAMES, 4, SHAPE_POINT);
		else if (!Q_strcmp(key,"shape_size"))  d->shape_size = atof(com_token);
		else if (!Q_strcmp(key,"cone_angle"))  d->cone_angle = atof(com_token);
		else if (!Q_strcmp(key,"speed"))       d->speed = atof(com_token);
		else if (!Q_strcmp(key,"speed_jitter"))d->speed_jitter = atof(com_token);
		else if (!Q_strcmp(key,"dir_mode"))    d->dir_mode = parse_enum(com_token, EMIT_DIR_NAMES, 3, DIR_ALONG_SHAPE);
		else if (!Q_strcmp(key,"spread"))      d->spread = atof(com_token);
		else if (!Q_strcmp(key,"radial_bias")) d->radial_bias = atof(com_token);
		else if (!Q_strcmp(key,"gravity"))     d->gravity_scale = atof(com_token);
		else if (!Q_strcmp(key,"drag"))        d->drag = atof(com_token);
		else if (!Q_strcmp(key,"life_min"))    d->life_min = atof(com_token);
		else if (!Q_strcmp(key,"life_max"))    d->life_max = atof(com_token);
		else if (!Q_strcmp(key,"style"))       d->style = parse_enum(com_token, EMIT_STYLE_NAMES, 3, STYLE_DOT);
		else if (!Q_strcmp(key,"size_start"))  d->size_start = atof(com_token);
		else if (!Q_strcmp(key,"size_peak"))   d->size_peak = atof(com_token);
		else if (!Q_strcmp(key,"size_end"))    d->size_end = atof(com_token);
		else if (!Q_strcmp(key,"ramp"))        parse_ramp(d, com_token);
		// unknown keys silently ignored (forward-compat)
	}
}
```

> NOTE on file enumeration: the engine reads files through the searchpath (PAK + loose dirs). There is no portable directory-listing in the engine core. **Simplest robust approach:** maintain an index file `id1/particles/index.txt` (one effect name per line) and load each `<name>.pcl`. Implement `R_EmitterLoadAll` to read `index.txt`, or — if the platform layer exposes a dir scan (`Sys_*`) — use it. The loader below uses the index file (portable, explicit, git-friendly).

```c
void R_EmitterLoadAll(void)
{
	char *list = (char *)COM_LoadTempFile("particles/index.txt");
	if (!list) { Con_DPrintf("R_EmitterLoadAll: no particles/index.txt\n"); return; }
	char line[EMIT_NAME_LEN + 16];
	char *p = list;
	while (*p) {
		int n = 0;
		while (*p && *p != '\n' && *p != '\r' && n < (int)sizeof(line)-1) line[n++] = *p++;
		line[n] = 0;
		while (*p == '\n' || *p == '\r') p++;
		// trim trailing spaces
		while (n > 0 && (line[n-1]==' '||line[n-1]=='\t')) line[--n]=0;
		if (n == 0 || line[0] == '#') continue;
		{
			char rel[EMIT_NAME_LEN + 32];
			Q_snprintf(rel, sizeof(rel), "particles/%s.pcl", line);
			emitter_load_file(rel);
		}
	}
}
```

> `COM_LoadTempFile`, `COM_Parse`/`com_token`, `Q_snprintf` are engine functions (see `common.c`). If `Q_snprintf` isn't available, use `sprintf` into a sized buffer (engine code does this widely) or `snprintf`.

- [ ] **Step 2: Remove the bootstrap def** added in Task 2.6 Step 4 (the `if (R_EmitterCount() == 0) { ... }` block) — real files replace it. Keep the `Cmd_AddCommand("particle_spawn", ...)` and the `R_EmitterLoadAll()` call.

- [ ] **Step 3: Compile gate** — `zig build`. Expected: success.

### Task 3.2: Seed `.pcl` files + index

**Files:**
- Create: `id1/particles/index.txt`, `id1/particles/campfire.pcl`, `id1/particles/spark_burst.pcl`

- [ ] **Step 1: `id1/particles/index.txt`**
```
# one effect name per line; loaded as particles/<name>.pcl
campfire
spark_burst
```

- [ ] **Step 2: `id1/particles/campfire.pcl`**
```
particle_effect
{
	"name"         "campfire"
	"emission"     "continuous"
	"rate"         "40"
	"duration"     "0"
	"shape"        "cone"
	"cone_angle"   "12"
	"dir_mode"     "up"
	"speed"        "55"
	"speed_jitter" "15"
	"spread"       "12"
	"gravity"      "-0.25"
	"drag"         "0.5"
	"life_min"     "0.9"
	"life_max"     "1.5"
	"style"        "blob"
	"size_start"   "1"
	"size_peak"    "6"
	"size_end"     "0"
	"ramp"         "0.0:111 0.45:107 1.0:97"
}
```

- [ ] **Step 3: `id1/particles/spark_burst.pcl`**
```
particle_effect
{
	"name"         "spark_burst"
	"emission"     "burst"
	"count"        "40"
	"shape"        "point"
	"dir_mode"     "along_shape"
	"speed"        "180"
	"speed_jitter" "60"
	"spread"       "75"
	"gravity"      "1.0"
	"drag"         "1.5"
	"life_min"     "0.3"
	"life_max"     "0.7"
	"style"        "dot"
	"ramp"         "0.0:244 0.4:241 1.0:0"
}
```

- [ ] **Step 4: Runtime check** — `zig build run -- +map start`, console: `particle_spawn campfire` (a rising flame) and `particle_spawn spark_burst` (a one-shot spark spray that falls). Confirm both render. `r_emitter_active` reports 1 while a campfire loops.

- [ ] **Step 5: Commit**
```bash
git add sdlquake/engine_src/r_emitter.c id1/particles/
git commit -m "feat(emitter): .pcl loader (index.txt) + campfire/spark_burst seeds"
```

### Task 3.3: Save + `particle_reload` with reload-safety

**Files:**
- Modify: `sdlquake/engine_src/r_emitter.c`

- [ ] **Step 1: Implement `R_EmitterSave`** (replace the stub):
```c
int R_EmitterSave(int idx)
{
	emitter_def_t *d = R_EmitterGetDef(idx);
	char path[256], body[2048];
	char ramp[256]; int i, n = 0;
	if (!d) return 0;

	ramp[0] = 0;
	for (i = 0; i < d->ramp_count; i++) {
		char seg[32];
		Q_snprintf(seg, sizeof(seg), "%s%.3g:%d", i ? " " : "", d->ramp_frac[i], (int)d->ramp_pal[i]);
		Q_strncat(ramp, seg, sizeof(ramp));
	}

	n = Q_snprintf(body, sizeof(body),
		"particle_effect\n{\n"
		"\t\"name\"         \"%s\"\n"
		"\t\"emission\"     \"%s\"\n"
		"\t\"count\"        \"%d\"\n"
		"\t\"rate\"         \"%.3g\"\n"
		"\t\"duration\"     \"%.3g\"\n"
		"\t\"shape\"        \"%s\"\n"
		"\t\"shape_size\"   \"%.3g\"\n"
		"\t\"cone_angle\"   \"%.3g\"\n"
		"\t\"dir_mode\"     \"%s\"\n"
		"\t\"speed\"        \"%.3g\"\n"
		"\t\"speed_jitter\" \"%.3g\"\n"
		"\t\"spread\"       \"%.3g\"\n"
		"\t\"radial_bias\"  \"%.3g\"\n"
		"\t\"gravity\"      \"%.3g\"\n"
		"\t\"drag\"         \"%.3g\"\n"
		"\t\"life_min\"     \"%.3g\"\n"
		"\t\"life_max\"     \"%.3g\"\n"
		"\t\"style\"        \"%s\"\n"
		"\t\"size_start\"   \"%.3g\"\n"
		"\t\"size_peak\"    \"%.3g\"\n"
		"\t\"size_end\"     \"%.3g\"\n"
		"\t\"ramp\"         \"%s\"\n"
		"}\n",
		d->name, EMIT_MODE_NAMES[d->mode], d->count, d->rate, d->duration,
		EMIT_SHAPE_NAMES[d->shape], d->shape_size, d->cone_angle,
		EMIT_DIR_NAMES[d->dir_mode], d->speed, d->speed_jitter, d->spread, d->radial_bias,
		d->gravity_scale, d->drag, d->life_min, d->life_max,
		EMIT_STYLE_NAMES[d->style], d->size_start, d->size_peak, d->size_end, ramp);

	Q_snprintf(path, sizeof(path), "particles/%s.pcl", d->name);
	COM_WriteFile(path, body, n);   // writes under com_gamedir (id1/)
	return 1;
}
```

> `COM_WriteFile` writes relative to the write game dir (`id1/`), so `"particles/<name>.pcl"` lands in `id1/particles/`. Confirm the dir exists (it will after Slice 3 Task 3.2). Also append the name to `index.txt` on first save — see Step 2.

- [ ] **Step 2: Append new names to `index.txt` on save.** After `COM_WriteFile` in `R_EmitterSave`, ensure the index contains the name (load, check, append if missing):
```c
	// keep index.txt in sync so the effect reloads next launch
	{
		char *idxbuf = (char *)COM_LoadTempFile("particles/index.txt");
		int present = 0;
		if (idxbuf) present = (strstr(idxbuf, d->name) != NULL);
		if (!present) {
			// rebuild from current registry (simple + correct)
			char acc[4096]; int j, m = 0;
			m += Q_snprintf(acc+m, sizeof(acc)-m, "# auto-maintained by R_EmitterSave\n");
			for (j = 0; j < EMIT_MAX_DEFS; j++)
				if (s_defs[j].used) m += Q_snprintf(acc+m, sizeof(acc)-m, "%s\n", s_defs[j].name);
			COM_WriteFile("particles/index.txt", acc, m);
		}
	}
```

- [ ] **Step 3: Implement `particle_reload` with reload-safety.** Add a command handler and register it in `R_EmitterInit`:
```c
static void R_ParticleReload_f(void)
{
	particle_t *p;
	int i;
	// Reload-safety: free all in-flight pt_emitter particles and stop all live
	// instances BEFORE rebuilding s_defs (stale def indices would be UB).
	R_EmitterStopAll();
	for (p = active_particles; p; p = p->next)
		if (p->type == pt_emitter) p->die = -1;  // reaped next R_DrawParticles pass
	for (i = 0; i < EMIT_MAX_DEFS; i++) s_defs[i].used = 0;  // clear registry
	R_EmitterLoadAll();
	Con_Printf("particle_reload: %d effect(s)\n", R_EmitterCount());
}
```
Register in `R_EmitterInit`:
```c
	Cmd_AddCommand("particle_reload", R_ParticleReload_f);
```

> The draw side already bounds-checks via `R_EmitterGetDef` returning NULL (integrate + draw both early-out), so even if a stale particle survives one frame it's safe. Setting `die=-1` retires them promptly.

- [ ] **Step 4: Compile gate** — `zig build`. Expected: success. (If `Q_strncat`/`Q_snprintf` aren't present, use `strncat`/`snprintf`.)

- [ ] **Step 5: Runtime check** — launch, `particle_spawn campfire`. Edit `id1/particles/campfire.pcl` (e.g. change `size_peak` to `12`) in another editor, then in console `particle_reload`, then `particle_spawn campfire` → bigger blobs. Confirm no crash and the old looping campfire stopped.

- [ ] **Step 6: Commit**
```bash
git add sdlquake/engine_src/r_emitter.c
git commit -m "feat(emitter): .pcl save + particle_reload with reload-safety"
```

---

# Slice 4 — Particle editor mode (UI)

Goal: `edit_particle.c` implementing `particle_mode` — effect list, inspector, palette-ramp + size-envelope widgets, preview controls. Replace the Slice 1 stub.

### Task 4.1: `IG_ColorSwatch` wrapper (for the palette grid)

**Files:**
- Modify: `sdlquake/engine/imgui_bridge.h`, `sdlquake/engine/imgui_bridge.cpp`

- [ ] **Step 1: Declare** in `imgui_bridge.h` (near `IG_ColorEdit3`, line ~120):
```c
// Clickable solid-color square (palette swatch). Returns 1 on click.
int  IG_ColorSwatch(const char *id, float r, float g, float b, float size);
```

- [ ] **Step 2: Define** in `imgui_bridge.cpp` (near the other `IG_` color funcs). Use ImGui's `ColorButton`:
```cpp
extern "C" int IG_ColorSwatch(const char *id, float r, float g, float b, float size)
{
    ImVec4 col(r, g, b, 1.0f);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop;
    return ImGui::ColorButton(id, col, flags, ImVec2(size, size)) ? 1 : 0;
}
```

- [ ] **Step 3: Compile gate** — `zig build`. Expected: success.

- [ ] **Step 4: Commit**
```bash
git add sdlquake/engine/imgui_bridge.h sdlquake/engine/imgui_bridge.cpp
git commit -m "feat(imgui): IG_ColorSwatch wrapper for palette swatches"
```

### Task 4.2: `edit_particle.c` skeleton + effect-list panel

**Files:**
- Create: `sdlquake/engine/editor/edit_particle.c`
- Modify: `build.zig` (editor source list, ~line 102)

- [ ] **Step 1: Create the file** with the mode vtable, state, and the effect-list panel:
```c
// edit_particle.c -- Particle editor mode. Edits emitter_def_t records in the
// r_emitter.c registry live; previews by spawning into the world. Engine-side
// (touches the renderer registry directly), like the rest of the editor.

#include "quakedef.h"
#include "r_emitter.h"
#include "imgui_bridge.h"
#include "editor_mode.h"

extern byte *host_basepal;  // 768 bytes RGB; for palette swatches

static int  s_sel = -1;       // selected registry index, -1 = none
static int  s_loop_handle = -1; // active loop-preview live handle
static char s_new_name[EMIT_NAME_LEN] = "new_effect";

// Forward decls of the panels (defined below).
static void panel_effect_list(void);
static void panel_inspector(void);

// ---- mode entry points ---------------------------------------------------
static void particle_enter(void)
{
	// pick the first effect if nothing selected
	if (s_sel < 0 || !R_EmitterGetDef(s_sel)) {
		int i; for (i = 0; i < EMIT_MAX_DEFS; i++)
			if (R_EmitterGetDef(i)) { s_sel = i; break; }
	}
}
static void particle_exit(void)
{
	if (s_loop_handle >= 0) { R_EmitterStopHandle(s_loop_handle); s_loop_handle = -1; }
}
static int particle_hide_fx(void) { return 0; } // SHOW particles in this mode

static void particle_draw_ui(void)
{
	panel_effect_list();
	panel_inspector();
}

// Exported vtable -- referenced by editor.c's s_modes[].
const editor_mode_t particle_mode = {
	.name = "Particle",
	.enter = particle_enter,
	.exit  = particle_exit,
	.draw_ui = particle_draw_ui,
	.hide_transient_fx = particle_hide_fx,
};

// ---- effect list ----------------------------------------------------------
static void panel_effect_list(void)
{
	IG_SetNextWindowPos(8.0f, 120.0f, IG_Cond_FirstUseEver);
	IG_SetNextWindowSize(220.0f, 360.0f, IG_Cond_FirstUseEver);
	if (!IG_Begin("Particle Effects", NULL, IG_WF_None)) { IG_End(); return; }

	// New
	IG_SetNextItemWidth(120);
	IG_InputText("##newname", s_new_name, sizeof(s_new_name), 0);
	IG_SameLine(0, -1);
	if (IG_Button("New")) {
		int idx = R_EmitterNew(s_new_name);
		if (idx >= 0) s_sel = idx;
	}
	IG_Separator();

	// List of used slots
	{
		int i;
		for (i = 0; i < EMIT_MAX_DEFS; i++) {
			emitter_def_t *d = R_EmitterGetDef(i);
			if (!d) continue;
			IG_PushID_Int(i);
			if (IG_Selectable(d->name, i == s_sel, 0)) s_sel = i;
			IG_PopID();
		}
	}

	IG_Separator();
	{
		emitter_def_t *d = R_EmitterGetDef(s_sel);
		IG_BeginDisabled(!d);
		if (IG_Button("Save") && d) {
			if (R_EmitterSave(s_sel)) Con_Printf("saved %s.pcl\n", d->name);
		}
		IG_SameLine(0, -1);
		if (IG_Button("Duplicate") && d) {
			char nm[EMIT_NAME_LEN];
			Q_snprintf(nm, sizeof(nm), "%.24s_copy", d->name);
			int idx = R_EmitterNew(nm);
			if (idx >= 0) { emitter_def_t *n = R_EmitterGetDef(idx); int keep=n->used; char kn[EMIT_NAME_LEN];
				Q_strncpy(kn, n->name, EMIT_NAME_LEN-1); *n = *d; n->used=keep; Q_strncpy(n->name, kn, EMIT_NAME_LEN-1); s_sel = idx; }
		}
		IG_SameLine(0, -1);
		if (IG_Button("Delete") && d) { R_EmitterDelete(s_sel); s_sel = -1; }
		IG_EndDisabled();
	}
	IG_End();
}
```

- [ ] **Step 2: Stub the inspector** for now (filled in 4.3-4.5) so the file compiles:
```c
static void panel_inspector(void) { /* Tasks 4.3-4.5 */ }
```

- [ ] **Step 3: Add to build.** In `build.zig` editor list (~line 102), add:
```zig
        "sdlquake/engine/editor/edit_particle.c",
```

- [ ] **Step 4: Compile gate** — `zig build`. Expected: **link error** — `particle_mode` is now defined in BOTH `edit_particle.c` (real) and `editor.c` (Slice 1 stub). Fixed in Task 4.6. Proceed.

### Task 4.3: Inspector — scalar/enum fields

**Files:**
- Modify: `sdlquake/engine/editor/edit_particle.c` (replace the `panel_inspector` stub)

- [ ] **Step 1: Implement the inspector body** with grouped widgets:
```c
static void panel_inspector(void)
{
	emitter_def_t *d = R_EmitterGetDef(s_sel);
	IG_SetNextWindowPos(236.0f, 120.0f, IG_Cond_FirstUseEver);
	IG_SetNextWindowSize(320.0f, 520.0f, IG_Cond_FirstUseEver);
	if (!IG_Begin("Particle Inspector", NULL, IG_WF_None)) { IG_End(); return; }
	if (!d) { IG_TextUnformatted("(no effect selected)"); IG_End(); return; }

	// Emit
	if (IG_CollapsingHeader("Emit", (1<<5))) {
		static const char * const modes[] = { "burst", "continuous" };
		IG_Combo("mode", &d->mode, modes, 2);
		// count is an int; edit via a temp float (no IG_DragInt wrapper exists).
		{ float c = (float)d->count; if (IG_DragFloat("count", &c, 1.0f, 1, 4096, "%.0f")) d->count = (int)c; }
		IG_DragFloat("rate (/s)", &d->rate, 1.0f, 0, 1000, "%.1f");
		IG_DragFloat("duration (s)", &d->duration, 0.05f, 0, 60, "%.2f");
	}
	// Spawn
	if (IG_CollapsingHeader("Spawn", (1<<5))) {
		static const char * const shapes[] = { "point", "sphere", "cone", "box" };
		IG_Combo("shape", &d->shape, shapes, 4);
		IG_DragFloat3("origin offset", d->origin_offset, 0.5f);
		IG_DragFloat("shape size", &d->shape_size, 0.5f, 0, 256, "%.1f");
		IG_DragFloat("cone angle", &d->cone_angle, 0.5f, 0, 90, "%.1f");
	}
	// Velocity
	if (IG_CollapsingHeader("Velocity", (1<<5))) {
		static const char * const dirs[] = { "along_shape", "inherit", "up" };
		IG_Combo("dir mode", &d->dir_mode, dirs, 3);
		IG_DragFloat("speed", &d->speed, 1.0f, 0, 2000, "%.1f");
		IG_DragFloat("speed jitter", &d->speed_jitter, 1.0f, 0, 1000, "%.1f");
		IG_DragFloat("spread (deg)", &d->spread, 0.5f, 0, 180, "%.1f");
		IG_DragFloat("radial bias", &d->radial_bias, 1.0f, -500, 500, "%.1f");
	}
	// Physics + lifetime
	if (IG_CollapsingHeader("Physics / Life", (1<<5))) {
		IG_DragFloat("gravity scale", &d->gravity_scale, 0.01f, -5, 5, "%.2f");
		IG_DragFloat("drag", &d->drag, 0.05f, 0, 20, "%.2f");
		IG_DragFloat("life min", &d->life_min, 0.05f, 0.05f, 20, "%.2f");
		IG_DragFloat("life max", &d->life_max, 0.05f, 0.05f, 20, "%.2f");
	}
	// Render (Task 4.4 adds size envelope; Task 4.5 adds ramp; Task 4.4 adds preview)
	if (IG_CollapsingHeader("Render", (1<<5))) {
		static const char * const styles[] = { "dot", "blob", "smoke" };
		IG_Combo("style", &d->style, styles, 3);
		// size envelope widgets -> Task 4.4
		// palette ramp widget   -> Task 4.5
	}

	IG_End();
}
```

> NOTE: delete the bogus `IG_DragFloat("count", (float*)0, 0);` placeholder line — it's shown only to flag that `count` is an int needing a temp float (the line right after it is the real one). Keep only the `{ float c = ...; }` block.

> `IG_Combo` signature is `int IG_Combo(const char *label, int *current_item, const char * const items[], int items_count)` (see `imgui_bridge.h:112`). `d->mode`/`d->shape`/etc. are `int`, matching `int*`.

- [ ] **Step 2: Compile gate** — `zig build`. Still expected to fail on the duplicate `particle_mode` (Task 4.6). Proceed (or temporarily comment the editor.c stub to verify this file compiles, then restore — optional).

### Task 4.4: Size-envelope widget + preview controls

**Files:**
- Modify: `sdlquake/engine/editor/edit_particle.c` (inside the "Render" header, replacing the size-envelope comment)

- [ ] **Step 1: Add the size-envelope sliders** (greyed for dot) inside the Render header, after the style combo:
```c
		IG_BeginDisabled(d->style == STYLE_DOT);
		IG_DragFloat("size start", &d->size_start, 0.1f, 0, 32, "%.2f");
		IG_DragFloat("size peak",  &d->size_peak,  0.1f, 0, 32, "%.2f");
		IG_DragFloat("size end",   &d->size_end,   0.1f, 0, 32, "%.2f");
		IG_EndDisabled();
		if (d->style == STYLE_DOT)
			IG_TextUnformatted("(dot size is distance-scaled; envelope N/A)");
```

- [ ] **Step 2: Add preview controls** at the end of `panel_inspector`, before `IG_End()`:
```c
	IG_Separator();
	IG_TextUnformatted("Preview");
	{
		// Spawn point = a short trace forward from the editor camera.
		vec3_t fwd, right, up, org, end;
		trace_t tr;
		AngleVectors(r_refdef.viewangles, fwd, right, up);
		VectorMA(r_refdef.vieworg, 200, fwd, org);   // 200u in front
		(void)end; (void)tr;

		if (IG_Button("Spawn at view")) {
			// one-shot for burst; for continuous this also parks an instance
			R_SpawnEffectIdx(s_sel, org, fwd);
		}
		IG_SameLine(0, -1);
		if (s_loop_handle < 0) {
			if (IG_Button("Loop preview")) {
				// force a continuous spawn parked in front of the camera
				int saved = d->mode; d->mode = EMIT_CONTINUOUS;
				s_loop_handle = R_SpawnEffectIdx(s_sel, org, fwd);
				d->mode = saved;
			}
		} else {
			if (IG_Button("Stop loop")) { R_EmitterStopHandle(s_loop_handle); s_loop_handle = -1; }
		}
		IG_SameLine(0, -1);
		if (IG_Button("Stop all")) { R_EmitterStopAll(); s_loop_handle = -1; }
	}
```

> `AngleVectors`, `VectorMA`, `r_refdef` are engine globals available via `quakedef.h`.

- [ ] **Step 2: Compile gate** — deferred to 4.6 (duplicate symbol). Proceed.

### Task 4.5: Palette-ramp editor widget

**Files:**
- Modify: `sdlquake/engine/editor/edit_particle.c`

- [ ] **Step 1: Add a palette-grid helper** (16×16 swatches; click sets a target stop's palette index):
```c
// Draw the 256-colour Quake palette as a 16x16 swatch grid. Returns the
// palette index clicked this frame, or -1.
static int palette_grid(float swatch)
{
	int clicked = -1, i;
	if (!host_basepal) return -1;
	for (i = 0; i < 256; i++) {
		float r = host_basepal[i*3+0] / 255.0f;
		float g = host_basepal[i*3+1] / 255.0f;
		float b = host_basepal[i*3+2] / 255.0f;
		char id[16]; Q_snprintf(id, sizeof(id), "##pal%d", i);
		if (IG_ColorSwatch(id, r, g, b, swatch)) clicked = i;
		if ((i & 15) != 15) IG_SameLine(0, 2);
	}
	return clicked;
}
```

- [ ] **Step 2: Add the ramp editor** inside the Render header (after the size-envelope block). Lets you set each stop's fraction (drag) and palette index (click a swatch with that stop "armed"):
```c
		IG_Separator();
		IG_TextUnformatted("Color ramp (palette index per stop)");
		{
			static int armed = 0;   // which stop the palette click targets
			int i;
			if (armed >= d->ramp_count) armed = 0;
			for (i = 0; i < d->ramp_count; i++) {
				IG_PushID_Int(1000 + i);
				// arming radio + the stop's current colour preview
				if (IG_RadioButton("##arm", armed == i)) armed = i;
				IG_SameLine(0, -1);
				if (host_basepal) {
					float r = host_basepal[d->ramp_pal[i]*3+0]/255.0f;
					float g = host_basepal[d->ramp_pal[i]*3+1]/255.0f;
					float b = host_basepal[d->ramp_pal[i]*3+2]/255.0f;
					IG_ColorSwatch("##cur", r, g, b, 16);
				}
				IG_SameLine(0, -1);
				IG_SetNextItemWidth(120);
				IG_DragFloat("frac", &d->ramp_frac[i], 0.01f, 0, 1, "%.2f");
				IG_PopID();
			}
			// add/remove stops
			if (d->ramp_count < EMIT_MAX_RAMP && IG_Button("+ stop")) {
				int n = d->ramp_count;
				d->ramp_frac[n] = 1.0f; d->ramp_pal[n] = 15; d->ramp_count++;
			}
			IG_SameLine(0, -1);
			if (d->ramp_count > 1 && IG_Button("- stop")) d->ramp_count--;

			// palette grid -> sets the armed stop's color
			{
				int picked = palette_grid(12.0f);
				if (picked >= 0 && armed < d->ramp_count) d->ramp_pal[armed] = (byte)picked;
			}
		}
```

- [ ] **Step 2: Compile gate** — deferred to 4.6. Proceed.

### Task 4.6: Wire `particle_mode` into the registry; remove the stub

**Files:**
- Modify: `sdlquake/engine/editor/editor.c`
- Modify: `sdlquake/engine/editor/editor_internal.h`

- [ ] **Step 1: Delete the temporary stub** in `editor.c` (added in Slice 1 Task 1.3 Step 3): remove `particle_stub_draw_ui` and the `static const editor_mode_t particle_mode = {...}` stub block at the bottom.

- [ ] **Step 2: Change the forward declaration** in `editor.c` from
```c
static const editor_mode_t particle_mode;   // defined in edit_particle.c (Slice 4)
```
to an `extern` (it's defined non-static in `edit_particle.c`):
```c
extern const editor_mode_t particle_mode;   // defined in edit_particle.c
```
And change `static const editor_mode_t map_mode;` to remain `static` (map_mode stays local to editor.c). The `s_modes[]` array mixes a static and an extern pointer — fine.

- [ ] **Step 3: Compile gate** — `zig build`. Expected: **success** (single `particle_mode` definition now).

- [ ] **Step 4: Runtime check** — `zig build run -- +map start`. F2 → switch to Particle mode. Confirm: effect list shows `campfire`/`spark_burst`; select `campfire`; inspector shows all fields; "Loop preview" parks a flame in front of the camera; tweak `size_peak` → blob grows live; open the ramp, arm a stop, click a palette swatch → flame color changes live; "Save" writes `id1/particles/campfire.pcl`; "Stop all" clears. Switch back to Map → map editor intact.

- [ ] **Step 5: Commit**
```bash
git add sdlquake/engine/editor/edit_particle.c sdlquake/engine/editor/editor.c \
        sdlquake/engine/editor/editor_internal.h build.zig
git commit -m "feat(editor): Particle editor mode -- list, inspector, ramp + envelope, preview"
```

---

# Slice 5 — Gameplay ABI

Goal: expose `SpawnParticleEffect` to the game DLL (ABI 36→37) and prove the route with a debug hook.

### Task 5.1: Add the ABI entry + version bump

**Files:**
- Modify: `sdlquake/game/game_api.h`

- [ ] **Step 1: Bump the version.** Change `#define GAME_API_VERSION 36` (line 7) to:
```c
#define GAME_API_VERSION 37
```

- [ ] **Step 2: Add the function pointer** to `engine_api_t`, after `SV_Decal` (line 248):
```c
    // Spawn an authored particle effect by name (particle editor / r_emitter.c).
    // org = world position; dir = forward direction (used by inherit/cone shapes).
    // Unknown name = no-op. ABI 36->37.
    void  (*SpawnParticleEffect)(const char *name, vec3_t org, vec3_t dir);
```

- [ ] **Step 3: Update the ABI changelog comment.** In `CLAUDE.md`'s sim module map note, the running list of ABI bumps mentions current `GAME_API_VERSION` is 36. (Documentation-only; can be done in the final docs task. Skip here to keep the commit focused.)

- [ ] **Step 4: Compile gate** — `zig build`. Expected: **fails** — `engine_funcs` in `hotreload.c` doesn't initialize the new field (a missing initializer warning is fine, but the symbol it should point to doesn't exist yet). Proceed to 5.2.

### Task 5.2: Wire it in `hotreload.c`

**Files:**
- Modify: `sdlquake/engine/hotreload.c` (the `engine_funcs` initializer at line 975)

- [ ] **Step 1: Extern-declare the runtime hook** near the top-of-file forward declarations (the file already forward-declares engine functions per its line-37 comment). Add:
```c
void R_SpawnParticleEffectByName(const char *name, vec3_t org, vec3_t dir);
```

- [ ] **Step 2: Add the initializer entry** inside the `static engine_api_t engine_funcs = { ... }` block (line 975+), alongside the other entries (e.g. after the `SV_Decal` line):
```c
    .SpawnParticleEffect = R_SpawnParticleEffectByName,
```

- [ ] **Step 3: Compile gate** — `zig build`. Expected: **success** (engine + `game.dll` rebuild at ABI 37; the loader's version check passes since both compile against the bumped header).

- [ ] **Step 4: Runtime check** — `zig build run -- +map start`. Confirm the game loads (no `GAME_API_VERSION mismatch` console error from the hot-reload loader). `r_emitter_active` still works.

- [ ] **Step 5: Commit**
```bash
git add sdlquake/game/game_api.h sdlquake/engine/hotreload.c
git commit -m "feat(abi): SpawnParticleEffect engine_api hook (GAME_API_VERSION 36->37)"
```

### Task 5.3: Prove the route from the game DLL

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c` (or `weapons_fire.c` — wherever an impulse handler is convenient)

- [ ] **Step 1: Add a debug impulse** that calls the hook from DLL code. Find the DLL's impulse dispatch (the fire system uses `impulse 210/211/...`). Add a handler for an unused impulse (e.g. `216`) that spawns `campfire` at the player's position:
```c
// Debug: prove the SpawnParticleEffect ABI route (particle editor).
// impulse 216 -> spawn the authored "campfire" effect at the player's feet.
if (self->v.impulse == 216 && eng->SpawnParticleEffect) {
    vec3_t org = { self->v.origin[0], self->v.origin[1], self->v.origin[2] };
    vec3_t up  = { 0, 0, 1 };
    eng->SpawnParticleEffect("campfire", org, up);
}
```
Place it next to the existing `impulse 210`/`211` handling so it's in the dispatch path. Match the local variable names actually in scope (`self`, `eng`/`engine`, the `entvars_t` accessor) — check the surrounding handlers.

- [ ] **Step 2: Compile gate** — `zig build`. Expected: success.

- [ ] **Step 3: Runtime check** — `zig build run -- +map start`, console `impulse 216`. Expected: a `campfire` flame appears at the player. This proves the DLL→engine→r_emitter route end to end.

- [ ] **Step 4: Commit**
```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(sim): impulse 216 debug -- spawn authored effect via SpawnParticleEffect"
```

### Task 5.4: Docs

**Files:**
- Modify: `CLAUDE.md` (Phase 8 ABI note + a short Particle Editor blurb)

- [ ] **Step 1: Update the ABI note** — in the `sim_nav.c` line that ends "`GAME_API_VERSION` 36 ...", append: "37 added `SpawnParticleEffect` for the data-driven particle editor (`r_emitter.c`)."

- [ ] **Step 2: Add an editor blurb** — under the editor module map, note `edit_particle.c` (Particle mode) and `editor_mode.h` (the mode seam), and under reference data note `id1/particles/*.pcl`.

- [ ] **Step 3: Commit**
```bash
git add CLAUDE.md
git commit -m "docs: particle editor + ABI 37 in CLAUDE.md"
```

---

## Self-review notes (gaps closed during planning)

- **`wind_drag_k[]` OOB** — `R_DrawParticles` indexes it by `p->type`; Task 2.3 adds the `pt_emitter` entry (a real crash risk if omitted).
- **Reload-safety** — Task 3.3 frees in-flight `pt_emitter` particles + stops live instances before clearing `s_defs`; draw/integrate also bounds-check via `R_EmitterGetDef`→NULL.
- **`particle_mode` definition ownership** — temporary stub in editor.c (Slice 1) → real definition in edit_particle.c (Slice 4 Task 4.6); the forward decl flips `static`→`extern`. The intermediate slices intentionally don't link cleanly at the duplicate-symbol point; Task 4.6 resolves it.
- **`Editor_RenderScene` runs when editor is closed** — preserved: dispatcher calls `MapMode_RenderScene` (self-gating) for Map, no-op for Particle-closed.
- **`r_emitter_active` single registration** — register once (in `r_main.c`); the alt site in `R_EmitterInit` is flagged to delete.
- **File enumeration** — engine has no portable dir-scan, so `.pcl` discovery uses an explicit `index.txt` (git-friendly, auto-maintained by Save).
- **int fields in ImGui** — `count` is `int`; the inspector edits it via a temp `float` + `IG_DragFloat` (no `IG_DragInt` wrapper exists).
- **Engine helper name drift** — `Q_snprintf`/`Q_strncpy`/`Q_strncat`/`Q_strcmp` are assumed; if a given name isn't in `common.h`, fall back to the C stdlib equivalent (engine code mixes both).
```

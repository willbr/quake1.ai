# Particle Editor — Design

**Date:** 2026-06-01
**Status:** Approved-in-principle (brainstorming) → ready for implementation plan
**Owner:** William Bettridge-Radford

## Summary

A **data-driven particle effect editor**, delivered as a **new editor mode** sibling
to the Phase 7 map editor. Authors define particle *effects* (emitters) as editable
data — emission, spawn shape, velocity, physics, lifetime, render style, palette-ramp
color — tune them live in an ImGui panel, preview them in-world, and save/load them as
`.pcl` preset files. Authored effects are **spawnable by name from gameplay** through a
new `engine_api` entry point, so the Phase 8 fire/oil and other sim systems can trigger
them.

This is the first of several planned editor modes; `.mdl` and texture editors are on the
roadmap. The design therefore introduces a lightweight **editor-mode seam** so future
modes plug in without churning the shell or the working map editor.

## Goals

- Author *new* particle effects without recompiling the engine.
- Live preview while editing (spawn at crosshair; loop a continuous emitter in view).
- Persist effects to disk as human-readable, git-diffable preset files.
- Let the game DLL fire authored effects by name.
- Establish a multi-mode editor shell that `.mdl`/texture editors can join later.

## Non-goals (YAGNI)

- No RGB color — the software renderer is 8-bit palette; color is a palette-index ramp.
- No new GPU/billboard render path — effects reuse the three existing software draw paths.
- No node-graph / scripting / sub-emitters / particle-spawns-particle. Flat emitter defs only.
- No migration of the existing hard-coded effects (rocket trail, blood, explosions) into
  the data model. They keep working unchanged. (A future task could re-express them as
  `.pcl` presets, but that is out of scope here.)
- No texture-mapped particles. Particles are palette splats / solid blobs / dithered smoke.

## Key constraints discovered in the codebase

1. **No mode abstraction exists.** `editor.c` *is* the map editor; its "modes" are only
   camera/view/face sub-states. Hosting a sibling mode requires a new seam.
2. **Reuse the particle pool.** Particles flow through a `free_particles → active_particles`
   singly-linked pool (`r_part.c`), integrated and freed in `R_DrawParticles`
   (`r_part.c:1873`, called from `r_main.c:1200`). The emitter runtime spawns into this
   same pool rather than building a parallel system.
3. **The 8-bit software renderer constrains "size."** Normal particles are distance-scaled
   point splats (`D_DrawParticle`). Only the *blob* (`D_DrawFireParticle`) and *smoke*
   (`D_DrawSmokeParticle`) paths have real size envelopes. So an effect carries a
   **render style** selecting one of these three paths; the size envelope only applies to
   blob/smoke.
4. **Color is a palette-index ramp.** `particle_t.color` is a palette index. Color over
   life is a list of `(life_fraction, palette_index)` stops, mirroring the engine's
   existing `ramp1/ramp2/ramp3` / `ramp_blood` arrays.
5. **`particle_t` is engine-internal and safe to grow.** The asm offset note in
   `d_iface.h` only matters if id386 is re-enabled (it is not in the SDL build), so adding
   a field is safe. The struct is not shared across the game ABI.
6. **Persistence fits the engine idiom.** `COM_Parse` (`common.c:911`) tokenizes the
   classic brace/key-value block format; `COM_WriteFile` (`common.c:1281`) writes files.
   No JSON dependency needed. The editor already writes files (`map_io.c`).

## Architecture

### Component map

| Piece | File(s) | New? | Role |
|---|---|---|---|
| Mode seam | `engine/editor/editor_mode.h`; refactor `editor.c`, `editor_ui.c` | new + wrap | `editor_mode_t` vtable; Map mode wraps existing code, Particle mode plugs in |
| Particle editor mode | `engine/editor/edit_particle.c` / `.h` | new | ImGui panels, preview triggers, mutates the live preset in the registry |
| Emitter runtime | `engine_src/r_emitter.c`; decls in `r_local.h` / `d_iface.h` | new | effect-def registry, live-emitter pool, `.pcl` load/save, per-frame tick |
| Particle hooks | `engine_src/r_part.c`, `engine_src/d_part.c` | edit | `pt_emitter` type + `short def`; integrate & draw from the def |
| ABI | `game/game_api.h` | edit | `SpawnParticleEffect` fn-ptr; `GAME_API_VERSION` 36 → 37 |
| Presets | `id1/particles/*.pcl` | new data | one file per effect |
| Build | `build.zig` | edit | add `r_emitter.c` to engine sources, `edit_particle.c` to editor sources |

### 1. Editor shell — the mode seam

```c
// engine/editor/editor_mode.h
typedef struct editor_mode_s {
    const char *name;                 // "Map", "Particle"
    void (*enter)(void);   void (*exit)(void);
    void (*frame)(void);              // per-frame mode logic (camera, hotkeys)
    void (*draw_ui)(void);            // ImGui panels for this mode
    void (*render_scene)(void);       // 3D overlay for this mode
    int  (*process_event)(void *ev);  // SDL event; returns 1 if consumed
    int  (*hide_transient_fx)(void);  // Map=1, Particle=0
    int  (*should_draw_player)(void);
} editor_mode_t;
```

The shell (`editor.c`) holds `static editor_mode_t *modes[]` and an active index. The
existing public entry points stay as the engine boundary and **dispatch to the active
mode**:

- `Editor_DrawUI` → draws a mode-switcher (combo/tab bar in the toolbar) then
  `active->draw_ui()`.
- `Editor_RenderScene` → `active->render_scene()`.
- `Editor_ProcessEvent` → `active->process_event()`.
- `Editor_PreRender` → `active->frame()`.
- `Editor_HideTransientFX` → `active->hide_transient_fx()` (this is why the seam matters:
  Map mode suppresses particles; Particle mode must *show* them).
- `Editor_ShouldDrawPlayer` → `active->should_draw_player()`.

Today's map-editor functions become `map_mode`'s slots — a thin wrap, **no rewrite of map
editor internals**. Switching modes calls `exit()` on the old and `enter()` on the new.
`.mdl`/texture editors later = additional `modes[]` registrations with zero shell churn.

### 2. Data model — `emitter_def_t` (one preset = one effect)

```c
// engine_src/r_emitter.h
typedef enum { EMIT_BURST, EMIT_CONTINUOUS } emit_mode_t;
typedef enum { SHAPE_POINT, SHAPE_SPHERE, SHAPE_CONE, SHAPE_BOX } emit_shape_t;
typedef enum { DIR_ALONG_SHAPE, DIR_INHERIT, DIR_UP } emit_dirmode_t;
typedef enum { STYLE_DOT, STYLE_BLOB, STYLE_SMOKE } emit_style_t;

#define EMIT_MAX_RAMP   8
#define EMIT_MAX_DEFS   128   // registry capacity

typedef struct {
    char            name[32];           // == filename stem; the lookup key

    emit_mode_t     mode;
    int             count;              // burst total
    float           rate;               // particles/sec (continuous)
    float           duration;           // sec; 0 = loop until stopped

    emit_shape_t    shape;
    vec3_t          origin_offset;
    float           shape_size;         // radius / box half-extent (per shape)
    float           cone_angle;         // half-angle, deg (SHAPE_CONE)

    float           speed, speed_jitter;
    emit_dirmode_t  dir_mode;
    float           spread;             // deg
    float           radial_bias;        // outward-from-origin velocity component

    float           gravity_scale;      // multiplies engine grav (sv_gravity-derived)
    float           drag;

    float           life_min, life_max; // sec

    emit_style_t    style;
    float           size_start, size_peak, size_end;  // blob/smoke only

    int             ramp_count;
    float           ramp_frac[EMIT_MAX_RAMP];   // 0..1 ascending
    byte            ramp_pal [EMIT_MAX_RAMP];   // palette index per stop
} emitter_def_t;
```

The registry is `emitter_def_t s_defs[EMIT_MAX_DEFS]` keyed by `name`. The editor mutates
entries in place so changes preview instantly.

### 3. Runtime — generic emitter

**Particle changes (`d_iface.h`, `r_part.c`, `d_part.c`):**

- New `pt_emitter` in `ptype_t`.
- Add `short def;` to `particle_t` (index into `s_defs`; only meaningful for `pt_emitter`).
- `R_DrawParticles` gains a `pt_emitter` branch: integrate position with
  `def->gravity_scale` and `def->drag`; compute `age = (cl.time - birth) / (die - birth)`;
  **no hardcoded ramp** — color and size come from the def at draw time.
- `d_part.c` routes `pt_emitter` to the draw path named by `def->style`
  (`D_DrawParticle` / `D_DrawFireParticle` / `D_DrawSmokeParticle`), sampling the
  size envelope (start→peak→end) and palette ramp at `age`.

**Emitter system (`r_emitter.c`):**

- `s_defs[]` registry, name-keyed; `R_EmitterFind(name)` → index or −1.
- **Live-emitter instance pool** — a small fixed array (`EMIT_MAX_LIVE = 64`) of running
  `continuous` emitters: `{ def_index, org, dir, born, expire, accum }`. `accum` carries
  fractional per-frame particle counts so low rates emit correctly.
- `R_UpdateEmitters(float dt)` — called once per frame next to `R_DrawParticles`
  (`r_main.c:1200`). For each live instance: emit `floor(rate*dt + accum)` particles into
  the free list; expire when `cl.time > expire` (unless `duration == 0`).
- `R_SpawnEffectIdx(idx, org, dir)` — `EMIT_BURST` spawns `count` immediately and registers
  nothing; `EMIT_CONTINUOUS` registers a live instance (returns a handle so the editor's
  loop-preview can stop it).
- A spawn helper builds each particle from the def: pick org within the shape, velocity
  from speed/jitter/dir_mode/spread/radial_bias, `die = cl.time + rand(life_min..life_max)`,
  `type = pt_emitter`, `def = idx`, `birth = cl.time`, initial `color = ramp_pal[0]`.
- Honors the existing pool-reserve etiquette (`SMOKE_GAMEPLAY_RESERVE`) so authored FX
  can't starve gameplay particles.
- Cvar `r_emitter_active` reports the live-instance count for headless/MCP asserts
  (mirrors the `fire_*_count` pattern).

### 4. Data flow

```
startup:  R_InitParticles ─→ R_EmitterLoadAll("id1/particles/*.pcl") ─→ s_defs[]
edit:     edit_particle.c mutates s_defs[idx] in place  (instant preview)
preview:  editor button ─→ R_SpawnEffectIdx(idx, cam_org, cam_fwd)
gameplay: game.dll ─→ eng->SpawnParticleEffect("campfire", org, dir)
                       └→ R_EmitterFind ─→ R_SpawnEffectIdx
each frame: R_UpdateEmitters(dt) ─→ spawn pt_emitter into free list
            R_DrawParticles      ─→ integrate + draw pt_emitter via its def
save:     editor ─→ R_EmitterSave(idx) ─→ COM_WriteFile("id1/particles/<name>.pcl")
```

### 5. Editor UI — Particle mode panels

- **Effect list** — pick / New / Duplicate / Delete / Save; per-effect dirty marker.
- **Inspector** — widgets for every §2 field, grouped Emit / Spawn / Velocity / Physics /
  Lifetime / Render. Combos for enums; sliders for scalars.
- **Palette-ramp editor** — add/drag stops on a 0→1 bar; clicking a stop opens the 16×16
  Quake palette swatch grid (host palette via the engine's `host_basepal`/`d_8to24table`).
- **Size-envelope** — start/peak/end sliders with a small sparkline; greyed for `STYLE_DOT`.
- **Preview controls** — *Spawn at crosshair* (one-shot burst at the camera-forward
  trace hit), *Loop preview* (parks a continuous instance in front of the camera; toggle
  off stops the handle), *Stop all*. Uses the editor's free-fly camera so particles read
  clearly.

### 6. Persistence — `.pcl` format

Quake KV-block text, parsed with `COM_Parse`, one file per effect under `id1/particles/`:

```
particle_effect
{
    "name"       "campfire"
    "emission"   "continuous"
    "rate"       "24"
    "duration"   "0"
    "shape"      "cone"
    "cone_angle" "12"
    "speed"      "60"
    "speed_jitter" "10"
    "spread"     "15"
    "gravity"    "-0.3"
    "drag"       "0.4"
    "life_min"   "1.1"
    "life_max"   "1.6"
    "style"      "blob"
    "size_start" "1"
    "size_peak"  "6"
    "size_end"   "0"
    "ramp"       "0.0:111 0.4:107 1.0:8"
}
```

- Unknown keys are ignored with a one-time warning (forward-compat).
- Missing keys take documented defaults (so a minimal file is valid).
- `ramp` is a space-separated `frac:palidx` list, ascending fraction.
- `R_EmitterLoadAll` scans the directory at startup; `particle_reload` console command
  re-scans. `R_EmitterSave` writes the active def back via `COM_WriteFile`.
- **Reload safety.** A live `pt_emitter` particle holds a `def` index. `particle_reload`
  and delete must not orphan in-flight particles into a stale/out-of-bounds def: on reload,
  free all active `pt_emitter` particles and stop all live emitter instances before
  rebuilding `s_defs[]` (simplest correct option). Drawing also bounds-checks `def`
  (`0 ≤ def < EMIT_MAX_DEFS` and slot in-use) and skips otherwise.

### 7. Gameplay integration

- `engine_api_t` (`game/game_api.h`): add
  `void (*SpawnParticleEffect)(const char *name, vec3_t org, vec3_t dir);`
  → bump `GAME_API_VERSION` **36 → 37**. Unknown name = no-op + one-time `Con_DPrintf`.
- Console command `particle_spawn <name> [x y z]` for quick testing without the editor
  (defaults to a trace from the player's eye).

## Verification

No unit-test suite exists; verification is **build-clean + in-game behavior** (per
`CLAUDE.md` and the user's "smoke test before handback" rule):

1. `zig build` clean (engine + `game.dll`, ABI 37 accepted by the loader).
2. Launch, open editor (F2), switch to Particle mode, confirm the map editor still works
   when switched back (seam regression check).
3. Author/preview an effect; *Spawn at crosshair* and *Loop preview* render particles.
4. `Save` writes a `.pcl`; `particle_reload` reloads it; restart picks it up.
5. `particle_spawn campfire` from the console spawns it.
6. Headless/MCP: `r_emitter_active` cvar reflects live instances; smoke-test rig
   (m7_skeleton, MCP teleport to (380,0,40) facing east, screenshot) shows the effect.
7. `game.dll` call path: a temporary `impulse`/debug hook calling
   `eng->SpawnParticleEffect` proves the ABI route (kept or removed per taste).

## Incremental delivery (build order)

Each slice is independently buildable and verifiable:

1. **Mode seam** — extract `editor_mode_t`, wrap the map editor as `map_mode`, add the
   switcher. Map editor behaves identically. (No particles yet.)
2. **Runtime core** — `pt_emitter`, `particle_t.def`, `r_emitter.c` registry + live pool +
   `R_UpdateEmitters`, integrate/draw branches. Drive it with one hardcoded def +
   `particle_spawn`. Prove particles render and obey the def.
3. **Persistence** — `.pcl` load/save, seed presets, `particle_reload`.
4. **Particle editor mode** — `edit_particle.c` panels, palette-ramp + size-envelope
   widgets, preview controls.
5. **Gameplay ABI** — `SpawnParticleEffect`, version bump, console command, debug hook.

## Open questions / defaults chosen

- **Render style limited to the 3 existing draw paths** (dot/blob/smoke) rather than adding
  a new billboard path. Chosen for scope; revisit if authors need more.
- **Palette-ramp color** (not RGB). Forced by the 8-bit software renderer; also the
  authentic Quake look.
- **`EMIT_MAX_DEFS = 128`, `EMIT_MAX_LIVE = 64`** — starting capacities; cheap to raise.
- **Gravity reference** — `gravity_scale` multiplies the engine's particle gravity constant
  (same source `R_DrawParticles` uses for `pt_grav`), so 1.0 ≈ a classic falling particle.

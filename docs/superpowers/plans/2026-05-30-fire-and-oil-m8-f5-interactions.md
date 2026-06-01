# Fire & Oil — M8 / F5 (Interactions) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the M8 fire loop with cross-system *interactions*: Gust extinguishes fire (and rescues a burning player), a burning enemy lights other nearby enemies (contact-spread), burning oil brightens the room (light "reveals you"), and the two carried-forward review items (igniter kill-attribution, STIM_FIRE stim-ring crowding) are resolved.

**Architecture:** All changes are **DLL-side**, in `sdlquake/game/sim/sim_fire.c` (+ its `sim.h` declaration) and `sdlquake/game/abilities.c`. No new engine ABI — `GAME_API_VERSION` stays **36**. Fire reuses primitives that already exist across the ABI (`Light_AddOverride`→`Lightmap_AddDelta`, `Wind_AddSmoke`, `T_Damage`, `SV_Fire`, `Stim_Emit`). Everything composes with the existing F1–F4 fire sim and the M1–M5 AI/light/wind systems.

**Tech Stack:** C (game.dll, modern-C — mid-block declarations OK), Zig build (`zig build game` rebuilds the DLL; `zig build run -- +map <m> --mcp-http 9876` launches windowed with the MCP rig). Hot-reload defers the DLL swap while a map is live — **restart a fresh instance to load new code** (see Verification rig notes).

**No unit-test harness exists in this project** (per `CLAUDE.md`). Per-task verification = **build success** (`zig build game`); end-to-end behavior is verified at runtime via the MCP rig in Task 5.

**Locked decisions** (from the spec's "F5 — locked decisions" block; do not re-litigate):
- Gust **consumes** lit oil in the cone (not snuff-to-unlit), and **always** puts the caster out (self-rescue).
- Contact-spread is **touching-range ~64u, immediate** (`fire_spread_radius` cvar).
- Contact-spread credits the **source's igniter (player-or-world), never the burning monster** — this closes the attribution carry-forward by construction.
- STIM_FIRE emits at **~2 Hz** per source.
- Oil-fire light = **paired ±`Light_AddOverride`**; burning edicts keep `EF_DIMLIGHT`.

---

## File Structure

| File | Change |
|---|---|
| `sdlquake/game/sim/sim_fire.c` | All new fire behavior: stim throttle, contact-spread + igniter-credit helper, `Fire_ExtinguishRegion`, oil-fire light pairing, smoke/light/spread cvars. |
| `sdlquake/game/sim/sim.h` | Declare the one new public fn `Fire_ExtinguishRegion`. |
| `sdlquake/game/abilities.c` | `gust_fire` calls `Fire_ExtinguishRegion` once (the F4 torch loop is untouched). |
| `CLAUDE.md` | Append F5 to the `sim_fire.c` fire bullet (Task 6). |
| `docs/superpowers/specs/2026-05-30-fire-and-oil-design.md` | Status line + locked decisions (already done at plan time). |

No new files. No `spawn.c` / `game_api.h` / `weapons.c` edits (no new entities, impulses, or ABI).

---

## Task 1: STIM_FIRE throttle (closes the stim-ring carry-forward)

Each burning edict and each lit oil patch currently emits `STIM_FIRE` every 10 Hz tick into the 512-entry / 5 s stim ring. Ten simultaneous fires = ~500 entries = ring full → sound/sight stims get evicted (AI goes deaf/blind). Throttle each source to ~2 Hz. AI avoidance is unaffected (it uses `Fire_NearestHazard`, a direct registry query, not the ring).

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`

- [ ] **Step 1: Add the throttle interval constant**

In `sim_fire.c`, in the constants block near the top (after `FIRE_SMOKE_RADIUS`, around line 27), add:

```c
#define FIRE_STIM_INTERVAL   0.5f              // 2 Hz: STIM_FIRE emit cadence per source (was 10 Hz; ring-crowding fix)
```

- [ ] **Step 2: Add a throttle timer to both source structs**

In `fire_burn_t` (the burn registry struct), after `float next_scorch;`:

```c
    float next_fire_stim;    // next g->time this burning edict may emit STIM_FIRE (2 Hz throttle)
```

In `oil_patch_t`, after `float next_dmg_time;`:

```c
    float next_fire_stim;    // next g->time this lit patch may emit STIM_FIRE (2 Hz throttle)
```

`fire_clear_slot` already zeroes the burn slot fields it cares about; add `s_burning[n].next_fire_stim = 0.0f;` there too so a recycled edict number starts fresh:

```c
static void fire_clear_slot(int n, edict_t *e) {
    if (n < 0 || n >= FIRE_MAX_BURNING) return;
    s_burning[n].active = 0;
    s_burning[n].corpse_timed = 0;
    s_burning[n].next_scorch = 0.0f;
    s_burning[n].next_fire_stim = 0.0f;
    ...
```

(Oil patches are `memset` to 0 on allocation in `Fire_AddOil`, so `next_fire_stim` starts at 0 there automatically.)

- [ ] **Step 3: Gate the burning-edict STIM_FIRE emission**

In `Fire_Frame`, the burning-edict loop emits `STIM_FIRE` unconditionally (the `stimulus_t st; ... Stim_Emit(&st);` block, ~line 616). Wrap it in a 2 Hz gate. Replace:

```c
        // Broadcast a fire stimulus so distant AI can register the threat.
        // F2 note: many simultaneous fire sources (oil patches) emitting at
        // 10 Hz each can crowd the 512-entry stim ring within the 5 s age
        // window and starve sound/sight stims — revisit throttling / ring
        // size when area fire lands.
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

with:

```c
        // Broadcast a fire stimulus so distant AI can register the threat.
        // Throttled to FIRE_STIM_INTERVAL (2 Hz) per source: at 10 Hz a dozen
        // simultaneous fires would saturate the 512-entry / 5 s stim ring and
        // evict sound/sight stims. Fire doesn't move fast, so 2 Hz is ample for
        // distant registration; AI *avoidance* uses Fire_NearestHazard (a direct
        // registry query), so it is unaffected by this throttle. (F5: closes the
        // F1/F2 stim-ring-crowding carry-forward.)
        if (g->time >= f->next_fire_stim) {
            f->next_fire_stim = g->time + FIRE_STIM_INTERVAL;
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

- [ ] **Step 4: Gate the oil-patch STIM_FIRE emission**

In `oil_frame`, the lit-patch branch emits `STIM_FIRE` unconditionally (the `stimulus_t st; ... Stim_Emit(&st);` block, ~line 507). Replace:

```c
        // Fire stimulus so AI registers/avoids the burning oil. F2 note: many
        // patches emitting at 10 Hz can crowd the 512-entry stim ring (see the
        // F1 note in Fire_Frame) -- revisit throttling if it starves stims.
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

with:

```c
        // Fire stimulus so AI registers/avoids the burning oil. Throttled to
        // FIRE_STIM_INTERVAL (2 Hz) per patch (F5: stim-ring-crowding fix) — a
        // burning trail of dozens of patches at 10 Hz would otherwise saturate
        // the ring. AI avoidance is unaffected (uses Fire_NearestHazard).
        if (g->time >= o->next_fire_stim) {
            o->next_fire_stim = g->time + FIRE_STIM_INTERVAL;
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

- [ ] **Step 5: Build**

Run: `zig build game`
Expected: builds clean, no warnings about the new fields.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "perf(fire): throttle STIM_FIRE to 2 Hz per source (M8/F5)

Closes the F1/F2 stim-ring-crowding carry-forward: a burning oil trail of
dozens of patches emitting at 10 Hz would saturate the 512-entry / 5 s stim
ring and evict sound/sight stims. Each burning edict and lit oil patch now
emits STIM_FIRE at 2 Hz. AI avoidance is unaffected (queries the fire
registry directly via Fire_NearestHazard, not the ring).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Contact-spread (entity→entity) + igniter-credit helper

The headline F5 moment: a burning enemy lights *other* nearby enemies (and props/barrels). Closes the igniter-attribution carry-forward by crediting the source's igniter (player-or-world), never the burning monster.

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`

- [ ] **Step 1: Add the spread-radius cvar**

In `Fire_Init`, after `eng->Cvar_Register("fire_oil_count", "0");`:

```c
    eng->Cvar_Register("fire_spread_radius", "64");  // entity->entity contact-spread reach
```

- [ ] **Step 2: Add the igniter-credit helper**

Add this static helper above `Fire_Frame` (e.g. just after `fire_find_edict`, or anywhere before `Fire_Frame`). It resolves a burn slot's *credit* igniter — the source's own igniter if it is a real, live, non-world entity (today only ever the player), else `NULL`. Returning `NULL` makes `Fire_Ignite` store `-1` (world fallback for damage attribution; last-known-pos fallback for the AI flee). A burning **monster is never returned**, so a monster is never stored as an igniter — which is exactly why the freed-and-reused-slot misattribution can't happen.

```c
// Resolve a burn slot's "credit" igniter for contact-spread: the source's own
// igniter, but only if it is a live, non-world entity (today that's just the
// player). Returns NULL otherwise (Fire_Ignite then stores -1 = world). This
// is what keeps a *monster* from ever being stored as an igniter -> the F1
// "freed-and-reused monster-igniter misresolution" carry-forward is closed by
// construction, and player credit propagates transitively down a spread chain.
static edict_t *fire_credit_igniter(const fire_burn_t *f) {
    edict_t *src = fire_find_edict(f->igniter_edict);
    if (src && !src->free && src != g->world) return src;
    return 0;
}
```

- [ ] **Step 3: Add the contact-spread scan to the burning-edict loop**

In `Fire_Frame`, inside the `for (edict_t *e = ...)` burning-edict loop, **after** the scorch-decal block (the `if (g->time >= f->next_scorch)` block, ~line 635) and before the loop's closing `}`, add the spread scan. It runs every tick for every burning edict (the burning set is small; this mirrors the documented O(N) registry walks elsewhere in the sim). Skip self, world, free, non-`takedamage`, and already-burning targets. Use 3D center distance so a fire on a ledge doesn't reach a target below.

```c
        // Contact-spread (F5): a burning edict ignites OTHER nearby flammable
        // edicts (monsters, props, barrels, even the player) within a touching
        // radius. This is the "burning enemy lights allies" moment. Bounded by
        // radius and per-tick; the burning set is small. Credit goes to the
        // source's igniter (player-or-world via fire_credit_igniter), never to
        // this burning monster -> attribution stays correct and a monster is
        // never stored as an igniter. Healthy monsters flee fire at ~160u
        // (sim_ai.c), so this mostly catches enemies that are cornered/packed
        // or a panicking burner crashing through them.
        {
            float sr = eng->Cvar_VariableValue("fire_spread_radius");
            if (sr > 0.0f) {
                float sr2 = sr * sr;
                float dps = f->dps;
                float secs = eng->Cvar_VariableValue("fire_secs");
                edict_t *credit = fire_credit_igniter(f);
                for (edict_t *o = eng->ED_Next(g->world); o; o = eng->ED_Next(o)) {
                    if (o == e || o == g->world || o->free) continue;
                    if (!o->v.takedamage) continue;
                    int on = eng->ED_GetNum(o);
                    if (on < 0 || on >= FIRE_MAX_BURNING || Fire_IsBurning(on)) continue;
                    float dx = o->v.origin[0] - e->v.origin[0];
                    float dy = o->v.origin[1] - e->v.origin[1];
                    float dz = o->v.origin[2] - e->v.origin[2];
                    if (dx*dx + dy*dy + dz*dz > sr2) continue;
                    Fire_IgniteMaybeCoated(o, secs, dps, credit);
                }
            }
        }
```

- [ ] **Step 4: Build**

Run: `zig build game`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(fire): entity->entity contact-spread + igniter-credit fix (M8/F5)

A burning edict now ignites other nearby takedamage edicts each fire tick,
within fire_spread_radius (default 64u) -- the 'burning enemy lights allies'
moment. Spread credits the source's igniter (player-or-world via the new
fire_credit_igniter helper), never the burning monster itself, so a monster
is never stored as an igniter and the F1 freed-and-reused-monster-igniter
misattribution carry-forward is closed by construction. Player credit
propagates transitively down a spread chain.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Gust extinguishes fire (`Fire_ExtinguishRegion`)

Gust gains a fire counter and a self-rescue: it puts the caster out (always), extinguishes burning edicts in the forward cone, and **consumes** lit oil patches in the cone (cancelling any scheduled cascade there). The F4 torch-snuff loop in `gust_fire` is left untouched — this is additive.

**Files:**
- Modify: `sdlquake/game/sim/sim.h` (declare the new fn)
- Modify: `sdlquake/game/sim/sim_fire.c` (implement)
- Modify: `sdlquake/game/abilities.c` (call it)

- [ ] **Step 1: Declare the new public fn in `sim.h`**

In the "Fire & oil" section of `sim.h`, after the `Fire_LightOilNear` declaration (~line 241):

```c
// Gust counter (F5): put the caster out (self-rescue, always), extinguish
// burning edicts in the forward cone, and CONSUME lit oil patches in the cone
// (cancelling any scheduled cascade there). `eye`/`forward` are the cone apex
// and axis; `cone_cos` is cos(half-angle); `range` is the reach. Called from
// abilities.c::gust_fire. DLL-internal -- no ABI change.
void Fire_ExtinguishRegion(edict_t *caster, const vec3_t eye, const vec3_t forward,
                           float range, float cone_cos);
```

- [ ] **Step 2: Implement `Fire_ExtinguishRegion` in `sim_fire.c`**

Add this near the other public fire functions (e.g. after `Fire_LightOilNear`, before `Fire_IgniteTraced`). It mirrors `gust_fire`'s flat-cone math (horizontal projection so floor oil ahead is caught when the player aims roughly level; falls back to 3D when looking near-vertical). Use a clean static cone-test helper (no macros). `oil_extinguish_patch` is referenced here but defined in this same task as a minimal stub (Task 4 extends it with the light un-pairing).

First, the static cone helper (place above `Fire_ExtinguishRegion`):

```c
// True if world point (px,py,pz) is within `range` of `eye` and inside the gust
// cone. Uses a flat horizontal cone when the aim is roughly level (so floor oil
// ahead is caught) and a full 3D cone when looking near-vertical. `fxy` is the
// pre-normalized horizontal forward; `flat_cone` selects the mode. Mirrors the
// cone math in abilities.c::gust_fire.
static int fire_point_in_cone(const vec3_t eye, const float fxy[2], int flat_cone,
                              const vec3_t forward, float range, float cone_cos,
                              float px, float py, float pz) {
    float dx = px - eye[0], dy = py - eye[1], dz = pz - eye[2];
    float d  = (float)sqrt(dx*dx + dy*dy + dz*dz);
    if (d > range || d < 1.0f) return 0;
    float nx = dx/d, ny = dy/d, nz = dz/d;
    if (flat_cone) {
        float nxy = (float)sqrt(nx*nx + ny*ny);
        if (nxy < 0.001f) return 0;
        return (nx*fxy[0] + ny*fxy[1]) / nxy >= cone_cos;
    }
    return (nx*forward[0] + ny*forward[1] + nz*forward[2]) >= cone_cos;
}
```

Minimal `oil_extinguish_patch` stub (also above `Fire_ExtinguishRegion`; Task 4 replaces it with the light-un-pairing version):

```c
// Consume a lit oil patch (Gust put it out, or it burned out). Task 4 extends
// this to un-pair the oil-fire room-brighten light override.
static void oil_extinguish_patch(oil_patch_t *o) {
    if (!o->active) return;
    o->active = 0;
    o->lit    = 0;
}
```

Then `Fire_ExtinguishRegion` itself:

```c
// Put out fire in a forward cone (Gust). Always extinguishes the caster
// (self-rescue). Extinguishes burning edicts and CONSUMES lit oil patches in
// the cone; cancels scheduled (not-yet-lit) cascades there so the cleared area
// doesn't immediately re-flare. No LOS test (matches the F4 torch-snuff loop).
void Fire_ExtinguishRegion(edict_t *caster, const vec3_t eye, const vec3_t forward,
                           float range, float cone_cos) {
    // Self-rescue: always put the caster out, regardless of the forward cone
    // (the caster is at the cone apex, not in front of it).
    if (caster) Fire_Extinguish(caster);

    // Flat-cone setup: horizontal projection of forward (matches gust_fire).
    float fxy_len = (float)sqrt(forward[0]*forward[0] + forward[1]*forward[1]);
    int   flat_cone = (fxy_len > 0.001f);
    float fxy[2] = { flat_cone ? forward[0]/fxy_len : 0.0f,
                     flat_cone ? forward[1]/fxy_len : 0.0f };

    // Burning edicts in the cone -> extinguish.
    for (edict_t *e = eng->ED_Next(g->world); e; e = eng->ED_Next(e)) {
        if (e == caster || e == g->world || e->free) continue;
        int n = eng->ED_GetNum(e);
        if (n < 0 || n >= FIRE_MAX_BURNING || !s_burning[n].active) continue;
        if (fire_point_in_cone(eye, fxy, flat_cone, forward, range, cone_cos,
                               e->v.origin[0], e->v.origin[1], e->v.origin[2]))
            Fire_Extinguish(e);
    }

    // Oil patches in the cone: consume lit ones; cancel scheduled cascades.
    for (int i = 0; i < OIL_MAX_PATCHES; i++) {
        oil_patch_t *o = &s_oil[i];
        if (!o->active) continue;
        if (!fire_point_in_cone(eye, fxy, flat_cone, forward, range, cone_cos,
                                o->origin[0], o->origin[1], o->origin[2])) continue;
        if (o->lit) {
            oil_extinguish_patch(o);   // consume (Task 4 adds light un-pairing here)
        } else if (o->ignite_at > 0.0f) {
            o->ignite_at = 0.0f;       // cancel a scheduled cascade in the cone
        }
    }
}
```

- [ ] **Step 3: Confirm definition ordering**

`fire_point_in_cone` and `oil_extinguish_patch` are `static` and must be defined *before* `Fire_ExtinguishRegion`. `oil_extinguish_patch` must also precede `oil_frame`'s burnout call (Task 4 Step 5) — placing both helpers just above `Fire_ExtinguishRegion` (which sits before `Fire_IgniteTraced`, well before `oil_frame`) satisfies both. Confirm ordering when you place the code.

- [ ] **Step 4: Call it from `gust_fire` in `abilities.c`**

In `abilities.c::gust_fire`, after the F4 flammable-light (torch) loop and before the "Spend energy + cooldown" block (~line 354), add:

```c
    // Fire counter (F5): Gust puts the caster out (self-rescue) and
    // extinguishes burning edicts + consumes lit oil in the cone.
    Fire_ExtinguishRegion(client, eye, forward, range, cone_cos);
```

`range`, `cone_cos`, `eye`, and `forward` are already in scope at the top of `gust_fire`. (`abilities.c` already `#include "sim/sim.h"`.)

- [ ] **Step 5: Build**

Run: `zig build game`
Expected: builds clean. If the statement-expression macro errors, switch to the static-helper fallback noted in Step 2.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim.h sdlquake/game/sim/sim_fire.c sdlquake/game/abilities.c
git commit -m "feat(fire): Gust extinguishes fire + self-rescue (M8/F5)

New DLL-internal Fire_ExtinguishRegion, called from gust_fire: always puts the
caster out (self-rescue, regardless of the forward cone), extinguishes burning
edicts in the cone, and consumes lit oil patches in the cone (cancelling any
scheduled cascade there). The F4 torch-snuff loop is unchanged. No ABI bump.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Oil-fire light brightening ("fire reveals you") + tuning cvars

Lit oil patches brighten the room via a **paired** `Light_AddOverride`: one positive delta when the patch lights, the matching negative when it goes out (burnout, Gust-consume, or pool-recycle). This makes the room visibly brighten *and* the M5 AI light-tier read brighter near the fire (so AI spots the player better). Burning edicts keep `EF_DIMLIGHT` (render-only). Also expose `fire_light` and `fire_smoke` cvars for in-session tuning.

**Why paired, not per-tick:** `Light_AddOverride` (sim_light.c) appends a persistent record and mirrors a `Lightmap_AddDelta` into the renderer — there is no per-record removal, so we must add exactly one `+delta` and later one `-delta` at the same position to net out. Calling it every tick would stack to infinity and exhaust the shared 1024-slot table.

**Files:**
- Modify: `sdlquake/game/sim/sim_fire.c`

- [ ] **Step 1: Add the tuning constants and cvars**

In the constants block, add (near `FIRE_SMOKE_*`):

```c
#define OIL_FIRE_LIGHT_RADIUS  160.0f    // reach of a lit oil patch's room-brightening override
```

In `Fire_Init`, after the `fire_spread_radius` registration (Task 2):

```c
    eng->Cvar_Register("fire_light", "96");          // lit-oil room-brighten delta (paired +/-)
    eng->Cvar_Register("fire_smoke", "0.12");        // smoke amount fed to the wind grid per fire tick
```

- [ ] **Step 2: Add a light-bookkeeping field to `oil_patch_t`**

After the throttle field added in Task 1 (`float next_fire_stim;`):

```c
    float light_delta;       // F5: the +delta applied via Light_AddOverride when lit (0 = none); subtract on extinguish
```

- [ ] **Step 3: Apply the positive override when a patch lights**

In `oil_light_patch`, after `o->ignite_at = 0.0f;` and the scorch decal, add the room-brighten override (applied exactly once — `oil_light_patch` early-returns if already lit):

```c
    // Room brightens while burning oil is lit -> visible lightmap delta AND the
    // M5 AI light-tier reads brighter near the fire (the "fire reveals you"
    // pillar). Paired: this +delta is matched by a -delta when the patch goes
    // out (burnout / Gust-consume / recycle). Stored so the - matches the +
    // even if the cvar changes mid-burn.
    o->light_delta = eng->Cvar_VariableValue("fire_light");
    if (o->light_delta != 0.0f)
        Light_AddOverride(o->origin, OIL_FIRE_LIGHT_RADIUS, o->light_delta);
```

- [ ] **Step 4: Make `oil_extinguish_patch` un-pair the light (extends Task 3's stub)**

Replace the minimal `oil_extinguish_patch` stub from Task 3 with the full version:

```c
// Consume a lit oil patch (Gust put it out, or it burned out). Un-pairs the
// room-brighten override so the lightmap delta nets to zero, then frees the slot.
static void oil_extinguish_patch(oil_patch_t *o) {
    if (!o->active) return;
    if (o->light_delta != 0.0f) {
        Light_AddOverride(o->origin, OIL_FIRE_LIGHT_RADIUS, -o->light_delta);
        o->light_delta = 0.0f;
    }
    o->active = 0;
    o->lit    = 0;
}
```

- [ ] **Step 5: Route the burnout path through `oil_extinguish_patch`**

In `oil_frame`, the lit-patch expiry currently does `o->active = 0;` directly:

```c
        // --- Lit patch ---
        if (g->time >= o->burn_until) {
            o->active = 0;   // oil consumed
            continue;
        }
```

Change it to un-pair the light:

```c
        // --- Lit patch ---
        if (g->time >= o->burn_until) {
            oil_extinguish_patch(o);   // oil consumed -> un-pair the room-brighten light
            continue;
        }
```

- [ ] **Step 6: Un-pair the light when recycling a lit patch in `Fire_AddOil`**

In `Fire_AddOil`, the pool-full branch recycles the oldest patch and then `memset`s it — which would zero `light_delta` *without* subtracting, leaking the override. Before the `memset(o, 0, sizeof(*o));`, un-pair if the recycled patch was lit:

```c
    oil_patch_t *o = &s_oil[slot];
    // If recycling a still-lit patch, un-pair its room-brighten light first so
    // the override doesn't leak (memset below would drop light_delta silently).
    if (o->active && o->lit && o->light_delta != 0.0f) {
        Light_AddOverride(o->origin, OIL_FIRE_LIGHT_RADIUS, -o->light_delta);
    }
    memset(o, 0, sizeof(*o));
```

- [ ] **Step 7: Use the `fire_smoke` cvar for both smoke emitters**

The two `Wind_AddSmoke(..., FIRE_SMOKE_AMOUNT, ...)` calls (oil patch in `oil_frame`, burning edict in `Fire_Frame`) use the `FIRE_SMOKE_AMOUNT` constant. Replace the amount with the cvar so it's tunable in-session. In `oil_frame`:

```c
        Wind_AddSmoke(o->origin, eng->Cvar_VariableValue("fire_smoke"), o->radius);
```

In `Fire_Frame`:

```c
        Wind_AddSmoke(e->v.origin, eng->Cvar_VariableValue("fire_smoke"), FIRE_SMOKE_RADIUS);
```

(Leave `FIRE_SMOKE_AMOUNT` defined as the default the cvar mirrors.)

- [ ] **Step 8: Build**

Run: `zig build game`
Expected: builds clean.

- [ ] **Step 9: Commit**

```bash
git add sdlquake/game/sim/sim_fire.c
git commit -m "feat(fire): burning oil brightens the room + tuning cvars (M8/F5)

Lit oil patches now add a paired +/- Light_AddOverride (fire_light, default 96)
so the room visibly brightens AND the M5 AI light-tier reads brighter near the
fire (the 'fire reveals you' pillar). The +delta is applied once on ignite and
the matching -delta on every lit->inactive path (burnout, Gust-consume,
pool-recycle) so the lightmap nets to zero and the shared override table can't
leak. fire_smoke cvar exposes the smoke amount for in-session tuning. Burning
edicts keep EF_DIMLIGHT (render-only). No ABI bump.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Runtime verification (MCP rig)

Verify the F5 behaviors end-to-end in a running instance. **No unit tests exist** — this is the real verification gate. Use the established MCP rig.

**Rig setup (from the F4 verification-rig learnings — reuse these to avoid known traps):**
- **Always launch a FRESH instance** to load the new DLL (hot-reload defers the swap while a map is live). Kill any process on port 9876 first, then:
  `zig build run -- +map start --mcp-http 9876`
  Use **`start.bsp`** — it has wall torches, a flat hall, and room to spawn monsters/oil; `e1m1` has no `light_torch`/`light_flame`.
- `list_entities` returns ALL non-free edicts with `id`/`classname`/`origin` (not filtered). Grab a target's `id`, then ignite precisely via `set_cvar fire_ignite_num <id>` (no crosshair aim needed).
- `wait_frames` advances game-time slowly; for a decisive kill crank `set_cvar fire_dps 60` so one DOT tick is lethal. Insert `screenshot` calls between `console_exec impulse` calls to force frame processing.
- Player/`inspect_entity` health reads are stale — read live state off a HUD screenshot or the console "player died" line.
- A reusable Python MCP client pattern is in `/tmp/f4_final.py` / `/tmp/f4_confirm.py` — copy and adapt to `/tmp/f5_verify.py`.

- [ ] **Step 1: Self-rescue — Gust puts the burning player out**

`teleport` to a known-good `start.bsp` spot; `console_exec god` OFF (so burn damage is real but we won't die instantly) — actually keep `god` ON to survive, since we only need to confirm the burn *flag* clears. Ignite the player (`set_cvar fire_ignite_num 1`), `wait_frames` a few, screenshot (player glows / red flash + `EF_DIMLIGHT`). Then `console_exec impulse`-bind or directly trigger Gust (`+gust`; or bind and press). After Gust, `wait_frames`, screenshot — the player's fire glow is gone.
Expected: player on fire → Gust → fire out. **PASS** = the `EF_DIMLIGHT`/flash is present before and absent after.

> Triggering Gust headlessly: Gust fires on the `+gust` button (button4). The MCP rig drives buttons via the held-command path; if `+gust` can't be pulsed from MCP, fall back to confirming `Fire_ExtinguishRegion`'s self-rescue by temporarily calling it — but prefer the real path. If button-pulsing is unavailable, document it as a rig limitation and verify self-rescue logically via the burning-edict-in-cone path instead (Step 3 covers the cone).

- [ ] **Step 2: Contact-spread — a burning enemy lights a neighbor**

Spawn two monsters close together (within ~64u). Easiest on `start.bsp`: use `console_exec impulse` monster-spawn if available, or `teleport` two existing monsters together, or spawn via the editor. If spawning two adjacent monsters is awkward, set `set_cvar fire_spread_radius 200` temporarily to make the spread reach generous, place/locate two monsters within that range, ignite one (`fire_ignite_num <idA>`), `wait_frames`, then read both monsters' `burning`/health: the second should start burning and taking DOT. Reset `fire_spread_radius` to 64 after.
Expected: monster A ignited → monster B catches fire within a couple of ticks. Confirm via the second monster's health dropping and/or `EF_DIMLIGHT`. **PASS** = B ignites from A with no oil between them.

- [ ] **Step 3: Gust consumes burning oil (firebreak/put-out)**

Pour an oil trail (`fire_oil_num` / `impulse 211` / `+pouroil`), light one end (`fire_oil_ignite 1`), confirm `fire_oil_count` > 0 and the trail is burning (screenshot). Aim at the burning oil and Gust. After Gust, read `fire_oil_count` and screenshot — the lit patches in the cone are gone (count drops) and the flames in the cone are out.
Expected: burning oil in the cone consumed. **PASS** = `fire_oil_count` drops and the flame is gone in the gusted cone.

- [ ] **Step 4: Oil-fire light — room brightens, then reverts**

In a dim area, screenshot before. Light an oil patch; `wait_frames`; screenshot — the area around the patch is visibly brighter. Let it burn out (or Gust-consume it); screenshot — brightness reverts (no residual bright spot, confirming the paired `-delta`).
Expected: brighten-on-light, revert-on-out. **PASS** = both transitions visible; no stuck bright patch after burnout.

- [ ] **Step 5: Throttle sanity (light check)**

Light several oil patches at once; confirm AI still reacts to non-fire stimuli (e.g. a monster still responds to player sound/sight) rather than going inert — a coarse check that the stim ring isn't saturated. This is a soft check; note observations.

- [ ] **Step 6: Record the verdict**

Write a short verdict (PASS/FAIL per step + screenshot paths) into the task notes. Reset any test cvars (`fire_dps 8`, `fire_spread_radius 64`, `god` off). If a step reveals a tuning need (spread radius too tight to ever trigger, light too dim/bright, smoke whiteout), adjust the relevant cvar default in `sim_fire.c` and note it — do not re-architect.

*(No commit unless a tuning-default change was made; if so, commit it with `tune(fire): … (M8/F5)`.)*

---

## Task 6: Docs + memory

- [ ] **Step 1: Update `CLAUDE.md`**

Append an F5 paragraph to the `sim_fire.c` fire bullet in the "Sim module map" section (the long bullet that currently ends with the F4 content). Summarize: F5 adds Gust-extinguish (`Fire_ExtinguishRegion`: self-rescue + cone extinguish + consume lit oil), entity→entity contact-spread (`fire_spread_radius`, credits source's igniter not the monster — closes the attribution carry-forward), STIM_FIRE throttled to 2 Hz (closes stim-ring carry-forward), and burning-oil room-brighten via paired `Light_AddOverride` (`fire_light`) + `fire_smoke` cvar. **No ABI bump (`GAME_API_VERSION` stays 36).** Read `CLAUDE.md` first (it comes from system context, not the Read tool, so the Edit will fail otherwise).

- [ ] **Step 2: Update the memory file**

Edit `/Users/wjbr/.claude/projects/-Users-wjbr-src-quake1-ai/memory/m8-fire-staged-build.md`: update the `description:` frontmatter to note F5 landed, add an F5 body paragraph (commits, what landed, both carry-forwards closed), and update the carry-forward line (the attribution + stim-ring items are now resolved; note the remaining oil-light override-table cap limit + the `oil_frame` O(patches×edicts) note still stand).

- [ ] **Step 3: Commit docs**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-05-30-fire-and-oil-design.md
git commit -m "docs(fire): record M8/F5 interactions (Gust-extinguish, contact-spread, light tuning)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

(The plan file and memory file follow the prior-stage pattern — plan left untracked locally; memory is outside the repo.)

---

## Self-review (against the spec)

- **Spec coverage:** Gust-extinguish (Task 3) ✓; contact-spread (Task 2) ✓; smoke/light tuning (Task 4) ✓; igniter-attribution carry-forward (Task 2 credit helper) ✓; STIM_FIRE crowding carry-forward (Task 1) ✓. The spec's §7 "all four interactions" — smoke (already F1/F2), light-reveals-you (Task 4), Gust-extinguish (Task 3), contact-spread (Task 2) ✓.
- **Type consistency:** `Fire_ExtinguishRegion` signature identical in `sim.h` (Step 3.1) and `sim_fire.c` (Step 3.2) and the `abilities.c` call (Step 3.4). `oil_extinguish_patch` is `static`, introduced as a stub in Task 3 then extended in Task 4 — Task 4 *replaces* it (same signature). `light_delta` / `next_fire_stim` field names consistent across Tasks 1/4. `fire_credit_igniter` returns `edict_t *`, consumed by `Fire_IgniteMaybeCoated(edict_t *)`.
- **No ABI bump:** confirmed — only DLL-internal additions; `game_api.h` untouched; `GAME_API_VERSION` stays 36.
- **Ordering hazard:** `oil_extinguish_patch` must be defined *before* `Fire_ExtinguishRegion` and before `oil_frame`'s burnout call; `fire_credit_igniter` before `Fire_Frame`. Each task's steps note placement.

# Fire & Oil (Phase 8 / M8) — Design

**Date:** 2026-05-30
**Status:** F1+F2+F3 implemented 2026-05-30 (F3 = oil gun + flamethrower); F4–F6 not started
**Phase:** 8 (Immersive-Sim Systems) — milestone **M8**, follows M1–M7

## Goal

Add fire as an emergent, systemic layer in the spirit of the existing Phase 8
work (Far Cry / Dishonored / BotW *emergence*, not a survival sim). The player
gains two complementary verbs — an **oil gun** that lays down flammable oil and a
**flamethrower** that ignites it (or sets things alight directly) — and the world
gains a fire/oil simulation that ties into the systems already built: the wind/
smoke grid (M4), the light tier (M5), the stimulus bus (M1), and the Gust ability
(M3).

The headline moment: **coat the floor in an oil trail, light one end, watch fire
race to a cluster of enemies (or an oil barrel), and watch the survivors panic
and flee while the smoke breaks their line of sight.**

## Relationship to prior Phase 8 work

The original immersive-sim spec (`2026-05-04-immersive-sim-systems-design.md`)
explicitly deferred fire: *"No pressure solve, no fire propagation, no gas."*
M8 picks up the deferred piece, but deliberately **stays out of full fluid/field
fire propagation**. Fire spread here is **discrete, bounded, patch-to-patch**
ignition on a timer — emergent in feel, but not a cellular solver. This honors
the original scoping decision while still delivering the "oil trail" fantasy.

## Pillars

1. **Emergence from orthogonal systems.** Fire is a new system that *pulls on*
   the existing ones (wind/smoke, light, stimulus, Gust) rather than a siloed
   weapon effect.
2. **Double-edged tools.** Oil and fire hurt the player too. Fire reveals you
   (it's bright). Oil is a tactical commitment, not a free win.
3. **Iteration loop respected.** Everything lives in `game.dll` and hot-reloads.
   No engine ABI bump for the MVP (see ABI delta).
4. **Bounded, deterministic sim.** Discrete oil-patch pools and a side-table burn
   registry — no unbounded edict spawning, no fluid solver, reproducible from
   inputs (MP-aware, MP-deferred — consistent with the M1 pillar).

## Architecture (Approach 1 — all in `game.dll`)

A new sim module `sim_fire.c`, peer to `sim_light.c` / `sim_wind.c`, wired into
`Sim_Frame` (`sim_main.c`). All fire/oil state lives in **DLL-side pools and
side-tables keyed by edict number** — exactly like `sim_light.c`'s override array
and `sim_ai.c`'s `ai_brain_t` table — never in `entvars_t`. This is what keeps
the ABI stable and the whole feature hot-reloadable.

The rejected alternative — an engine-side fire system — would force `engine_api_t`
churn, break hot-reload iteration on the part of the code that needs the most
tuning, and buy nothing: every primitive fire needs (lightmap delta, smoke
injection, particles, damage, traces) is already exposed across the ABI.

### File layout

```
sdlquake/game/
  weapons_fire.c     NEW — oil gun + flamethrower fire functions (Phase 6 weapon2 pattern)
  weapons_fire.h     NEW
  sim/
    sim.h            EDIT — add STIM_FIRE; add Fire_* API; add burning flag to ai_brain_t
    sim_fire.c       NEW — oil-patch pool, burn registry, cascade, tick, render, Gust hook
    sim_ai.c         EDIT — STIM_FIRE avoidance + on-fire panic/flee overlay
    sim_main.c       EDIT — call Fire_Frame() in Sim_Frame ordering
  abilities.c        EDIT — Gust calls Fire_ExtinguishRegion()
  misc.c             EDIT — misc_oilbarrel (extends existing barrel), (re)lightable torches
  spawn.c            EDIT — register new classnames
  items.c / player.c EDIT — weapon pickups, impulse selection, ammo wiring
id1/maps/
  ai_t10_fire.map    NEW — dedicated fire test level (ai_tNN convention)
```

`sim.h` remains the only header crossed between sim modules.

### Engine ABI delta

**None for the MVP.** `GAME_API_VERSION` stays **33**. Fire reuses existing
`engine_api_t` calls:

- Fire → light: `Lightmap_AddDelta(pos, radius, color, owner=GAMEPLAY)` with a
  **positive** delta — the literal inverse of Gust's negative-delta torch-snuff
  (`sim_light.c::Light_AddOverride`). `Lightmap_ClearOwner` already clears on
  level change.
- Fire → smoke: `Wind_AddSmoke(origin, amount, radius)` (M4 grid already breaks
  AI LOS via `Wind_PathOcclusion`).
- Visuals: `SV_Particle` / `SV_Smoke` (orange/red palette band) + `EF_DIMLIGHT`
  on burning edicts for a true dynamic light.
- Damage: `T_Damage` on a timer (DOT).
- Audio: `SV_StartSound` / `SV_AmbientSound` (`ambience/fire1.wav` exists).

`STIM_FIRE` is added to `stim_kind_t` in `sim.h`, which is DLL-internal — **not**
an ABI change. New `ai_brain_t` fields are likewise DLL-internal.

**ABI bumps are deferred to optional polish only:** a dedicated fuel HUD gauge or
a proper flat oil-slick floor-decal painter would each need engine-side work and
a version bump. Neither is in the MVP (see locked decisions A and B).

## System 1 — Burn primitive (the one genuinely new mechanic)

There is no damage-over-time precedent in the codebase today (`T_Damage` is
instant-only). `sim_fire.c` introduces a **burning registry**: a side-table
keyed by edict number holding `{burn_until, dmg_rate, igniter_edict, next_tick}`.

- A **10 Hz fire tick** iterates the registry and calls the existing `T_Damage`
  on each burning edict (attacker = `igniter` so kills are attributed; falls back
  to `world`).
- Works uniformly for monsters, the player, barrels, and props — anything with an
  edict number.
- Burning **ends** on: timeout (`burn_until`), death, entering water
  (`SV_PointContents` == water/slime/lava-already-dead), or Gust.
- A "coated" flag (set by oil contact, §2) means the edict ignites instantly on
  any fire contact and burns longer/hotter.

## System 2 — Oil substance

A DLL-managed **oil-patch pool** (fixed-capacity array; records:
`{origin, radius, amount, lit_state, deposit_time, ignite_time}`). Patches are
**not edicts** — this keeps the 600-edict budget free of puddles and keeps the
pool cheap to iterate.

- **Deposit:** oil gun, oil barrels, and map entities call
  `Fire_AddOil(origin, radius, amount)`.
- **Persistence:** patches persist until ignited or until a generous timeout
  (tunable; long enough to set up trails).
- **Coating:** if an oil stream/patch lands on an edict, that edict gets the
  "coated" flag in the burn registry → instant ignition on fire contact.
- **Merge/cap:** nearby deposits merge (grow `amount`/`radius` up to a cap)
  rather than spawning unbounded records; oldest patches are recycled when the
  pool is full (logged, never silent — consistent with project convention).
- **Render:** drawn each frame from the pool as dark particle clusters
  (`SV_Particle`/`SV_Smoke`, dark palette). See locked decision B for fidelity.

## System 3 — Cascade (fire spread)

When a patch ignites, the fire tick scans for **nearby/touching unlit patches**
and schedules each to ignite after a short delay → fire visibly **races down an
oil trail**. This is discrete patch-to-patch ignition on a timer, **not** a fluid
solver (honors the chosen tier).

- An edict standing in a now-burning patch ignites (enters the burn registry).
- A **burning edict moving through oil ignites it** (contact spread) — so a
  panicking, burning enemy becomes a hazard to its own group and can light oil
  it runs across.
- Burning patches burn for a tunable duration, doing area DOT to edicts inside
  their radius each tick, then expire (oil consumed).

## System 4 — Weapons (Phase 6 `weapon2` pattern)

Both weapons live behind the existing `self->v.weapon2 != 0` Phase 6 dispatch
(`W_Attack` → `W_Attack_Phase6`), implemented in a new `weapons_fire.c` so they
don't tangle with `weapons_phase6.c`'s Doom/Wolf state.

- **Oil gun** — sprays a short stream that deposits oil patches at the trace
  endpoint and coats hit monsters. Inert until ignited.
- **Flamethrower** — short-range **cone**: ignites oil patches and sets hit
  edicts burning, plus direct DOT inside the cone. Continuous fuel drain while
  held.
- **Selection/grant:** impulse commands select them; `weapon_oilgun` /
  `weapon_flamethrower` pickups (and an impulse cheat) grant them. New `IT_*`
  bits in `game_defs.h`.

### Locked decision A — ammo

Both weapons draw from a **shared fuel pool that reuses `ammo_cells`**. Rationale:
the status bar already draws `cells`, so this is **zero HUD/engine work**, and the
player rarely contends with the lightning gun early. A dedicated fuel gauge is a
deferred polish item (needs engine-side sbar work + ABI bump).

## System 5 — Flammables & ignition sources

- **Player burns** — standing in burning oil, or firing the flamethrower
  point-blank into a wall, ignites the player; Gust puts you out (§7). Optional
  "burned to death" obituary as polish.
- **Oil barrels** (`misc_oilbarrel`) — **extend the existing barrel**
  (`misc.c::barrel_explode`, `takedamage = DAMAGE_AIM`): on death → explosion +
  `Fire_AddOil` spray of burning patches around it. Completes the
  trail → barrel → boom loop.
- **Torches / lights** — fire **(re)lights** extinguished or unlit torches:
  positive `Lightmap_AddDelta`, restore the flame model + looping `fire1.wav`.
  A lit torch ignites oil that touches it. The clean inverse of the existing
  Gust-extinguishes-torch mechanic (closes that loop both directions).
- **Breakable wooden props** — catch fire, burn away over a few seconds, then
  emit the existing `STIM_PROP_BROKEN` stimulus on consumption.
- **Ignition sources for oil:** flamethrower, explosions (rockets / grenades /
  barrels), lava + lavaball, lit torches.

## System 6 — AI reaction

- **`STIM_FIRE`** added to `stim_kind_t`. Burning patches and burning edicts emit
  it each tick (intensity ∝ fire size, position = patch/edict origin).
- **On fire → panic:** a `burning` overlay flag on `ai_brain_t` forces a
  flee/panic behavior — run away from the nearest threat, suppress attacks — until
  the fire goes out or the monster dies. Reuses existing movement code.
- **Not burning → avoid:** when choosing a movement step, the brain **rejects
  steps that would enter a burning region** and flees if it finds itself inside
  one (queries `Fire_HazardAt(pos)`).

### Scope honesty — AI nav avoidance

MVP avoidance is **"won't step into fire + flee if inside."** Full A* **re-routing**
around dynamic fire is a **stretch goal, not MVP** — the navmesh is static and
baked, so dynamic re-routing is a meaningfully larger task. The MVP behavior is
honest about this: monsters won't suicide into fire, but they won't compute a
clever detour around a wall of flame either.

## System 7 — Cross-system interactions (all four selected)

- **Smoke** — burning patches feed `Wind_AddSmoke`; the M4 grid already breaks AI
  LOS via `Wind_PathOcclusion`, so fire throws up a concealing smoke screen for
  free.
- **Light (reveals you)** — burning patches add a positive `Lightmap_AddDelta`
  (the room visibly brightens, and AI near the fire sees the player better via
  the M5 light-tier sense filter), and burning edicts set `EF_DIMLIGHT` for a
  real-time dynamic light. Double-edged: fire is bright.
- **Gust extinguishes fire** — `abilities.c` Gust calls a new
  `Fire_ExtinguishRegion(origin, dir, cone)` (DLL-internal) that snuffs oil
  patches, clears burning edicts, and puts out the player inside the cone —
  mirroring its existing torch-snuff. Gives fire a counter and a self-rescue.
- **Contact spread** — covered in §3 (burning edict ignites oil + other edicts on
  contact).

> Note: the user explicitly **declined wind *steering* of the flame front** (the
> "trails + wind-fanned" option). Fire *emits* smoke into the wind grid, but the
> wind does **not** push the flame front. Keep these separate.

## System 8 — Visuals & audio

- **Fire:** `SV_Particle` / `SV_Smoke` in the orange/red palette band, animated
  from the pool each frame, plus `EF_DIMLIGHT`. Reuse the existing
  `flame.mdl` / `lavaball` palette feel.
- **Oil (locked decision B):** dark flat **particle cluster** for the MVP. A
  proper flat floor **decal** (modeled on the engine's `spawn_blood_pool` floor
  painter) reads far better but needs a small engine-side oil-decal call (ABI
  touch) — **deferred to follow-up polish**.
- **Audio:** looping `ambience/fire1.wav` for active fire; new oil-gun and
  flamethrower fire sounds; all precached at spawn/level init.

## Deployment & testing

### Locked decision C — where it lives

A **dedicated fire test level** `id1/maps/ai_t10_fire.map` (following the t01–t09
AI-test-level convention) wires oil barrels, pre-placed slicks, torches, breakable
props, and monsters into one room to exercise the whole system. We do **not**
auto-retrofit id1 maps with oil/props (weird and risky) — but the oil gun works in
any map because the player makes their own oil. The systems are also exercised in
`m7_skeleton`.

### Verification

- **MCP rig** (per the existing smoke-test pattern): give the fire weapons, spray
  oil, ignite, screenshot, and confirm DOT is applied and monsters panic/flee.
- Optional **`fire_query` MCP tool** (list live oil patches + burning edicts) in
  the spirit of `nav_edges_near`, for headless assertions.
- Headless bot run where feasible.
- Build success + in-game visual/audio correctness remain the primary methods
  (no unit-test harness in this project).

## Staged milestones (each independently testable)

| Stage | Delivers | Verify by |
|---|---|---|
| **F1 Burn primitive** | Burn registry + 10 Hz DOT tick + fire particles/light/smoke + `STIM_FIRE` emission | MCP-ignite a monster → it takes DOT, panics, others avoid |
| **F2 Oil substance** | Oil-patch pool, deposit/coat, cascade, ignite-on-contact | MCP-spawn oil + ignite → trail races, edicts in it ignite |
| **F3 Weapons** ✅ | Oil gun + flamethrower (Phase 6 `weapon2`), pickups, impulses, `cells` ammo | Spray + torch in-game |
| **F4 Flammables** | `misc_oilbarrel`, (re)lightable torches, breakable props, player-burns | Trail → barrel → boom; light a torch; burn a crate; burn self |
| **F5 Interactions** | Gust-extinguish, contact-spread, smoke/light tuning | Gust out a fire; burning enemy lights allies |
| **F6 Test level + tooling** | `ai_t10_fire.map`, optional `fire_query` MCP tool, balance/perf pass | Full-room playtest + MCP assertions |

## Non-goals (YAGNI)

- No fluid/cellular fire-propagation solver (discrete patch cascade only).
- No wind *steering* of the flame front (declined; fire only *emits* smoke).
- No dedicated fuel HUD gauge for MVP (reuse `cells`).
- No floor-decal oil rendering for MVP (particles; decal is follow-up polish).
- No auto-retrofit of id1 maps with oil/barrels/props.
- No full A* re-routing of AI around dynamic fire for MVP (avoid + flee only).
- No new engine ABI for MVP (`GAME_API_VERSION` stays 33).

## Risks & open questions

- **Edict-free oil pool capacity / merge tuning.** Need a sensible cap and merge
  radius so dense oil-gun spraying doesn't thrash the pool. Log recycling, never
  silently drop.
- **DOT balance.** Burn dmg_rate / duration vs. monster HP and player HP — must be
  threatening but not instantly lethal. Tune in F5/F6.
- **Panic-flee vs. existing FSM.** The `burning` overlay must compose cleanly with
  IDLE/SUSPICIOUS/SEARCHING/COMBAT without deadlocking movement. Prototype early
  in F1.
- **Light/smoke double-dip.** Fire adds light *and* smoke to the same spot; confirm
  the M5 light-tier sense and M4 LOS-occlusion combine sanely (bright but
  smoke-obscured) rather than canceling oddly.
- **`Lightmap_AddDelta` churn.** Many small moving fire lights writing lightmap
  deltas every tick could be costly; may need to throttle/aggregate. Watch in
  perf capture.
- **Stretch — oil decal & fuel gauge** would be the first things to add an ABI
  bump for, once the MVP proves the feel.
- **F1-built; carried forward (from F1 reviews):**
  - **Igniter kill-attribution (revisit in F5).** The burn registry stores the
    igniter as an edict *number*; `fire_find_edict` could misresolve it if a
    *monster* igniter is freed and its slot reused mid-burn. Unreachable in F1
    (igniters are only the player and world), but F5's "burning enemy ignites
    allies" makes monster igniters real — stamp/validate the igniter, or accept
    the world fallback, then.
  - **Stim-ring crowding (revisit in F2).** Each burning source emits
    `STIM_FIRE` at 10 Hz into the 512-entry, 5 s stim ring; many simultaneous
    oil-patch fires could evict sound/sight stims. Throttle or coalesce when
    area fire lands. (Also noted in a code comment by the emit site.)

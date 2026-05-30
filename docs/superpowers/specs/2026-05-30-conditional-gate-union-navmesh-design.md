# Conditional-gate union navmesh — design

**Date:** 2026-05-30
**Status:** Approved (design); pending spec review → implementation plan
**Area:** `sdlquake/game/sim/sim_nav.c` (nav bake + A*), `sdlquake/game/` items/serverflags, engine console command

## Problem

`start.bsp` (the hub) contains brush entities whose collision state is decided
at spawn time from `serverflags & 15` (the four episode sigils):

- `func_bossgate` — a 16u-thick floor **slab** (top at z=0, footprint
  X 336..736, Y 1600..2016) that **caps a deep shaft**. The boss
  `trigger_changelevel "end"` sits at the bottom (≈ z −608, past a zombie pit
  at z −264). Solid while `(serverflags & 15) != 15` (closed → stand on it,
  shaft capped, boss unreachable). Non-solid once all four sigils are held
  (open → descend the shaft to the boss).
- `func_episodegate` ×4 — same threshold-slab shape, one per episode entrance.
  Each carries a spawnflag bit (E1=1…E4=8) and becomes **solid once you hold
  that episode's rune** (`misc.c:482`), sealing the completed episode.

The flood-fill bake (`bake_floodfill`) reads **live** collision, so a single
bake captures exactly one gate configuration. The nav cache is keyed only by
`start-<bspsize>.nav` (`sim_nav.c:2053`) with no gate state in it. Therefore a
mesh baked in one state is silently reused in another: e.g. a closed-state mesh
(slab-top nodes, capped shaft) is served after the player returns to the hub
holding all four runes, so the bot pathing is wrong for the live world.

A prior change (`feat(nav): keep func_bossgate solid during bake`, commit
20578cb) made the **closed** state correct by keeping `func_bossgate` solid
during the bake so nodes seat on the slab top (verified: gate-centre node feet
z −95.97 → 0.00). This spec covers making **both** states correct in one mesh.

## Goal / non-goals

**Goal:** One navmesh per map that encodes *all* conditional-gate states, with
per-edge conditions so A* follows the correct subgraph for the live
`serverflags`. Covers `func_bossgate` + the four `func_episodegate`. The cache
stays a single state-independent file.

**Non-goals:** No new movement/traversal style (no new `NAV_EDGE_*` kind — the
gate top and the descent are ordinary walk/drop edges). No change to how keyed
doors work beyond reusing their tagging machinery. No mid-play gate-state
animation — gate state only changes across map loads.

## Approach: union mesh + conditional edges

Chosen over per-state cache files because it adapts at query time with zero
rebakes and generalizes the existing key-locked-door pattern
(`requires_items` + `player_items`, tagged at `sim_nav.c:1657`).

### 1. Conditional-edge model

Two per-edge bitmask predicates (not a literal C enum — composes with the
existing key-door field):

- `requires_items` (exists today): edge valid only if
  `(items & requires) == requires`.
- `forbids_items` (**new**): edge invalid if
  `forbids != 0 && (items & forbids) == forbids` ("blocked once you hold *all*
  these bits").

A* edge-skip becomes one added clause:

```
skip edge if (items & requires) != requires
          or (forbids != 0 && (items & forbids) == forbids)
```

**Sigil bits.** Reserve `IT_SIGIL1..4 = 1<<28 .. 1<<31` in the items bitmask.
These align with the engine's existing convention — `sv_main.c:649` already
sends `serverflags << 28` in the client items stat. Highest gameplay item is
`IT_QUAD = 1<<22`, and keys are `1<<17`/`1<<18`, so 28–31 are unused by
inventory and do not collide.

**Effective items at query time:** `eff = player_items | sigil_bits`, where
`sigil_bits = ((int)serverflags & 15) << 28`. Keyed-door edges (which require
key bits, never sigil bits) are unaffected: a monster with `player_items = 0`
still skips them.

Per-gate tagging (uniform across gate types):

| Edge | Exists when | Tag |
|---|---|---|
| bossgate slab-top | solid: lack all 4 sigils | `forbids = SIG1\|2\|3\|4` |
| bossgate descent | open: have all 4 | `requires = SIG1\|2\|3\|4` |
| episode-gate *e* slab-top | solid: completed ep *e* | `requires = SIGe` |
| episode-gate *e* entry passage | open: not completed | `forbids = SIGe` |

### 2. Union bake

The flood can't have a gate both solid and non-solid in one pass, so the bake
runs the gate world in both configurations and merges, attributing each
conditional edge to the specific gate whose bbox it interacts with.

- **Primary flood — all conditional gates forced SOLID.** Generalizes the
  existing `func_bossgate` keep-solid (commit 20578cb) to `func_bossgate` +
  `func_episodegate` in the bake's neutralize whitelist (`sim_nav.c:925`).
  Establishes the canonical node set and every gate's slab-top nodes.
- **Tag closed-only edges.** Edges whose nodes sit on a gate's top surface
  (z ≈ gate top, inside its xy footprint) get that gate's "solid" predicate.
  Reuses the bbox-crossing test from the Phase 4.6 key-door tagging
  (`sim_nav.c:1657`).
- **Supplemental pass — each gate non-solid, seeded from its approach nodes,
  bounded to its neighbourhood.** Discovers the open-passage / descent edges
  (e.g. the chain of drop/walk edges from the bossgate lip down the shaft).
  These link into the lower shaft nodes the primary flood already baked as a
  *disconnected* component (confirmed by probe: nodes exist at z ≈ −360…−664 by
  the boss trigger even when closed). New edges get that gate's "open"
  predicate.
- **Merge into one mesh.** Most edges unconditional; only gate-adjacent edges
  carry a predicate.

### 3. Query, cache, dev tooling

- **Query sites** (`sim_ai.c:397`, `:428`, and the autonomous bot) compute
  `eff = player_items | sigil_bits` from live `serverflags` and pass it in.
  Even monster pathing then respects the live gate state — strictly more
  correct than today.
- **Cache stays state-independent.** One mesh encodes all states, so the key
  remains `start-<bspsize>.nav` — no `serverflags` in the filename, no rebake
  on hub re-entry. The new `forbids_items` field is serialized → **bump
  `NAV_VERSION`** (currently 21 after commit 20578cb).
- **Dev command `serverflags <n>`.** One-shot setter for testing/debugging:
  sets `svs.serverflags` (persists across `changelevel`) and live
  `pr_global_struct->serverflags` (immediate effect). Deterministic and
  headless-friendly, unlike incremental `impulse 11` (which needs a live
  client and routes poorly through the headless console).
- **Debug/MCP readout.** `sim_nav_debug` overlay and `nav_edges_near`
  (`mcp_server.c:1312`) report each edge's condition (gate + open/closed) for
  inspection.

## Verification

- **Graph-level (primary):** with the union mesh loaded, set `serverflags 0` →
  assert A* finds a path spawn → bossgate slab-top and **no** path to the boss
  `end` trigger; set `serverflags 15` → assert A* finds spawn → boss `end`
  trigger and the slab-top edges are skipped. No reload needed (query-time
  filtering). Drive via `nav_edges_near` / a path query over MCP.
- **Physical traversal (secondary):** `serverflags 15` then `changelevel start`
  so the gate respawns non-solid; confirm the bot physically descends the
  shaft to the boss trigger.
- **Closed-state regression:** the commit-20578cb result still holds — all
  gate-footprint nodes seat at feet z=0 when closed.

## Risks / open items

- **Supplemental-pass seeding/bounds** is the trickiest implementation detail:
  it must produce a *connected chain* of drop/walk edges down the ~600u shaft
  (multi-step over intermediate ledges), not one illegal 600u drop, and link to
  the existing far-side component. Exact seed set + bound radius to be settled
  in the plan after measuring the real open-state descent.
- **Edge attribution** across the two passes relies on the gate-bbox proximity
  test; verify it doesn't mis-tag unrelated nearby floor edges.
- **Bake cost:** supplemental passes are localized/bounded, paid only on cache
  miss; expected to be small relative to the primary flood.

## Files affected

- `sdlquake/game/sim/sim_nav.c` — neutralize-whitelist generalization, closed
  tag pass, supplemental open pass, edge predicate fields + serialization,
  `NAV_VERSION` bump, A* skip clause, debug readout.
- `sdlquake/game/sim/sim.h` — `forbids_items` on the edge record; sigil-bit
  helpers.
- `sdlquake/game/game_defs.h` — `IT_SIGIL1..4` (1<<28..1<<31).
- `sdlquake/game/sim/sim_ai.c` (+ autonomous bot) — pass effective items.
- engine console command `serverflags <n>` (engine-side; sets `svs` + live
  global).
- `sdlquake/mcp/mcp_server.c` — edge-condition readout in `nav_edges_near`.

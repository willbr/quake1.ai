# Navmesh debug tooling: phase tags + MCP edge query

**Status:** approved
**Date:** 2026-05-26
**Author:** wjbr + Claude
**Related code:** `sdlquake/game/sim/sim_nav.c`, `sdlquake/mcp/mcp_server.c`

## Problem

Some navmesh edges punch through walls. Recent commits (924bc8f, b87e32a,
49a82f3) fixed several such cases in Phase 4.5 lift-link logic by adding
traceline validation, but there is no general-purpose tooling to (a) find
which phase of the bake produced a given bad edge or (b) inspect edges
near a point textually rather than only as a visual overlay.

The debug overlay already colour-codes by edge **kind** (WALK / JUMP /
TELEPORT / …) — that tells you *what kind of traversal* the edge
represents but not *which builder block emitted it*. Two phases can emit
the same kind: e.g. PLAT_LINK and BUTTON_LINK both originate from
Phase 4.5 sub-blocks. Conversely, an investigator faced with a
wall-piercing line currently has no way to ask "which phase put that
there?" short of reading the source and reasoning by elimination.

## Goals

1. Let an MCP-driven debugger (Claude) pinpoint which builder phase
   added any given edge.
2. Let the human filter the overlay to one phase at a time without
   re-baking.
3. Give the MCP client a text dump of edges near a point so it can
   reason about geometry without screenshots.

## Non-goals

- Stepwise rebake (rebake-stopping-after-N). The phase mask gives the
  same isolation more cheaply.
- Per-call-site granularity beyond the seven listed phases. If a
  phase's investigation needs finer resolution we'll subdivide that
  one phase as a follow-up.
- A bake log file. The MCP query covers the same use case with less
  noise.

## Design

### Phase enum

Seven tags, one per logical sub-block of the bake. Phases 1, 2, 4.6
don't emit edges so they get no tag.

```c
// sim_nav.c
enum {
    NAV_PHASE_BFS_WALK          = 0,  // Phase 3:   adjacency WALK edges
    NAV_PHASE_JUMP_DROP         = 1,  // Phase 3.5: JUMP_UP / DROP_DOWN
    NAV_PHASE_TELE_SRC          = 2,  // Phase 4:   TELEPORT from trigger anchor
    NAV_PHASE_TELE_NEAR         = 3,  // Phase 4:   TELEPORT from nearby-node fallback
    NAV_PHASE_LIFT_RIDE         = 4,  // Phase 4.5: PLAT_RIDE top<->bot
    NAV_PHASE_LIFT_PLAT_LINK    = 5,  // Phase 4.5: PLAT_LINK walk-on/off
    NAV_PHASE_LIFT_BUTTON_SHOOT = 6,  // Phase 4.5: BUTTON_LINK + SHOOT_LINK
    NAV_PHASE_COUNT             = 7,
};
```

### `nav_edge_t` change

The struct already has `unsigned char _pad[3]` after `kind`. We use
`_pad[0]` for the phase tag and shrink the padding:

```c
typedef struct {
    int      from, to;
    float    weight;
    unsigned char kind;        // NAV_EDGE_*
    unsigned char phase;       // NAV_PHASE_*  (new)
    unsigned char _pad[2];
    unsigned int  requires_items;
} nav_edge_t;
```

No size change, no file-format break: the on-disk layout already
serialises the full struct via `fwrite(m->edges, sizeof(nav_edge_t),
m->edge_count, f)`. Existing `.nav` files baked before this change
will have `phase == 0` (BFS_WALK) for every edge — wrong but
harmless; one rebake fixes it. We don't bump a file version because
old files are still loadable, just mislabelled until rebaked.

### Threading phase through `add_edge`

Two options considered:

1. **New parameter on `add_edge`**, update every call site. ~12 sites.
2. **Wrapper `add_edge_p(..., phase)`** that calls the existing
   `add_edge`. Lets us leave any future internal call site untouched.

Picked option 1. The 12 sites already exist and won't grow often; an
extra parameter is more honest than a wrapper that just sets a field.

### Overlay filter

New cvar `sim_nav_debug_phase_mask`, default `0xFF` (all phases visible
— backwards compatible). In `Sim_Nav_Frame`:

```c
unsigned mask = (unsigned)eng->Cvar_VariableValue("sim_nav_debug_phase_mask");
if (mask == 0) mask = 0xFF;   // treat "unset / zero" as "show all"

for (...edges...) {
    nav_edge_t *e = &s_mesh->edges[i];
    if (!(mask & (1u << e->phase))) continue;
    // ... existing kind→colour logic stays unchanged
}
```

Per-kind colour stays. Phase mask is an orthogonal filter on top.

### MCP tool: `nav_edges_near`

Input:
```json
{ "x": 100.0, "y": 200.0, "z": 40.0, "radius": 256.0 }
```

Output:
```json
{
  "count": 12,
  "truncated": false,
  "edges": [
    {
      "from": [100, 200, 40],
      "to":   [164, 200, 40],
      "kind": "WALK",
      "phase": "BFS_WALK",
      "weight": 64.0
    },
    ...
  ]
}
```

Filter: include an edge if **either** endpoint is within `radius` of
`(x,y,z)` (Euclidean). Cap at 200 entries; if exceeded set
`truncated: true`. Implemented in `mcp_server.c` against
`Sim_Nav_Get()` — this is engine-side so it crosses the game-DLL ABI
via the existing read-only mesh-access function.

Edge case: mesh not baked → return `{ "count": 0, "edges": [],
"error": "no navmesh loaded" }`.

### MCP tool: `nav_bake_phases`

No args. Returns a static enumeration so the client can map id↔name
without reading the source:

```json
{
  "phases": [
    { "id": 0, "name": "BFS_WALK",          "source": "Phase 3"   },
    { "id": 1, "name": "JUMP_DROP",         "source": "Phase 3.5" },
    { "id": 2, "name": "TELE_SRC",          "source": "Phase 4"   },
    { "id": 3, "name": "TELE_NEAR",         "source": "Phase 4"   },
    { "id": 4, "name": "LIFT_RIDE",         "source": "Phase 4.5" },
    { "id": 5, "name": "LIFT_PLAT_LINK",    "source": "Phase 4.5" },
    { "id": 6, "name": "LIFT_BUTTON_SHOOT", "source": "Phase 4.5" }
  ]
}
```

Hard-coded table next to the enum definition. If the enum grows, both
move together.

## Data flow

```
Builder (bake_floodfill, called once per Sim_Nav_LevelInit)
    ↓ add_edge(..., kind, items, phase)
sim_navmesh_t::edges[] (each edge carries its phase byte)
    ↓
    ├── Sim_Nav_Frame  ─→  overlay (mask filter + per-kind colour)
    └── Sim_Nav_Get    ─→  mcp_server.c::nav_edges_near  →  JSON
```

Engine never reads `phase` for anything but debug; bot pathing is
unaffected.

## Components

| File | Change |
|------|--------|
| `sdlquake/game/sim/sim_nav.c` | Add `NAV_PHASE_*` enum, add `phase` field to `nav_edge_t`, add `phase` parameter to `add_edge`, update all ~12 call sites, add `sim_nav_debug_phase_mask` cvar registration + filter in `Sim_Nav_Frame`. |
| `sdlquake/mcp/mcp_server.c` | Two new tool handlers: `nav_edges_near` (calls `g_game_api->nav_edges_near`, formats JSON) and `nav_bake_phases` (returns the static phase table — hard-coded here, kept in sync with the enum manually). Tool registration entries in the `tools/list` response. |
| `sdlquake/game/game_api.h` | Add `nav_edges_near` to `game_api_t` (DLL-side function — does the radius filter and copies up to N edges into a caller-provided buffer of `{from[3], to[3], kind, phase, weight}` records). Bump `GAME_API_VERSION` (current 26). Pattern mirrors the existing `nav_path` entry on the same struct. |
| `sdlquake/game/sim/sim_nav.c` (impl) | Implementation of the new `game_api_t` entry: walks `s_mesh->edges`, applies the radius filter, fills the caller buffer, returns count + truncation flag. |

## Testing

No automated test suite in this repo. Verification is manual:

1. Build, run `+map e1m1`.
2. `set sim_nav_debug 1; set sim_nav_ztest 0` — overlay visible.
3. `set sim_nav_debug_phase_mask 0x10` — only `LIFT_RIDE` edges
   visible; visually compare to before the change (where all kinds
   drew).
4. `set sim_nav_debug_phase_mask 0xFF` — back to all edges.
5. From an MCP client: `nav_bake_phases` returns the 7-entry table.
6. From an MCP client: `nav_edges_near {x,y,z, radius=256}` near the
   player returns a non-empty edge list with `phase` strings matching
   `nav_bake_phases` names.
7. Save → quit → reload the bake; phase tags survive
   serialise/deserialise (no truncation, no shift).

## Risks

- **.nav file mislabel** on first load: existing baked files have
  `phase == 0` for every edge until rebaked. Mitigation: the overlay's
  default mask is `0xFF` so nothing visually breaks; investigation
  workflows just need to rebake (`nav_rebuild` or delete the .nav
  cache) before trusting the filter. Document this in the commit
  message.
- **Phase enum drift**: if someone adds a new add_edge call site and
  forgets to assign a phase, the field defaults to 0 (BFS_WALK) and
  silently misattributes. Mitigation: make the `add_edge` parameter
  required (no default) — compiler will catch missed sites.

## Out of scope (deferred)

- Stepwise rebake. Revisit if phase-mask resolution proves too coarse.
- Bake log file. Revisit if MCP query proves too noisy.
- Splitting Phase 4.5 sub-phases further (per-call-site granularity).
  Revisit if `LIFT_PLAT_LINK` or `LIFT_BUTTON_SHOOT` turns out to
  hide a bug that needs finer resolution.

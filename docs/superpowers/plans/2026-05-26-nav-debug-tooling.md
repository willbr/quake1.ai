# Nav debug tooling: phase tags + MCP edge query — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-edge phase tags + a cvar filter + two MCP tools (`nav_edges_near`, `nav_bake_phases`) so a wall-piercing nav link can be attributed to a specific builder phase without re-baking or reading the source by elimination.

**Architecture:** One-byte `phase` field stolen from `nav_edge_t::_pad[0]`. All `add_edge` call sites pass an explicit `NAV_PHASE_*` tag (compile-time enforced — no default). `Sim_Nav_Frame` filters draws via a new `sim_nav_debug_phase_mask` cvar. A new `game_api_t::nav_edges_near` function lets the engine query nearby edges; MCP wraps it as `nav_edges_near` and ships a static phase enum as `nav_bake_phases`.

**Tech Stack:** Zig build, C (gnu89 in engine, modern C in DLL/platform/MCP), SDL3. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-05-26-nav-debug-tooling-design.md`

---

## File map (all files exist; no new files needed)

- Modify: `sdlquake/game/sim/sim_nav.c` — phase enum, struct field, `add_edge` sig, 12 call sites, cvar registration, overlay filter, `Sim_Nav_EdgesNear` implementation.
- Modify: `sdlquake/game/game_api.h` — bump version 30→31, add `nav_edges_near` function-pointer field.
- Modify: `sdlquake/game/game_main.c` — add `game_nav_edges_near` wrapper + wire into `s_api` initializer.
- Modify: `sdlquake/engine/hotreload.c` — add `MCP_NavEdgesNear` shim mirroring the existing `MCP_DamageEntity` pattern.
- Modify: `sdlquake/mcp/mcp_server.c` — add `tool_nav_edges_near` + `tool_nav_bake_phases` handlers, two entries in `MCP_TOOLS_RESULT`, two dispatch cases.

No new files are created; the change is small enough that splitting across new TUs would obscure rather than help.

---

## Task 1: Phase enum, `nav_edge_t` field, `add_edge` signature, all call sites

This is one atomic change because the `add_edge` signature change breaks the build until every call site updates. Commit only after the build is green.

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c` (enum at ~line 124, struct at ~line 126, `add_edge` at ~line 252, call sites at lines 863, 921, 957, 983, 1035, 1036, 1070, 1071, 1087, 1088, 1122, 1154)

- [ ] **Step 1: Add the phase enum**

Insert immediately after the existing `NAV_EDGE_*` enum (just before the `nav_edge_t` typedef at line ~126):

```c
// Per-edge phase tag — which sub-block of bake_floodfill emitted the
// edge. Distinct from NAV_EDGE_* (which describes traversal style)
// because two phases can emit the same kind (e.g. Phase 4.5 emits
// both PLAT_LINK and BUTTON_LINK). Used by the debug overlay's phase
// mask and by the nav_edges_near MCP tool. Keep in sync with the
// table in mcp_server.c::tool_nav_bake_phases.
enum {
    NAV_PHASE_BFS_WALK          = 0,  // Phase 3:   adjacency WALK edges
    NAV_PHASE_JUMP_DROP         = 1,  // Phase 3.5: JUMP_UP / DROP_DOWN pairs
    NAV_PHASE_TELE_SRC          = 2,  // Phase 4:   TELEPORT from trigger anchor
    NAV_PHASE_TELE_NEAR         = 3,  // Phase 4:   TELEPORT from nearby-node fallback
    NAV_PHASE_LIFT_RIDE         = 4,  // Phase 4.5: PLAT_RIDE top<->bot
    NAV_PHASE_LIFT_PLAT_LINK    = 5,  // Phase 4.5: PLAT_LINK walk-on/off
    NAV_PHASE_LIFT_BUTTON_SHOOT = 6,  // Phase 4.5: BUTTON_LINK + SHOOT_LINK
    NAV_PHASE_COUNT             = 7,
};
```

- [ ] **Step 2: Add the `phase` field to `nav_edge_t`**

Replace:

```c
typedef struct {
    int      from, to;
    float    weight;
    unsigned char kind;        // NAV_EDGE_*
    unsigned char _pad[3];
    unsigned int  requires_items;  // bitmask matched against player.items
} nav_edge_t;
```

with:

```c
typedef struct {
    int      from, to;
    float    weight;
    unsigned char kind;        // NAV_EDGE_*
    unsigned char phase;       // NAV_PHASE_* — which bake sub-block emitted this
    unsigned char _pad[2];
    unsigned int  requires_items;  // bitmask matched against player.items
} nav_edge_t;
```

Struct size is unchanged (`_pad` shrinks from 3 to 2). `.nav` cache files baked before this change will deserialize with `phase == 0` (BFS_WALK) for every edge — wrong-but-harmless; one rebake fixes it. This is intentional per the spec; no version bump on the file format.

- [ ] **Step 3: Update `add_edge` signature + implementation**

Replace the `add_edge` definition at line ~252:

```c
static int add_edge(sim_navmesh_t *m, int *cap, int from, int to,
                    float weight, unsigned char kind, unsigned int req_items) {
    if (m->edge_count >= *cap) {
        int nc = *cap ? *cap * 2 : 1024;
        nav_edge_t *e = realloc(m->edges, sizeof(nav_edge_t) * nc);
        if (!e) return 0;
        m->edges = e;
        *cap     = nc;
    }
    nav_edge_t *e = &m->edges[m->edge_count++];
    e->from   = from;
    e->to     = to;
    e->weight = weight;
    e->kind   = kind;
    e->_pad[0] = e->_pad[1] = e->_pad[2] = 0;
    e->requires_items = req_items;
    return 1;
}
```

with (note new `phase` parameter + assignment + shorter `_pad` clear):

```c
static int add_edge(sim_navmesh_t *m, int *cap, int from, int to,
                    float weight, unsigned char kind,
                    unsigned int req_items, unsigned char phase) {
    if (m->edge_count >= *cap) {
        int nc = *cap ? *cap * 2 : 1024;
        nav_edge_t *e = realloc(m->edges, sizeof(nav_edge_t) * nc);
        if (!e) return 0;
        m->edges = e;
        *cap     = nc;
    }
    nav_edge_t *e = &m->edges[m->edge_count++];
    e->from   = from;
    e->to     = to;
    e->weight = weight;
    e->kind   = kind;
    e->phase  = phase;
    e->_pad[0] = e->_pad[1] = 0;
    e->requires_items = req_items;
    return 1;
}
```

`phase` is the **last** parameter (no default — C has none anyway) so the compiler errors on any missed call site after this point.

- [ ] **Step 4: Update all 12 `add_edge` call sites**

Each existing call gets a trailing argument. Verbatim replacements:

Line ~863 (Phase 3 BFS walk):
```c
add_edge(m, &cap_edges, cur, next_idx, w, NAV_EDGE_WALK, 0, NAV_PHASE_BFS_WALK);
```

Line ~921 (Phase 3.5 jump/drop):
```c
add_edge(m, &cap_edges, i, j, dist + bias, edge_k, 0, NAV_PHASE_JUMP_DROP);
```

Line ~957 (Phase 4 teleport from trigger anchor — this is the multi-line one; pass the phase as the new trailing arg):
```c
add_edge(m, &cap_edges, anchors[i].node_index, dst_node, 0.0f,
         NAV_EDGE_TELEPORT, 0, NAV_PHASE_TELE_SRC);
```

Line ~983 (Phase 4 teleport from nearby-node fallback):
```c
add_edge(m, &cap_edges, p, dst_node, 0.0f, NAV_EDGE_TELEPORT, 0, NAV_PHASE_TELE_NEAR);
```

Lines ~1035 / ~1036 (Phase 4.5 plat ride, both directions):
```c
add_edge(m, &cap_edges, top_idx, bot_idx, ride_cost, NAV_EDGE_PLAT_RIDE, 0, NAV_PHASE_LIFT_RIDE);
add_edge(m, &cap_edges, bot_idx, top_idx, ride_cost, NAV_EDGE_PLAT_RIDE, 0, NAV_PHASE_LIFT_RIDE);
```

Lines ~1070 / ~1071 (Phase 4.5 plat-link to top):
```c
add_edge(m, &cap_edges, p, top_idx, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
add_edge(m, &cap_edges, top_idx, p, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
```

Lines ~1087 / ~1088 (Phase 4.5 plat-link to bot):
```c
add_edge(m, &cap_edges, p, bot_idx, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
add_edge(m, &cap_edges, bot_idx, p, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
```

Line ~1122 (Phase 4.5 button/shoot link — multi-line; the `kind` variable here is computed locally as either `NAV_EDGE_BUTTON_LINK` or `NAV_EDGE_SHOOT_LINK`, but both share the single phase tag):
```c
add_edge(m, &cap_edges, anchors[k].node_index,
         /* existing dst arg */, /* existing weight */, edge_kind, 0,
         NAV_PHASE_LIFT_BUTTON_SHOOT);
```
Apply this update *without renaming any existing local* — the call site has variables like `edge_kind` and existing dst/weight already in scope; just append `, NAV_PHASE_LIFT_BUTTON_SHOOT` before the closing `)`. If the existing variable is named differently (e.g. `kind`), use that name verbatim.

Line ~1154 (Phase 4.5 plat-link to bot in the button-trigger sub-branch):
```c
add_edge(m, &cap_edges, bot_idx, p, w, NAV_EDGE_PLAT_LINK, 0, NAV_PHASE_LIFT_PLAT_LINK);
```

- [ ] **Step 5: Build and verify**

Run: `zig build game 2>&1 | tail -20`

Expected: clean build, no warnings. If there are missed call sites the compiler will say "too few arguments to function 'add_edge'" with a line number — go fix and re-run.

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "$(cat <<'EOF'
feat(nav): tag every edge with the bake sub-block that emitted it

Adds a NAV_PHASE_* enum (7 values, one per logical add_edge block in
bake_floodfill) and a one-byte phase field stolen from nav_edge_t::_pad.
Every add_edge call now takes an explicit phase tag — compiler-enforced,
no default — so new call sites can't silently misattribute.

No struct size change. .nav cache files baked before this commit will
deserialize with phase=0 (BFS_WALK) on every edge until rebaked; the
default debug mask is 0xFF so nothing visually breaks.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Overlay phase-mask cvar + filter in `Sim_Nav_Frame`

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c` (cvar registration at ~line 1396, overlay loop at ~line 1434)

- [ ] **Step 1: Register the new cvar in `Sim_Nav_Init`**

Locate `Sim_Nav_Init` (around line ~1393). Just after the existing `eng->Cvar_Register("sim_nav_debug_range", "1024");` line, add:

```c
    // Bitmask over NAV_PHASE_* — bit N selects phase N. Default 0xFF
    // shows all phases. Special-case: a literal 0 also means "show
    // all" so accidental `set sim_nav_debug_phase_mask 0` doesn't
    // make the overlay vanish.
    eng->Cvar_Register("sim_nav_debug_phase_mask", "255");
```

- [ ] **Step 2: Add the filter in `Sim_Nav_Frame`**

Locate the existing edge-draw loop in `Sim_Nav_Frame` (around line 1434). Immediately after the `nav_edge_t *e = &s_mesh->edges[i];` line (line ~1435), insert:

```c
        // Phase mask filter — independent of the kind-based colouring
        // applied below. Mask defaults to 0xFF (all); treat literal 0
        // as "all" too so the overlay can't be accidentally blanked.
        {
            unsigned mask = (unsigned)eng->Cvar_VariableValue("sim_nav_debug_phase_mask");
            if (mask == 0) mask = 0xFFu;
            if (!(mask & (1u << e->phase))) continue;
        }
```

Note: the cvar lookup happens per-edge inside the inner loop. That's fine — `Cvar_VariableValue` is a hash lookup and the overlay is already gated by `sim_nav_debug > 0`, so the cost is paid only when the user is actively debugging. Hoisting it would add LOC for no measurable win.

- [ ] **Step 3: Build**

Run: `zig build game 2>&1 | tail -10`

Expected: clean build.

- [ ] **Step 4: Manual verification in-game**

Run the game with the smoke-test rig and exercise the mask. Per memory `smoke-test-rig`: use `m7_skeleton`, teleport to (380, 0, 40) facing east via the MCP `teleport` tool, then `togglemenu` to dismiss, then `screenshot_gpu`. If MCP isn't running, do the equivalent at the console:

```
map m7_skeleton
set sim_nav_debug 1
set sim_nav_ztest 0
```

Then in three successive console invocations capture screenshots and check that:

| Mask value | Expected visible |
|---|---|
| `255` (default) | All edges (visually identical to the kind-colouring screenshot before this task) |
| `1`  | Only orange (WALK) edges from Phase 3 |
| `16` | Only blue PLAT_RIDE edges from Phase 4.5 |
| `0`  | All edges (the literal-0 escape hatch) |

If `mask=1` still shows non-orange edges, Task 1 misattributed a call site — re-grep `add_edge` and check the phase tag.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "$(cat <<'EOF'
feat(nav): sim_nav_debug_phase_mask cvar filters overlay by bake phase

Bitmask over NAV_PHASE_*; default 255 shows every phase (no visual
regression). Literal 0 is treated as "all" so the overlay can't be
blanked by accident.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: `Sim_Nav_EdgesNear` DLL function

**Files:**
- Modify: `sdlquake/game/sim/sim.h` (declaration)
- Modify: `sdlquake/game/sim/sim_nav.c` (implementation)

- [ ] **Step 1: Add the public record struct + function declaration to `sim.h`**

Locate the existing nav declarations (`Sim_Nav_Get`, `Sim_Nav_PathTo` etc.). Add immediately below them:

```c
// Snapshot of one nav edge for debug/inspection consumers (MCP).
// Coords are world-space copies of mesh node positions; safe to use
// even after a rebake.
typedef struct {
    float from[3];
    float to[3];
    float weight;
    unsigned char kind;   // NAV_EDGE_*
    unsigned char phase;  // NAV_PHASE_*
} sim_nav_edge_record_t;

// Fill `out` with edges whose either endpoint is within `radius` of
// `center`. Caller provides `max_records` capacity. Returns the
// number written. If `truncated_out` is non-NULL it is set to 1 when
// more edges matched than fit (in which case the function still
// returns `max_records`). Safe to call before bake — returns 0.
int Sim_Nav_EdgesNear(const float center[3], float radius,
                      sim_nav_edge_record_t *out, int max_records,
                      int *truncated_out);
```

- [ ] **Step 2: Implement `Sim_Nav_EdgesNear` in `sim_nav.c`**

Add at the end of the existing public function block (after `Sim_Nav_Get` at line ~1406):

```c
int Sim_Nav_EdgesNear(const float center[3], float radius,
                      sim_nav_edge_record_t *out, int max_records,
                      int *truncated_out) {
    if (truncated_out) *truncated_out = 0;
    if (!s_mesh || !s_ready || !out || max_records <= 0) return 0;
    if (radius <= 0.0f) return 0;

    float r2 = radius * radius;
    int written = 0;
    for (int i = 0; i < s_mesh->edge_count; i++) {
        const nav_edge_t *e = &s_mesh->edges[i];
        const float *a = s_mesh->points[e->from].pos;
        const float *b = s_mesh->points[e->to].pos;

        float da[3] = { a[0]-center[0], a[1]-center[1], a[2]-center[2] };
        float db[3] = { b[0]-center[0], b[1]-center[1], b[2]-center[2] };
        float ra2 = da[0]*da[0] + da[1]*da[1] + da[2]*da[2];
        float rb2 = db[0]*db[0] + db[1]*db[1] + db[2]*db[2];
        if (ra2 > r2 && rb2 > r2) continue;

        if (written >= max_records) {
            if (truncated_out) *truncated_out = 1;
            return written;   // bail early; flag set
        }
        out[written].from[0] = a[0]; out[written].from[1] = a[1]; out[written].from[2] = a[2];
        out[written].to[0]   = b[0]; out[written].to[1]   = b[1]; out[written].to[2]   = b[2];
        out[written].weight  = e->weight;
        out[written].kind    = e->kind;
        out[written].phase   = e->phase;
        written++;
    }
    return written;
}
```

The "bail early on overflow" matters because edge counts can be ~60k on e1m1; we don't want to scan the rest once we know we're truncated.

- [ ] **Step 3: Build**

Run: `zig build game 2>&1 | tail -10`

Expected: clean build.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/game/sim/sim.h sdlquake/game/sim/sim_nav.c
git commit -m "$(cat <<'EOF'
feat(nav): Sim_Nav_EdgesNear queries edges within a radius

Returns up to N edge snapshots (from/to coords, weight, kind, phase)
whose either endpoint is within radius of a query point. Caller-owned
buffer; truncation flag set when the cap is hit. Used by the new
MCP nav_edges_near tool.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: `game_api_t::nav_edges_near` ABI entry

**Files:**
- Modify: `sdlquake/game/game_api.h` (version bump + new function pointer)
- Modify: `sdlquake/game/game_main.c` (wrapper + struct init)

- [ ] **Step 1: Bump `GAME_API_VERSION` and add the function pointer**

In `sdlquake/game/game_api.h`:

Change line 7 from:
```c
#define GAME_API_VERSION 30
```
to:
```c
#define GAME_API_VERSION 31
```

After the existing `nav_path` field (line 323-327, ending with `int max_waypoints);`), add:

```c
    // Debug-only nav edge query for the MCP nav_edges_near tool. Fills
    // up to `max_records` snapshots of edges within `radius` of the
    // 3-float `center` point. Returns the number written; sets
    // `*truncated_out` to 1 if more edges matched than fit (when
    // truncated_out is non-NULL). The record layout is fixed by
    // sim_nav_edge_record_t in sim.h — engine-side code should match
    // it via an extern struct rather than redeclaring (see
    // hotreload.c::MCP_NavEdgesNear).
    int   (*nav_edges_near)(const float *center, float radius,
                            void *out_records, int max_records,
                            int *truncated_out);
} game_api_t;
```

(Note `out_records` is `void *` to avoid leaking `sim_nav_edge_record_t` into `game_api.h`. The caller casts on the way in via `Sim_Nav_EdgesNear`; the engine-side consumer casts on the way out.)

- [ ] **Step 2: Add the wrapper in `game_main.c`**

After the existing `game_nav_path` definition (ends at line 111), add:

```c
// MCP debug bridge — see game_api.h::nav_edges_near.
static int game_nav_edges_near(const float *center, float radius,
                               void *out_records, int max_records,
                               int *truncated_out) {
    return Sim_Nav_EdgesNear(center, radius,
                             (sim_nav_edge_record_t *)out_records,
                             max_records, truncated_out);
}
```

- [ ] **Step 3: Wire into the `s_api` initializer**

Locate `s_api` (line 139). Append the new field after the existing `game_nav_path,` line (line 162):

```c
static game_api_t s_api = {
    GAME_API_VERSION,
    game_init,
    game_shutdown,
    game_start_frame,
    game_entity_spawn,
    game_entity_think,
    game_entity_touch,
    game_client_connect,
    game_client_disconnect,
    game_put_client_in_server,
    game_client_prethink,
    game_client_postthink,
    game_client_kill,
    game_set_new_parms,
    game_set_change_parms,
    game_list_spawn_classes,
    game_debug_draw_overlays,
    Doors_OpenAll,
    Doors_OpenAllSecret,
    game_mcp_damage,
    Wind_SampleVelocity,
    game_ai_inspect,
    game_nav_path,
    game_nav_edges_near,
};
```

- [ ] **Step 4: Build the engine + DLL**

Run: `zig build 2>&1 | tail -15`

Expected: clean build. The engine and DLL both compile against the new `GAME_API_VERSION 31`; the loader's version-mismatch reject only fires across mismatched builds, which won't happen in one `zig build` invocation.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/game_api.h sdlquake/game/game_main.c
git commit -m "$(cat <<'EOF'
feat(game-api): nav_edges_near — MCP-facing nav edge query

ABI version bumped to 31. Thin wrapper around Sim_Nav_EdgesNear so
the engine-side MCP server can query nearby nav edges without linking
against the DLL's sim module.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: `MCP_NavEdgesNear` engine-side shim

**Files:**
- Modify: `sdlquake/engine/hotreload.c` (new shim mirroring `MCP_DamageEntity`)

- [ ] **Step 1: Add the shim at the bottom of `hotreload.c`**

After the existing `MCP_DamageEntity` definition at line 1179-1183, add:

```c
// MCP-only: forward a nav-edge query through the loaded game DLL. The
// out_records buffer is treated as opaque — its layout (from[3],
// to[3], weight, kind, phase) is defined by sim_nav_edge_record_t in
// the DLL's sim.h; engine callers (mcp_server.c) declare a matching
// struct locally to read the result. Returns 0 if no DLL is loaded
// or the bridge isn't wired.
int MCP_NavEdgesNear(const float *center, float radius,
                     void *out_records, int max_records,
                     int *truncated_out)
{
    if (truncated_out) *truncated_out = 0;
    if (!g_game_api || !g_game_api->nav_edges_near) return 0;
    return g_game_api->nav_edges_near(center, radius, out_records,
                                      max_records, truncated_out);
}
```

- [ ] **Step 2: Build**

Run: `zig build 2>&1 | tail -10`

Expected: clean build.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/engine/hotreload.c
git commit -m "$(cat <<'EOF'
feat(hotreload): MCP_NavEdgesNear shim into the DLL

Mirrors the MCP_DamageEntity pattern. Lets mcp_server.c reach the new
game_api_t::nav_edges_near without including game_api.h directly.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: MCP tool `nav_edges_near`

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (add JSON-float helper, tool handler, tools list entry, dispatch case)

- [ ] **Step 1: Add a `json_float` helper**

The existing helpers cover `json_int` and `json_vec3` but there's no scalar-float reader. Add immediately after `json_int` (around line 518):

```c
// Parse a scalar JSON number for "key": <num>. Permissive — accepts
// ints or floats. Returns 1 on success.
static int json_float(const char *json, const char *key, float *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    char *endp = NULL;
    float v = strtof(p, &endp);
    if (endp == p) return 0;
    *out = v;
    return 1;
}
```

- [ ] **Step 2: Add the tool handler**

Insert a new tool function in a sensible spot (next to `tool_inspect_entity` at line ~1096 is fine — they're both inspection tools). Add:

```c
// ---------------------------------------------------------------------------
// Tool: nav_edges_near -- query nav edges whose either endpoint is within
// `radius` of {x,y,z}. Returns JSON with from/to coords, kind name, phase
// name, weight, and a truncation flag. Capped at NAV_EDGES_NEAR_CAP records
// to keep payload bounded (e1m1 has ~60k edges; an unbounded query could
// dump megabytes). Routes via game_api->nav_edges_near.
// ---------------------------------------------------------------------------

#define NAV_EDGES_NEAR_CAP 200

// Mirror of sim_nav_edge_record_t in sdlquake/game/sim/sim.h. Engine
// side cannot include sim.h (it's DLL-private); we redeclare. If the
// DLL layout ever changes, bump GAME_API_VERSION and update both.
typedef struct {
    float from[3];
    float to[3];
    float weight;
    unsigned char kind;
    unsigned char phase;
} mcp_nav_edge_record_t;

static const char *nav_edge_kind_name(unsigned char k) {
    switch (k) {
    case 0: return "WALK";
    case 1: return "JUMP_UP";
    case 2: return "DROP_DOWN";
    case 3: return "PLAT_RIDE";
    case 4: return "TELEPORT";
    case 5: return "PLAT_LINK";
    case 6: return "SHOOT_LINK";
    case 7: return "BUTTON_LINK";
    default: return "UNKNOWN";
    }
}

static const char *nav_phase_name(unsigned char p) {
    switch (p) {
    case 0: return "BFS_WALK";
    case 1: return "JUMP_DROP";
    case 2: return "TELE_SRC";
    case 3: return "TELE_NEAR";
    case 4: return "LIFT_RIDE";
    case 5: return "LIFT_PLAT_LINK";
    case 6: return "LIFT_BUTTON_SHOOT";
    default: return "UNKNOWN";
    }
}

static void tool_nav_edges_near(const char *id_json, const char *args)
{
    float x, y, z, radius;
    if (!args ||
        !json_float(args, "x", &x) ||
        !json_float(args, "y", &y) ||
        !json_float(args, "z", &z) ||
        !json_float(args, "radius", &radius)) {
        mcp_error(id_json, -32602, "need x, y, z, radius (numbers)");
        return;
    }
    if (radius <= 0.0f) {
        mcp_error(id_json, -32602, "radius must be positive");
        return;
    }

    extern int MCP_NavEdgesNear(const float *center, float radius,
                                void *out_records, int max_records,
                                int *truncated_out);
    float center[3] = { x, y, z };
    mcp_nav_edge_record_t recs[NAV_EDGES_NEAR_CAP];
    int truncated = 0;
    int n = MCP_NavEdgesNear(center, radius, recs, NAV_EDGES_NEAR_CAP, &truncated);

    // Build raw JSON in one buffer; escape into the JSON-string field
    // exposed by mcp_text_result. Worst-case bytes: ~180 per edge
    // (coords, names, weight, formatting) × 200 = 36 KB. Use a heap
    // buffer to stay off the thread stack.
    size_t cap = (size_t)NAV_EDGES_NEAR_CAP * 200 + 256;
    char *raw = (char *)malloc(cap);
    if (!raw) { mcp_error(id_json, -32603, "oom"); return; }

    int off = snprintf(raw, cap,
        "{\"count\":%d,\"truncated\":%s,\"edges\":[",
        n, truncated ? "true" : "false");
    for (int i = 0; i < n && off > 0 && (size_t)off < cap; i++) {
        const mcp_nav_edge_record_t *r = &recs[i];
        off += snprintf(raw + off, cap - (size_t)off,
            "%s{\"from\":[%.2f,%.2f,%.2f],"
            "\"to\":[%.2f,%.2f,%.2f],"
            "\"kind\":\"%s\",\"phase\":\"%s\",\"weight\":%.3f}",
            i ? "," : "",
            r->from[0], r->from[1], r->from[2],
            r->to[0],   r->to[1],   r->to[2],
            nav_edge_kind_name(r->kind),
            nav_phase_name(r->phase),
            (double)r->weight);
    }
    if ((size_t)off < cap) {
        off += snprintf(raw + off, cap - (size_t)off, "]}");
    }

    // Escape raw into a JSON string for mcp_text_result. Allocate
    // 2x raw size + slack for worst-case escaping.
    size_t esc_cap = cap * 2 + 64;
    char *escaped = (char *)malloc(esc_cap);
    if (!escaped) { free(raw); mcp_error(id_json, -32603, "oom"); return; }
    char *d = escaped;
    char *dend = escaped + esc_cap - 1;
    d = json_escape_append(d, dend, raw);
    *d = '\0';
    mcp_text_result(id_json, escaped);
    free(escaped);
    free(raw);
}
```

- [ ] **Step 3: Add the tool entry to `MCP_TOOLS_RESULT`**

In `MCP_TOOLS_RESULT` (line 1425). Just before the closing `"]}"` (line 1530), add:

```c
      "," \
      "{\"name\":\"nav_edges_near\"," \
       "\"description\":\"Return nav edges whose either endpoint is within `radius` of (x,y,z). Each edge reports from/to coords, kind (NAV_EDGE_* name), phase (NAV_PHASE_* name — which bake sub-block emitted it), and weight. Capped at 200 records; sets truncated=true when the cap is hit. Returns count=0 with no error before the first bake completes\"," \
       "\"inputSchema\":{\"type\":\"object\"," \
         "\"properties\":{" \
           "\"x\":{\"type\":\"number\"}," \
           "\"y\":{\"type\":\"number\"}," \
           "\"z\":{\"type\":\"number\"}," \
           "\"radius\":{\"type\":\"number\",\"description\":\"world units; > 0\"}}," \
         "\"required\":[\"x\",\"y\",\"z\",\"radius\"]}}"
```

- [ ] **Step 4: Add the dispatch case**

In the `tools/call` block (around line 1562). After the last existing `else if` entry (`wait_frames` at line ~1691), add:

```c
        else if (strcmp(tool_name, "nav_edges_near") == 0)
        {
            const char *args = strstr(line, "\"arguments\":");
            tool_nav_edges_near(id_json, args ? args : "");
        }
```

- [ ] **Step 5: Build**

Run: `zig build 2>&1 | tail -15`

Expected: clean build. If you get an "implicit declaration of `MCP_NavEdgesNear`" error, the extern in step 2 was placed wrong — confirm it's inside `tool_nav_edges_near`.

- [ ] **Step 6: Manual MCP smoke test**

Start the game with the HTTP MCP transport:
```bash
zig build run -- +map e1m1 --mcp-http 9876
```

In another shell, call the new tool (the `mcp_call.py` script is the canonical one-shot client):
```bash
python3 scripts/mcp_call.py nav_edges_near '{"x":544,"y":288,"z":24,"radius":256}'
```

Expected JSON shape:
```json
{"count": <some integer>, "truncated": false, "edges": [
  {"from":[...], "to":[...], "kind":"WALK", "phase":"BFS_WALK", "weight":...},
  ...
]}
```

If `count == 0` immediately after loading e1m1, the navmesh hasn't finished baking yet — wait a few seconds and retry. If it's still 0, check that `Sim_Nav_IsReady()` returns 1 (poll `get_player_state` or watch for `nav: bake complete` in console).

- [ ] **Step 7: Commit**

```bash
git add sdlquake/mcp/mcp_server.c
git commit -m "$(cat <<'EOF'
feat(mcp): nav_edges_near tool — query nav edges near a point

JSON-RPC tool that returns up to 200 nav edges within a radius, each
with from/to coords, kind name, phase name, and weight. Lets an
MCP-driven debugger pinpoint which bake sub-block emitted a
wall-piercing link without re-reading the source.

Adds a json_float scalar helper (existing helpers covered ints and
vec3s only).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: MCP tool `nav_bake_phases`

**Files:**
- Modify: `sdlquake/mcp/mcp_server.c` (handler + tools-list entry + dispatch case)

- [ ] **Step 1: Add the handler**

Insert immediately after `tool_nav_edges_near` (the one added in Task 6):

```c
// ---------------------------------------------------------------------------
// Tool: nav_bake_phases -- enumerate NAV_PHASE_* tags so MCP clients can
// map id<->name without reading sim_nav.c. Hard-coded mirror of the enum
// in sdlquake/game/sim/sim_nav.c; keep in sync.
// ---------------------------------------------------------------------------

static void tool_nav_bake_phases(const char *id_json)
{
    static const char *raw =
        "{\"phases\":["
          "{\"id\":0,\"name\":\"BFS_WALK\",\"source\":\"Phase 3\"},"
          "{\"id\":1,\"name\":\"JUMP_DROP\",\"source\":\"Phase 3.5\"},"
          "{\"id\":2,\"name\":\"TELE_SRC\",\"source\":\"Phase 4\"},"
          "{\"id\":3,\"name\":\"TELE_NEAR\",\"source\":\"Phase 4\"},"
          "{\"id\":4,\"name\":\"LIFT_RIDE\",\"source\":\"Phase 4.5\"},"
          "{\"id\":5,\"name\":\"LIFT_PLAT_LINK\",\"source\":\"Phase 4.5\"},"
          "{\"id\":6,\"name\":\"LIFT_BUTTON_SHOOT\",\"source\":\"Phase 4.5\"}"
        "]}";

    char escaped[1024];
    char *d = escaped;
    char *dend = escaped + sizeof(escaped) - 1;
    d = json_escape_append(d, dend, raw);
    *d = '\0';
    mcp_text_result(id_json, escaped);
}
```

- [ ] **Step 2: Add the tool entry to `MCP_TOOLS_RESULT`**

Just before the closing `"]}"` of `MCP_TOOLS_RESULT` (which by now has the `nav_edges_near` entry from Task 6), add:

```c
      "," \
      "{\"name\":\"nav_bake_phases\"," \
       "\"description\":\"Enumerate the seven NAV_PHASE_* tags emitted by the navmesh bake (id, name, source phase comment). Returned by the nav_edges_near tool as the `phase` field. Pair with sim_nav_debug_phase_mask cvar to isolate a phase in the overlay (bit N = phase N).\"," \
       "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}"
```

- [ ] **Step 3: Add the dispatch case**

After the `nav_edges_near` dispatch added in Task 6, add:

```c
        else if (strcmp(tool_name, "nav_bake_phases") == 0)
        {
            tool_nav_bake_phases(id_json);
        }
```

- [ ] **Step 4: Build**

Run: `zig build 2>&1 | tail -10`

Expected: clean build.

- [ ] **Step 5: Manual MCP smoke test**

With the game running (`zig build run -- +map e1m1 --mcp-http 9876`):

```bash
python3 scripts/mcp_call.py nav_bake_phases '{}'
```

Expected output (exact):
```json
{"phases":[
  {"id":0,"name":"BFS_WALK","source":"Phase 3"},
  {"id":1,"name":"JUMP_DROP","source":"Phase 3.5"},
  {"id":2,"name":"TELE_SRC","source":"Phase 4"},
  {"id":3,"name":"TELE_NEAR","source":"Phase 4"},
  {"id":4,"name":"LIFT_RIDE","source":"Phase 4.5"},
  {"id":5,"name":"LIFT_PLAT_LINK","source":"Phase 4.5"},
  {"id":6,"name":"LIFT_BUTTON_SHOOT","source":"Phase 4.5"}
]}
```

- [ ] **Step 6: Commit**

```bash
git add sdlquake/mcp/mcp_server.c
git commit -m "$(cat <<'EOF'
feat(mcp): nav_bake_phases tool — enumerate NAV_PHASE_* tags

Static metadata tool so MCP clients can map phase id↔name without
reading sim_nav.c. Hard-coded mirror of the enum; comment in both
files notes they must move together.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: End-to-end investigation rehearsal

Manual exercise of the new workflow against a known case. No code change — this is the verification that the spec's stated use case actually works.

- [ ] **Step 1: Delete any cached .nav file so we get a fresh bake with phase tags**

```bash
rm -f id1/cache/navmesh/*.nav 2>/dev/null
ls id1/cache/navmesh/*.nav 2>/dev/null || echo "no cached nav files (good)"
```

- [ ] **Step 2: Start the game with MCP**

```bash
zig build run -- +map e1m1 --mcp-http 9876
```

Wait for the console to print bake completion (`nav: bake complete` or similar).

- [ ] **Step 3: Pick a player location and query nearby edges**

```bash
python3 scripts/mcp_call.py get_player_state '{}'
```

Note the `origin` field. Use it for:

```bash
python3 scripts/mcp_call.py nav_edges_near '{"x":<px>,"y":<py>,"z":<pz>,"radius":512}'
```

Expected: a non-empty `edges` array with phase values like `BFS_WALK`, `JUMP_DROP`, `LIFT_RIDE` etc. depending on what's nearby.

- [ ] **Step 4: Isolate one phase visually**

At the in-game console:
```
set sim_nav_debug 1
set sim_nav_ztest 0
set sim_nav_debug_phase_mask 16
```

(16 = bit 4 = `LIFT_RIDE`.) Then:
```bash
python3 scripts/mcp_call.py screenshot_gpu '{}'
```

Open the saved PNG. Expect to see only PLAT_RIDE (blue) edges, nothing else.

- [ ] **Step 5: Reset and commit nothing (workflow exercise only)**

```bash
git status   # should show no modifications
```

If `git status` shows changes, something leaked — investigate before continuing.

---

## Self-review checklist (skill-required)

After writing the plan I re-checked against the spec:

**Spec coverage:**
- "Phase enum (7 values)" → Task 1 step 1 ✓
- "`phase` field in `nav_edge_t`, no struct size change" → Task 1 step 2 ✓
- "`add_edge` takes phase as required param" → Task 1 step 3 ✓
- "All 12 call sites tagged" → Task 1 step 4 ✓ (12 verbatim replacements)
- "`sim_nav_debug_phase_mask` cvar, default 255, 0 means all" → Task 2 ✓
- "Per-kind colour stays orthogonal" → Task 2 step 2 (only adds a `continue`, colour logic untouched) ✓
- "MCP tool `nav_edges_near`, radius filter, cap 200, truncation flag, edge case unbaked-mesh" → Task 6 ✓
- "MCP tool `nav_bake_phases`" → Task 7 ✓
- "Bump GAME_API_VERSION" → Task 4 step 1 ✓ (note: spec said current was 26, actual is 30, plan bumps 30→31)
- "Risk: .nav file mislabel on old caches" → mentioned in Task 1 step 2 comment + commit message ✓
- "Risk: enum drift" → mitigated by required parameter (Task 1 step 3) ✓

**Placeholder scan:** No "TBD", "TODO", "implement later", "similar to Task N" without code. The one `/* existing dst arg */` placeholder in Task 1 step 4 is at the multi-line call site at line 1122; I marked it explicitly because the existing args span multiple lines and I want the engineer to use whatever locals are already in scope rather than guess at names. The "appropriate" anti-pattern doesn't apply — the actual instruction is "append `, NAV_PHASE_LIFT_BUTTON_SHOOT` before the closing `)`" which is concrete.

**Type consistency:** `sim_nav_edge_record_t` is defined once in sim.h (Task 3) and mirrored once in mcp_server.c as `mcp_nav_edge_record_t` (Task 6) with the same layout; both fields are 6 floats + 2 unsigned chars (28 bytes). `Sim_Nav_EdgesNear` signature in sim.h (Task 3) matches the call in `game_nav_edges_near` (Task 4) matches the call in `MCP_NavEdgesNear` (Task 5) matches the call in `tool_nav_edges_near` (Task 6).

**Scope:** Single coherent change, fits one plan. No decomposition needed.

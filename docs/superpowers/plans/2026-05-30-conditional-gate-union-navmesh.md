# Conditional-gate Union Navmesh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `start.bsp`'s navmesh correct in *both* gate states — one union mesh whose per-edge conditions let A* follow the slab-top subgraph when the conditional gates are closed and the descent subgraph when they're open.

**Architecture:** Add a second per-edge predicate `forbids_items` alongside the existing `requires_items`, reserve sigil bits 28–31 in the items mask, and OR live `serverflags` sigils into the query's item set. The bake forces all conditional gates solid for the primary flood (tagging slab-top edges "closed"), then runs a bounded per-gate non-solid supplemental flood (tagging descent edges "open"). Cache stays one state-independent file (format bumped).

**Tech Stack:** C (gnu89 engine + modern-C game DLL), Zig build, SDL3, MCP/HTTP for headless verification. No unit-test framework — verification is `zig build game` + headless bake + `scripts/mcp_call.py` assertions.

---

## Reference: design spec

`docs/superpowers/specs/2026-05-30-conditional-gate-union-navmesh-design.md`

## Conventions used by every verification step

Start a headless instance with the MCP HTTP transport, wait for the bake, then query:

```bash
# launch (background)
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
# wait for bake
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
# ... queries via: python3 scripts/mcp_call.py <tool> '<json>'
# stop when done
pkill -f "zig-out/bin/quake"
```

Key coordinates in `start.bsp` (world units):
- **bossgate slab top centre:** `(536, 1808, 24)` (node origin; feet z=0).
- **boss `end` trigger (shaft bottom):** `(-96, 1760, -608)`.
- **player spawn:** read live via `python3 scripts/mcp_call.py get_player_state` → `origin`.

## File structure

| File | Responsibility | Change |
|---|---|---|
| `sdlquake/game/game_defs.h` | item bit constants | add `IT_SIGIL1..4` |
| `sdlquake/game/sim/sim_nav.c` | bake + A* | `forbids_items` field, skip clause, NAV_VERSION bump, gate keep-solid generalization, closed/open tag passes, supplemental flood |
| `sdlquake/game/sim/sim_ai.c` | brain path queries | pass effective items |
| `sdlquake/game/game_api.h` | engine↔DLL ABI | add `nav_test_path`, bump `GAME_API_VERSION` |
| `sdlquake/engine_src/host_cmd.c` | console commands | add `serverflags`, add `nav_testpath` |
| `sdlquake/mcp/mcp_server.c` | MCP debug | edge-condition readout in `nav_edges_near` |

---

## Task 1: Reserve sigil bits in the item mask

**Files:**
- Modify: `sdlquake/game/game_defs.h` (after `#define IT_QUAD 4194304`)

- [ ] **Step 1: Add the sigil bit constants**

These align with the engine convention `serverflags << 28` already used at `sv_main.c:649`.

```c
// Sigil/rune bits — NOT inventory. Reserved (bits 28..31) so nav-edge
// conditions can gate on serverflags. Mirrors the engine's
// `serverflags << 28` client-stat packing (sv_main.c:649).
#define IT_SIGIL1           (1<<28)
#define IT_SIGIL2           (1<<29)
#define IT_SIGIL3           (1<<30)
#define IT_SIGIL4           (1u<<31)
#define IT_ALL_SIGILS       (IT_SIGIL1|IT_SIGIL2|IT_SIGIL3|IT_SIGIL4)  /* == 15<<28 */
```

- [ ] **Step 2: Build**

Run: `zig build game`
Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git add sdlquake/game/game_defs.h
git commit -m "feat(nav): reserve IT_SIGIL1..4 item bits (28-31) for gate conditions"
```

---

## Task 2: Add `forbids_items` edge predicate + A* skip clause

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c` — `nav_edge_t` struct, `add_edge`, A* skip, `NAV_VERSION`

- [ ] **Step 1: Add the field to `nav_edge_t`**

Find the struct (search `typedef struct {` immediately after `NAV_PHASE_COUNT`). It currently ends:

```c
    unsigned char _pad[2];
    unsigned int  requires_items;  // bitmask matched against player.items
} nav_edge_t;
```

Replace with:

```c
    unsigned char _pad[2];
    unsigned int  requires_items;  // valid only if (items & requires) == requires
    unsigned int  forbids_items;   // invalid if forbids!=0 && (items & forbids)==forbids
} nav_edge_t;
```

- [ ] **Step 2: Default-init it in `add_edge`**

In `add_edge`, after the line `e->requires_items = req_items;`, add:

```c
    e->forbids_items = 0;
```

- [ ] **Step 3: Add the skip clause in `Sim_Nav_PathTo`**

Find (≈ line 2225):

```c
            if (e->requires_items & ~player_items) continue;
```

Replace with:

```c
            if (e->requires_items & ~player_items) continue;
            // Inverse lock: edge exists only while the player LACKS the full
            // forbidden set (e.g. a gate's slab-top vanishes once you hold all
            // sigils). forbids==0 means unconditional.
            if (e->forbids_items &&
                (player_items & e->forbids_items) == e->forbids_items) continue;
```

- [ ] **Step 4: Bump `NAV_VERSION` (cache format changed)**

Find `#define NAV_VERSION    21` and change to:

```c
// v22: nav_edge_t gained forbids_items (gate-state conditions).
#define NAV_VERSION    22
```

- [ ] **Step 5: Build**

Run: `zig build game`
Expected: exit 0.

- [ ] **Step 6: Verify the bake still works (no behavior change yet — forbids=0 everywhere)**

```bash
rm -f id1/cache/navmesh/start-*.nav
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
grep -a "sim_nav: bake " /tmp/cg.log
python3 scripts/mcp_call.py nav_edges_near '{"x":536,"y":1808,"z":0,"radius":80}' | head -c 200
pkill -f "zig-out/bin/quake"
```

Expected: a `sim_nav: bake NNNN nodes` line and a non-empty edges JSON. Bake count should match the pre-change baseline (~5575) since no edges are conditioned yet.

- [ ] **Step 7: Commit**

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "feat(nav): add forbids_items edge predicate + A* inverse-lock skip"
```

---

## Task 3: Feed serverflags sigils into the brain path queries

**Files:**
- Modify: `sdlquake/game/sim/sim_ai.c` — the two `Sim_Nav_PathTo` calls (≈ lines 396 and 427)

- [ ] **Step 1: Add a local helper near the top of the file**

After the includes / file-scope declarations, add:

```c
// Live sigil bits derived from serverflags, in the IT_SIGIL1..4 slots
// (bits 28..31). A* uses these to open/close conditional-gate edges.
static unsigned int nav_sigil_items(void) {
    return (unsigned int)(((int)g->serverflags & 15) << 28);
}
```

- [ ] **Step 2: Pass them at both call sites**

Find both calls (they currently pass `0u` as `player_items`):

```c
            b->path_len = Sim_Nav_PathTo(e->v.origin, node->v.origin,
                                         b->path_pts, NULL, 0u, 32);
```
and
```c
            b->path_len = Sim_Nav_PathTo(e->v.origin, b->last_known_pos,
                                         b->path_pts, NULL, 0u, 32);
```

Replace `0u` with `nav_sigil_items()` in both.

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: exit 0.

- [ ] **Step 4: Verify bot pathing is unaffected (no gate edges tagged yet)**

```bash
HEADLESS=1 ./scripts/run_ai_tests.sh 2>&1 | grep -aE "ALL SCENARIOS PASSED|MISS:" | head
```

Expected: `ALL SCENARIOS PASSED` (or the same pre-existing `t08_bridge` strand noted in project memory — confirm no *new* misses).

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_ai.c
git commit -m "feat(nav): pass live serverflags sigils into brain path queries"
```

---

## Task 4: `serverflags <n>` engine console command

**Files:**
- Modify: `sdlquake/engine_src/host_cmd.c` — new handler + `Cmd_AddCommand` in `Host_InitCommands`

- [ ] **Step 1: Add the handler**

Near the other `Host_*_f` handlers (e.g. above `Host_InitCommands`), add:

```c
/*
==================
Host_Serverflags_f

Dev/test: set the episode-sigil bits directly. Writes the persistent
store (svs.serverflags, survives changelevel), the QC global
(pr_global_struct->serverflags, used by the items stat at sv_main.c:649),
and the game-DLL mirror (game_globals.serverflags, read by the nav query)
so the change takes effect immediately without a reload.
==================
*/
void Host_Serverflags_f (void)
{
	if (Cmd_Argc() != 2)
	{
		Con_Printf ("serverflags is %d\n", (int)svs.serverflags);
		return;
	}
	int v = atoi(Cmd_Argv(1));
	svs.serverflags = v;
	pr_global_struct->serverflags = v;
	game_globals.serverflags = v;
	Con_Printf ("serverflags set to %d\n", v);
}
```

- [ ] **Step 2: Register it in `Host_InitCommands`**

Next to the other `Cmd_AddCommand(...)` calls, add:

```c
	Cmd_AddCommand ("serverflags", Host_Serverflags_f);
```

- [ ] **Step 3: Build the engine + DLL**

Run: `zig build`
Expected: exit 0. (If `game_globals` is not in scope, add `extern game_globals_t game_globals;` — it's defined in `sv_main.c`; confirm the symbol name with `grep -n "game_globals_t game_globals" sdlquake/engine_src/*.c`.)

- [ ] **Step 4: Verify it sets the value**

```bash
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "player entered" /tmp/cg.log && break; sleep 0.5; done
python3 scripts/mcp_call.py console_exec '{"command":"serverflags 15"}'
python3 scripts/mcp_call.py console_exec '{"command":"serverflags"}'
python3 scripts/mcp_call.py console_tail '{"lines":6}'
pkill -f "zig-out/bin/quake"
```

Expected: console tail shows `serverflags set to 15` then `serverflags is 15`.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/host_cmd.c
git commit -m "feat(host): serverflags <n> dev command (sets svs + QC + DLL mirror)"
```

---

## Task 5: `nav_testpath` verification command (graph-level path query)

**Files:**
- Modify: `sdlquake/game/game_api.h` — add vtable entry, bump `GAME_API_VERSION`
- Modify: `sdlquake/game/sim/sim_nav.c` — implement `Sim_Nav_TestPath`
- Modify: `sdlquake/game/game_dll.c` (the file that fills the `game_api_t` struct — confirm with `grep -rln "GAME_API_VERSION" sdlquake/game`) — wire the entry
- Modify: `sdlquake/engine_src/host_cmd.c` — `nav_testpath` console command

- [ ] **Step 1: Add the ABI entry + bump version**

In `game_api.h`, near the existing `nav_edges_near` / `nav_rebake` entries in `game_api_t`, add:

```c
    // Dev/test: run a path query from->to using the live serverflags sigil
    // set; returns waypoint count (0 = no path). Console: `nav_testpath`.
    int   (*nav_test_path)(const float *from, const float *to);
```

Bump the version (currently 26 per CLAUDE.md; confirm the literal):

```c
#define GAME_API_VERSION 27
```

- [ ] **Step 2: Implement it in `sim_nav.c`**

Add near `Sim_Nav_PathTo`:

```c
int Sim_Nav_TestPath(const float *from, const float *to) {
    vec3_t out[64];
    unsigned int items = (unsigned int)(((int)g->serverflags & 15) << 28);
    vec3_t f = { from[0], from[1], from[2] };
    vec3_t t = { to[0],   to[1],   to[2]   };
    int n = Sim_Nav_PathTo(f, t, out, NULL, items, 64);
    char buf[160];
    snprintf(buf, sizeof(buf),
        "nav_testpath: %d waypoints (serverflags sigils=0x%x)\n",
        n, items);
    eng->Con_Print(buf);
    return n;
}
```

Declare it in `sdlquake/game/sim/sim.h` next to `Sim_Nav_PathTo`:

```c
int Sim_Nav_TestPath(const float *from, const float *to);
```

- [ ] **Step 3: Wire the vtable entry**

In the file that initializes `game_api_t` (where `nav_edges_near`/`nav_rebake` are assigned), add:

```c
    .nav_test_path = Sim_Nav_TestPath,
```

(Match the existing assignment style — positional or designated.)

- [ ] **Step 4: Add the console command in `host_cmd.c`**

```c
void Host_NavTestPath_f (void)
{
	if (Cmd_Argc() != 7)
	{
		Con_Printf ("usage: nav_testpath x1 y1 z1 x2 y2 z2\n");
		return;
	}
	float from[3] = { atof(Cmd_Argv(1)), atof(Cmd_Argv(2)), atof(Cmd_Argv(3)) };
	float to[3]   = { atof(Cmd_Argv(4)), atof(Cmd_Argv(5)), atof(Cmd_Argv(6)) };
	extern int Host_GameNavTestPath(const float *from, const float *to); // thin shim
	Host_GameNavTestPath(from, to);
}
```

The shim routes to the loaded `game_api`. Find how `nav_rebake` reaches the DLL (search `nav_rebake` in `host_cmd.c` / `hotreload.c`) and mirror that exact call path for `nav_test_path` instead of inventing a new one. Register:

```c
	Cmd_AddCommand ("nav_testpath", Host_NavTestPath_f);
```

- [ ] **Step 5: Build**

Run: `zig build`
Expected: exit 0. A `GAME_API_VERSION` mismatch at load means the DLL/engine got out of sync — rebuild both.

- [ ] **Step 6: Verify it runs (path exists somewhere obvious)**

```bash
rm -f id1/cache/navmesh/start-*.nav
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
FROM=$(python3 scripts/mcp_call.py get_player_state | python3 -c 'import sys,json;d=json.load(sys.stdin);o=json.loads(d["result"]["content"][0]["text"])["origin"];print(int(o[0]),int(o[1]),int(o[2]))')
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM 536 1808 24\"}"
python3 scripts/mcp_call.py console_tail '{"lines":4}'
pkill -f "zig-out/bin/quake"
```

Expected: `nav_testpath: N waypoints` with N > 0 (spawn → bossgate slab-top is walkable when closed).

- [ ] **Step 7: Commit**

```bash
git add sdlquake/game/game_api.h sdlquake/game/sim/sim_nav.c sdlquake/game/sim/sim.h sdlquake/engine_src/host_cmd.c sdlquake/game/game_dll.c
git commit -m "feat(nav): nav_testpath dev command (game_api v27) for graph-level path checks"
```

---

## Task 6: Edge-condition readout in `nav_edges_near` (MCP)

**Files:**
- Modify: `sdlquake/game/sim/sim.h` — add `forbids` to `sim_nav_edge_record_t`
- Modify: `sdlquake/game/sim/sim_nav.c` — `Sim_Nav_EdgesNear` fills it
- Modify: `sdlquake/mcp/mcp_server.c` — mirror struct + emit `requires`/`forbids` in JSON

- [ ] **Step 1: Extend the public record**

In `sim.h`, the `sim_nav_edge_record_t` currently has `weight, kind, phase`. Add:

```c
    unsigned int requires_items;
    unsigned int forbids_items;
```

- [ ] **Step 2: Fill them in `Sim_Nav_EdgesNear`**

Find where it copies `weight/kind/phase` into the out record and add:

```c
        out[w].requires_items = m->edges[i].requires_items;
        out[w].forbids_items  = m->edges[i].forbids_items;
```

- [ ] **Step 3: Mirror in the MCP struct + JSON**

In `mcp_server.c`, `mcp_nav_edge_record_t` (the "Mirror of sim_nav_edge_record_t") gains the two `unsigned int` fields in the same order. In `tool_nav_edges_near`'s per-edge `snprintf`, append:

```c
            ",\"requires\":%u,\"forbids\":%u"
```
with `r->requires_items, r->forbids_items` added to the arg list.

- [ ] **Step 4: Build**

Run: `zig build`
Expected: exit 0.

- [ ] **Step 5: Verify (fields present, still 0 everywhere pre-tagging)**

```bash
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
python3 scripts/mcp_call.py nav_edges_near '{"x":536,"y":1808,"z":0,"radius":40}' | grep -o '"requires":[0-9]*,"forbids":[0-9]*' | head -3
pkill -f "zig-out/bin/quake"
```

Expected: lines like `"requires":0,"forbids":0` (fields present; all unconditional until Task 7/8).

- [ ] **Step 6: Commit**

```bash
git add sdlquake/game/sim/sim.h sdlquake/game/sim/sim_nav.c sdlquake/mcp/mcp_server.c
git commit -m "feat(nav): expose edge requires/forbids in nav_edges_near"
```

---

## Task 7: Keep episode gates solid + tag closed-only (slab-top) edges

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c` — neutralize whitelist + new closed-tag pass

- [ ] **Step 1: Generalize the keep-solid whitelist**

Find the `else if (!strcmp(icn, "func_bossgate")) {` branch added in commit 20578cb. Change its condition to also match episode gates:

```c
                    } else if (!strcmp(icn, "func_bossgate") ||
                               !strcmp(icn, "func_episodegate")) {
```

(Keep the existing comment body; it applies to both threshold-slab gates.)

- [ ] **Step 2: Add a closed-tag pass after the key-door tagging block**

Immediately after the Phase 4.6 key-locked-door block (search `"%d edges tagged with key requirements"`, insert after its closing brace), add:

```c
    // --- Phase 4.7: conditional-gate slab-top tagging (closed state) -------
    // Each func_bossgate / func_episodegate was kept SOLID for the bake, so
    // the flood seated standable nodes on its TOP surface. Those nodes vanish
    // once the gate opens, so tag their edges with the gate's "solid"
    // predicate. Polarity differs: bossgate is solid while you LACK all 4
    // sigils (forbids ALL); an episode gate is solid while you HOLD its rune
    // (requires that bit).
    {
        int slab_edges_tagged = 0;
        edict_t *ge = eng->ED_Next(g->world);
        while (ge) {
            const char *cn = ge->v.classname;
            int is_boss = cn && !strcmp(cn, "func_bossgate");
            int is_ep   = cn && !strcmp(cn, "func_episodegate");
            if (is_boss || is_ep) {
                unsigned int req = 0, forb = 0;
                if (is_boss) forb = (unsigned int)IT_ALL_SIGILS;
                else         req  = (unsigned int)(((int)ge->v.spawnflags & 15) << 28);

                // Absolute top-surface footprint (origin + maxs), expanded.
                float xmn = ge->v.origin[0] + ge->v.mins[0] - 8;
                float ymn = ge->v.origin[1] + ge->v.mins[1] - 8;
                float xmx = ge->v.origin[0] + ge->v.maxs[0] + 8;
                float ymx = ge->v.origin[1] + ge->v.maxs[1] + 8;
                float topz = ge->v.origin[2] + ge->v.maxs[2];  // slab top
                // Node origin of a player standing on top ≈ topz + 24.
                float zlo = topz + 8, zhi = topz + 40;

                for (int k = 0; k < m->edge_count; k++) {
                    nav_edge_t *e = &m->edges[k];
                    int on_top = 0;
                    for (int end = 0; end < 2 && !on_top; end++) {
                        int p = end ? e->to : e->from;
                        float px = m->points[p].pos[0];
                        float py = m->points[p].pos[1];
                        float pz = m->points[p].pos[2];
                        if (px >= xmn && px <= xmx && py >= ymn && py <= ymx &&
                            pz >= zlo && pz <= zhi) on_top = 1;
                    }
                    if (on_top) {
                        e->requires_items |= req;
                        e->forbids_items  |= forb;
                        slab_edges_tagged++;
                    }
                }
            }
            ge = eng->ED_Next(ge);
        }
        if (slab_edges_tagged > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf),
                "sim_nav: %d gate slab-top edges tagged (closed)\n",
                slab_edges_tagged);
            eng->Con_Print(buf);
        }
    }
```

(Ensure `game_defs.h` is included by `sim_nav.c` so `IT_ALL_SIGILS` resolves; if not, add the include or define `IT_ALL_SIGILS` locally as `(15<<28)`.)

- [ ] **Step 3: Build**

Run: `zig build game`
Expected: exit 0.

- [ ] **Step 4: Verify closed-only tagging**

```bash
rm -f id1/cache/navmesh/start-*.nav
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "slab-top edges tagged" /tmp/cg.log && break; sleep 0.5; done
grep -a "slab-top edges tagged" /tmp/cg.log
# bossgate slab-top edges should carry forbids = 15<<28 = 4026531840
python3 scripts/mcp_call.py nav_edges_near '{"x":536,"y":1808,"z":24,"radius":40}' | grep -o '"forbids":[0-9]*' | sort -u | head
# graph behavior: closed -> slab-top reachable; open -> not (slab edges forbidden)
FROM=$(python3 scripts/mcp_call.py get_player_state | python3 -c 'import sys,json;d=json.load(sys.stdin);o=json.loads(d["result"]["content"][0]["text"])["origin"];print(int(o[0]),int(o[1]),int(o[2]))')
python3 scripts/mcp_call.py console_exec '{"command":"serverflags 0"}'
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM 536 1808 24\"}"
python3 scripts/mcp_call.py console_exec '{"command":"serverflags 15"}'
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM 536 1808 24\"}"
python3 scripts/mcp_call.py console_tail '{"lines":8}'
pkill -f "zig-out/bin/quake"
```

Expected: tag-count line printed; bossgate slab-top edges show `"forbids":4026531840`; `nav_testpath` to the slab top returns **N>0 with serverflags 0** and **0 with serverflags 15** (slab-top edges forbidden once all sigils held). Spawn→boss still 0 in both (descent edges arrive in Task 8).

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "feat(nav): keep episode gates solid + tag gate slab-top edges (closed state)"
```

---

## Task 8: Supplemental per-gate open-passage flood

**Files:**
- Modify: `sdlquake/game/sim/sim_nav.c` — new supplemental pass inside `bake_floodfill`, before the `solid_saves` restore

This is the crux task. It runs a **bounded** flood with one gate at a time set non-solid, seeded from the gate's approach nodes, and tags every edge it creates with that gate's "open" predicate. It reuses the exact primary-flood machinery (`seat_probe`, `eng->SV_WalkMove`, `eng->SV_DropToFloor`, `grid_find`, `add_point`, `add_edge`).

- [ ] **Step 1: Measure the real open-state descent first**

Temporarily understand the shaft so the bound radius is right:

```bash
rm -f id1/cache/navmesh/start-*.nav
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
grep -a "component " /tmp/cg.log         # note the boss-shaft component's xy/z extent
python3 scripts/mcp_call.py nav_edges_near '{"x":-96,"y":1760,"z":-560,"radius":300}' \
  | python3 -c 'import sys,json;d=json.load(sys.stdin);e=json.loads(d["result"]["content"][0]["text"])["edges"];zs=sorted({round(x[k][2]) for x in e for k in ("from","to")});print("shaft node z:",zs[:5],"...",zs[-5:])'
pkill -f "zig-out/bin/quake"
```

Record: the boss-shaft component's xy footprint and the highest shaft-node z. The supplemental flood must bridge from the gate lip (z≈top) down to that highest shaft node. Set `GATE_OPEN_RADIUS` below to cover the horizontal gate↔shaft gap (start at 768; widen if Step 4 shows no connection).

- [ ] **Step 2: Add the supplemental pass**

Find the end of `bake_floodfill` where saved solids are restored (search `solid_saves` and the loop that does `it->v.solid = solid_saves[...]`). Insert this block **immediately before** that restore loop (gates must still be in their primary-pass solid state going in):

```c
    // --- Phase 4.8: per-gate open-passage flood (open state) ---------------
    // For each conditional gate, drop it to non-solid and flood OUTWARD from
    // its approach nodes, bounded to GATE_OPEN_RADIUS of the gate centre. New
    // walkable positions and the edges reaching them are tagged with the
    // gate's "open" predicate (bossgate: requires ALL sigils; episode gate:
    // forbids its rune bit), so A* only uses them once the gate is open.
    {
        const float GATE_OPEN_RADIUS = 768.0f;   // tune from Step 1 measurement
        edict_t *ge = eng->ED_Next(g->world);
        while (ge) {
            const char *cn = ge->v.classname;
            int is_boss = cn && !strcmp(cn, "func_bossgate");
            int is_ep   = cn && !strcmp(cn, "func_episodegate");
            if (!(is_boss || is_ep) || ge->v.solid <= (float)SOLID_NOT) {
                ge = eng->ED_Next(ge); continue;
            }
            unsigned int open_req = 0, open_forb = 0;
            if (is_boss) open_req  = (unsigned int)IT_ALL_SIGILS;
            else         open_forb = (unsigned int)(((int)ge->v.spawnflags & 15) << 28);

            float cx = ge->v.origin[0] + (ge->v.mins[0]+ge->v.maxs[0])*0.5f;
            float cy = ge->v.origin[1] + (ge->v.mins[1]+ge->v.maxs[1])*0.5f;
            float cz = ge->v.origin[2] + ge->v.maxs[2];
            float r2 = GATE_OPEN_RADIUS * GATE_OPEN_RADIUS;

            float saved_solid = ge->v.solid;
            ge->v.solid = (float)SOLID_NOT;

            // Seed: existing nodes within the radius (approach + already-baked
            // shaft component) become BFS starts.
            int sub_head = q_tail;   // reuse the primary queue tail as a marker
            for (int p = 0; p < m->point_count; p++) {
                float dx = m->points[p].pos[0]-cx, dy = m->points[p].pos[1]-cy;
                if (dx*dx + dy*dy > r2) continue;
                if (q_tail >= q_cap) {
                    int nc = q_cap ? q_cap*2 : 256;
                    int *nq = realloc(queue, sizeof(int)*nc);
                    if (!nq) break;
                    queue = nq; q_cap = nc;
                }
                queue[q_tail++] = p;
            }

            // Bounded BFS — same step machinery as Phase 3, but every new edge
            // is tagged open and expansion stops at the radius.
            int sub_iters = 0;
            while (sub_head < q_tail && sub_iters < MAX_EXPAND_ITERS) {
                sub_iters++;
                int cur = queue[sub_head++];
                if (cur < 0 || cur >= m->point_count) continue;
                vec3_t cur_pos = { m->points[cur].pos[0],
                                   m->points[cur].pos[1],
                                   m->points[cur].pos[2] };
                float ddx = cur_pos[0]-cx, ddy = cur_pos[1]-cy;
                if (ddx*ddx + ddy*ddy > r2) continue;   // stay local

                for (int d = 0; d < 8; d++) {
                    vec3_t scratch;
                    if (!seat_probe(probe, cur_pos, scratch)) break;
                    probe->v.flags = (float)((int)probe->v.flags | FL_PARTIALGROUND);
                    if (!eng->SV_WalkMove(probe, k_yaws[d], FLOOD_STEP)) continue;
                    if (!eng->SV_DropToFloor(probe)) continue;
                    vec3_t end = { probe->v.origin[0], probe->v.origin[1],
                                   probe->v.origin[2] };
                    float dz = end[2] - cur_pos[2];
                    if (dz < -POST_WALK_MAX_DROP_Z || dz > POST_WALK_MAX_DROP_Z)
                        continue;
                    int next_idx = grid_find(&grd, m, end);
                    int is_new = 0;
                    if (next_idx < 0) {
                        if (m->point_count >= MAX_NODES) continue;
                        next_idx = add_point(m, &cap_points, &grd, end);
                        if (next_idx < 0) continue;
                        is_new = 1;
                    }
                    if (next_idx == cur) continue;
                    // Tag the connecting edge open. Skip if an identical
                    // unconditional walk edge already exists (it belongs to
                    // both states and must stay unconditional).
                    int dup = 0;
                    int o0 = 0; // linear scan is fine; edge set is local here
                    for (o0 = m->edge_count-1; o0 >= 0 && o0 > m->edge_count-64; o0--) {
                        if (m->edges[o0].from == cur && m->edges[o0].to == next_idx) {
                            dup = 1; break;
                        }
                    }
                    if (!dup) {
                        float ex = m->points[next_idx].pos[0]-cur_pos[0];
                        float ey = m->points[next_idx].pos[1]-cur_pos[1];
                        float ez = m->points[next_idx].pos[2]-cur_pos[2];
                        float w  = (float)sqrt(ex*ex+ey*ey+ez*ez);
                        if (w < 1.0f) w = 1.0f;
                        add_edge(m, &cap_edges, cur, next_idx, w,
                                 NAV_EDGE_WALK, 0, NAV_PHASE_BFS_WALK);
                        nav_edge_t *ne = &m->edges[m->edge_count-1];
                        ne->requires_items |= open_req;
                        ne->forbids_items  |= open_forb;
                    }
                    if (is_new) {
                        if (q_tail >= q_cap) {
                            int nc = q_cap ? q_cap*2 : 256;
                            int *nq = realloc(queue, sizeof(int)*nc);
                            if (!nq) continue;
                            queue = nq; q_cap = nc;
                        }
                        queue[q_tail++] = next_idx;
                    }
                }
            }
            ge->v.solid = saved_solid;
            ge = eng->ED_Next(ge);
        }
    }
```

- [ ] **Step 3: Rebuild adjacency reflects new edges**

Confirm `build_adjacency(m)` is called **after** `bake_floodfill` returns (search the caller). The supplemental edges must be in the adjacency CSR before A* runs. If `build_adjacency` is called inside `bake_floodfill` before this block, move this block above it.

Run: `zig build game`
Expected: exit 0.

- [ ] **Step 4: Verify the open path exists (primary success criterion)**

```bash
rm -f id1/cache/navmesh/start-*.nav
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
FROM=$(python3 scripts/mcp_call.py get_player_state | python3 -c 'import sys,json;d=json.load(sys.stdin);o=json.loads(d["result"]["content"][0]["text"])["origin"];print(int(o[0]),int(o[1]),int(o[2]))')
echo "--- closed: boss unreachable, slab reachable ---"
python3 scripts/mcp_call.py console_exec '{"command":"serverflags 0"}'
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM -96 1760 -608\"}"
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM 536 1808 24\"}"
echo "--- open: boss reachable, slab forbidden ---"
python3 scripts/mcp_call.py console_exec '{"command":"serverflags 15"}'
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM -96 1760 -608\"}"
python3 scripts/mcp_call.py console_exec "{\"command\":\"nav_testpath $FROM 536 1808 24\"}"
python3 scripts/mcp_call.py console_tail '{"lines":12}'
pkill -f "zig-out/bin/quake"
```

Expected:
- serverflags 0: spawn→boss = **0 waypoints**; spawn→slab = **N>0**.
- serverflags 15: spawn→boss = **N>0**; spawn→slab = **0 waypoints**.

If serverflags-15 spawn→boss is still 0, the supplemental flood didn't bridge to the shaft component: re-run Step 1, increase `GATE_OPEN_RADIUS`, and check whether the descent needs drop edges larger than `POST_WALK_MAX_DROP_Z` (if so, add a bounded drop-edge synthesis mirroring Phase 3.5, tagged with `open_req`/`open_forb`, inside the same per-gate loop). Iterate Step 2–4.

- [ ] **Step 5: Commit**

```bash
git add sdlquake/game/sim/sim_nav.c
git commit -m "feat(nav): per-gate open-passage supplemental flood (open-state descent edges)"
```

---

## Task 9: Full verification + close-out

**Files:** none (verification + docs)

- [ ] **Step 1: Closed-state regression (commit 20578cb result still holds)**

```bash
rm -f id1/cache/navmesh/start-*.nav
zig build run -- --headless --mcp-http 9876 +map start > /tmp/cg.log 2>&1 &
for i in $(seq 1 40); do grep -aq "sim_nav: bake " /tmp/cg.log && break; sleep 0.5; done
python3 scripts/mcp_call.py nav_edges_near '{"x":536,"y":1808,"z":24,"radius":120}' \
 | python3 -c 'import sys,json;d=json.load(sys.stdin);e=json.loads(d["result"]["content"][0]["text"])["edges"];zs=sorted({round(x[k][2],1) for x in e for k in ("from","to") if 336<=x[k][0]<=736 and 1600<=x[k][1]<=2016});print("gate-footprint origin z:",zs)'
pkill -f "zig-out/bin/quake"
```

Expected: footprint origin z values clustered at ~24 (feet z=0 on the slab top) — closed state unchanged.

- [ ] **Step 2: Bot regression**

```bash
HEADLESS=1 ./scripts/run_ai_tests.sh 2>&1 | grep -aE "ALL SCENARIOS PASSED|MISS:"
```

Expected: `ALL SCENARIOS PASSED` (modulo the pre-existing `t08_bridge` strand documented in project memory — no *new* misses).

- [ ] **Step 3: Update CLAUDE.md sim-module notes**

In `CLAUDE.md`, under the Phase 8 sim-module section, note that `sim_nav.c` now bakes a union mesh with `forbids_items` gate conditions and the `serverflags`/`nav_testpath` dev commands. Keep it to one or two lines matching the surrounding style.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: note conditional-gate union navmesh in CLAUDE.md"
```

---

## Self-review notes

- **Spec coverage:** conditional-edge model → Tasks 1,2; sigil bits 28–31 → Task 1; effective items at query → Task 3; union bake (keep-solid + closed tag + open pass) → Tasks 7,8; state-independent cache + NAV_VERSION bump → Task 2; `serverflags <n>` → Task 4; debug/MCP readout → Task 6; verification (graph-level both states, closed regression) → Tasks 5,7,8,9.
- **Open risk carried from spec:** Task 8 may need bounded drop-edge synthesis if the descent exceeds `POST_WALK_MAX_DROP_Z`; Step 4 makes that an explicit iterate-until-green gate rather than a hidden assumption.
- **Type consistency:** `forbids_items` (unsigned int) is defined in Task 2 and used identically in Tasks 6,7,8; `nav_test_path`/`Sim_Nav_TestPath` signature consistent across Task 5; `IT_ALL_SIGILS` defined Task 1, used Tasks 7,8.

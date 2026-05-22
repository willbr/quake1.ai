# MCP Server

`sdlquake/mcp/mcp_server.c` exposes the running engine to MCP clients
(Claude Code etc.) as a JSON-RPC 2.0 endpoint speaking the Model Context
Protocol 2024-11-05 dialect.

## Two transports

Selected at startup via the engine command line:

| Flag | Transport | When to use |
|---|---|---|
| `--mcp-stdio` | stdio | Claude Code spawns `quake.exe` as a child process; everything is line-framed over stdin/stdout. |
| `--mcp-http PORT` | HTTP + SSE | Engine is already running; client connects to `http://localhost:PORT/sse`. |

### stdio (`--mcp-stdio`)

- Background SDL thread reads stdin line-by-line, pushes each line as a
  JSON-RPC request into a 16-slot ring queue (`mcp_queue`).
- The main thread drains the queue in `MCP_Frame()` every host frame,
  processes the request synchronously against game state, and writes a
  response to stdout.
- `mcp_active = 1` makes the engine suppress all normal console-print
  output so stdout stays a clean JSON-RPC channel.
- `.mcp.json` at the repo root tells Claude Code how to spawn the
  process and which env vars to set.

### HTTP + SSE (`--mcp-http PORT`)

- Background thread accepts TCP connections, handles two routes:
  - `GET /sse` — opens a Server-Sent-Events stream. Responses are
    written as `data: {json}\n\n` frames.
  - `POST /message` — body is a JSON-RPC request; pushed into the
    same queue as stdin lines.
  - `POST /call` — synchronous one-shot: enqueues the request,
    waits for the response, returns it in the HTTP body. Added
    2026-05-15 specifically for unit tests and shell scripting (no
    need to manage an SSE connection just to run one tool).
- `.mcp.json` switches to `{ "type": "sse", "url": "http://localhost:7777/sse" }`.

`MCP_FRAME_MAX` is **64 KB** (was 4 KB until 2026-05-15). The smaller cap
silently truncated large `list_entities` responses; see
`memory/project_mcp_sse_truncation.md` for the incident.

## Threading model

```
┌────────────────────────────────┐
│ Background reader thread       │
│   read stdin / accept HTTP     │
│   parse one JSON-RPC request   │
│   push line into mcp_queue ────┼──┐
└────────────────────────────────┘  │ SDL_Mutex
                                    │
┌────────────────────────────────┐  │
│ Main thread (Host_Frame)       │  │
│   MCP_Frame():                 │◄─┘
│     while (pop from queue):    │
│       dispatch tool            │
│       write response           │
└────────────────────────────────┘
```

All tool implementations run on the main thread, so they can touch
edicts and renderer state without locking. Tools that need cross-frame
state (e.g. `wait_frames`) park a pending-response slot on the main
thread and resolve it in a later `MCP_Frame`.

## Tool catalogue

Dispatch is a string-compare ladder at `mcp_server.c:1264+`:

| Tool | What it does |
|---|---|
| `get_player_state` | Position, view angles, velocity, health, armor, ammo, current weapon, map name. |
| `list_entities` | Every live edict's index, classname, origin, model. |
| `set_cvar` | Set any cvar by name to a string value (engine parses to int/float as needed). |
| `get_cvar` | Read a cvar back. |
| `console_exec` | Queue any console command via `Cbuf_AddText`. |
| `console_tail` | Last N lines from the engine console scrollback. |
| `screenshot` | Save `vid.buffer` to PNG (writes path returned in response). |
| `sample_pixel` | One ARGB readout from `vid.buffer` at an x,y. |
| `teleport` | Move the player to an origin (writes through `SV_SetOrigin`). |
| `wait_frames` | Park the response for N main-loop frames — used to synchronise tool calls with the next render. |
| `editor_get_scene` | Structured JSON of the Phase 7 in-game editor's scene. |
| `editor_brush_add` | Append a 6-plane AABB brush (mins/maxs). |
| `editor_entity_add` | Append a point entity at an origin. |
| `editor_set_kv` | Upsert a key/value pair on a specific entity. |
| `editor_select` | Programmatic selection — no cursor needed. |

The protocol also handles MCP-required RPCs: `initialize`,
`notifications/initialized`, `tools/list`, `tools/call`, `ping`, plus a
catch-all `error -32601` for unknown methods.

## Why this works

All MCP tools are *synchronous against game state* — they execute in
`MCP_Frame()`, which runs after physics and game thinks for the
current frame. So an entity created by `editor_entity_add` is visible
to a follow-up `list_entities` in the next frame, and a `set_cvar` on
a renderer toggle takes effect on the very next `SCR_UpdateScreen`.

`wait_frames` exists because some tools (e.g. screenshot after a
teleport + look-around sequence) need to give the renderer a chance to
re-rasterise. The parked-response queue lets the client send a
single chain of synchronous calls without polling.

## Known limitation

`+map <name>` from a background-launched HTTP/SSE process has an
unrelated loopback-connect issue: the engine starts in disconnected
state and the first map load needs a client to attach. Workaround:
issue `console_exec map <name>` over MCP after the engine is up.

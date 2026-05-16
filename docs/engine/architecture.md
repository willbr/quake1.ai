# Architecture

## Source-tree split

```
sdlquake/
├── engine_src/      WinQuake engine, forked. We own it; compare against ref/Quake-master/.
├── platform/        SDL3 replacements for sys_/vid_/in_/snd_/net_win.c. Modern C.
├── mcp/             Stdio + HTTP/SSE JSON-RPC MCP server (mcp_server.c).
├── engine/          Engine-side glue: hot-reload, ImGui dev overlay, debug draw,
│                    virtual FS shim, in-game editor (engine/editor/).
├── game/            Hot-reloadable game DLL source. QuakeC ported to C.
│   └── sim/         Phase 8 immersive-sim layer (stim bus, AI FSM, navmesh, wind, light).
└── vendor/
    ├── SDL3-3.4.8/  Headers + prebuilt x64 Windows .dll/.lib.
    ├── imgui-1.92.8 Dear ImGui + SDL3 + SDL_Renderer backends.
    ├── qbsp/        id-qbsp compiler, GPLv2. Compiled in-process for the editor.
    ├── light/       id-LIGHT compiler. Same in-process pattern.
    ├── vis/         id-VIS PVS calculator. Same in-process pattern.
    └── stb/         stb_image/stb_image_write for screenshot/sample.
```

The platform-layer files in `sdlquake/platform/` replace these original units:

| New (SDL3) | Replaces (WinQuake) | Role |
|---|---|---|
| `sys_sdl.c` | `sys_win.c` | `main()`, timing (`SDL_GetTicksNS`), `Sys_Error`, file I/O, command-line parsing |
| `vid_sdl.c` | `vid_win.c` | `SDL_Window` + `SDL_Renderer`; 8-bit framebuffer → `SDL_Texture` → present |
| `in_sdl.c` | `in_win.c` | SDL event polling; scancode → Quake-key mapping; relative mouse |
| `snd_sdl.c` | `snd_win.c` | `SDL_AudioStream` get-callback feeding Quake's DMA ring buffer |
| `net_sdl.c` | `net_wins.c` | Winsock net stub (loopback only by default) |
| `winquake.h` | `winquake.h` | Stub: replaces DirectDraw/DirectSound types with no-op equivalents; shadows the original |

`sdlquake/platform/` is on the include path **before** `sdlquake/engine_src/`,
so our `winquake.h` shadows the original DirectX-laden one. Same trick for
`mgraph.h`, `vid_palette.h`.

## Compiler flags

Engine files (`sdlquake/engine_src/*.c`) are compiled with:

```
-DSDLQUAKE -std=gnu89 -fcommon -fno-strict-aliasing -fwrapv -w -fno-sanitize=undefined
```

- `gnu89 + fcommon` matches the K&R-era tentative-definition behaviour the
  original MSVC build relied on.
- `-fno-sanitize=undefined` is intentional. The engine deliberately uses
  float→int truncation, signed overflow, and other constructs that are UB by
  the standard but well-defined on x86. We don't want UBSan panicking at
  runtime.

Platform files (`sdlquake/platform/*.c`) and the engine glue
(`sdlquake/engine/*.c`) omit `-std=gnu89` — they're written in modern C and
use C99/C11 features freely.

The game DLL (`sdlquake/game/*.c`) builds with `-std=c11` but keeps
`-fno-sanitize=undefined` for the same QuakeC-truncation reason.

Dear ImGui builds with `-std=c++17`.

## Build system (Zig)

A single `build.zig` at the repo root drives the entire build. Steps:

| Step | What it does |
|---|---|
| `zig build` | Compiles `quake.exe` and `game.dll` into `zig-out/bin/`. Installs `SDL3.dll` next to the exe. |
| `zig build run -- [args]` | Build, then run with `args` forwarded to Quake. Working dir pinned to repo root so `id1/` resolves. |
| `zig build game` | Rebuild **only** `game.dll`. Pair with `--hot-reload` in the engine for fast iteration. |
| `zig build extract` | Run `tools/extract_phase6/extract.zig`: reads `ref/wolf3d-data/` + `ref/doom-data/` and writes loose `.spr`/`.wav` into `id1/`. |

Build-time option:

- `-Dnative_game=true` (default) — game logic comes from `game.dll`.
- `-Dnative_game=false` — fall back to the original QuakeC VM
  (`pr_cmds.c` + `pr_exec.c` get compiled in). Useful as a reference.

## Runtime topology

```
                       ┌──────────────────────────────────┐
                       │ Main thread                      │
   stdin/SDL events ──►│                                  │──► SDL window
                       │  Host_Frame                      │──► SDL audio
                       │   ├─ SV_Frame (game.dll)         │──► SDL net
                       │   │   └─ engine_api_t / globals  │
                       │   ├─ CL_ReadFromServer           │
                       │   ├─ MCP_Frame (drain queue)     │
                       │   ├─ ImGui rendering             │
                       │   └─ SCR_UpdateScreen            │
                       └──────────────────────────────────┘
                              ▲              ▲
                              │ enqueue      │ enqueue
                              │              │
         ┌────────────────────┴───┐  ┌───────┴────────────┐
         │ MCP background thread  │  │ ImGui input ticks  │
         │  reads stdin / TCP     │  │  (same thread,     │
         │  pushes JSON-RPC lines │  │   via host loop)   │
         └────────────────────────┘  └────────────────────┘
```

The engine is single-threaded except for the MCP server's reader thread,
which pushes JSON-RPC lines into a mutex-protected ring. The main thread
drains the queue in `MCP_Frame()` each Host frame. All game-state mutations
happen on the main thread — no engine locks are needed for game logic.

Audio runs on SDL3's callback thread but only reads/writes Quake's DMA ring
buffer; no locking required because the ring is producer/consumer with
indexes that are updated atomically.

## Frame loop

`Host_Frame` (in `engine_src/host.c`):

1. Read commands from console buffer and execute (`Cbuf_Execute`).
2. Run the server frame:
   - For each client: parse `clc_move` packets into entvars (button bits etc).
   - `SV_Physics`: integrate `MOVETYPE_*` entities.
   - Game DLL `start_frame` callback.
   - Per-entity `entity_think` callbacks (where `nextthink <= time`).
   - Dispatch touch callbacks (`entity_touch`).
3. Send server updates to clients.
4. Client frame: parse server messages, run prediction, update view.
5. Tick MCP, debug overlays, ImGui.
6. `SCR_UpdateScreen`: render 3D, status bar, console, overlays into
   `vid.buffer` (palette indices). `VID_Update` expands to ARGB and presents.

The host targets 72 Hz (`host_maxfps`) by default; frames are short-circuited
if too little time has passed. Game-side `nextthink` scheduling runs at
whatever rate `Host_Frame` does — usually 10 Hz monster ticks via repeated
`think → set nextthink` chains.

## Why this is a fork, not a mod

We don't ship a `progs.dat` and we don't link the QuakeC VM by default. The
game's edict-driven server logic lives in `sdlquake/game/`, which compiles
to a hot-reloadable `game.dll` exporting `Game_GetAPI()`. The engine talks to
the DLL through `game_api_t` (DLL → engine) and `engine_api_t` (engine → DLL),
defined in `sdlquake/game/game_api.h`. See [game-loop.md](game-loop.md) for
the full ABI.

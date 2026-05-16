# Game Loop, Edicts, Hot-Reload

## What lives where

The original engine had server logic split across QuakeC `progs.dat` and the
C engine. We ported all the QuakeC to C (`sdlquake/game/*.c`) and put it in
a hot-reloadable shared library `game.dll`. The engine talks to the DLL
through a tightly-versioned ABI defined in `sdlquake/game/game_api.h`.

```
┌─────────────────────────┐         engine_api_t (engine → game)
│ Engine (quake.exe)      │◄────────────────────────────────────────┐
│                         │                                         │
│  Host_Frame             │  game_api_t (game → engine)             │
│   └─ SV_Frame ──────────┼─►  init / start_frame /                 │
│       └─ for each ent:  │    entity_spawn / entity_think /        │
│          dispatch to ───┼─►  entity_touch / client_* /            │
│                         │    list_spawn_classes / open_all_doors  │
│  game_globals_t* ──────►│  (shared struct: time, self, other,     │
│                         │   trace_*, v_forward/up/right, parms)   │
└─────────────────────────┘                                         │
                                                                    │
                          ┌─────────────────────────────────────────┘
                          │
                  ┌───────▼─────────────┐
                  │ game.dll            │
                  │  Game_GetAPI() ─────┼─► returns game_api_t*
                  │  init(engine, g) ───┼─► caches them in globals;
                  │                     │   registers cvars, runs Sim_Init
                  └─────────────────────┘
```

`engine_api_t` and `game_api_t` are POD vtables of function pointers — the
DLL's `Game_GetAPI()` returns a `game_api_t *`, and the engine hands the
DLL its own `engine_api_t *` plus a shared `game_globals_t *` in the
`init` call.

## The shared globals struct

`game_globals_t` (see `game_api.h`) is the equivalent of QuakeC's
`self`, `other`, `trace_*`, `pr_global_struct` blob — one allocation,
owned by the game DLL, pointed to by both sides:

- Engine writes `time` / `frametime` / `self` / `other` before invoking
  any game callback.
- Game writes `trace_*` and `v_forward/up/right` after running its own
  builtins (which delegate back to `engine_api->SV_Traceline` and
  `engine_api->MakeVectors`).

The `world` field is the engine's world entity, used as a sentinel for
"no entity found". This is the C equivalent of QuakeC's `world`-as-falsy
idiom — see [the world-sentinel memory](../../memory/project_world_sentinel_pattern.md)
for the full story. Every engine bridge that returns an edict (e.g.
`engine_api->ED_Find` in `engine/hotreload.c`) maps NULL to `g->world`
so game-side code can do `if (ent == g->world)` cleanly.

## ABI versioning

`GAME_API_VERSION` lives at the top of `game_api.h`. Bump it whenever:

- `engine_api_t` or `game_api_t` adds, removes, or reorders a function
  pointer.
- `entvars_t` or `edict_t` layout changes (since both sides use
  `offsetof` on them).
- Any of the fixed-size globals (`parm[16]`) changes size.

The loader (`HotReload_Init` in `engine/hotreload.c`) rejects DLLs whose
`game_api->version` doesn't match. Currently at **18** (bumped from 16 → 17
for `button3`/`button4` in entvars; 17 → 18 for `engine_api->Sample_Lightmap`).

## edict_t

The engine and the DLL share the same `edict_t` definition (via
`game_types.h` in the DLL build, via `progs.h` in the engine build, both
emitted from the same backing fields). At minimum:

```c
typedef struct edict_s {
    qboolean      free;
    link_t        area;          // bbox links for collision broadphase
    int           num_leafs;
    short         leafnums[MAX_ENT_LEAFS];
    entity_state_t baseline;
    float         freetime;
    entvars_t     v;             // ← game-side fields (origin, health, ...)
    // ... a couple of internal book-keeping fields ...
} edict_t;
```

`entvars_t` has ~120 fields (matches QuakeC's `entity_t` definition).
Float-typed even where it should be bool/int — that's a QuakeC artefact we
kept so saves/loads roundtrip via the same `NF_FLOAT` table.

The game-side `edict_t` matches the engine's exact layout because the
hot-reload code in `engine/hotreload.c` includes `game_types.h` under the
`NATIVE_GAME` build, computes `offsetof(edict_t, v.origin)` etc., and
hands those offsets to the DLL via a few accessor functions in
`sv_bridge.c`. Mismatched layouts crash with `ACCESS_VIOLATION` at a small
hex address — that hex address is the field's byte offset.

## Hot-reload mechanism

`engine/hotreload.c`. The strategy is to keep the running engine pointing
at a *copy* of `game.dll` so Zig's linker can overwrite the original
during a rebuild:

1. On startup `HotReload_Init` copies `zig-out/bin/game.dll` to
   `zig-out/bin/game_loaded.dll` and `SDL_LoadObject`s the copy.
2. With `--hot-reload`, `HotReload_Frame` polls the mtime of
   `game.dll` (the source path) every ~1 s. On change:
   - Wait for the file to settle (size stable for 250 ms).
   - `SDL_UnloadObject` the old `game_loaded.dll`.
   - Copy the new `game.dll` → `game_loaded.dll`.
   - `SDL_LoadObject` and resolve `Game_GetAPI`.
   - Check `game_api->version == GAME_API_VERSION` — if not, abort the
     reload (the old DLL stays unloaded, but at least we don't crash).
   - Call `game_api->init(&engine_api, g)` again. The DLL is expected to
     re-precache models/sounds it tracks itself.
3. Without `--hot-reload`, polling is off — the DLL is loaded once at
   startup and stays put.

Typical iteration:

```sh
# Terminal A: keep the engine running
zig build run -- --hot-reload +map e1m1

# Terminal B: edit a game/*.c file, then
zig build game
```

The next call to `HotReload_Frame` picks up the new mtime, swaps the
DLL, and the live engine reflects the changes — usually within a second
of the build completing.

## Cvar bridge

Cvars registered from the game DLL via `engine_api->Cvar_Register` are
stored in a small fixed-size table (`MAX_GAME_CVARS = 32`) inside
`hotreload.c`. The table owns the `name`, `string`, and a `cvar_t`
struct laid out identically to the engine's. On reload the engine
*keeps* the previously-registered cvars — the values you tweaked in the
console survive the swap. This is important for the iteration loop:
otherwise every reload would reset your debug toggles.

## VFS shim

`engine_api->LoadFile(path, &size)` is implemented in
`engine/virtual_fs.c` and delegates to `COM_LoadHunkFile`. The PAK search
path is set up by the engine at startup (`common.c::COM_InitFilesystem`);
the DLL doesn't see PAKs as a separate concept, just files. Free the
buffer with `free()` when done.

## Console commands the DLL owns

The engine registers `opendoors` / `opensecretdoors` as console commands,
but dispatches them through `game_api->open_all_doors` /
`open_all_secret_doors`. This keeps the door-finding logic with the rest
of the door code in `game/doors.c` — out of the engine. Older DLLs may
have NULL slots, so the engine handlers null-check.

## Editor introspection

`game_api->list_spawn_classes` returns a NULL-terminated array of every
classname the DLL knows how to spawn (`monster_*`, `weapon_*`,
`light_*`, `info_*`, `func_*`, `trigger_*`, …). The Phase 7 editor uses
this to populate its entity palette without hardcoding the list. About
89 entries; see `docs/port-audit.md` for the spawn-table cross-check.

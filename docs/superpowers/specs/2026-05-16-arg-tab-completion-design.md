# Argument tab completion for `map` / `editor_*`

**Date:** 2026-05-16
**Status:** Approved

## Goal

When the console line is `map <partial>` or `editor_load|save|new <partial>`, Tab cycles through matching map names instead of failing the cmd/cvar/alias match. Shift+Tab reverses. The state machine (snapshot → cycle → wrap-to-partial → invalidate-on-edit) from `2026-05-13-console-tab-completion-cycling-design.md` is reused unchanged.

## Background

`Key_TabComplete` (`sdlquake/engine_src/keys.c:178`) currently treats the entire editor contents as the partial and matches against the command, cvar, and alias registries. That gives no completion for arguments: `map e1m<Tab>` searches for a command named "map e1m", finds nothing, no-op.

The previous spec explicitly listed "last-token-only completion" and "map / model / sound name completion" as out of scope. This spec lifts both restrictions for a single, well-defined case: the first token names a command that has registered an argument-completer; the partial is everything after the first space.

Map name sources:
- Vanilla id1 levels live inside `id1/pak0.pak` / `pak1.pak` (`maps/<name>.bsp`).
- Custom / extracted levels land loose in `id1/maps/<name>.bsp`.
- Editor source maps live loose in `id1/maps/<name>.map` (`.map` is never packed).

`pack_t` and `searchpath_t` are file-static to `common.c`, so the pack-walking has to live there.

## Design

### Per-command arg-completer registry (`cmd.c` / `cmd.h`)

```c
typedef int (*arg_completer_t)(const char *partial, char **out, int max, int count);

void Cmd_RegisterArgCompleter (const char *cmdname, arg_completer_t fn);
arg_completer_t Cmd_FindArgCompleter (const char *cmdname);
```

Fixed-size table (`#define CMD_MAX_ARG_COMPLETERS 16`). Linear lookup. The `cmdname` pointer is borrowed; the caller must pass a string-literal or otherwise stable pointer — same lifetime contract `Cmd_AddCommand` already imposes.

The completer contract mirrors the existing `Cmd_CompleteCommandAll`: append matches into `out[count..max-1]` and return the new count. The match pointers must be stable for the lifetime of one tab-cycle (until the cycle invalidates and the completer is re-run, at which point the same buffers are reused).

### `Key_TabComplete` dispatch (`keys.c`)

At the top of the function, before the cmd/cvar/alias walk:

1. Find the first space in `console_le.buf`. If absent or at position 0, fall through to existing behavior.
2. Copy the first word into a local `cmdname[64]`. Look up via `Cmd_FindArgCompleter`. If NULL, fall through.
3. Otherwise: the prefix is `<cmdname><spaces-up-to-arg>`, the partial is the rest. Store the prefix length so we can write `prefix + match` back on commit; store `tab_partial` as the arg-only partial.
4. Run the completer to populate `tab_matches`. Dedup the same way (caller-side, in `keys.c`).
5. On commit: `LE_SetText(prefix); LE_InsertString(match);` — no trailing space.

State variables added (file-scoped, alongside existing `tab_*`):

```c
static char  tab_prefix[LE_MAX_LINE];     /* "" when not in arg-completion mode */
static int   tab_prefix_len;
```

When `tab_prefix_len == 0`, behavior is exactly the existing global-name cycle. When `> 0`, the wrap-to-partial state restores `prefix + tab_partial` and the per-match write is `prefix + tab_matches[i]`.

`tab_committed_line` continues to snapshot the full editor contents, so the existing edit-invalidates-cycle check works without change.

### `COM_EnumMatchingFiles` (`common.c` / `common.h`)

```c
typedef void (*COM_FileMatch_cb_t)(const char *basename, void *userdata);

void COM_EnumMatchingFiles (const char *subdir,
                            const char *suffix,
                            COM_FileMatch_cb_t cb,
                            void *userdata);
```

- `subdir`: directory name within each searchpath (no leading/trailing slash), e.g. `"maps"`.
- `suffix`: extension including dot, e.g. `".bsp"`.
- Walks `com_searchpaths`. For each pack: iterates `pack->files[i].name`; if it starts with `"<subdir>/"` and ends with `suffix`, invokes `cb` with the middle segment (basename, suffix stripped). For each on-disk searchpath: calls `Sys_EnumerateDir("<filename>/<subdir>", cb_wrapper, ...)`; the wrapper filters entries ending in `suffix` and invokes `cb` with the stripped basename.

Dedup is the caller's responsibility — the helper just enumerates.

### `Sys_EnumerateDir` (`sys.h` + `platform/sys_sdl.c`)

```c
typedef void (*Sys_EnumDir_cb_t)(const char *fname, void *userdata);
void Sys_EnumerateDir (const char *path, Sys_EnumDir_cb_t cb, void *userdata);
```

Implementation thunks through `SDL_EnumerateDirectory`. Failure (missing dir, permission) is silent — completion just yields zero on-disk matches. Vestigial `sys_dos.c` / `sys_linux.c` / etc. are not in `build.zig` and don't need updates.

### Completers

`host_cmd.c`:

```c
static char map_pool[256][32];
static int  map_pool_n;
static int  map_pool_partial_len;
static const char *map_pool_partial;

static void map_pool_cb (const char *basename, void *ud);   // dedups + prefix-checks
static int  Host_Map_CompleteBspName (const char *partial, char **out, int max, int count);
```

`Host_Map_CompleteBspName` calls `COM_EnumMatchingFiles("maps", ".bsp", map_pool_cb, NULL)`. The callback rejects entries whose basename doesn't match `partial`, rejects duplicates (linear scan of `map_pool`), and otherwise copies into `map_pool[map_pool_n++]` and appends `&map_pool[i][0]` into `out[count++]`. Registered immediately after `Cmd_AddCommand("map", ...)` in `Host_InitCommands`.

`editor.c`: symmetric, with `(".map" / "map" pool / Editor_CompleteMapName)`. Registered for `editor_load`, `editor_save`, `editor_new` in `Editor_Init` after each `Cmd_AddCommand`.

256 entries × 32 chars per name is a 16 KB per-pool fixed allocation — vanilla Quake has 29 maps; even shipping every Q1 SP+DM mod ever made would not push 256.

## Smoke test

Open console (`~`) and:

1. Type `map e<Tab>` — cycles `e1m1`, `e1m2`, … `e4m8`. Shift+Tab reverses.
2. Type `map s<Tab>` — cycles `start`, `start2`, etc.
3. Type `map z<Tab>` (no matches) — line stays as typed, no crash.
4. Type `editor_load <Tab>` — cycles loose `.map` files in `id1/maps/`.
5. Type `editor_save <Tab>` — same set as `editor_load`.
6. Type `mapxx<Tab>` (no space) — falls through to global completion as today.
7. Press Backspace mid-cycle, Tab again — fresh cycle on the new partial.
8. Existing global completion (`sv_<Tab>`, `qua<Tab>`) still works unchanged.

## Out of scope

- Cursor-not-at-end completion.
- Quoted args, escapes.
- Multi-arg commands (`bind a +att<Tab>` still treats the whole line as a partial against cmd names — same as today).
- Fuzzy matching, case-insensitive matching, partial substring.
- Completion of `playdemo`, `load`, `save`, `exec`, `playsound` etc. — easy to add later; out of scope here.

## Touched files

- `sdlquake/engine_src/cmd.c` / `cmd.h` — registry.
- `sdlquake/engine_src/common.c` / `common.h` — `COM_EnumMatchingFiles`.
- `sdlquake/engine_src/sys.h` — `Sys_EnumerateDir` prototype + callback typedef.
- `sdlquake/platform/sys_sdl.c` — `Sys_EnumerateDir` impl via `SDL_EnumerateDirectory`.
- `sdlquake/engine_src/keys.c` — prefix/arg-completer dispatch in `Key_TabComplete`.
- `sdlquake/engine_src/host_cmd.c` — `Host_Map_CompleteBspName` + registration.
- `sdlquake/engine/editor/editor.c` — `Editor_CompleteMapName` + 3 registrations.

No game-DLL ABI bump. No build-system change.

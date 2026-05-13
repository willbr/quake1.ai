# Console history persistence

**Date:** 2026-05-13
**Status:** Approved

## Goal

Preserve the player's console command history across runs of the engine, so that pressing UpArrow in the console after relaunching the game brings back recently entered commands.

## Background

The console's command-line history lives in `key_lines[32][MAXCMDLINE]`, a fixed-size ring buffer declared in `sdlquake/engine_src/keys.c`. Each slot stores a `]`-prefixed string. UpArrow / DownArrow in `Key_Console` walk the ring (`history_line = (history_line - 1) & 31`) skipping empty slots (`!key_lines[i][1]`). Enter advances `edit_line` (`= (edit_line + 1) & 31`) and resets the next slot to `]\0`.

The buffer is initialized fresh in `Key_Init` and never persisted. Nothing else in the engine reads or writes these slots — `console.c:56` only `extern`s the array for display while typing.

## Design

### Buffer size

Grow the ring from 32 to 128 lines. The `32` and `& 31` literals are localized:

| File | Line(s) | Change |
|---|---|---|
| `sdlquake/engine_src/keys.c` | 30, 168, 205, 209, 220, 524 | `32` → `CMDLINES`, `& 31` → `& CMDLINES_MASK` |
| `sdlquake/engine_src/console.c` | 56 | `extern char key_lines[32]...` → `extern char key_lines[CMDLINES]...` |

Add to the top of `keys.c`:

```c
#define CMDLINES        128
#define CMDLINES_MASK   (CMDLINES - 1)
```

`CMDLINES` must remain a power of two so the existing `& MASK` ring arithmetic is correct. Expose the macros via `keys.h` so `console.c` can reference `CMDLINES` in the `extern`.

### File format

Path: `<com_gamedir>/history.txt` (e.g. `id1/history.txt`, alongside `config.cfg`). Per-mod, since `-game hipnotic` will set `com_gamedir` to `hipnotic/`.

Plain text, one command per line, oldest first, no `]` prefix, LF newlines. Lines longer than `MAXCMDLINE - 2` are truncated on load (leaving room for the `]` prefix and `\0`). Blank lines are skipped on load. Missing file is treated as empty history (silent — no error).

### Load: `Key_LoadHistory(void)`

Called at the tail of `Key_Init` (which runs after `COM_InitFilesystem` in `Host_Init`, so `com_gamedir` is valid).

Behavior:
1. `fopen(va("%s/history.txt", com_gamedir), "r")`. Return silently if NULL.
2. Read lines with `fgets`. For each non-empty trimmed line, write `]` to `key_lines[n][0]`, copy up to `MAXCMDLINE - 2` chars after, NUL-terminate. Increment `n`. Stop at `CMDLINES - 1` lines (leaving one slot for the current edit line).
3. After load:
   - `edit_line = n & CMDLINES_MASK;`
   - `key_lines[edit_line][0] = ']'; key_lines[edit_line][1] = 0;`
   - `history_line = edit_line;`
   - `key_linepos = 1;`

UpArrow then walks back through the restored slots (skipping the freshly cleared edit slot, then any empties) exactly as it does for in-session history.

### Save: `Key_SaveHistory(void)`

Rewrites the file from scratch (truncate-and-write). Single-player engine — no concurrent writers, no need for tempfile + rename.

Behavior:
1. `fopen(va("%s/history.txt", com_gamedir), "w")`. On failure, `Con_Printf` a one-line warning and return.
2. Iterate the ring starting at `(edit_line + 1) & CMDLINES_MASK` (oldest), looping back to `edit_line`. For each slot where `key_lines[i][1] != 0`, write `key_lines[i] + 1` (skip `]`) followed by `\n`.
3. Do **not** write the current edit slot — it's the prompt the user is typing into, not a committed command.
4. `fclose`.

Worst case: 128 lines × ~256 bytes = ~32 KB. Negligible per Enter on modern storage.

### Dedup and empty-line suppression

The current K_ENTER branch advances `edit_line` unconditionally, even when the typed line is empty (`key_lines[edit_line][1] == 0`) or identical to the immediately previous command. Both leave junk in the ring. New behavior, evaluated in this order *after* `Cbuf_AddText` echoes the command but *before* the ring advance:

1. **Empty:** if `key_lines[edit_line][1] == 0`, reset `key_linepos = 1` and return without advancing `edit_line` or calling `Key_SaveHistory`.
2. **Duplicate:** let `prev = (edit_line - 1) & CMDLINES_MASK`. If `key_lines[prev][1] != 0` and `strcmp(key_lines[edit_line] + 1, key_lines[prev] + 1) == 0`, clear the current slot (`key_lines[edit_line][1] = 0; key_linepos = 1;`) and return without advancing `edit_line` — the previous identical entry remains the most-recent and `prev` is reused next time. Do not save (file is already consistent).
3. **New entry:** advance `edit_line` and `history_line` as today, reset the next slot to `]\0`, then call `Key_SaveHistory()`.

This matches bash's `HISTCONTROL=ignoredups,ignorespace`-ish behavior — minus the leading-space case, which Quake doesn't need.

### Hooks

| Where | What | Why |
|---|---|---|
| `keys.c` — end of `Key_Init` | `Key_LoadHistory();` | Restore on startup |
| `keys.c` — `Key_Console` K_ENTER branch, after `Cbuf_AddText` / `Con_Printf` and before `edit_line` advance | dedup + empty-line check (see above) | Avoid junk entries |
| `keys.c` — `Key_Console` K_ENTER branch, after ring advance (new-entry path only) | `Key_SaveHistory();` | Crash-durable persistence |
| `host.c` — tail of `Host_WriteConfiguration` | `Key_SaveHistory();` | Belt-and-braces at clean shutdown |

### Header exposure

Add to `sdlquake/engine_src/keys.h`:

```c
#define CMDLINES        128
#define CMDLINES_MASK   (CMDLINES - 1)

void Key_LoadHistory (void);
void Key_SaveHistory (void);
```

`Key_LoadHistory` is only called internally from `Key_Init`, but exposing both makes the API symmetric and useful for testing.

## What is explicitly out of scope

- **No cvar to toggle.** Always on. Easy to add later (`con_history_save`) if desired.
- **No atomic write (temp + rename).** Single writer, single-player game; if the engine crashes mid-fwrite the worst case is a truncated last line, recoverable on next launch.
- **No history-grep / Ctrl-R search.** Out of scope; UpArrow remains the only navigation.
- **No timestamping or session boundaries** in the file. Plain command list, one per line.
- **No migration from a hypothetical earlier on-disk format.** This is the first format.

## Verification

This codebase has no test suite. Manual smoke test:

1. `zig build run -- +map e1m1`.
2. Open console (`~`), enter several commands (`echo hello`, `god`, `noclip`, etc.).
3. Press Enter on a duplicate — verify it does not double-list when UpArrowing.
4. Quit cleanly. Confirm `id1/history.txt` exists and contains the commands oldest-first, no `]` prefixes.
5. Relaunch. Open console. Press UpArrow — verify previous commands appear in reverse-chronological order.
6. Confirm the `]` prompt is clean (no leftover edit-line state from previous run).
7. Delete `id1/history.txt`, relaunch — verify silent start with empty history.

## Touched files

- `sdlquake/engine_src/keys.c` — buffer size constants, `Key_LoadHistory`, `Key_SaveHistory`, hooks in `Key_Init` and `Key_Console`, dedup/empty-line check.
- `sdlquake/engine_src/keys.h` — `CMDLINES` / `CMDLINES_MASK` macros, two new prototypes.
- `sdlquake/engine_src/console.c` — update one `extern` from `[32]` to `[CMDLINES]`.
- `sdlquake/engine_src/host.c` — one `Key_SaveHistory()` call at tail of `Host_WriteConfiguration`.

No platform-layer, build, or game-DLL changes.

# Console tab completion cycling

**Date:** 2026-05-13
**Status:** Approved

## Goal

In the in-game console, pressing Tab on an ambiguous partial should cycle through every matching command, cvar, and alias — not silently lock onto the first match. Shift+Tab cycles backward. Editing the line ends the cycle and starts a new one on the next Tab.

## Background

Tab handling lives in `Key_Console` (`sdlquake/engine_src/keys.c:208-222`):

```c
if (key == K_TAB)
{
    cmd = Cmd_CompleteCommand (key_lines[edit_line]+1);
    if (!cmd)
        cmd = Cvar_CompleteVariable (key_lines[edit_line]+1);
    if (cmd)
    {
        Q_strcpy (key_lines[edit_line]+1, cmd);
        key_linepos = Q_strlen(cmd)+1;
        key_lines[edit_line][key_linepos] = ' ';
        key_linepos++;
        key_lines[edit_line][key_linepos] = 0;
        return;
    }
}
```

Both completion helpers walk a singly-linked list and `return` on the first `Q_strncmp` match (`cmd.c:599-602`, `cvar.c:91-93`). Aliases (`cmd_alias` in `cmd.c:35`) are not consulted at all. Result: if 12 cvars start with `sv_`, only the first one in list order is ever reachable from Tab.

Shift state is tracked by the global `shift_down` in `keys.c:32`, set in `keys.c:744`. The SDL platform layer maps both `SDL_SCANCODE_LSHIFT` and `SDL_SCANCODE_RSHIFT` to `K_SHIFT` (`platform/in_sdl.c:40-41`), so the existing global is reliable in this port.

`cvar_vars` is already declared `extern` in `cvar.h:97`. `cmd_alias` is declared non-static in `cmd.h:35`. `cmd_functions` is `static` to `cmd.c`, so enumerating commands must happen inside `cmd.c`.

## Design

### Behavior

1. **Tab on a fresh line** (no completion cycle in progress, or the line content differs from what completion last wrote):
   - Snapshot the partial: copy `key_lines[edit_line] + 1` into `tab_partial`.
   - Walk command list, then cvar list, then alias list. Push every name whose first `tab_partial_len` chars match `tab_partial` (case-sensitive, matching the existing `Q_strncmp`) into `tab_matches[]`, deduplicating by name.
   - If no matches: do nothing (line unchanged), reset state. Pressing Tab again repeats the walk — no caching of "nothing found".
   - If matches exist: set `tab_index = 0`, replace the line with `tab_matches[0]` followed by `' '`, snapshot the new line into `tab_committed_line`.

2. **Tab while cycling** (line content matches `tab_committed_line` and `tab_match_count > 0`):
   - `shift_down == false`: advance `tab_index`. If it was `count-1`, wrap to `-1` (partial-restored state) and write `tab_partial` back to the line with no trailing space; otherwise write `tab_matches[tab_index]` plus `' '`.
   - `shift_down == true`: same in reverse — decrement, with `0` wrapping to `-1` and `-1` wrapping to `count-1`.
   - Snapshot the new line into `tab_committed_line` after writing.

3. **Any non-Tab key handled by `Key_Console`** invalidates the cycle. The simplest, most robust way: at the top of `Key_Console`, if `key != K_TAB && key != K_SHIFT && tab_match_count != 0`, zero `tab_match_count` and `tab_partial_len`. K_SHIFT is excluded because Shift down/up events themselves are routed through `Key_Console` (`consolekeys[K_SHIFT] = true` at `keys.c:660`); they must not reset the cycle.

4. **Single match** still completes on first Tab. Subsequent Tabs are visually no-ops: the cycle goes `0 → -1 (partial) → 0 → ...`, which is correct and useful (it lets the user back out of an unintended completion).

### Match collection

Three new helpers, each appending into a caller-provided array and returning the new count. They do not clear the array, so the caller can chain them:

```c
// cmd.c (cmd_functions has file-static scope)
int Cmd_CompleteCommandAll (char *partial, char **out, int max, int count);
int Cmd_CompleteAliasAll   (char *partial, char **out, int max, int count);

// cvar.c
int Cvar_CompleteVariableAll (char *partial, char **out, int max, int count);
```

Contract:
- `partial` non-NULL, length > 0. Empty partial returns `count` unchanged (matches existing helpers' behavior — empty partial is treated as no completion).
- Walks its list, appending each `Q_strncmp(partial, name, len) == 0` match into `out[count++]` until `count == max`. If full, stops silently and returns `max`.
- Pointers stored in `out` point into the respective struct's stable `name` field (cmd / cvar / alias entries are heap-allocated once and never freed during a session — safe to hold across frames). Aliases use `cmdalias_t::name` which is a fixed-size `char[32]` inside the struct, not a pointer — same lifetime guarantee.
- No dedup inside the helpers; dedup happens in `keys.c` after all three runs.

Header additions:

```c
// cmd.h
int Cmd_CompleteCommandAll (char *partial, char **out, int max, int count);
int Cmd_CompleteAliasAll   (char *partial, char **out, int max, int count);

// cvar.h
int Cvar_CompleteVariableAll (char *partial, char **out, int max, int count);
```

The existing `Cmd_CompleteCommand` / `Cvar_CompleteVariable` functions stay in place; they're trivial and removing them is unrelated cleanup.

### State (file-scoped statics in `keys.c`)

```c
#define TAB_MAX_MATCHES 256

static char  tab_partial[MAXCMDLINE];
static int   tab_partial_len;
static char *tab_matches[TAB_MAX_MATCHES];
static int   tab_match_count;       // 0 ⇒ no cycle in progress
static int   tab_index;             // -1 ⇒ showing partial, 0..count-1 ⇒ showing match
static char  tab_committed_line[MAXCMDLINE];
```

`TAB_MAX_MATCHES = 256` comfortably exceeds Quake's command universe (mods stay well under this). If the snapshot would overflow, print `Con_Printf("tab completion: 256 match limit reached, truncating\n")` once and use the first 256.

### Dedup

After all three list walks, the caller in `keys.c` does an O(n²) pairwise `strcmp` dedup in-place. n ≤ 256 with realistic match counts under ~30, so this is negligible. Dedup matters because the engine forbids cvar/command name collisions (`Cmd_AddCommand` checks `Cvar_FindVar`), but aliases share a namespace with both — `alias quit "quit"` is legal and would otherwise appear twice.

### Cycle invalidation hook

At the top of `Key_Console`, after the function's existing declarations and before the K_ENTER branch:

```c
if (key != K_TAB && key != K_SHIFT && tab_match_count != 0)
{
    tab_match_count = 0;
    tab_partial_len = 0;
}
```

This sits above K_ENTER so that pressing Enter on a completed command also clears state — necessary because the K_ENTER branch reassigns `key_lines` slots and resets `key_linepos`, leaving `tab_committed_line` stale.

### Replacement for the K_TAB block

The current 15-line block becomes a call into a new static helper:

```c
static void Key_TabComplete (qboolean reverse);
```

…that implements the state machine above. Keeping it in `keys.c` is fine — it's the only consumer of `tab_*` state.

### Header exposure

No new public types or extern globals. Only the three list-walk helpers gain prototypes in `cmd.h` / `cvar.h`.

## What is explicitly out of scope

- **Last-token-only completion.** `bind a +att<TAB>` will still treat the full `bind a +att` as the partial and match it against whole command names (so it will find nothing, same as today). Fixing this requires parsing the line into tokens — separate change.
- **Map / model / sound name completion.** Tab completes against the cmd/cvar/alias registries only.
- **Bash-style "list all matches" UX.** The user picked cycling.
- **Case-insensitive matching.** Existing helpers are case-sensitive and Quake names are conventionally lowercase. Keep parity.
- **Removing the old `Cmd_CompleteCommand` / `Cvar_CompleteVariable` helpers.** They're 6 lines each and have no in-tree callers after this change, but yanking them is unrelated cleanup.
- **Persisting the cycle across history-up/-down.** UpArrow / DownArrow rewrite `key_lines[edit_line]` from the ring; the invalidation hook resets the cycle, which is the right behavior — the user has selected a different command.

## Verification

No test suite. Manual smoke test (after `zig build run -- +map e1m1` and opening the console):

1. Type `sv_` then Tab repeatedly. Verify each press advances to a different cvar starting with `sv_`. After the last, the line returns to `sv_` (partial). Next Tab returns to the first match.
2. Shift+Tab on the same partial cycles backward through the same set.
3. Type a partial that matches a command, a cvar, and an alias (e.g. `alias quit "quit"` first, then `qu<Tab>`). Verify all three are reachable and no duplicates appear.
4. After completing to e.g. `sv_friction `, press Backspace once and then Tab — verify a fresh cycle starts on the new (shorter) partial.
5. Partial with one match (e.g. a unique prefix): Tab fills it; second Tab restores the partial; third Tab fills again.
6. Partial with zero matches: Tab is a no-op.
7. Press Enter on a completed command, then Tab again on a fresh line — verify the cycle starts fresh and does not see stale `tab_committed_line` state.

## Touched files

- `sdlquake/engine_src/cmd.c` — add `Cmd_CompleteCommandAll`, `Cmd_CompleteAliasAll`.
- `sdlquake/engine_src/cmd.h` — two new prototypes.
- `sdlquake/engine_src/cvar.c` — add `Cvar_CompleteVariableAll`.
- `sdlquake/engine_src/cvar.h` — one new prototype.
- `sdlquake/engine_src/keys.c` — static state, `Key_TabComplete`, invalidation hook in `Key_Console`, replacement of the K_TAB block.

No platform-layer, build, header-shadow, or game-DLL changes.

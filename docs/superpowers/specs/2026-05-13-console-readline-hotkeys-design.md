# Console line editor & readline hotkeys

**Status:** design approved 2026-05-13.
**Scope:** add a proper insertion-cursor line editor to the Quake console plus the standard bash readline hotkeys. Excludes `Ctrl+R` reverse-i-search (future work) and changes to chat input (`Key_Message`).

## Motivation

The console today is not a line editor. `key_linepos` doubles as both length and cursor, so typing always appends at the end, `K_LEFTARROW` decrements `key_linepos` destructively (the next keystroke truncates the suffix), and `K_RIGHTARROW` is unhandled. `Con_DrawInput` mirrors that model: it stomps the trailing NUL with the cursor glyph and fills the remainder with spaces.

The user wants familiar bash-style line editing in the in-game console — `Ctrl+A/E`, `Ctrl+B/F`, `Ctrl+K/U/W`, `Ctrl+D`, `Ctrl+L`, etc. None of those are meaningful without a real insertion cursor, so the cursor refactor and the hotkey set ship as one feature.

## Approach

Extract the live line state into a small `line_editor_t` struct exposed by a new `line_editor.[ch]` module. `keys.c` keeps the persisted history ring (`key_lines[CMDLINES]`) — that's just storage and stays untouched — but the *live* edit buffer moves out of the ring slot and into a single `static line_editor_t console_le;`. `Key_Console` becomes a key→op dispatch. `Con_DrawInput` renders from a scratch buffer composed each frame, leaving `console_le.buf` immutable.

## Module API — `line_editor.h`

```c
#define LE_MAX_LINE 256

typedef struct {
    char buf[LE_MAX_LINE];   // raw editable text, NUL-terminated, no prompt prefix
    int  len;                // strlen(buf), cached
    int  cursor;             // 0..len; insertion point (chars to its left)
} line_editor_t;

void LE_Reset      (line_editor_t *le);
void LE_SetText    (line_editor_t *le, const char *s);   // also moves cursor to end

qboolean LE_InsertChar     (line_editor_t *le, int c);   // false if full
void     LE_DeleteBack     (line_editor_t *le);
void     LE_DeleteForward  (line_editor_t *le);
void     LE_MoveLeft       (line_editor_t *le);
void     LE_MoveRight      (line_editor_t *le);
void     LE_BeginningOfLine(line_editor_t *le);
void     LE_EndOfLine      (line_editor_t *le);
void     LE_KillToEnd      (line_editor_t *le);
void     LE_KillToStart    (line_editor_t *le);
void     LE_KillPrevWord   (line_editor_t *le);          // whitespace-delimited
```

Pure data ops, no globals, no I/O. No kill ring — killed text is discarded (Ctrl+Y is out of scope).

`LE_KillPrevWord` deletes back from the cursor over any run of whitespace, then over the non-whitespace word before it (bash semantics).

## Integration in `keys.c`

The ring `key_lines[CMDLINES]` is the persisted history. It keeps its `']' + content` storage format and the wraparound bookkeeping in `edit_line` / `history_line`. What changes is that `key_lines[edit_line]` is no longer the live edit buffer — it becomes the next-write slot, written only when Enter commits.

Lifecycle:

- `Key_Init` and `Key_LoadHistory` end with `LE_Reset(&console_le)`.
- `Key_Console` dispatches each key (see table below).
- **Enter commit**: write `']' + console_le.buf` into `key_lines[edit_line]`, run the existing dedupe / advance / `Key_SaveHistory` logic, then `LE_Reset(&console_le)`. `Cbuf_AddText` receives `console_le.buf` directly (no leading `']'` skip).
- **Up/Down history**: existing ring-walk loop runs, then `LE_SetText(&console_le, key_lines[history_line] + 1)`. The empty case (wrap back to `edit_line`) sets the editor empty.
- **Tab completion**: rewritten against `console_le`. Reads `console_le.buf` as the partial. Replaces via `LE_SetText` on each cycle step. The cycle-active check (`tab_committed_line`) compares against `console_le.buf`.

`ctrl_down` mirrors `shift_down`: declared at file scope in `keys.c`, set inside `Key_Event` on `K_CTRL` up/down, alongside the existing `K_SHIFT` handler.

## Key dispatch table

`Key_Console` dispatch, in order:

1. **`ctrl_down` + letter** — fast-path table:
   | key | action |
   |---|---|
   | `a` | `LE_BeginningOfLine` |
   | `e` | `LE_EndOfLine` |
   | `b` | `LE_MoveLeft` |
   | `f` | `LE_MoveRight` |
   | `p` | same as `K_UPARROW` |
   | `n` | same as `K_DOWNARROW` |
   | `k` | `LE_KillToEnd` |
   | `u` | `LE_KillToStart` |
   | `w` | `LE_KillPrevWord` |
   | `l` | `Cbuf_AddText("clear\n")` |
   | `d` | `LE_DeleteForward` (no-op on empty line) |
   | `h` | `LE_DeleteBack` |

   Any other Ctrl+letter falls through (consumed, no action) — we don't want them inserting as literals.

2. **Non-Ctrl specials**:
   | key | action |
   |---|---|
   | `K_ENTER` | commit path (uses `console_le.buf`) |
   | `K_TAB` | `Key_TabComplete` (rewritten against `console_le`) |
   | `K_BACKSPACE` | `LE_DeleteBack` |
   | `K_LEFTARROW` | `LE_MoveLeft` (no longer destructive) |
   | `K_RIGHTARROW` | `LE_MoveRight` (now handled) |
   | `K_DEL` | `LE_DeleteForward` |
   | `K_UPARROW` | history prev → `LE_SetText` |
   | `K_DOWNARROW` | history next → `LE_SetText` |
   | `K_HOME` | `LE_BeginningOfLine` (was: top of scrollback) |
   | `K_END` | `LE_EndOfLine` (was: bottom of scrollback) |
   | `K_PGUP`, `K_MWHEELUP` | scrollback up (unchanged) |
   | `K_PGDN`, `K_MWHEELDOWN` | scrollback down (unchanged) |

3. **Printable 32..126**: `LE_InsertChar`.

### Behaviour changes baked into the table

- `K_LEFTARROW` is non-destructive (was destructive).
- `K_HOME` / `K_END` move to line start / end (were top / bottom of scrollback). Scrollback ends are still reachable via repeated PGUP/PGDN.
- `K_DEL` is newly handled.

## Rendering — `Con_DrawInput`

The current "write cursor into the live buffer, write NUL afterwards to remove it" trick is dropped; `console_le.buf` is never mutated by rendering.

Each frame `Con_DrawInput` composes a scratch buffer of `1 + len + 1 + slack` chars:

1. Byte 0 is `']'`.
2. Bytes `1..1+len` are `console_le.buf`.
3. Byte `1+len` is `' '` (slot for an end-of-line cursor).
4. The blinking cursor glyph (`10 + ((int)(realtime*con_cursorspeed) & 1)`) overlays index `1 + cursor`.
5. Horizontal prestep: if `1 + cursor >= con_linewidth`, shift the visible window by `1 + cursor - con_linewidth + 1` so the cursor stays on the last column (same logic as today, driven by cursor instead of line length).
6. Pad to `con_linewidth` with spaces, draw the visible window.

## Edge cases

- **`Ctrl+D` on empty line**: no-op. (Bash exits the shell here; we don't want surprise console-close.)
- **`Ctrl+L`**: invokes the existing `clear` console command (clears scrollback). Input line is preserved naturally because the editor lives outside the scrollback buffer.
- **`Ctrl+W` at start of line**: no-op (nothing to kill).
- **Ctrl+letter for unmapped letters** (e.g. `Ctrl+S`, `Ctrl+G`, `Ctrl+Q`, `Ctrl+R`): swallowed, no insert. Reserves room to add more later (notably `Ctrl+R` in a follow-up).
- **`Shift+Tab`** still drives reverse Tab cycling — unchanged.
- **`MAXCMDLINE` vs `LE_MAX_LINE`**: both 256. The ring slot is `MAXCMDLINE` bytes for `']' + content + NUL`, so the editable content length is capped at `MAXCMDLINE - 2 = 254` chars. `LE_InsertChar` enforces this by refusing inserts when `len >= MAXCMDLINE - 2`. The current code has the same effective cap (`key_linepos < MAXCMDLINE - 1` lets `key_linepos` reach 254 max, so content positions 1..254 = 254 chars), so no regression.
- **Control chars from SDL**: SDL gives us `'a'` (97) when Ctrl+A is pressed, not 0x01. The existing `key < 32 || key > 127` guard for printable insert keeps working as-is; Ctrl detection is via `ctrl_down`.

## Files touched

- **New**: `sdlquake/engine_src/line_editor.h`, `sdlquake/engine_src/line_editor.c`.
- **Modified**:
  - `sdlquake/engine_src/keys.c` — line edit refactor, hotkey dispatch, Tab completion rewrite, `ctrl_down` tracking.
  - `sdlquake/engine_src/console.c` — `Con_DrawInput` rewrite.
- **Build**: `line_editor.c` joins the engine compile list in `build.zig` alongside `keys.c`. Same `-std=gnu89 -fcommon -fno-sanitize=undefined` flags.

No changes to: `history.txt` format, ring layout, `Key_LoadHistory` / `Key_SaveHistory`, chat input (`Key_Message`), the key bindings table, the SDL platform layer.

## Test plan

No unit-test suite exists; verification is by smoke test per `CLAUDE.md`.

1. `zig build run -- +map e1m1`, open console (`~`).
2. Type `bind w +forward` and verify:
   - `LEFTARROW` moves the cursor without deleting.
   - `Ctrl+A` jumps to start; `Ctrl+E` to end.
   - `Ctrl+B` / `Ctrl+F` equal `LEFTARROW` / `RIGHTARROW`.
   - `HOME` / `END` equal `Ctrl+A` / `Ctrl+E`.
   - Insert mid-line: type a char, see the suffix push right.
   - `Backspace` mid-line deletes char before cursor; `Delete` / `Ctrl+D` deletes at cursor; `Ctrl+H` equals backspace.
   - `Ctrl+K` kills from cursor to end; `Ctrl+U` from start to cursor; `Ctrl+W` kills previous word.
   - `Ctrl+L` clears scrollback (same as `clear`), input line preserved.
   - `Enter` commits, cursor resets, `id1/history.txt` updates.
3. `UP` to recall a history entry, edit it mid-string, `DOWN` to walk forward.
4. `Tab` on a partial → cycles via `Tab` / `Shift+Tab`. Edit a char, `Tab` again starts a fresh cycle.
5. `PGUP` / `PGDN` still scroll back/forward; `Ctrl+P` / `Ctrl+N` walk history.

If those behave as bash-readline would (modulo missing pieces like `Ctrl+R`), the feature is done.

## Out of scope (future work)

- `Ctrl+R` reverse incremental history search (needs its own mini editor mode).
- Word motion (`Alt+B`, `Alt+F`, `Alt+D`).
- Transpose (`Ctrl+T`).
- Kill ring + yank (`Ctrl+Y`).
- Sharing the editor with chat input (`Key_Message`).

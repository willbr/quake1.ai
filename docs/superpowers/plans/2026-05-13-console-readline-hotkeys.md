# Console Line Editor + Readline Hotkeys Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a proper insertion-cursor line editor to the Quake console plus bash-style readline hotkeys (Ctrl+A/E/B/F/P/N/K/U/W/L/D/H), per the spec at `docs/superpowers/specs/2026-05-13-console-readline-hotkeys-design.md`.

**Architecture:** A new `line_editor.[ch]` module exposes a pure-data `line_editor_t` (buffer + length + cursor). `keys.c` holds a single static `console_le` as the live edit buffer; the existing `key_lines[CMDLINES]` ring stays as persisted-history storage only. `Key_Console` becomes a key→op dispatch (with a Ctrl-letter table); `Con_DrawInput` renders from `console_le` via a per-frame scratch buffer without mutating it.

**Tech Stack:** C (gnu89 per project), Zig build system, SDL3 platform layer (already maps Ctrl as `K_CTRL`). No test framework in the project — verification is build + in-game smoke test, per `CLAUDE.md`.

**File structure (all changes inside `sdlquake/engine_src/` plus `build.zig`):**

| File | Action | Responsibility |
|---|---|---|
| `sdlquake/engine_src/line_editor.h` | create | Public API: types and ops (`LE_Reset`, `LE_InsertChar`, …) |
| `sdlquake/engine_src/line_editor.c` | create | Pure implementation of the line ops |
| `sdlquake/engine_src/keys.c` | modify | Live edit moves into `console_le`; new Ctrl dispatch; Tab/Enter/history paths use editor; add `ctrl_down` tracking |
| `sdlquake/engine_src/console.c` | modify | `Con_DrawInput` rewrite (scratch-buffer render, no mutation of editor state) |
| `build.zig` | modify | Add `"line_editor.c"` to `engine_files` array |

No changes to: `Key_Message` (chat input), key bindings table, SDL platform layer, history.txt format, ring layout (`Key_LoadHistory` / `Key_SaveHistory`).

---

## Task 1: Add line_editor module

**Files:**
- Create: `sdlquake/engine_src/line_editor.h`
- Create: `sdlquake/engine_src/line_editor.c`
- Modify: `build.zig` (add `"line_editor.c"` to the `engine_files` array)

The module is pure data: no globals, no I/O. After this task, nothing in the engine uses it yet; it just compiles into an unreferenced TU.

- [ ] **Step 1: Create `sdlquake/engine_src/line_editor.h`**

```c
#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

#include "quakedef.h"  // for qboolean, MAXCMDLINE

#define LE_MAX_LINE MAXCMDLINE

typedef struct
{
	char	buf[LE_MAX_LINE];	// raw editable text, NUL-terminated, no prompt prefix
	int		len;				// strlen(buf), cached
	int		cursor;				// 0..len; insertion point
} line_editor_t;

void		LE_Reset           (line_editor_t *le);
void		LE_SetText         (line_editor_t *le, const char *s);

qboolean	LE_InsertChar      (line_editor_t *le, int c);
void		LE_DeleteBack      (line_editor_t *le);
void		LE_DeleteForward   (line_editor_t *le);
void		LE_MoveLeft        (line_editor_t *le);
void		LE_MoveRight       (line_editor_t *le);
void		LE_BeginningOfLine (line_editor_t *le);
void		LE_EndOfLine       (line_editor_t *le);
void		LE_KillToEnd       (line_editor_t *le);
void		LE_KillToStart     (line_editor_t *le);
void		LE_KillPrevWord    (line_editor_t *le);

#endif
```

- [ ] **Step 2: Create `sdlquake/engine_src/line_editor.c`**

```c
/*
line_editor.c — insertion-cursor line editor used by the console.
*/
#include "quakedef.h"
#include "line_editor.h"

// Ring slot is MAXCMDLINE bytes for ']' + content + NUL, so content length
// is capped at MAXCMDLINE - 2.
#define LE_MAX_CONTENT (MAXCMDLINE - 2)

void LE_Reset (line_editor_t *le)
{
	le->buf[0] = 0;
	le->len = 0;
	le->cursor = 0;
}

void LE_SetText (line_editor_t *le, const char *s)
{
	int n = (int)strlen(s);
	if (n > LE_MAX_CONTENT)
		n = LE_MAX_CONTENT;
	memcpy(le->buf, s, (size_t)n);
	le->buf[n] = 0;
	le->len = n;
	le->cursor = n;
}

qboolean LE_InsertChar (line_editor_t *le, int c)
{
	if (le->len >= LE_MAX_CONTENT)
		return false;
	// Shift suffix [cursor..len] one byte right (including the NUL).
	memmove(&le->buf[le->cursor + 1], &le->buf[le->cursor],
			(size_t)(le->len - le->cursor + 1));
	le->buf[le->cursor] = (char)c;
	le->cursor++;
	le->len++;
	return true;
}

void LE_DeleteBack (line_editor_t *le)
{
	if (le->cursor == 0)
		return;
	memmove(&le->buf[le->cursor - 1], &le->buf[le->cursor],
			(size_t)(le->len - le->cursor + 1));
	le->cursor--;
	le->len--;
}

void LE_DeleteForward (line_editor_t *le)
{
	if (le->cursor == le->len)
		return;
	memmove(&le->buf[le->cursor], &le->buf[le->cursor + 1],
			(size_t)(le->len - le->cursor));
	le->len--;
}

void LE_MoveLeft (line_editor_t *le)
{
	if (le->cursor > 0)
		le->cursor--;
}

void LE_MoveRight (line_editor_t *le)
{
	if (le->cursor < le->len)
		le->cursor++;
}

void LE_BeginningOfLine (line_editor_t *le)
{
	le->cursor = 0;
}

void LE_EndOfLine (line_editor_t *le)
{
	le->cursor = le->len;
}

void LE_KillToEnd (line_editor_t *le)
{
	le->buf[le->cursor] = 0;
	le->len = le->cursor;
}

void LE_KillToStart (line_editor_t *le)
{
	if (le->cursor == 0)
		return;
	memmove(le->buf, &le->buf[le->cursor],
			(size_t)(le->len - le->cursor + 1));
	le->len -= le->cursor;
	le->cursor = 0;
}

void LE_KillPrevWord (line_editor_t *le)
{
	int start = le->cursor;
	// Walk past trailing whitespace before the cursor.
	while (start > 0 && le->buf[start - 1] == ' ')
		start--;
	// Walk past the non-whitespace word.
	while (start > 0 && le->buf[start - 1] != ' ')
		start--;
	if (start == le->cursor)
		return;	// nothing to kill
	memmove(&le->buf[start], &le->buf[le->cursor],
			(size_t)(le->len - le->cursor + 1));
	le->len -= (le->cursor - start);
	le->cursor = start;
}
```

- [ ] **Step 3: Add `"line_editor.c"` to `build.zig`**

Modify the `engine_files` array (around line 43 in `build.zig`) to add `"line_editor.c"` right after `"keys.c"`:

```zig
        "keys.c",
        "line_editor.c",
        "mathlib.c",
```

- [ ] **Step 4: Build**

Run: `zig build`
Expected: build succeeds. No warnings about unused functions (gnu89 doesn't warn on unused statics; non-static functions are not flagged).

- [ ] **Step 5: Commit**

```bash
git add sdlquake/engine_src/line_editor.h sdlquake/engine_src/line_editor.c build.zig
git commit -m "feat(console): add line_editor module"
```

---

## Task 2: Switchover — route console editing through `console_le`

**Files:**
- Modify: `sdlquake/engine_src/keys.c`
- Modify: `sdlquake/engine_src/console.c`

After this task the console has a real insertion cursor: `K_LEFTARROW` / `K_RIGHTARROW` move non-destructively, `K_HOME` / `K_END` jump to line start / end, `K_DEL` deletes at cursor, the cursor renders mid-line correctly, and Tab completion + history recall + Enter all flow through the editor. No Ctrl-letter hotkeys yet — those land in Task 3.

- [ ] **Step 1: In `keys.c`, add the include, the editor state, and `ctrl_down` tracking**

At the top of `keys.c` (just under `#include "quakedef.h"`):

```c
#include "quakedef.h"
#include "line_editor.h"
```

Find the file-scope declarations (around `int shift_down=false;` on line 32) and add `ctrl_down` and the live editor right after:

```c
int		shift_down=false;
int		ctrl_down=false;
int		key_lastpress;

int		edit_line=0;
int		history_line=0;

static line_editor_t console_le;
```

In `Key_Event` find the existing K_SHIFT handler (around line 847):

```c
	if (key == K_SHIFT)
		shift_down = down;
```

Add the K_CTRL handler immediately below it:

```c
	if (key == K_SHIFT)
		shift_down = down;
	if (key == K_CTRL)
		ctrl_down = down;
```

In `Key_Init` find the trailing `Key_LoadHistory ();` call (around line 806) and add an editor reset right after:

```c
	Key_LoadHistory ();
	LE_Reset (&console_le);
```

- [ ] **Step 2: In `keys.c`, replace the tab-completion module to use `console_le`**

Replace the entire tab-completion block in `keys.c` — from the `//==========...` comment header above `TAB_MAX_MATCHES` (around line 152) through the end of `Key_TabComplete` (around line 265) — with the version below. The cycle bookkeeping is unchanged; only the buffer the partial is read from / written to changes (now `console_le.buf` via `LE_SetText`, instead of `key_lines[edit_line]`).

```c
//============================================================================
// Tab-completion cycling state.
//
// One static state machine for the console's Tab key. Pressing Tab snapshots
// the current partial from console_le, walks the cmd / cvar / alias
// registries to collect every match, replaces the editor contents with the
// first match (followed by a space), and remembers what it wrote.
// Subsequent Tabs (while the editor still matches what we wrote last time)
// advance through the match list; Shift+Tab walks it backwards. After the
// last match the cycle visits a "partial restored" state (the user's
// original typed text, no completion) before wrapping back to the first
// match. Any edit to the line is detected on the next Tab by comparing
// against tab_committed_line, and starts a fresh cycle.
//============================================================================
#define TAB_MAX_MATCHES 256

static char  tab_partial[LE_MAX_LINE];
static int   tab_partial_len;
static char *tab_matches[TAB_MAX_MATCHES];
static int   tab_match_count;			// 0 ⇒ no cycle in progress
static int   tab_index;					// -1 ⇒ partial restored, 0..count-1 ⇒ match N
static char  tab_committed_line[LE_MAX_LINE];

static void Key_TabComplete (qboolean reverse)
{
	char	*partial;
	int		partial_len;
	qboolean cycling;
	int		i, j;

	partial = console_le.buf;
	partial_len = console_le.len;

	cycling = (tab_match_count > 0)
		&& !Q_strcmp (console_le.buf, tab_committed_line);

	if (!cycling)
	{
		int n, dedup_count;

		tab_match_count = 0;
		tab_partial_len = 0;
		if (partial_len == 0)
			return;

		Q_strcpy (tab_partial, partial);
		tab_partial_len = partial_len;

		n = 0;
		n = Cmd_CompleteCommandAll   (tab_partial, tab_matches, TAB_MAX_MATCHES, n);
		n = Cvar_CompleteVariableAll (tab_partial, tab_matches, TAB_MAX_MATCHES, n);
		n = Cmd_CompleteAliasAll     (tab_partial, tab_matches, TAB_MAX_MATCHES, n);

		if (n == TAB_MAX_MATCHES)
			Con_Printf ("tab completion: %d match limit reached, truncating\n", TAB_MAX_MATCHES);

		// Dedup in place. n is small in practice (typically < 30), so O(n^2)
		// is fine. Aliases share a namespace with cmds/cvars and may collide.
		dedup_count = 0;
		for (i = 0 ; i < n ; i++)
		{
			for (j = 0 ; j < dedup_count ; j++)
				if (!Q_strcmp (tab_matches[i], tab_matches[j]))
					break;
			if (j == dedup_count)
				tab_matches[dedup_count++] = tab_matches[i];
		}

		if (dedup_count == 0)
			return;

		tab_match_count = dedup_count;
		tab_index = reverse ? dedup_count - 1 : 0;
	}
	else
	{
		if (reverse)
		{
			if (tab_index == -1)
				tab_index = tab_match_count - 1;
			else if (tab_index == 0)
				tab_index = -1;
			else
				tab_index--;
		}
		else
		{
			if (tab_index == -1)
				tab_index = 0;
			else if (tab_index == tab_match_count - 1)
				tab_index = -1;
			else
				tab_index++;
		}
	}

	if (tab_index == -1)
	{
		LE_SetText (&console_le, tab_partial);
	}
	else
	{
		LE_SetText (&console_le, tab_matches[tab_index]);
		LE_InsertChar (&console_le, ' ');
	}

	Q_strcpy (tab_committed_line, console_le.buf);
}
```

- [ ] **Step 3: In `keys.c`, add history-walk helpers above `Key_Console`**

Insert these two static helpers just above the `Key_Console` definition (so they're in scope for the dispatch). They factor out the existing UP/DOWN ring-walk logic so the Task 3 Ctrl+P / Ctrl+N hotkeys can call them too.

```c
static void Key_HistoryPrev (void)
{
	do
	{
		history_line = (history_line - 1) & CMDLINES_MASK;
	} while (history_line != edit_line
			&& !key_lines[history_line][1]);
	if (history_line == edit_line)
		history_line = (edit_line + 1) & CMDLINES_MASK;
	LE_SetText (&console_le, key_lines[history_line] + 1);
}

static void Key_HistoryNext (void)
{
	if (history_line == edit_line)
		return;
	do
	{
		history_line = (history_line + 1) & CMDLINES_MASK;
	}
	while (history_line != edit_line
		&& !key_lines[history_line][1]);
	if (history_line == edit_line)
		LE_Reset (&console_le);
	else
		LE_SetText (&console_le, key_lines[history_line] + 1);
}
```

- [ ] **Step 4: In `keys.c`, replace `Key_Console` with the editor-routed version**

Replace the entire `Key_Console` function (the existing block from `void Key_Console (int key)` through its closing brace, around lines 274–408) with:

```c
/*
====================
Key_Console

Interactive line editing and console scrollback. All line edits flow
through console_le; the ring slot at key_lines[edit_line] is only written
when Enter commits a line.
====================
*/
void Key_Console (int key)
{
	if (key == K_ENTER)
	{
		int prev;

		// Commit editor contents into the ring slot. console_le.buf is
		// capped at MAXCMDLINE-2 so '\]' + content + NUL fits.
		key_lines[edit_line][0] = ']';
		Q_strcpy (key_lines[edit_line] + 1, console_le.buf);

		Cbuf_AddText (console_le.buf);
		Cbuf_AddText ("\n");
		Con_Printf ("]%s\n", console_le.buf);

		// Empty line: don't advance the ring, don't save.
		if (!console_le.buf[0])
		{
			LE_Reset (&console_le);
			if (cls.state == ca_disconnected)
				SCR_UpdateScreen ();
			return;
		}

		// Duplicate of the immediately previous command: keep the previous
		// entry as the most recent, clear the just-typed slot, and skip the
		// save (file already reflects this state).
		prev = (edit_line - 1) & CMDLINES_MASK;
		if (key_lines[prev][1]
			&& !Q_strcmp (console_le.buf, key_lines[prev] + 1))
		{
			key_lines[edit_line][1] = 0;
			history_line = edit_line;
			LE_Reset (&console_le);
			if (cls.state == ca_disconnected)
				SCR_UpdateScreen ();
			return;
		}

		// New entry: advance ring and persist.
		edit_line = (edit_line + 1) & CMDLINES_MASK;
		history_line = edit_line;
		key_lines[edit_line][0] = ']';
		key_lines[edit_line][1] = 0;
		LE_Reset (&console_le);
		Key_SaveHistory ();
		if (cls.state == ca_disconnected)
			SCR_UpdateScreen ();	// force an update, because the command
									// may take some time
		return;
	}

	if (key == K_TAB)
	{
		Key_TabComplete (shift_down);
		return;
	}

	if (key == K_BACKSPACE)	{ LE_DeleteBack    (&console_le); return; }
	if (key == K_LEFTARROW)	{ LE_MoveLeft      (&console_le); return; }
	if (key == K_RIGHTARROW){ LE_MoveRight     (&console_le); return; }
	if (key == K_DEL)		{ LE_DeleteForward (&console_le); return; }
	if (key == K_HOME)		{ LE_BeginningOfLine(&console_le); return; }
	if (key == K_END)		{ LE_EndOfLine     (&console_le); return; }

	if (key == K_UPARROW)	{ Key_HistoryPrev (); return; }
	if (key == K_DOWNARROW)	{ Key_HistoryNext (); return; }

	if (key == K_PGUP || key == K_MWHEELUP)
	{
		con_backscroll += 2;
		if (con_backscroll > con_totallines - (vid.height>>3) - 1)
			con_backscroll = con_totallines - (vid.height>>3) - 1;
		return;
	}

	if (key == K_PGDN || key == K_MWHEELDOWN)
	{
		con_backscroll -= 2;
		if (con_backscroll < 0)
			con_backscroll = 0;
		return;
	}

	if (key < 32 || key > 127)
		return;	// non printable

	LE_InsertChar (&console_le, key);
}
```

Note: `K_INS` (insert mode toggle) and the old `K_HOME` / `K_END` scrollback-end bindings are dropped per the spec. Scrollback end is still reachable via repeated PGUP/PGDN.

- [ ] **Step 5: In `console.c`, rewrite `Con_DrawInput`**

At the top of `console.c`, add the include after the existing `#include "quakedef.h"`:

```c
#include "quakedef.h"
#include "line_editor.h"
```

And add an `extern` declaration for the editor (top of file, near the other externs around line 58):

```c
extern	int		key_linepos;
extern	line_editor_t	console_le;
```

`key_linepos` is now unused by `console.c` after the rewrite — leave the extern in place if other code in `console.c` references it elsewhere, otherwise drop it. (Grep `console.c` for other uses; if `key_linepos` is referenced only inside the function we're replacing, remove the extern too.)

Replace the existing `Con_DrawInput` function (around lines 526–557) with:

```c
/*
================
Con_DrawInput

Renders the live edit line from console_le. The buffer is not mutated;
each frame we walk visible columns and compute what character belongs
there. The blinking cursor glyph overlays whatever character sits at the
cursor position.

The input line scrolls horizontally if the cursor passes the right edge.
================
*/
void Con_DrawInput (void)
{
	int		y;
	int		col;
	int		s = Con_Scale();
	int		cursor_col;		// logical column of cursor (0 = prompt slot)
	int		col_offset;
	int		cursor_glyph;

	if (key_dest != key_console && !con_forcedup)
		return;

	// Logical text: ']' at col 0, console_le.buf at cols 1..len, ' ' at col len+1.
	// Cursor sits at col 1 + console_le.cursor.
	cursor_col = 1 + console_le.cursor;

	// Horizontal scroll so the cursor stays visible.
	col_offset = 0;
	if (cursor_col >= con_linewidth)
		col_offset = cursor_col - con_linewidth + 1;

	cursor_glyph = 10 + ((int)(realtime * con_cursorspeed) & 1);

	y = con_vislines - 16 * s;

	for (col = 0 ; col < con_linewidth ; col++)
	{
		int logical_col = col + col_offset;
		int ch;

		if (logical_col == cursor_col)
			ch = cursor_glyph;
		else if (logical_col == 0)
			ch = ']';
		else if (logical_col >= 1 && logical_col <= console_le.len)
			ch = (unsigned char)console_le.buf[logical_col - 1];
		else
			ch = ' ';

		Con_DrawCharScaled ((col + 1) * 8 * s, y, ch, s);
	}
}
```

- [ ] **Step 6: Update `Con_ToggleConsole_f` in `console.c` to reset the editor on toggle**

`Con_ToggleConsole_f` (around lines 72–92 in `console.c`) currently clears the live line on console-close-while-connected via `key_linepos = 1;`. After the switchover, the editor owns the live line, so reset it instead. Find:

```c
		if (cls.state == ca_connected)
		{
			key_dest = key_game;
			key_lines[edit_line][1] = 0;	// clear any typing
			key_linepos = 1;
		}
```

Replace with:

```c
		if (cls.state == ca_connected)
		{
			key_dest = key_game;
			LE_Reset (&console_le);
		}
```

The `key_lines[edit_line][1] = 0;` line is removed because the ring slot is no longer the live buffer (it's only written by Enter).

- [ ] **Step 7: Remove the now-dead `extern int key_linepos;` in `console.c`**

After steps 5–6, `console.c` no longer references `key_linepos` at all. Remove the line:

```c
extern	int		key_linepos;
```

(near the top of the file). `key_linepos` itself is still defined in `keys.c` because other code (e.g. `mcp_server.c`, the editor) may depend on it — but per `grep -rn "extern int key_linepos\|key_linepos" sdlquake/`, if only `keys.c` and the original `console.c` referenced it, the `keys.c` definition can stay (it's no longer updated but it's harmless dead state). Don't delete it from `keys.c` in this task — that's a follow-up cleanup, not in scope.

- [ ] **Step 8: Build**

Run: `zig build`
Expected: build succeeds with no errors. Warnings about unused `key_linepos` writes in `console.c` (if any) are OK for this task — they'll be cleaned up only if they're truly dead.

- [ ] **Step 9: Smoke test — basic line editor behavior**

Run: `zig build run -- +map e1m1`

Open the console (`~`), then verify each of the following. **Do not skip this step** — these are the behaviors the spec promised.

| Input | Expected |
|---|---|
| Type `bind w +forward` | text appears, cursor at end |
| Press `LEFTARROW` four times | cursor moves left, no chars deleted |
| Press a printable key | char inserts mid-line, suffix pushes right |
| Press `RIGHTARROW` | cursor advances |
| Press `HOME` | cursor jumps to start (right after `]`) |
| Press `END` | cursor jumps to end |
| Press `BACKSPACE` mid-line | deletes char before cursor |
| Press `DEL` mid-line | deletes char at cursor |
| Press `UP` then `DOWN` | walks history, cursor lands at end |
| Press `TAB` after typing `con`, then `TAB` again | cycles through completions |
| Press `ENTER` | command runs, `id1/history.txt` updated, editor resets |
| Press `PGUP` / `PGDN` | scrollback scrolls |

If any of these misbehave, debug before continuing. The most likely failure modes:

- Cursor renders at wrong column → `Con_DrawInput` `cursor_col` math wrong.
- Suffix gets truncated when inserting mid-line → `LE_InsertChar` `memmove` size wrong.
- LEFTARROW deletes → `K_LEFTARROW` still routing to `LE_DeleteBack`.

- [ ] **Step 10: Commit**

```bash
git add sdlquake/engine_src/keys.c sdlquake/engine_src/console.c
git commit -m "feat(console): route line editing through line_editor"
```

---

## Task 3: Ctrl-letter readline hotkeys

**Files:**
- Modify: `sdlquake/engine_src/keys.c`

After this task, the standard bash readline hotkeys are live: Ctrl+A/E (home/end), Ctrl+B/F (left/right), Ctrl+P/N (history prev/next), Ctrl+K (kill to end), Ctrl+U (kill to start), Ctrl+W (kill prev word), Ctrl+L (clear scrollback), Ctrl+D (delete forward), Ctrl+H (backspace).

- [ ] **Step 1: In `keys.c`, add the Ctrl-letter dispatch at the top of `Key_Console`**

Insert this block as the very first thing inside `Key_Console`, before the `if (key == K_ENTER)` check:

```c
	// Ctrl+letter readline hotkeys. Any unmapped Ctrl+letter is swallowed
	// (consumed, no insert) so it doesn't appear as a literal in the
	// buffer. This also reserves space for future additions (e.g. Ctrl+R).
	if (ctrl_down && key >= 'a' && key <= 'z')
	{
		switch (key)
		{
		case 'a': LE_BeginningOfLine (&console_le); break;
		case 'e': LE_EndOfLine       (&console_le); break;
		case 'b': LE_MoveLeft        (&console_le); break;
		case 'f': LE_MoveRight       (&console_le); break;
		case 'p': Key_HistoryPrev    (); break;
		case 'n': Key_HistoryNext    (); break;
		case 'k': LE_KillToEnd       (&console_le); break;
		case 'u': LE_KillToStart     (&console_le); break;
		case 'w': LE_KillPrevWord    (&console_le); break;
		case 'l': Cbuf_AddText       ("clear\n"); break;
		case 'h': LE_DeleteBack      (&console_le); break;
		case 'd':
			// No-op on empty line (don't close console; that's a future
			// decision if we want bash-like exit behavior).
			if (console_le.len > 0)
				LE_DeleteForward (&console_le);
			break;
		default:
			// Swallow other Ctrl+letter combos.
			break;
		}
		return;
	}
```

- [ ] **Step 2: Build**

Run: `zig build`
Expected: build succeeds.

- [ ] **Step 3: Smoke test — full readline hotkey set**

Run: `zig build run -- +map e1m1`

Open console (`~`). Type `bind w +forward` (with a trailing space, leave the cursor at end). Then:

| Hotkey | Expected |
|---|---|
| `Ctrl+A` | cursor jumps to start (just after `]`) |
| `Ctrl+E` | cursor jumps to end |
| `Ctrl+B` | cursor moves left one char |
| `Ctrl+F` | cursor moves right one char |
| `Ctrl+H` | deletes char before cursor (= backspace) |
| `Ctrl+D` (with content) | deletes char at cursor |
| `Ctrl+D` (with empty buffer) | nothing happens |
| `Ctrl+K` (cursor mid-line) | line truncated at cursor |
| `Ctrl+U` (cursor mid-line) | text before cursor removed, cursor at 0 |
| `Ctrl+W` (cursor at end) | last word killed (whitespace-delimited) |
| `Ctrl+L` | scrollback cleared, current input line preserved |
| `Ctrl+P` | history prev (same as UP) |
| `Ctrl+N` | history next (same as DOWN) |
| `Ctrl+R` | nothing happens (swallowed, no insert) — confirms unmapped Ctrl+letter is correctly consumed |

Also verify regressions are absent:
- Non-Ctrl typing still works.
- LEFT/RIGHT/HOME/END/DEL still work.
- Tab cycling still works.
- Enter commits, history.txt updates.

- [ ] **Step 4: Commit**

```bash
git add sdlquake/engine_src/keys.c
git commit -m "feat(console): bash-style readline hotkeys"
```

- [ ] **Step 5: Push**

Per project convention (no PR flow), push directly to master:

```bash
git push
```

---

## Done

After the three tasks, the console behaves like a bash readline shell within the limits of the spec (no `Ctrl+R`, no word-motion, no kill ring). Update `ideas.md` "Next 5" to reflect — strike or rewrite item 5 (`console: CTRL+R search + readline hotkeys`) to leave only `CTRL+R search` as remaining work. This is optional cleanup, not a step.

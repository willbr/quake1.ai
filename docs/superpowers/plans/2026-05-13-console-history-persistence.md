# Console History Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist the console command history (UpArrow recall) to disk so it survives engine restarts.

**Architecture:** Grow the existing in-memory ring buffer from 32 → 128 commands. Save the ring (text, oldest-first, no `]` prefix) to `<com_gamedir>/history.txt` on every Enter and at clean shutdown. Load the file at the end of `Key_Init`, populating the ring and positioning `edit_line` past the loaded data so the player has a clean prompt with full UpArrow access to prior commands. Suppress empty entries and consecutive duplicates.

**Tech Stack:** C89 (engine_src is built `-std=gnu89 -fcommon`), Zig 0.16 build system, SDL3 platform layer. No test suite — verification is manual smoke testing inside the game (`zig build run`).

**Spec:** `docs/superpowers/specs/2026-05-13-console-history-persistence-design.md`

---

## File Structure

| File | Role | Why touched |
|---|---|---|
| `sdlquake/engine_src/keys.h` | Public key/console interface | Add `CMDLINES` / `CMDLINES_MASK` macros + two new prototypes (`Key_LoadHistory`, `Key_SaveHistory`) |
| `sdlquake/engine_src/keys.c` | Ring buffer + line editor + history I/O | Resize ring, add load/save functions, restructure K_ENTER branch (empty / dedup / new-entry), call `Key_LoadHistory` from `Key_Init` |
| `sdlquake/engine_src/console.c` | Console rendering — externs the ring | Update the `extern` to use `CMDLINES` |
| `sdlquake/engine_src/host.c` | Shutdown / config persistence | One `Key_SaveHistory()` call at the tail of `Host_WriteConfiguration` |

No platform-layer, game-DLL, or build-system changes. `MAXCMDLINE` stays duplicated in `keys.c` and `console.c` as it is today — out of scope to consolidate.

---

## Task 1: Resize the ring buffer from 32 to 128

**Files:**
- Modify: `sdlquake/engine_src/keys.h` (add macros at the bottom, before existing prototypes)
- Modify: `sdlquake/engine_src/keys.c:30, 168, 205, 209, 220, 524`
- Modify: `sdlquake/engine_src/console.c:56`

- [ ] **Step 1: Add `CMDLINES` macros to `keys.h`**

Edit `sdlquake/engine_src/keys.h`. Insert these lines just before `typedef enum {key_game, ...}` on line 120 (i.e. after the `K_MWHEELDOWN` block, line 117):

```c
// Console command-history ring buffer size. Must be a power of 2 — the
// existing edit_line / history_line arithmetic uses `& CMDLINES_MASK`.
#define CMDLINES        128
#define CMDLINES_MASK   (CMDLINES - 1)

```

- [ ] **Step 2: Update the ring declaration in `keys.c`**

In `sdlquake/engine_src/keys.c:30`, change:

```c
char	key_lines[32][MAXCMDLINE];
```

to:

```c
char	key_lines[CMDLINES][MAXCMDLINE];
```

- [ ] **Step 3: Update ring arithmetic in `Key_Console`**

In `sdlquake/engine_src/keys.c`, replace every `& 31` and `(... + 1) & 31` with `& CMDLINES_MASK`:

- Line 168: `edit_line = (edit_line + 1) & 31;` → `edit_line = (edit_line + 1) & CMDLINES_MASK;`
- Line 205: `history_line = (history_line - 1) & 31;` → `history_line = (history_line - 1) & CMDLINES_MASK;`
- Line 209: `history_line = (edit_line+1)&31;` → `history_line = (edit_line+1) & CMDLINES_MASK;`
- Line 220: `history_line = (history_line + 1) & 31;` → `history_line = (history_line + 1) & CMDLINES_MASK;`

- [ ] **Step 4: Update the init loop in `Key_Init`**

In `sdlquake/engine_src/keys.c:524`, change:

```c
	for (i=0 ; i<32 ; i++)
```

to:

```c
	for (i=0 ; i<CMDLINES ; i++)
```

- [ ] **Step 5: Update the extern in `console.c`**

In `sdlquake/engine_src/console.c:56`, change:

```c
extern	char	key_lines[32][MAXCMDLINE];
```

to:

```c
extern	char	key_lines[CMDLINES][MAXCMDLINE];
```

`CMDLINES` reaches `console.c` because `console.c` includes `quakedef.h` which includes `keys.h`.

- [ ] **Step 6: Build**

Run:
```sh
zig build
```

Expected: clean compile, no warnings about `CMDLINES` undefined or array size mismatches.

- [ ] **Step 7: Smoke test — confirm console still works**

Run:
```sh
zig build run -- +map e1m1
```

In-game:
1. Press `~` to open the console.
2. Type `echo one` <Enter>, `echo two` <Enter>, `echo three` <Enter>.
3. Press UpArrow three times — verify `echo three`, `echo two`, `echo one` appear in order.
4. Press DownArrow — verify the empty prompt returns.
5. Quit (`quit` or close window).

Expected: identical behavior to pre-change — the buffer is just bigger but no other behavior has changed.

- [ ] **Step 8: Commit**

```sh
git add sdlquake/engine_src/keys.h sdlquake/engine_src/keys.c sdlquake/engine_src/console.c
git commit -m "$(cat <<'EOF'
refactor(keys): parameterize console history ring as CMDLINES (128)

Replace hard-coded 32-slot ring + `& 31` mask with CMDLINES/CMDLINES_MASK
macros in keys.h. Prep for history persistence; no behavior change.
EOF
)"
```

---

## Task 2: Add `Key_LoadHistory` and `Key_SaveHistory` (definitions only)

**Files:**
- Modify: `sdlquake/engine_src/keys.h` (two new prototypes)
- Modify: `sdlquake/engine_src/keys.c` (two new function bodies, just after `Key_WriteBindings`)

This task adds the functions but does not call them yet, so behavior is unchanged.

- [ ] **Step 1: Add prototypes to `keys.h`**

In `sdlquake/engine_src/keys.h`, add to the prototype block at the bottom (after `void Key_ClearStates (void);`):

```c
void Key_LoadHistory (void);
void Key_SaveHistory (void);
```

- [ ] **Step 2: Add `Key_LoadHistory` to `keys.c`**

In `sdlquake/engine_src/keys.c`, insert the following function immediately before `Key_Init` (i.e. before line 515, the `===================` / `Key_Init` comment block):

```c
/*
===================
Key_LoadHistory

Loads <com_gamedir>/history.txt into the key_lines ring, oldest entry first.
Each non-empty line is stored with a ']' prefix. After load, edit_line is
positioned past the last loaded entry with a clean prompt; UpArrow walks
back through the restored history. Silent no-op if the file is absent.
===================
*/
void Key_LoadHistory (void)
{
	FILE	*f;
	char	line[MAXCMDLINE];
	int		n, len;

	f = fopen (va("%s/history.txt", com_gamedir), "r");
	if (!f)
		return;

	n = 0;
	while (n < CMDLINES - 1 && fgets(line, sizeof(line), f))
	{
		// Strip trailing newline / CR.
		len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = 0;
		if (len == 0)
			continue;
		// Reserve byte 0 for ']' and one byte for NUL.
		if (len > MAXCMDLINE - 2)
		{
			line[MAXCMDLINE - 2] = 0;
			len = MAXCMDLINE - 2;
		}
		key_lines[n][0] = ']';
		memcpy (&key_lines[n][1], line, (size_t)len + 1);
		n++;
	}
	fclose (f);

	edit_line = n & CMDLINES_MASK;
	history_line = edit_line;
	key_lines[edit_line][0] = ']';
	key_lines[edit_line][1] = 0;
	key_linepos = 1;
}

```

- [ ] **Step 3: Add `Key_SaveHistory` to `keys.c`**

Immediately after `Key_LoadHistory` (still before `Key_Init`), insert:

```c
/*
===================
Key_SaveHistory

Rewrites <com_gamedir>/history.txt from the ring buffer, oldest first.
Skips the current edit slot (a live prompt, not a committed command) and
any empty slots. Bounded by CMDLINES * MAXCMDLINE (~32 KB) so the
truncate-and-rewrite-per-Enter cost is negligible.
===================
*/
void Key_SaveHistory (void)
{
	FILE	*f;
	int		i;

	if (!host_initialized || isDedicated)
		return;

	f = fopen (va("%s/history.txt", com_gamedir), "w");
	if (!f)
	{
		Con_Printf ("Couldn't write history.txt.\n");
		return;
	}

	i = (edit_line + 1) & CMDLINES_MASK;
	while (i != edit_line)
	{
		if (key_lines[i][1])
			fprintf (f, "%s\n", key_lines[i] + 1);
		i = (i + 1) & CMDLINES_MASK;
	}

	fclose (f);
}

```

- [ ] **Step 4: Build**

Run:
```sh
zig build
```

Expected: clean compile. The new functions are defined but unused — gnu89 / `-fcommon` does not warn on unused static-storage functions, so this should produce no warnings.

- [ ] **Step 5: Smoke test — confirm engine still launches**

Run:
```sh
zig build run -- +map e1m1
```

Expected: game boots normally, console works as before. The new functions are not wired in yet — this is just a build sanity check.

Quit cleanly.

- [ ] **Step 6: Commit**

```sh
git add sdlquake/engine_src/keys.h sdlquake/engine_src/keys.c
git commit -m "$(cat <<'EOF'
feat(keys): add Key_LoadHistory / Key_SaveHistory (unwired)

Read/write <com_gamedir>/history.txt as a plain-text command list, oldest
first, no ']' prefix. Truncates over-long lines on load. Save skips the
live edit slot and empty slots. No call sites yet — wired in the next
commits.
EOF
)"
```

---

## Task 3: Wire load on startup + restructure K_ENTER (empty / dedup / save)

**Files:**
- Modify: `sdlquake/engine_src/keys.c` (call site in `Key_Init`, rework K_ENTER branch in `Key_Console`)

- [ ] **Step 1: Call `Key_LoadHistory` at end of `Key_Init`**

In `sdlquake/engine_src/keys.c`, the original `Key_Init` ends after the consolekeys/keyshift init blocks. Find the closing `}` of `Key_Init`. Insert this immediately before the closing brace:

```c

	Key_LoadHistory ();
```

(Use Grep / Read first to confirm the exact line — the bottom of `Key_Init` should be a final `keyshift[...]` assignment or similar; the new call goes after all of those and before the function's terminal `}`.)

- [ ] **Step 2: Rework the K_ENTER branch in `Key_Console`**

The current branch (`sdlquake/engine_src/keys.c:163-176`) looks like:

```c
	if (key == K_ENTER)
	{
		Cbuf_AddText (key_lines[edit_line]+1);	// skip the >
		Cbuf_AddText ("\n");
		Con_Printf ("%s\n",key_lines[edit_line]);
		edit_line = (edit_line + 1) & CMDLINES_MASK;
		history_line = edit_line;
		key_lines[edit_line][0] = ']';
		key_linepos = 1;
		if (cls.state == ca_disconnected)
			SCR_UpdateScreen ();	// force an update, because the command
									// may take some time
		return;
	}
```

Replace it with:

```c
	if (key == K_ENTER)
	{
		int prev;

		Cbuf_AddText (key_lines[edit_line]+1);	// skip the >
		Cbuf_AddText ("\n");
		Con_Printf ("%s\n",key_lines[edit_line]);

		// Empty line: don't advance the ring, don't save.
		if (!key_lines[edit_line][1])
		{
			key_linepos = 1;
			if (cls.state == ca_disconnected)
				SCR_UpdateScreen ();
			return;
		}

		// Duplicate of the immediately previous command: keep the previous
		// entry as the most recent, clear the just-typed slot, and skip the
		// save (file already reflects this state).
		prev = (edit_line - 1) & CMDLINES_MASK;
		if (key_lines[prev][1]
			&& !Q_strcmp (key_lines[edit_line]+1, key_lines[prev]+1))
		{
			key_lines[edit_line][1] = 0;
			key_linepos = 1;
			history_line = edit_line;
			if (cls.state == ca_disconnected)
				SCR_UpdateScreen ();
			return;
		}

		// New entry: advance ring and persist.
		edit_line = (edit_line + 1) & CMDLINES_MASK;
		history_line = edit_line;
		key_lines[edit_line][0] = ']';
		key_lines[edit_line][1] = 0;
		key_linepos = 1;
		Key_SaveHistory ();
		if (cls.state == ca_disconnected)
			SCR_UpdateScreen ();	// force an update, because the command
									// may take some time
		return;
	}
```

Three things to note:
- `Q_strcmp` is the engine's strcmp wrapper, already used throughout `keys.c`.
- In the new-entry path we now explicitly NUL-terminate the next slot (`[edit_line][1] = 0`). The original code only set `[0] = ']'`, relying on the slot being clean from prior `Key_Init` zeroing. After loading history we cannot assume that.
- The duplicate-suppression path clears `[edit_line][1]` so the just-typed text is gone, then sets `history_line = edit_line` so UpArrow starts from the freshly-cleared slot (same behavior as the new-entry path) and walks back to find the (now most-recent) duplicate.

- [ ] **Step 3: Build**

Run:
```sh
zig build
```

Expected: clean compile.

- [ ] **Step 4: Smoke test — round-trip restart**

Run:
```sh
zig build run -- +map e1m1
```

In-game:
1. Open console (`~`).
2. Type `echo alpha` <Enter>, `echo beta` <Enter>, `echo gamma` <Enter>.
3. Type `echo gamma` <Enter> again — this is a duplicate.
4. Press <Enter> on an empty prompt — this is an empty line.
5. Type `echo delta` <Enter>.
6. Press UpArrow repeatedly — confirm the order is `delta`, `gamma`, `beta`, `alpha`. The duplicate `gamma` and the empty line must NOT appear as separate entries.
7. Type `quit` <Enter>.

Inspect the file:
```sh
type id1\history.txt
```

Expected contents (exact, oldest first):
```
echo alpha
echo beta
echo gamma
echo delta
```

Now relaunch:
```sh
zig build run -- +map e1m1
```

Open the console. UpArrow four times. Expected order: `echo delta`, `echo gamma`, `echo beta`, `echo alpha`. Press DownArrow until you return to the empty `]` prompt. Verify the prompt is clean (no stale text).

Quit.

- [ ] **Step 5: Smoke test — missing-file case**

Delete the history file:
```sh
del id1\history.txt
```

Relaunch:
```sh
zig build run -- +map e1m1
```

Open the console. UpArrow — expected: nothing happens, prompt stays empty (or shows the empty `]` ring slot we just cleared). No error in the console output.

Quit.

- [ ] **Step 6: Commit**

```sh
git add sdlquake/engine_src/keys.c
git commit -m "$(cat <<'EOF'
feat(console): persist history across sessions

Key_Init now loads <com_gamedir>/history.txt and the K_ENTER branch
saves after each new committed command. Empty Enters and consecutive
duplicates are suppressed (bash-style ignoredups) so they don't pollute
the ring or the file.
EOF
)"
```

---

## Task 4: Save on clean shutdown

**Files:**
- Modify: `sdlquake/engine_src/host.c:261-281` (tail of `Host_WriteConfiguration`)

The per-Enter save already keeps the file current. This task adds belt-and-braces save at clean shutdown for any future code path that might mutate the ring without going through Enter.

- [ ] **Step 1: Hook `Key_SaveHistory` into `Host_WriteConfiguration`**

In `sdlquake/engine_src/host.c`, the current function ends like this (line 267-281):

```c
	if (host_initialized & !isDedicated)
	{
		f = fopen (va("%s/config.cfg",com_gamedir), "w");
		if (!f)
		{
			Con_Printf ("Couldn't write config.cfg.\n");
			return;
		}
		
		Key_WriteBindings (f);
		Cvar_WriteVariables (f);

		fclose (f);
	}
}
```

The early `return` inside the `if (!f)` branch means we'd skip the history save on a config-write failure. Restructure to always save history if we got past the dedicated/initialized guard. Replace lines 267-280 (the body of the `if (host_initialized & !isDedicated)` block) with:

```c
	if (host_initialized & !isDedicated)
	{
		f = fopen (va("%s/config.cfg",com_gamedir), "w");
		if (f)
		{
			Key_WriteBindings (f);
			Cvar_WriteVariables (f);
			fclose (f);
		}
		else
		{
			Con_Printf ("Couldn't write config.cfg.\n");
		}

		Key_SaveHistory ();
	}
```

This preserves the original "warn on config.cfg failure" behavior while ensuring history saves regardless. Both writes share the `host_initialized & !isDedicated` guard.

- [ ] **Step 2: Build**

Run:
```sh
zig build
```

Expected: clean compile.

- [ ] **Step 3: Smoke test — shutdown writes file**

Delete the history file to start clean:
```sh
del id1\history.txt
```

Run:
```sh
zig build run -- +map e1m1
```

Open the console. Type `echo zulu` <Enter>. Type `quit` <Enter>.

Inspect:
```sh
type id1\history.txt
```

Expected: file exists, contains `echo zulu` followed by a newline. (Per-Enter save would already have produced this, so the shutdown save is a no-op overwrite — that's the intended outcome.)

- [ ] **Step 4: Smoke test — full spec verification**

Run the full verification list from the spec (`docs/superpowers/specs/2026-05-13-console-history-persistence-design.md` § Verification):

1. `zig build run -- +map e1m1`.
2. Open console (`~`), enter several commands.
3. Press Enter on a duplicate — verify it does not double-list when UpArrowing.
4. Quit cleanly. Confirm `id1/history.txt` exists, oldest-first, no `]` prefixes.
5. Relaunch. UpArrow — verify previous commands appear in reverse-chronological order.
6. Confirm the `]` prompt is clean on launch.
7. Delete `id1/history.txt`, relaunch — silent empty-history start.

All steps must pass.

- [ ] **Step 5: Commit**

```sh
git add sdlquake/engine_src/host.c
git commit -m "$(cat <<'EOF'
feat(host): save console history at clean shutdown

Host_WriteConfiguration now calls Key_SaveHistory alongside the config.cfg
write. The per-Enter save already keeps the file current; this is a
belt-and-braces hook for any future code that mutates the ring outside
the Enter path.
EOF
)"
```

---

## Self-Review Notes

- **Spec coverage:** Buffer growth (Task 1), file format + load (Task 2 functions + Task 3 hook), save-on-Enter (Task 3), save-on-shutdown (Task 4), dedup / empty-line suppression (Task 3 K_ENTER restructure). All four spec sections (Buffer size, File format, Key_LoadHistory, Key_SaveHistory, Dedup) and all four hook rows in the Hooks table map to a task.
- **Placeholder scan:** No "TODO", "TBD", "implement appropriate X". All code blocks are concrete.
- **Type consistency:** `Key_LoadHistory`, `Key_SaveHistory`, `CMDLINES`, `CMDLINES_MASK` spelled identically across header and call sites. `Q_strcmp` is the existing wrapper used elsewhere in `keys.c`.
- **Out of scope (not tasks):** No cvar toggle, no atomic rename, no migration. These are explicitly listed under "What is explicitly out of scope" in the spec.

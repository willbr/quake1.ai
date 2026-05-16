/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "quakedef.h"
#include "line_editor.h"

/*

key up events are sent even if in console mode

*/


char	key_lines[CMDLINES][MAXCMDLINE];
int		key_linepos;
int		shift_down=false;
int		ctrl_down=false;
int		key_lastpress;

int		edit_line=0;
int		history_line=0;

line_editor_t console_le;

keydest_t	key_dest;

int		key_count;			// incremented every key event

char	*keybindings[256];
qboolean	consolekeys[256];	// if true, can't be rebound while in console
qboolean	menubound[256];	// if true, can't be rebound while in menu
int		keyshift[256];		// key to map to if shift held down in console
int		key_repeats[256];	// if > 1, it is autorepeating
qboolean	keydown[256];

typedef struct
{
	char	*name;
	int		keynum;
} keyname_t;

keyname_t keynames[] =
{
	{"TAB", K_TAB},
	{"ENTER", K_ENTER},
	{"ESCAPE", K_ESCAPE},
	{"SPACE", K_SPACE},
	{"BACKSPACE", K_BACKSPACE},
	{"UPARROW", K_UPARROW},
	{"DOWNARROW", K_DOWNARROW},
	{"LEFTARROW", K_LEFTARROW},
	{"RIGHTARROW", K_RIGHTARROW},

	{"ALT", K_ALT},
	{"CTRL", K_CTRL},
	{"SHIFT", K_SHIFT},
	
	{"F1", K_F1},
	{"F2", K_F2},
	{"F3", K_F3},
	{"F4", K_F4},
	{"F5", K_F5},
	{"F6", K_F6},
	{"F7", K_F7},
	{"F8", K_F8},
	{"F9", K_F9},
	{"F10", K_F10},
	{"F11", K_F11},
	{"F12", K_F12},

	{"INS", K_INS},
	{"DEL", K_DEL},
	{"PGDN", K_PGDN},
	{"PGUP", K_PGUP},
	{"HOME", K_HOME},
	{"END", K_END},

	{"MOUSE1", K_MOUSE1},
	{"MOUSE2", K_MOUSE2},
	{"MOUSE3", K_MOUSE3},

	{"JOY1", K_JOY1},
	{"JOY2", K_JOY2},
	{"JOY3", K_JOY3},
	{"JOY4", K_JOY4},

	{"AUX1", K_AUX1},
	{"AUX2", K_AUX2},
	{"AUX3", K_AUX3},
	{"AUX4", K_AUX4},
	{"AUX5", K_AUX5},
	{"AUX6", K_AUX6},
	{"AUX7", K_AUX7},
	{"AUX8", K_AUX8},
	{"AUX9", K_AUX9},
	{"AUX10", K_AUX10},
	{"AUX11", K_AUX11},
	{"AUX12", K_AUX12},
	{"AUX13", K_AUX13},
	{"AUX14", K_AUX14},
	{"AUX15", K_AUX15},
	{"AUX16", K_AUX16},
	{"AUX17", K_AUX17},
	{"AUX18", K_AUX18},
	{"AUX19", K_AUX19},
	{"AUX20", K_AUX20},
	{"AUX21", K_AUX21},
	{"AUX22", K_AUX22},
	{"AUX23", K_AUX23},
	{"AUX24", K_AUX24},
	{"AUX25", K_AUX25},
	{"AUX26", K_AUX26},
	{"AUX27", K_AUX27},
	{"AUX28", K_AUX28},
	{"AUX29", K_AUX29},
	{"AUX30", K_AUX30},
	{"AUX31", K_AUX31},
	{"AUX32", K_AUX32},

	{"PAUSE", K_PAUSE},

	{"MWHEELUP", K_MWHEELUP},
	{"MWHEELDOWN", K_MWHEELDOWN},

	{"SEMICOLON", ';'},	// because a raw semicolon seperates commands

	{NULL,0}
};

/*
==============================================================================

			LINE TYPING INTO THE CONSOLE

==============================================================================
*/


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
static int   tab_match_count;			/* 0 => no cycle in progress */
static int   tab_index;					/* -1 => partial restored, 0..count-1 => match N */
static char  tab_committed_line[LE_MAX_LINE];
/* When prefix_len > 0, the cycle is in argument-completion mode: the
 * editor buffer is "<prefix><arg-partial>", tab_partial holds just the
 * arg-partial, and committed matches are written as "<prefix><match>"
 * (no trailing space). When 0, it's the original whole-line cycle over
 * cmd/cvar/alias names. */
static char  tab_prefix[LE_MAX_LINE];
static int   tab_prefix_len;

/* Split the editor buffer into "<cmdname> <arg-partial>" if applicable.
 * Sets *prefix_out (incl. trailing space[s]), *prefix_len_out, and
 * *arg_partial_out (pointer into console_le.buf). Returns the registered
 * arg-completer for cmdname, or NULL if the line isn't in that form. */
static arg_completer_t Key_LookupArgCompleter (
	const char **arg_partial_out, char *prefix_out, int prefix_max, int *prefix_len_out)
{
	char *buf = console_le.buf;
	int   len = console_le.len;
	int   i, word_end;
	char  cmdname[64];
	arg_completer_t fn;

	/* First word ends at the first space. */
	for (i = 0 ; i < len && buf[i] != ' ' ; i++)
		;
	word_end = i;
	if (word_end == 0 || word_end >= len)
		return NULL;		/* no space, or line is just "<word>" with no space */
	if (word_end >= (int)sizeof(cmdname))
		return NULL;
	memcpy (cmdname, buf, word_end);
	cmdname[word_end] = 0;

	fn = Cmd_FindArgCompleter (cmdname);
	if (!fn)
		return NULL;

	/* Skip any extra spaces so the prefix matches what the user typed. */
	for (i = word_end ; i < len && buf[i] == ' ' ; i++)
		;
	if (i >= prefix_max)
		return NULL;
	memcpy (prefix_out, buf, i);
	prefix_out[i] = 0;
	*prefix_len_out = i;
	*arg_partial_out = buf + i;
	return fn;
}

static void Key_TabComplete (qboolean reverse)
{
	qboolean cycling;
	int		i, j;

	cycling = (tab_match_count > 0)
		&& !Q_strcmp (console_le.buf, tab_committed_line);

	if (!cycling)
	{
		const char		*partial;
		int				partial_len;
		int				n, dedup_count;
		arg_completer_t	arg_fn;
		char			prefix[LE_MAX_LINE];
		int				prefix_len = 0;

		tab_match_count = 0;
		tab_partial_len = 0;
		tab_prefix_len  = 0;
		tab_prefix[0]   = 0;
		if (console_le.len == 0)
			return;

		arg_fn = Key_LookupArgCompleter (&partial, prefix, sizeof(prefix), &prefix_len);
		if (arg_fn)
		{
			partial_len = Q_strlen ((char *)partial);
			if (partial_len == 0)
				return;					/* empty arg-partial -> no completion */
			Q_strcpy (tab_partial, (char *)partial);
			tab_partial_len = partial_len;
			Q_strcpy (tab_prefix, prefix);
			tab_prefix_len = prefix_len;

			n = arg_fn (partial, tab_matches, TAB_MAX_MATCHES, 0);
		}
		else
		{
			partial = console_le.buf;
			partial_len = console_le.len;

			Q_strcpy (tab_partial, (char *)partial);
			tab_partial_len = partial_len;

			n = 0;
			n = Cmd_CompleteCommandAll   ((char *)tab_partial, tab_matches, TAB_MAX_MATCHES, n);
			n = Cvar_CompleteVariableAll ((char *)tab_partial, tab_matches, TAB_MAX_MATCHES, n);
			n = Cmd_CompleteAliasAll     ((char *)tab_partial, tab_matches, TAB_MAX_MATCHES, n);
		}

		if (n == TAB_MAX_MATCHES)
			Con_Printf ("tab completion: %d match limit reached, truncating\n", TAB_MAX_MATCHES);

		/* Dedup in place. n is small in practice (typically < 30), so O(n^2)
		 * is fine. Aliases share a namespace with cmds/cvars and may collide. */
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

	if (tab_prefix_len > 0)
	{
		/* Argument-completion mode: write "<prefix><match>" (no trailing
		 * space), or restore "<prefix><partial>" on the wrap-to-partial
		 * step. LE_SetText caps at LE_MAX_LINE-1 internally. */
		char line[LE_MAX_LINE];
		const char *suffix = (tab_index == -1) ? tab_partial : tab_matches[tab_index];
		snprintf (line, sizeof(line), "%s%s", tab_prefix, suffix);
		LE_SetText (&console_le, line);
	}
	else if (tab_index == -1)
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
	/* Ctrl+letter readline hotkeys. Any unmapped Ctrl+letter is swallowed
	   (consumed, no insert) so it doesn't appear as a literal in the
	   buffer. This also reserves space for future additions (e.g. Ctrl+R). */
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
			/* No-op on empty line (don't close console; that's a future
			   decision if we want bash-like exit behavior). */
			if (console_le.len > 0)
				LE_DeleteForward (&console_le);
			break;
		default:
			/* Swallow other Ctrl+letter combos. */
			break;
		}
		return;
	}

	if (key == K_ENTER)
	{
		int prev;

		/* Commit editor contents into the ring slot. console_le.buf is
		 * capped at MAXCMDLINE-2 so ']' + content + NUL fits. */
		key_lines[edit_line][0] = ']';
		Q_strcpy (key_lines[edit_line] + 1, console_le.buf);

		Cbuf_AddText (console_le.buf);
		Cbuf_AddText ("\n");
		Con_Printf ("]%s\n", console_le.buf);

		/* Empty line: don't advance the ring, don't save. */
		if (!console_le.buf[0])
		{
			LE_Reset (&console_le);
			if (cls.state == ca_disconnected)
				SCR_UpdateScreen ();
			return;
		}

		/* Duplicate of the immediately previous command: keep the previous
		 * entry as the most recent, clear the just-typed slot, and skip the
		 * save (file already reflects this state). */
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

		/* New entry: advance ring and persist. */
		edit_line = (edit_line + 1) & CMDLINES_MASK;
		history_line = edit_line;
		key_lines[edit_line][0] = ']';
		key_lines[edit_line][1] = 0;
		LE_Reset (&console_le);
		Key_SaveHistory ();
		if (cls.state == ca_disconnected)
			SCR_UpdateScreen ();	/* force an update, because the command
									 * may take some time */
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
		return;	/* non printable */

	LE_InsertChar (&console_le, key);
}

//============================================================================

char chat_buffer[32];
qboolean team_message = false;

void Key_Message (int key)
{
	static int chat_bufferlen = 0;

	if (key == K_ENTER)
	{
		if (team_message)
			Cbuf_AddText ("say_team \"");
		else
			Cbuf_AddText ("say \"");
		Cbuf_AddText(chat_buffer);
		Cbuf_AddText("\"\n");

		key_dest = key_game;
		chat_bufferlen = 0;
		chat_buffer[0] = 0;
		return;
	}

	if (key == K_ESCAPE)
	{
		key_dest = key_game;
		chat_bufferlen = 0;
		chat_buffer[0] = 0;
		return;
	}

	if (key < 32 || key > 127)
		return;	// non printable

	if (key == K_BACKSPACE)
	{
		if (chat_bufferlen)
		{
			chat_bufferlen--;
			chat_buffer[chat_bufferlen] = 0;
		}
		return;
	}

	if (chat_bufferlen == 31)
		return; // all full

	chat_buffer[chat_bufferlen++] = key;
	chat_buffer[chat_bufferlen] = 0;
}

//============================================================================


/*
===================
Key_StringToKeynum

Returns a key number to be used to index keybindings[] by looking at
the given string.  Single ascii characters return themselves, while
the K_* names are matched up.
===================
*/
int Key_StringToKeynum (char *str)
{
	keyname_t	*kn;
	
	if (!str || !str[0])
		return -1;
	if (!str[1])
		return str[0];

	for (kn=keynames ; kn->name ; kn++)
	{
		if (!Q_strcasecmp(str,kn->name))
			return kn->keynum;
	}
	return -1;
}

/*
===================
Key_KeynumToString

Returns a string (either a single ascii char, or a K_* name) for the
given keynum.
FIXME: handle quote special (general escape sequence?)
===================
*/
char *Key_KeynumToString (int keynum)
{
	keyname_t	*kn;	
	static	char	tinystr[2];
	
	if (keynum == -1)
		return "<KEY NOT FOUND>";
	if (keynum > 32 && keynum < 127)
	{	// printable ascii
		tinystr[0] = keynum;
		tinystr[1] = 0;
		return tinystr;
	}
	
	for (kn=keynames ; kn->name ; kn++)
		if (keynum == kn->keynum)
			return kn->name;

	return "<UNKNOWN KEYNUM>";
}


/*
===================
Key_SetBinding
===================
*/
void Key_SetBinding (int keynum, char *binding)
{
	char	*new;
	int		l;
			
	if (keynum == -1)
		return;

// free old bindings
	if (keybindings[keynum])
	{
		Z_Free (keybindings[keynum]);
		keybindings[keynum] = NULL;
	}
			
// allocate memory for new binding
	l = Q_strlen (binding);	
	new = Z_Malloc (l+1);
	Q_strcpy (new, binding);
	new[l] = 0;
	keybindings[keynum] = new;	
}

/*
===================
Key_Unbind_f
===================
*/
void Key_Unbind_f (void)
{
	int		b;

	if (Cmd_Argc() != 2)
	{
		Con_Printf ("unbind <key> : remove commands from a key\n");
		return;
	}
	
	b = Key_StringToKeynum (Cmd_Argv(1));
	if (b==-1)
	{
		Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	Key_SetBinding (b, "");
}

void Key_Unbindall_f (void)
{
	int		i;
	
	for (i=0 ; i<256 ; i++)
		if (keybindings[i])
			Key_SetBinding (i, "");
}


/*
===================
Key_Bind_f
===================
*/
void Key_Bind_f (void)
{
	int			i, c, b;
	char		cmd[1024];
	
	c = Cmd_Argc();

	if (c != 2 && c != 3)
	{
		Con_Printf ("bind <key> [command] : attach a command to a key\n");
		return;
	}
	b = Key_StringToKeynum (Cmd_Argv(1));
	if (b==-1)
	{
		Con_Printf ("\"%s\" isn't a valid key\n", Cmd_Argv(1));
		return;
	}

	if (c == 2)
	{
		if (keybindings[b])
			Con_Printf ("\"%s\" = \"%s\"\n", Cmd_Argv(1), keybindings[b] );
		else
			Con_Printf ("\"%s\" is not bound\n", Cmd_Argv(1) );
		return;
	}
	
// copy the rest of the command line
	cmd[0] = 0;		// start out with a null string
	for (i=2 ; i< c ; i++)
	{
		if (i > 2)
			strcat (cmd, " ");
		strcat (cmd, Cmd_Argv(i));
	}

	Key_SetBinding (b, cmd);
}

/*
============
Key_WriteBindings

Writes lines containing "bind key value"
============
*/
void Key_WriteBindings (FILE *f)
{
	int		i;

	for (i=0 ; i<256 ; i++)
		if (keybindings[i])
			if (*keybindings[i])
				fprintf (f, "bind \"%s\" \"%s\"\n", Key_KeynumToString(i), keybindings[i]);
}


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


/*
===================
Key_Init
===================
*/
void Key_Init (void)
{
	int		i;

	for (i=0 ; i<CMDLINES ; i++)
	{
		key_lines[i][0] = ']';
		key_lines[i][1] = 0;
	}
	key_linepos = 1;
	
//
// init ascii characters in console mode
//
	for (i=32 ; i<128 ; i++)
		consolekeys[i] = true;
	consolekeys[K_ENTER] = true;
	consolekeys[K_TAB] = true;
	consolekeys[K_LEFTARROW] = true;
	consolekeys[K_RIGHTARROW] = true;
	consolekeys[K_UPARROW] = true;
	consolekeys[K_DOWNARROW] = true;
	consolekeys[K_BACKSPACE] = true;
	consolekeys[K_DEL] = true;
	consolekeys[K_HOME] = true;
	consolekeys[K_END] = true;
	consolekeys[K_PGUP] = true;
	consolekeys[K_PGDN] = true;
	consolekeys[K_SHIFT] = true;
	consolekeys[K_CTRL] = true;
	consolekeys[K_MWHEELUP] = true;
	consolekeys[K_MWHEELDOWN] = true;
	consolekeys['`'] = false;
	consolekeys['~'] = false;

	for (i=0 ; i<256 ; i++)
		keyshift[i] = i;
	for (i='a' ; i<='z' ; i++)
		keyshift[i] = i - 'a' + 'A';
	keyshift['1'] = '!';
	keyshift['2'] = '@';
	keyshift['3'] = '#';
	keyshift['4'] = '$';
	keyshift['5'] = '%';
	keyshift['6'] = '^';
	keyshift['7'] = '&';
	keyshift['8'] = '*';
	keyshift['9'] = '(';
	keyshift['0'] = ')';
	keyshift['-'] = '_';
	keyshift['='] = '+';
	keyshift[','] = '<';
	keyshift['.'] = '>';
	keyshift['/'] = '?';
	keyshift[';'] = ':';
	keyshift['\''] = '"';
	keyshift['['] = '{';
	keyshift[']'] = '}';
	keyshift['`'] = '~';
	keyshift['\\'] = '|';

	menubound[K_ESCAPE] = true;
	for (i=0 ; i<12 ; i++)
		menubound[K_F1+i] = true;

//
// register our functions
//
	Cmd_AddCommand ("bind",Key_Bind_f);
	Cmd_AddCommand ("unbind",Key_Unbind_f);
	Cmd_AddCommand ("unbindall",Key_Unbindall_f);

	Key_LoadHistory ();
	LE_Reset (&console_le);
}

/*
===================
Key_Event

Called by the system between frames for both key up and key down events
Should NOT be called during an interrupt!
===================
*/
void Key_Event (int key, qboolean down)
{
	char	*kb;
	char	cmd[1024];

	keydown[key] = down;

	if (!down)
		key_repeats[key] = 0;

	key_lastpress = key;
	key_count++;
	if (key_count <= 0)
	{
		return;		// just catching keys for Con_NotifyBox
	}

// update auto-repeat status
	if (down)
	{
		key_repeats[key]++;
		if (key != K_BACKSPACE && key != K_PAUSE && key_repeats[key] > 1)
		{
			return;	// ignore most autorepeats
		}
			
		if (key >= 200 && !keybindings[key])
			Con_Printf ("%s is unbound, hit F4 to set.\n", Key_KeynumToString (key) );
	}

	if (key == K_SHIFT)
		shift_down = down;
	if (key == K_CTRL)
		ctrl_down = down;

//
// handle escape specialy, so the user can never unbind it
//
	if (key == K_ESCAPE)
	{
		if (!down)
			return;
		switch (key_dest)
		{
		case key_message:
			Key_Message (key);
			break;
		case key_menu:
			M_Keydown (key);
			break;
		case key_game:
		case key_console:
			M_ToggleMenu_f ();
			break;
		default:
			Sys_Error ("Bad key_dest");
		}
		return;
	}

//
// key up events only generate commands if the game key binding is
// a button command (leading + sign).  These will occur even in console mode,
// to keep the character from continuing an action started before a console
// switch.  Button commands include the kenum as a parameter, so multiple
// downs can be matched with ups
//
	if (!down)
	{
		kb = keybindings[key];
		if (kb && kb[0] == '+')
		{
			sprintf (cmd, "-%s %i\n", kb+1, key);
			Cbuf_AddText (cmd);
		}
		if (keyshift[key] != key)
		{
			kb = keybindings[keyshift[key]];
			if (kb && kb[0] == '+')
			{
				sprintf (cmd, "-%s %i\n", kb+1, key);
				Cbuf_AddText (cmd);
			}
		}
		return;
	}

//
// during demo playback, most keys bring up the main menu
//
	if (cls.demoplayback && down && consolekeys[key] && key_dest == key_game)
	{
		M_ToggleMenu_f ();
		return;
	}

//
// if not a consolekey, send to the interpreter no matter what mode is
//
	if ( (key_dest == key_menu && menubound[key])
	|| (key_dest == key_console && !consolekeys[key])
	|| (key_dest == key_game && ( !con_forcedup || !consolekeys[key] ) ) )
	{
		kb = keybindings[key];
		if (kb)
		{
			if (kb[0] == '+')
			{	// button commands add keynum as a parm
				sprintf (cmd, "%s %i\n", kb, key);
				Cbuf_AddText (cmd);
			}
			else
			{
				Cbuf_AddText (kb);
				Cbuf_AddText ("\n");
			}
		}
		return;
	}

	if (!down)
		return;		// other systems only care about key down events

	if (shift_down)
	{
		key = keyshift[key];
	}

	switch (key_dest)
	{
	case key_message:
		Key_Message (key);
		break;
	case key_menu:
		M_Keydown (key);
		break;

	case key_game:
	case key_console:
		Key_Console (key);
		break;
	default:
		Sys_Error ("Bad key_dest");
	}
}


/*
===================
Key_ClearStates
===================
*/
void Key_ClearStates (void)
{
	int		i;

	for (i=0 ; i<256 ; i++)
	{
		keydown[i] = false;
		key_repeats[i] = 0;
	}
}


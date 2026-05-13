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
// console.c

#ifdef NeXT
#include <libc.h>
#endif
#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <fcntl.h>
#include "quakedef.h"
#include "line_editor.h"

int 		con_linewidth;

float		con_cursorspeed = 4;

#define		CON_TEXTSIZE	16384

qboolean 	con_forcedup;		// because no entities to refresh

int			con_totallines;		// total lines in console scrollback
int			con_backscroll;		// lines up from bottom to display
int			con_current;		// where next message will be printed
int			con_x;				// offset in current line for next print
char		*con_text=0;

cvar_t		con_notifytime = {"con_notifytime","3"};		//seconds

#define	NUM_CON_TIMES 4
float		con_times[NUM_CON_TIMES];	// realtime time the line was generated
								// for transparent notify lines

int			con_vislines;

qboolean	con_debuglog;

extern	char	key_lines[CMDLINES][MAXCMDLINE];
extern	int		edit_line;
extern	line_editor_t	console_le;


qboolean	con_initialized;

int			con_notifylines;		// scan lines to clear for notify lines

extern void M_Menu_Main_f (void);

/*
================
Con_ToggleConsole_f
================
*/
void Con_ToggleConsole_f (void)
{
	if (key_dest == key_console)
	{
		if (cls.state == ca_connected)
		{
			key_dest = key_game;
			LE_Reset (&console_le);
		}
		else
		{
			M_Menu_Main_f ();
		}
	}
	else
		key_dest = key_console;
	
	SCR_EndLoadingPlaque ();
	memset (con_times, 0, sizeof(con_times));
}

/*
================
Con_Clear_f
================
*/
void Con_Clear_f (void)
{
	if (con_text)
		Q_memset (con_text, ' ', CON_TEXTSIZE);
}

						
/*
================
Con_ClearNotify
================
*/
void Con_ClearNotify (void)
{
	int		i;
	
	for (i=0 ; i<NUM_CON_TIMES ; i++)
		con_times[i] = 0;
}

						
/*
================
Con_MessageMode_f
================
*/
extern qboolean team_message;

void Con_MessageMode_f (void)
{
	key_dest = key_message;
	team_message = false;
}

						
/*
================
Con_MessageMode2_f
================
*/
void Con_MessageMode2_f (void)
{
	key_dest = key_message;
	team_message = true;
}

						
/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void Con_CheckResize (void)
{
	int		i, j, width, oldwidth, oldtotallines, numlines, numchars;
	char	tbuf[CON_TEXTSIZE];

	{
		int cs = vid.height / 200;
		if (cs < 1) cs = 1;
		width = ((vid.width / cs) >> 3) - 2;
	}

	if (width == con_linewidth)
		return;

	if (width < 1)			// video hasn't been initialized yet
	{
		width = 38;
		con_linewidth = width;
		con_totallines = CON_TEXTSIZE / con_linewidth;
		Q_memset (con_text, ' ', CON_TEXTSIZE);
	}
	else
	{
		oldwidth = con_linewidth;
		con_linewidth = width;
		oldtotallines = con_totallines;
		con_totallines = CON_TEXTSIZE / con_linewidth;
		numlines = oldtotallines;

		if (con_totallines < numlines)
			numlines = con_totallines;

		numchars = oldwidth;
	
		if (con_linewidth < numchars)
			numchars = con_linewidth;

		Q_memcpy (tbuf, con_text, CON_TEXTSIZE);
		Q_memset (con_text, ' ', CON_TEXTSIZE);

		for (i=0 ; i<numlines ; i++)
		{
			for (j=0 ; j<numchars ; j++)
			{
				con_text[(con_totallines - 1 - i) * con_linewidth + j] =
						tbuf[((con_current - i + oldtotallines) %
							  oldtotallines) * oldwidth + j];
			}
		}

		Con_ClearNotify ();
	}

	con_backscroll = 0;
	con_current = con_totallines - 1;
}


/*
================
Con_Init
================
*/
void Con_Init (void)
{
#define MAXGAMEDIRLEN	1000
	char	temp[MAXGAMEDIRLEN+1];
	char	*t2 = "/qconsole.log";

	con_debuglog = COM_CheckParm("-condebug");

	if (con_debuglog)
	{
		if (strlen (com_gamedir) < (MAXGAMEDIRLEN - strlen (t2)))
		{
			sprintf (temp, "%s%s", com_gamedir, t2);
			unlink (temp);
		}
	}

	con_text = Hunk_AllocName (CON_TEXTSIZE, "context");
	Q_memset (con_text, ' ', CON_TEXTSIZE);
	con_linewidth = -1;
	Con_CheckResize ();
	
	Con_Printf ("Console initialized.\n");

//
// register our commands
//
	Cvar_RegisterVariable (&con_notifytime);

	Cmd_AddCommand ("toggleconsole", Con_ToggleConsole_f);
	Cmd_AddCommand ("messagemode", Con_MessageMode_f);
	Cmd_AddCommand ("messagemode2", Con_MessageMode2_f);
	Cmd_AddCommand ("clear", Con_Clear_f);
	con_initialized = true;
}


/*
===============
Con_Linefeed
===============
*/
void Con_Linefeed (void)
{
	con_x = 0;
	con_current++;
	Q_memset (&con_text[(con_current%con_totallines)*con_linewidth]
	, ' ', con_linewidth);
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the notify window will pop up.
================
*/
void Con_Print (char *txt)
{
	int		y;
	int		c, l;
	static int	cr;
	int		mask;
	
	con_backscroll = 0;

	if (txt[0] == 1)
	{
		mask = 128;		// go to colored text
		S_LocalSound ("misc/talk.wav");
	// play talk wav
		txt++;
	}
	else if (txt[0] == 2)
	{
		mask = 128;		// go to colored text
		txt++;
	}
	else
		mask = 0;


	while ( (c = *txt) )
	{
	// count word length
		for (l=0 ; l< con_linewidth ; l++)
			if ( txt[l] <= ' ')
				break;

	// word wrap
		if (l != con_linewidth && (con_x + l > con_linewidth) )
			con_x = 0;

		txt++;

		if (cr)
		{
			con_current--;
			cr = false;
		}

		
		if (!con_x)
		{
			Con_Linefeed ();
		// mark time for transparent overlay
			if (con_current >= 0)
				con_times[con_current % NUM_CON_TIMES] = realtime;
		}

		switch (c)
		{
		case '\n':
			con_x = 0;
			break;

		case '\r':
			con_x = 0;
			cr = 1;
			break;

		default:	// display character and advance
			y = con_current % con_totallines;
			con_text[y*con_linewidth+con_x] = c | mask;
			con_x++;
			if (con_x >= con_linewidth)
				con_x = 0;
			break;
		}
		
	}
}


/*
================
Con_DebugLog
================
*/
void Con_DebugLog(char *file, char *fmt, ...)
{
    va_list argptr; 
    static char data[1024];
    int fd;
    
    va_start(argptr, fmt);
    vsprintf(data, fmt, argptr);
    va_end(argptr);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
}


/*
================
Con_Printf

Handles cursor positioning, line wrapping, etc
================
*/
#define	MAXPRINTMSG	4096
// FIXME: make a buffer size safe vsprintf?
void Con_Printf (char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
	static qboolean	inupdate;
	
	va_start (argptr,fmt);
	vsprintf (msg,fmt,argptr);
	va_end (argptr);
	
// also echo to debugging console
	Sys_Printf ("%s", msg);	// also echo to debugging console

// log all messages to file
	if (con_debuglog)
		Con_DebugLog(va("%s/qconsole.log",com_gamedir), "%s", msg);

	if (!con_initialized)
		return;
		
	if (cls.state == ca_dedicated)
		return;		// no graphics mode

// write it to the scrollable buffer
	Con_Print (msg);
	
// update the screen if the console is displayed
	if (cls.signon != SIGNONS && !scr_disabled_for_loading )
	{
	// protect against infinite loop if something in SCR_UpdateScreen calls
	// Con_Printd
		if (!inupdate)
		{
			inupdate = true;
			SCR_UpdateScreen ();
			inupdate = false;
		}
	}
}

/*
================
Con_DPrintf

A Con_Printf that only shows up if the "developer" cvar is set
================
*/
void Con_DPrintf (char *fmt, ...)
{
	va_list		argptr;
	char		msg[MAXPRINTMSG];
		
	if (!developer.value)
		return;			// don't confuse non-developers with techie stuff...

	va_start (argptr,fmt);
	vsprintf (msg,fmt,argptr);
	va_end (argptr);
	
	Con_Printf ("%s", msg);
}


/*
==================
Con_SafePrintf

Okay to call even when the screen can't be updated
==================
*/
void Con_SafePrintf (char *fmt, ...)
{
	va_list		argptr;
	char		msg[1024];
	int			temp;
		
	va_start (argptr,fmt);
	vsprintf (msg,fmt,argptr);
	va_end (argptr);

	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	Con_Printf ("%s", msg);
	scr_disabled_for_loading = temp;
}


/*
==============================================================================

DRAWING

==============================================================================
*/

// Console drawing uses a 320x200 logical character grid (8x8 cells, 38 chars
// per line). Nearest-neighbor scale by vid.height/200 so the text grows
// proportionally with the render resolution.
static int Con_Scale (void)
{
	int s = vid.height / 200;
	return s < 1 ? 1 : s;
}

static void Con_DrawCharScaled (int x, int y, int num, int s)
{
	extern byte *draw_chars;
	num &= 255;
	if (y + 8*s <= 0 || y >= vid.height)
		return;
	int row = num >> 4;
	int col = num & 15;
	byte *source = draw_chars + (row << 10) + (col << 3);
	for (int v = 0; v < 8; v++)
	{
		byte *src_row = source + v * 128;
		for (int yy = 0; yy < s; yy++)
		{
			int sy = y + v*s + yy;
			if (sy < 0)
				continue;
			if (sy >= vid.height)
				return;
			byte *p = (byte *)vid.conbuffer + sy * vid.conrowbytes + x;
			for (int u = 0; u < 8; u++)
			{
				byte c = src_row[u];
				if (!c)
					continue;
				byte *pp = p + u * s;
				for (int xx = 0; xx < s; xx++)
					pp[xx] = c;
			}
		}
	}
}


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
	int		cursor_col;		/* logical column of cursor (0 = prompt slot) */
	int		col_offset;
	int		cursor_glyph;

	if (key_dest != key_console && !con_forcedup)
		return;

	/* Logical text: ']' at col 0, console_le.buf at cols 1..len, ' ' at col len+1.
	 * Cursor sits at col 1 + console_le.cursor. */
	cursor_col = 1 + console_le.cursor;

	/* Horizontal scroll so the cursor stays visible. */
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


/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify (void)
{
	int		x, v;
	char	*text;
	int		i;
	float	time;
	int		s = Con_Scale();
	extern char chat_buffer[];

	v = 0;
	for (i= con_current-NUM_CON_TIMES+1 ; i<=con_current ; i++)
	{
		if (i < 0)
			continue;
		time = con_times[i % NUM_CON_TIMES];
		if (time == 0)
			continue;
		time = realtime - time;
		if (time > con_notifytime.value)
			continue;
		text = con_text + (i % con_totallines)*con_linewidth;

		clearnotify = 0;
		scr_copytop = 1;

		for (x = 0 ; x < con_linewidth ; x++)
			Con_DrawCharScaled ((x+1)*8*s, v, text[x], s);

		v += 8*s;
	}


	if (key_dest == key_message)
	{
		static char say[] = "say:";

		clearnotify = 0;
		scr_copytop = 1;

		for (x = 0 ; say[x] ; x++)
			Con_DrawCharScaled ((x+1)*8*s, v, say[x], s);

		x = 0;
		while (chat_buffer[x])
		{
			Con_DrawCharScaled ((x+5)*8*s, v, chat_buffer[x], s);
			x++;
		}
		Con_DrawCharScaled ((x+5)*8*s, v, 10+((int)(realtime*con_cursorspeed)&1), s);
		v += 8*s;
	}

	if (v > con_notifylines)
		con_notifylines = v;
}

/*
================
Con_DrawConsole

Draws the console with the solid background
The typing input line at the bottom should only be drawn if typing is allowed
================
*/
void Con_DrawConsole (int lines, qboolean drawinput)
{
	int				i, x, y;
	int				rows;
	char			*text;
	int				j;
	int				s = Con_Scale();

	if (lines <= 0)
		return;

// draw the background
	Draw_ConsoleBackground (lines);

// draw the text
	con_vislines = lines;

	rows = (lines - 16*s) / (8*s);		// rows of text to draw
	y = lines - 16*s - rows * 8*s;		// may start slightly negative

	for (i= con_current - rows + 1 ; i<=con_current ; i++, y += 8*s)
	{
		j = i - con_backscroll;
		if (j<0)
			j = 0;
		text = con_text + (j % con_totallines)*con_linewidth;

		for (x=0 ; x<con_linewidth ; x++)
			Con_DrawCharScaled ((x+1)*8*s, y, text[x], s);
	}

// draw the input prompt, user text, and cursor if desired
	if (drawinput)
		Con_DrawInput ();
}


/*
==================
Con_NotifyBox
==================
*/
void Con_NotifyBox (char *text)
{
	double		t1, t2;

// during startup for sound / cd warnings
	Con_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n");

	Con_Printf (text);

	Con_Printf ("Press a key.\n");
	Con_Printf("\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n");

	key_count = -2;		// wait for a key down and up
	key_dest = key_console;

	do
	{
		t1 = Sys_FloatTime ();
		SCR_UpdateScreen ();
		Sys_SendKeyEvents ();
		t2 = Sys_FloatTime ();
		realtime += t2-t1;		// make the cursor blink
	} while (key_count < 0);

	Con_Printf ("\n");
	key_dest = key_game;
	realtime = 0;				// put the cursor back to invisible
}

/*
=================
Con_GetLastLines

Copies the last `n` console lines into `out` (NUL-terminated, lines joined by
'\n'), trimming trailing spaces. Used by the MCP `console_tail` tool so an
external agent can read engine diagnostics after issuing a console command.

Returns bytes written (excluding terminator). Caps at outsz-1 bytes.
=================
*/
int Con_GetLastLines (int n, char *out, int outsz)
{
	int		want, start, line, col, last;
	int		written = 0;
	char	*p, *end;

	if (!out || outsz <= 0) return 0;
	if (n <= 0 || !con_text || con_linewidth <= 0 || con_totallines <= 0)
	{ out[0] = '\0'; return 0; }

	/* Clamp: can't read more than the ring holds. con_current itself is
	 * the line getting next-printed-into, so it's the latest content. */
	want = n;
	if (want > con_totallines) want = con_totallines;
	start = con_current - want + 1;
	if (start < 0) start = 0;

	p   = out;
	end = out + outsz - 1;        /* leave room for trailing NUL */

	for (line = start; line <= con_current && p < end; line++)
	{
		const char *src = con_text + (line % con_totallines) * con_linewidth;

		/* Find last non-space char so we don't trail dozens of pad spaces.
		 * Engine sets high bit for color; mask before comparing. */
		last = -1;
		for (col = 0; col < con_linewidth; col++)
		{
			char c = (char)((unsigned char)src[col] & 0x7f);
			if (c != ' ') last = col;
		}

		for (col = 0; col <= last && p < end; col++)
		{
			char c = (char)((unsigned char)src[col] & 0x7f);
			if (c < ' ' || c > '~') c = ' ';   /* drop weirdness */
			*p++ = c;
		}
		if (p < end) *p++ = '\n';
	}
	*p = '\0';
	written = (int)(p - out);
	return written;
}


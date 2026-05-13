/*
line_editor.c -- insertion-cursor line editor used by the console.
*/
#include "quakedef.h"
#include "line_editor.h"

/*
Ring slot is MAXCMDLINE bytes for ']' + content + NUL, so content length
is capped at MAXCMDLINE - 2.
*/
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
	/* Shift suffix [cursor..len] one byte right (including the NUL). */
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
	/* Walk past trailing whitespace before the cursor. */
	while (start > 0 && le->buf[start - 1] == ' ')
		start--;
	/* Walk past the non-whitespace word. */
	while (start > 0 && le->buf[start - 1] != ' ')
		start--;
	if (start == le->cursor)
		return;	/* nothing to kill */
	memmove(&le->buf[start], &le->buf[le->cursor],
			(size_t)(le->len - le->cursor + 1));
	le->len -= (le->cursor - start);
	le->cursor = start;
}

#ifndef LINE_EDITOR_H
#define LINE_EDITOR_H

/* Requires quakedef.h to be included before this header (for qboolean, MAXCMDLINE). */

#define LE_MAX_LINE MAXCMDLINE

typedef struct
{
	char	buf[LE_MAX_LINE];	/* raw editable text, NUL-terminated, no prompt prefix */
	int		len;				/* strlen(buf), cached */
	int		cursor;				/* 0..len; insertion point */
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

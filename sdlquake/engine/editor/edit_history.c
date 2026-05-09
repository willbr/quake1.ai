// edit_history.c -- snapshot-based undo/redo stack.

#include "quakedef.h"
#include "edit_scene.h"
#include "edit_history.h"

#include <stdlib.h>
#include <string.h>

#define HISTORY_MAX 64

typedef struct snap_s {
    char *text;
    char  desc[32];
} snap_t;

static snap_t s_undo[HISTORY_MAX];
static snap_t s_redo[HISTORY_MAX];
static int    s_undo_n;
static int    s_redo_n;

static void clear_stack(snap_t *stack, int *n)
{
    int i;
    for (i = 0; i < *n; i++)
    {
        if (stack[i].text) { free(stack[i].text); stack[i].text = NULL; }
        stack[i].desc[0] = 0;
    }
    *n = 0;
}

void History_Init(void)
{
    s_undo_n = 0;
    s_redo_n = 0;
    memset(s_undo, 0, sizeof(s_undo));
    memset(s_redo, 0, sizeof(s_redo));
}

void History_Shutdown(void)
{
    History_Clear();
}

void History_Clear(void)
{
    clear_stack(s_undo, &s_undo_n);
    clear_stack(s_redo, &s_redo_n);
}

// Take a snapshot of the current scene + a short description.
static int snapshot_current(snap_t *out, const char *desc)
{
    char *text;
    if (!Scene_Serialize(&text, NULL)) return 0;
    out->text = text;
    if (desc) {
        size_t n = strlen(desc);
        if (n >= sizeof(out->desc)) n = sizeof(out->desc) - 1;
        memcpy(out->desc, desc, n);
        out->desc[n] = 0;
    } else {
        out->desc[0] = 0;
    }
    return 1;
}

// Push to a stack (FIFO: oldest entries drop off the bottom when full).
static void stack_push(snap_t *stack, int *n, snap_t s)
{
    if (*n == HISTORY_MAX)
    {
        if (stack[0].text) free(stack[0].text);
        memmove(&stack[0], &stack[1], (HISTORY_MAX - 1) * sizeof(snap_t));
        (*n)--;
    }
    stack[*n] = s;
    (*n)++;
}

void History_Push(const char *desc)
{
    snap_t s;
    if (!snapshot_current(&s, desc)) return;
    stack_push(s_undo, &s_undo_n, s);
    // Any new edit invalidates the redo timeline.
    clear_stack(s_redo, &s_redo_n);
}

int History_CanUndo(void) { return s_undo_n > 0; }
int History_CanRedo(void) { return s_redo_n > 0; }

int History_Undo(void)
{
    snap_t cur, top;
    if (s_undo_n == 0) return 0;
    if (!snapshot_current(&cur, "redo")) return 0;
    top = s_undo[s_undo_n - 1];
    s_undo_n--;
    if (!Scene_DeserializeText(top.text))
    {
        // Restore failed — roll back our own move.
        free(cur.text);
        s_undo[s_undo_n++] = top;
        return 0;
    }
    free(top.text);
    stack_push(s_redo, &s_redo_n, cur);
    Con_Printf("editor: undo (%d left)\n", s_undo_n);
    return 1;
}

int History_Redo(void)
{
    snap_t cur, top;
    if (s_redo_n == 0) return 0;
    if (!snapshot_current(&cur, "undo")) return 0;
    top = s_redo[s_redo_n - 1];
    s_redo_n--;
    if (!Scene_DeserializeText(top.text))
    {
        free(cur.text);
        s_redo[s_redo_n++] = top;
        return 0;
    }
    free(top.text);
    stack_push(s_undo, &s_undo_n, cur);
    Con_Printf("editor: redo (%d left)\n", s_redo_n);
    return 1;
}

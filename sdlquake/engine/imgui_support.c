// imgui_support.c -- bridges Quake state to the Dear ImGui overlay
// Compiled with engine C flags (gnu89) so quakedef.h is safe to include.

#include "quakedef.h"
#include "imgui_support.h"

/* pr_strings is the progs string heap; string_t is just an offset into it. */
extern char *pr_strings;

/* Console circular buffer (defined in console.c). */
extern char *con_text;
extern int   con_current;
extern int   con_linewidth;
extern int   con_totallines;

void *ImguiSupport_GetCvarList(void)
{
    return cvar_vars;
}

const char *ImguiSupport_CvarName(void *cv)
{
    return ((cvar_t *)cv)->name;
}

const char *ImguiSupport_CvarString(void *cv)
{
    return ((cvar_t *)cv)->string;
}

void *ImguiSupport_CvarNext(void *cv)
{
    return ((cvar_t *)cv)->next;
}

void ImguiSupport_CvarSet(const char *name, const char *value)
{
    Cvar_Set((char *)name, (char *)value);
}

int ImguiSupport_GetNumEdicts(void)
{
    return sv.active ? sv.num_edicts : 0;
}

void ImguiSupport_GetEdict(int i, const char **classname,
                            float *x, float *y, float *z)
{
    *classname = NULL;
    if (!sv.active || i <= 0 || i >= sv.num_edicts)
        return;

    edict_t *e = EDICT_NUM(i);
    if (!e || e->free)
        return;

    *classname = pr_strings + e->v.classname;
    *x = e->v.origin[0];
    *y = e->v.origin[1];
    *z = e->v.origin[2];
}

int ImguiSupport_GetNumConsoleLines(void)
{
    if (!con_text || con_linewidth <= 0 || con_totallines <= 0) return 0;
    int n = con_current + 1;
    return n < con_totallines ? n : con_totallines;
}

/* Returns number of printable chars written (0 = line was blank). */
int ImguiSupport_GetConsoleLine(int from_bottom, char *buf, int buf_size)
{
    if (!con_text || con_linewidth <= 0 || buf_size <= 0) { buf[0] = '\0'; return 0; }

    int idx = ((con_current - from_bottom) % con_totallines + con_totallines) % con_totallines;
    const char *src = con_text + idx * con_linewidth;

    int max = buf_size - 1;
    if (max > con_linewidth) max = con_linewidth;

    int len = 0;
    int i;
    for (i = 0; i < max; i++) {
        char c = src[i] & 0x7F;   /* strip Quake color bit */
        if (c == '\0') break;
        if (c >= 32 && c < 127)
            buf[len++] = c;
    }
    while (len > 0 && buf[len - 1] == ' ') len--;  /* trim trailing spaces */
    buf[len] = '\0';
    return len;
}

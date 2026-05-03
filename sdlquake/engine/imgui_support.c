// imgui_support.c -- bridges Quake state to the Dear ImGui overlay
// Compiled with engine C flags (gnu89) so quakedef.h is safe to include.

#include "quakedef.h"
#include "imgui_support.h"

/* pr_strings is the progs string heap; string_t is just an offset into it. */
extern char *pr_strings;

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

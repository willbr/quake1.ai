// imgui_support.h -- C shim exposing Quake state to imgui_layer.cpp
// Implemented in imgui_support.c which includes quakedef.h.

#ifndef IMGUI_SUPPORT_H
#define IMGUI_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Cvar list (opaque void* to keep quake types out of C++ headers)
void       *ImguiSupport_GetCvarList(void);
const char *ImguiSupport_CvarName(void *cv);
const char *ImguiSupport_CvarString(void *cv);
void       *ImguiSupport_CvarNext(void *cv);
void        ImguiSupport_CvarSet(const char *name, const char *value);

// Entity list
int         ImguiSupport_GetNumEdicts(void);
void        ImguiSupport_GetEdict(int i, const char **classname,
                                  float *x, float *y, float *z);

// Console log (circular buffer; from_bottom=0 is most recent line)
int         ImguiSupport_GetNumConsoleLines(void);
int         ImguiSupport_GetConsoleLine(int from_bottom, char *buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_SUPPORT_H

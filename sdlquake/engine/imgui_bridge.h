// imgui_bridge.h -- C-callable wrappers over the Dear ImGui C++ API.
// Implemented in imgui_bridge.cpp (C++ glue, no logic).
// imgui_layer.c is the only consumer.

#ifndef IMGUI_BRIDGE_H
#define IMGUI_BRIDGE_H

#include <SDL3/SDL.h>

// ---------------------------------------------------------------------------
// ImGuiWindowFlags subset
// ---------------------------------------------------------------------------
#define IG_WF_None            0
#define IG_WF_NoTitleBar      (1<<0)
#define IG_WF_NoResize        (1<<1)
#define IG_WF_NoMove          (1<<2)
#define IG_WF_NoCollapse      (1<<5)
#define IG_WF_NoSavedSettings (1<<8)

// ---------------------------------------------------------------------------
// ImGuiCond
// ---------------------------------------------------------------------------
#define IG_Cond_Always       1
#define IG_Cond_Once         2
#define IG_Cond_FirstUseEver 4

// ---------------------------------------------------------------------------
// ImGuiTableFlags subset
// ---------------------------------------------------------------------------
#define IG_TF_Resizable  (1<<0)
#define IG_TF_RowBg      (1<<6)
#define IG_TF_Borders    (128|512|256|1024)   // all four border bits
#define IG_TF_ScrollY    (1<<25)

// ---------------------------------------------------------------------------
// ImGuiTableColumnFlags subset
// ---------------------------------------------------------------------------
#define IG_TCF_WidthStretch (1<<3)
#define IG_TCF_WidthFixed   (1<<4)

// ---------------------------------------------------------------------------
// ImGuiInputTextFlags subset
// ---------------------------------------------------------------------------
#define IG_ITF_EnterReturnsTrue (1<<6)

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

// Context
void  IG_CreateContext(void);
void  IG_DestroyContext(void);
void  IG_StyleColorsDark(void);
void  IG_SetIniFilename(const char *path);   // pass NULL to disable .ini

// Per-frame info
float IG_GetFramerate(void);

// Frame lifecycle
void  IG_NewFrame(void);
void  IG_Render(void);

// Windows
int   IG_Begin(const char *name, int *p_open, int flags);
void  IG_End(void);
void  IG_SetNextWindowPos(float x, float y, int cond);
void  IG_SetNextWindowSize(float w, float h, int cond);

// Widgets
void  IG_TextUnformatted(const char *text);
void  IG_SetNextItemWidth(float w);
int   IG_InputText(const char *label, char *buf, int buf_size, int flags);
int   IG_Checkbox(const char *label, int *v);

// Scroll
void  IG_SetScrollHereY(float ratio);

// Tables
int   IG_BeginTable(const char *id, int cols, int flags,
                    float outer_w, float outer_h);
void  IG_EndTable(void);
void  IG_TableSetupScrollFreeze(int cols, int rows);
void  IG_TableSetupColumn(const char *label, int col_flags, float init_width);
void  IG_TableHeadersRow(void);
void  IG_TableNextRow(void);
int   IG_TableSetColumnIndex(int col);

// SDL3 input backend
int   IG_ImplSDL3_InitForSDLRenderer(SDL_Window *w, SDL_Renderer *r);
void  IG_ImplSDL3_Shutdown(void);
void  IG_ImplSDL3_NewFrame(void);
int   IG_ImplSDL3_ProcessEvent(const SDL_Event *ev);

// SDL_Renderer3 rendering backend
int   IG_ImplSDLRenderer3_Init(SDL_Renderer *r);
void  IG_ImplSDLRenderer3_Shutdown(void);
void  IG_ImplSDLRenderer3_NewFrame(void);
void  IG_ImplSDLRenderer3_RenderDrawData(SDL_Renderer *r);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_BRIDGE_H

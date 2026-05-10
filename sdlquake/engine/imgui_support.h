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
void        ImguiSupport_ExecCommand(const char *cmd);

// Tab completion: writes first command/cvar match + trailing space into out.
// Returns 1 on match, 0 if no match found.
int         ImguiSupport_TabComplete(const char *partial, char *out, int out_size);

// Returns a human-readable description for a cvar name, or NULL if unknown.
const char *ImguiSupport_CvarDescription(const char *name);

// AI dev panel shared state (written by game DLL each frame, read by imgui layer).
#define IMGUI_AI_MAX_ROWS 64
typedef struct {
    int          edict_num;
    int          state;          // ai_state_t value
    float        alert_level;
    float        last_known_pos[3];
    int          target_edict;
} imgui_ai_row_t;

void   ImguiSupport_AI_Clear(void);
void   ImguiSupport_AI_Push(const imgui_ai_row_t *row);
int    ImguiSupport_AI_Count(void);
const  imgui_ai_row_t *ImguiSupport_AI_Row(int i);

// Nav minimap shared state (written by game DLL each frame, read by imgui layer).
#define IMGUI_NAV_MAX_POINTS  4096
#define IMGUI_NAV_MAX_EDGES   16384
typedef struct { float x, y; }              imgui_nav_point_t;
typedef struct { unsigned short a, b; }     imgui_nav_edge_t;
typedef struct {
    int   has_path;
    int   path_len;
    float path_xy[64];   // up to 32 (x,y) pairs
} imgui_nav_active_t;

void ImguiSupport_Nav_Set(const imgui_nav_point_t *pts, int np,
                          const imgui_nav_edge_t *eds, int ne);
void ImguiSupport_Nav_SetPath(const imgui_nav_active_t *p);
int  ImguiSupport_Nav_Count(int *out_np, int *out_ne);
const imgui_nav_point_t  *ImguiSupport_Nav_Points(void);
const imgui_nav_edge_t   *ImguiSupport_Nav_Edges(void);
const imgui_nav_active_t *ImguiSupport_Nav_Path(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_SUPPORT_H

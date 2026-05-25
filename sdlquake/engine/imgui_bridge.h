// imgui_bridge.h -- C-callable wrappers over the Dear ImGui C++ API.
// Implemented in imgui_bridge.cpp (C++ glue, no logic).
// imgui_layer.c is the only consumer.

#ifndef IMGUI_BRIDGE_H
#define IMGUI_BRIDGE_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

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
// ImGuiSelectableFlags subset
// ---------------------------------------------------------------------------
#define IG_SF_AllowDoubleClick (1<<2)

// ---------------------------------------------------------------------------
// ImGuiInputTextFlags subset
// ---------------------------------------------------------------------------
#define IG_ITF_EnterReturnsTrue    (1<<6)
#define IG_ITF_CallbackCompletion  (1<<18)

// Opaque callback data passed to IG_InputTextWithCompletion.
typedef struct {
    char *buf;
    int   buf_text_len;
    int   buf_size;
    int   buf_dirty;
    int   cursor_pos;
} IG_CompletionData;
typedef void (*IG_CompletionCallback)(IG_CompletionData *data);

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
int   IG_WantCaptureMouse(void);    // 1 if ImGui currently owns the mouse
int   IG_WantCaptureKeyboard(void); // 1 if a text widget is focused

// Release all currently-tracked keyboard / mouse state. Used when closing
// the dev overlay so a KEY_UP that was dropped while the overlay was
// closing doesn't leave the corresponding key "stuck" inside ImGui's view.
void IG_ClearInputs(void);
int   IG_IsMouseDoubleClicked(int button); // 1 if button was just double-clicked
void  IG_GetDisplaySize(float *w, float *h);

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
int   IG_InputTextWithCompletion(const char *label, char *buf, int buf_size,
                                 int extra_flags, IG_CompletionCallback cb);
int   IG_Checkbox(const char *label, int *v);
int   IG_Button(const char *label);
int   IG_SmallButton(const char *label);
int   IG_Selectable(const char *label, int selected, int flags);
void  IG_SameLine(float offset_x, float spacing);   // pass 0,-1 for default
void  IG_Separator(void);
void  IG_Spacing(void);
int   IG_Combo(const char *label, int *current_item, const char * const items[],
               int items_count);
int   IG_DragFloat3(const char *label, float v[3], float speed);
int   IG_InputFloat3(const char *label, float v[3]);
int   IG_DragFloat (const char *label, float *v, float speed,
                    float vmin, float vmax);
int   IG_SliderFloat(const char *label, float *v,
                     float vmin, float vmax, const char *format);
int   IG_ColorEdit3 (const char *label, float col[3]);
int   IG_CollapsingHeader(const char *label, int flags);  /* flags=0 default */
// 3-state radio rows in the Debug Render panel use IG_RadioButton; the panel
// drives the active flag itself (returns 1 on click). BeginDisabled / EndDisabled
// grey out r_drawpaths_what's bit checkboxes when r_drawpaths == 0.
int   IG_RadioButton (const char *label, int active);
void  IG_BeginDisabled(int disabled);
void  IG_EndDisabled  (void);
// Item state queries — used to gate per-drag history pushes (snapshot on
// activate, push on deactivate-after-edit so each drag becomes one undo
// step, not hundreds). IsItemHovered drives tooltip-on-thumbnail UX.
int   IG_IsItemActivated(void);
int   IG_IsItemDeactivatedAfterEdit(void);
int   IG_IsItemHovered(void);
// True while the mouse is held down after starting a click on the last
// submitted item. Used by the Profile histogram to support click-and-drag
// scrubbing across the frame strip.
int   IG_IsItemActive(void);

// Image / image button. `tex` is an SDL_Texture* on the SDLRenderer3
// backend (cast through ImTextureID). Caller owns the texture lifetime;
// the bridge does not retain it.
typedef void *IG_TextureID;
void  IG_Image(IG_TextureID tex, float w, float h);
int   IG_ImageButton(const char *id, IG_TextureID tex, float w, float h);

// Tooltip on hovered item — pair these around the tooltip body.
void  IG_BeginTooltip(void);
void  IG_EndTooltip  (void);

void  IG_PushID_Int(int id);
void  IG_PopID(void);

// Child windows
int   IG_BeginChild(const char *id, float w, float h, int child_flags, int window_flags);
void  IG_EndChild(void);

// Scroll / focus
void  IG_SetScrollHereY(float ratio);
void  IG_SetKeyboardFocusHere(int offset);

// Layout queries
float IG_GetFrameHeightWithSpacing(void);
// Width of the current window's content region (excluding scrollbar). Used
// to auto-wrap thumbnail grids when the user resizes the window.
float IG_GetContentRegionAvailX(void);

// Tables
int   IG_BeginTable(const char *id, int cols, int flags,
                    float outer_w, float outer_h);
void  IG_EndTable(void);
void  IG_TableSetupScrollFreeze(int cols, int rows);
void  IG_TableSetupColumn(const char *label, int col_flags, float init_width);
void  IG_TableHeadersRow(void);
void  IG_TableNextRow(void);
int   IG_TableSetColumnIndex(int col);

// Canvas drawing (backed by ImDrawList). Coordinates are canvas-relative;
// IG_BeginCanvas reserves the rectangle and stashes its screen origin.
void  IG_BeginCanvas(const char *id, float w, float h);
void  IG_CanvasLine(float x0, float y0, float x1, float y1, unsigned int col);
void  IG_CanvasRectFilled(float x0, float y0, float x1, float y1, unsigned int col);
void  IG_CanvasRect      (float x0, float y0, float x1, float y1, unsigned int col);
void  IG_CanvasText      (float x, float y, unsigned int col, const char *text);
// Mouse position relative to the active canvas. Sentinel (-1,-1) if outside.
void  IG_CanvasMousePos  (float *x, float *y);
int   IG_IsItemHoveredEx (void);  // alias used by perf panel
void  IG_EndCanvas(void);

// Built-in plot — handy for the FPS / frametime mini-graphs. `values` is a
// circular buffer; `offset` is the index of the oldest sample.
void  IG_PlotLines(const char *label, const float *values, int count,
                   int offset, const char *overlay,
                   float scale_min, float scale_max,
                   float w, float h);

// SDL3 input backend
int   IG_ImplSDL3_InitForOther(SDL_Window *w);
void  IG_ImplSDL3_Shutdown(void);
void  IG_ImplSDL3_NewFrame(void);
int   IG_ImplSDL3_ProcessEvent(const SDL_Event *ev);

// SDL_GPU3 rendering backend.
// PrepareDrawData must run BEFORE opening the swapchain render pass — it
// uploads the vertex/index buffers via a transfer pass on the same command
// buffer. RenderDrawData runs inside the render pass and emits draw calls.
int   IG_ImplSDLGPU3_Init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format);
void  IG_ImplSDLGPU3_Shutdown(void);
void  IG_ImplSDLGPU3_NewFrame(void);
void  IG_ImplSDLGPU3_PrepareDrawData(SDL_GPUCommandBuffer *cmd);
void  IG_ImplSDLGPU3_RenderDrawData (SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_BRIDGE_H

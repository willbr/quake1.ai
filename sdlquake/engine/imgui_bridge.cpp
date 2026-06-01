// imgui_bridge.cpp -- C++ glue between imgui and our C layer.
// Zero logic here: every function is a one-liner that delegates to imgui.

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui_bridge.h"

extern "C" {

// Context
void IG_CreateContext(void)             { IMGUI_CHECKVERSION(); ImGui::CreateContext(); }
void IG_DestroyContext(void)            { ImGui::DestroyContext(); }
void IG_StyleColorsDark(void)           { ImGui::StyleColorsDark(); }
void IG_SetIniFilename(const char *p)   { ImGui::GetIO().IniFilename = p; }

// Per-frame info
float IG_GetFramerate(void)             { return ImGui::GetIO().Framerate; }
int   IG_WantCaptureMouse(void)         { return ImGui::GetIO().WantCaptureMouse ? 1 : 0; }
int   IG_WantCaptureKeyboard(void)      { return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0; }

// Drop any stuck key / mouse state. Called when the layer closes — the
// previous open period may have forwarded a KEY_DOWN whose KEY_UP couldn't
// be forwarded (because the close happened in the same frame), leaving
// ImGui thinking the key is held. Subsequent Shortcut(Esc, Repeat) etc.
// would then auto-fire on the next time the layer opens, deactivating any
// just-clicked InputText.
//
// We have to clear BOTH the processed state (ClearInputKeys/Mouse) AND the
// pending event queue (ClearEventsQueue) — the Esc DOWN that triggered the
// close is sitting in the queue and would otherwise be applied on the next
// NewFrame after the layer reopens.
void IG_ClearInputs(void)
{
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
}
int   IG_IsMouseDoubleClicked(int button){ return ImGui::IsMouseDoubleClicked((ImGuiMouseButton)button) ? 1 : 0; }
void  IG_GetDisplaySize(float *w, float *h)
{
    ImVec2 sz = ImGui::GetIO().DisplaySize;
    if (w) *w = sz.x;
    if (h) *h = sz.y;
}

// Frame lifecycle
void IG_NewFrame(void)  { ImGui::NewFrame(); }
void IG_Render(void)    { ImGui::Render(); }

// Windows
int IG_Begin(const char *name, int *p_open, int flags)
{
    return ImGui::Begin(name, (bool *)p_open, flags) ? 1 : 0;
}
void IG_End(void) { ImGui::End(); }
void IG_SetNextWindowPos(float x, float y, int cond)
{
    ImGui::SetNextWindowPos(ImVec2(x, y), (ImGuiCond)cond);
}
void IG_SetNextWindowSize(float w, float h, int cond)
{
    ImGui::SetNextWindowSize(ImVec2(w, h), (ImGuiCond)cond);
}

// Widgets
void IG_TextUnformatted(const char *text) { ImGui::TextUnformatted(text); }
void IG_SetNextItemWidth(float w)         { ImGui::SetNextItemWidth(w); }
int  IG_InputText(const char *label, char *buf, int buf_size, int flags)
{
    return ImGui::InputText(label, buf, (size_t)buf_size, flags) ? 1 : 0;
}

static int completion_trampoline(ImGuiInputTextCallbackData *data)
{
    IG_CompletionData cd;
    cd.buf          = data->Buf;
    cd.buf_text_len = data->BufTextLen;
    cd.buf_size     = data->BufSize;
    cd.buf_dirty    = 0;
    cd.cursor_pos   = data->CursorPos;
    ((IG_CompletionCallback)data->UserData)(&cd);
    if (cd.buf_dirty) {
        data->BufTextLen = cd.buf_text_len;
        data->CursorPos  = cd.cursor_pos;
        data->BufDirty   = true;
    }
    return 0;
}
int  IG_InputTextWithCompletion(const char *label, char *buf, int buf_size,
                                int extra_flags, IG_CompletionCallback cb)
{
    return ImGui::InputText(label, buf, (size_t)buf_size,
        extra_flags | ImGuiInputTextFlags_CallbackCompletion,
        completion_trampoline, (void *)cb) ? 1 : 0;
}
int  IG_Checkbox(const char *label, int *v)
{
    bool b = (*v != 0);
    bool changed = ImGui::Checkbox(label, &b);
    *v = b ? 1 : 0;
    return changed ? 1 : 0;
}
int  IG_Button(const char *label)
{
    return ImGui::Button(label) ? 1 : 0;
}
int  IG_SmallButton(const char *label)
{
    return ImGui::SmallButton(label) ? 1 : 0;
}
int  IG_Selectable(const char *label, int selected, int flags)
{
    return ImGui::Selectable(label, selected != 0, flags) ? 1 : 0;
}
void IG_SameLine(float offset, float spacing)
{
    ImGui::SameLine(offset, spacing);
}
void IG_Separator(void)               { ImGui::Separator(); }
void IG_Spacing(void)                 { ImGui::Spacing(); }
int  IG_Combo(const char *label, int *current_item, const char * const items[],
              int items_count)
{
    return ImGui::Combo(label, current_item, items, items_count) ? 1 : 0;
}
int  IG_DragFloat3(const char *label, float v[3], float speed)
{
    return ImGui::DragFloat3(label, v, speed) ? 1 : 0;
}
int  IG_InputFloat3(const char *label, float v[3])
{
    return ImGui::InputFloat3(label, v) ? 1 : 0;
}
int  IG_DragFloat(const char *label, float *v, float speed,
                  float vmin, float vmax)
{
    return ImGui::DragFloat(label, v, speed, vmin, vmax) ? 1 : 0;
}
int  IG_SliderFloat(const char *label, float *v,
                    float vmin, float vmax, const char *format)
{
    return ImGui::SliderFloat(label, v, vmin, vmax,
                              format ? format : "%.3f") ? 1 : 0;
}
int  IG_ColorEdit3(const char *label, float col[3])
{
    return ImGui::ColorEdit3(label, col) ? 1 : 0;
}
int  IG_ColorSwatch(const char *id, float r, float g, float b, float size)
{
    ImVec4 col(r, g, b, 1.0f);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop;
    return ImGui::ColorButton(id, col, flags, ImVec2(size, size)) ? 1 : 0;
}
int  IG_CollapsingHeader(const char *label, int flags)
{
    return ImGui::CollapsingHeader(label, (ImGuiTreeNodeFlags)flags) ? 1 : 0;
}
int  IG_RadioButton(const char *label, int active)
{
    return ImGui::RadioButton(label, active != 0) ? 1 : 0;
}
void IG_BeginDisabled(int disabled) { ImGui::BeginDisabled(disabled != 0); }
void IG_EndDisabled  (void)         { ImGui::EndDisabled(); }
int  IG_IsItemActivated(void)            { return ImGui::IsItemActivated() ? 1 : 0; }
int  IG_IsItemDeactivatedAfterEdit(void) { return ImGui::IsItemDeactivatedAfterEdit() ? 1 : 0; }
int  IG_IsItemHovered(void)              { return ImGui::IsItemHovered() ? 1 : 0; }
int  IG_IsItemActive(void)               { return ImGui::IsItemActive()  ? 1 : 0; }
void IG_Image(IG_TextureID tex, float w, float h)
{
    ImGui::Image(ImTextureRef((ImTextureID)tex), ImVec2(w, h));
}
int  IG_ImageButton(const char *id, IG_TextureID tex, float w, float h)
{
    return ImGui::ImageButton(id, ImTextureRef((ImTextureID)tex),
                              ImVec2(w, h)) ? 1 : 0;
}
void IG_BeginTooltip(void) { ImGui::BeginTooltip(); }
void IG_EndTooltip  (void) { ImGui::EndTooltip();   }
void IG_PushID_Int(int id)            { ImGui::PushID(id); }
void IG_PopID(void)                   { ImGui::PopID(); }

// Child windows
int  IG_BeginChild(const char *id, float w, float h, int child_flags, int window_flags)
{
    return ImGui::BeginChild(id, ImVec2(w, h),
        (ImGuiChildFlags)child_flags, (ImGuiWindowFlags)window_flags) ? 1 : 0;
}
void IG_EndChild(void) { ImGui::EndChild(); }

// Scroll / focus
void IG_SetScrollHereY(float ratio)      { ImGui::SetScrollHereY(ratio); }
void IG_SetKeyboardFocusHere(int offset) { ImGui::SetKeyboardFocusHere(offset); }

// Layout queries
float IG_GetFrameHeightWithSpacing(void) { return ImGui::GetFrameHeightWithSpacing(); }
float IG_GetContentRegionAvailX(void)    { return ImGui::GetContentRegionAvail().x; }

// Tables
int IG_BeginTable(const char *id, int cols, int flags, float ow, float oh)
{
    return ImGui::BeginTable(id, cols, flags, ImVec2(ow, oh)) ? 1 : 0;
}
void IG_EndTable(void)                                    { ImGui::EndTable(); }
void IG_TableSetupScrollFreeze(int cols, int rows)        { ImGui::TableSetupScrollFreeze(cols, rows); }
void IG_TableSetupColumn(const char *l, int f, float w)   { ImGui::TableSetupColumn(l, f, w); }
void IG_TableHeadersRow(void)                             { ImGui::TableHeadersRow(); }
void IG_TableNextRow(void)                                { ImGui::TableNextRow(); }
int  IG_TableSetColumnIndex(int col)  { return ImGui::TableSetColumnIndex(col) ? 1 : 0; }

// Canvas drawing (backed by ImDrawList). The InvisibleButton in BeginCanvas
// gives us a hit-testable widget so callers can check IG_IsItemHoveredEx().
static ImVec2 s_canvas_origin;
static ImVec2 s_canvas_size;
static bool   s_canvas_hovered;

void IG_BeginCanvas(const char *id, float w, float h)
{
    s_canvas_origin = ImGui::GetCursorScreenPos();
    s_canvas_size   = ImVec2(w, h);
    ImGui::InvisibleButton(id, ImVec2(w, h));
    s_canvas_hovered = ImGui::IsItemHovered();
}

void IG_CanvasLine(float x0, float y0, float x1, float y1, unsigned int col)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(s_canvas_origin.x + x0, s_canvas_origin.y + y0),
                ImVec2(s_canvas_origin.x + x1, s_canvas_origin.y + y1),
                (ImU32)col, 1.0f);
}

void IG_CanvasRectFilled(float x0, float y0, float x1, float y1, unsigned int col)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(s_canvas_origin.x + x0, s_canvas_origin.y + y0),
                      ImVec2(s_canvas_origin.x + x1, s_canvas_origin.y + y1),
                      (ImU32)col);
}

void IG_CanvasRect(float x0, float y0, float x1, float y1, unsigned int col)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRect(ImVec2(s_canvas_origin.x + x0, s_canvas_origin.y + y0),
                ImVec2(s_canvas_origin.x + x1, s_canvas_origin.y + y1),
                (ImU32)col);
}

void IG_CanvasText(float x, float y, unsigned int col, const char *text)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(s_canvas_origin.x + x, s_canvas_origin.y + y),
                (ImU32)col, text);
}

void IG_CanvasMousePos(float *x, float *y)
{
    ImVec2 m = ImGui::GetIO().MousePos;
    float lx = m.x - s_canvas_origin.x;
    float ly = m.y - s_canvas_origin.y;
    if (lx < 0 || ly < 0 || lx > s_canvas_size.x || ly > s_canvas_size.y) {
        if (x) *x = -1; if (y) *y = -1;
        return;
    }
    if (x) *x = lx;
    if (y) *y = ly;
}

int IG_IsItemHoveredEx(void) { return s_canvas_hovered ? 1 : 0; }

void IG_EndCanvas(void)
{
    // Space already reserved by InvisibleButton; nothing to do.
}

void IG_PlotLines(const char *label, const float *values, int count,
                  int offset, const char *overlay,
                  float scale_min, float scale_max,
                  float w, float h)
{
    ImGui::PlotLines(label, values, count, offset, overlay,
                     scale_min, scale_max, ImVec2(w, h));
}

// SDL3 input backend
int  IG_ImplSDL3_InitForOther(SDL_Window *w)
    { return ImGui_ImplSDL3_InitForOther(w) ? 1 : 0; }
void IG_ImplSDL3_Shutdown(void)         { ImGui_ImplSDL3_Shutdown(); }
void IG_ImplSDL3_NewFrame(void)         { ImGui_ImplSDL3_NewFrame(); }
int  IG_ImplSDL3_ProcessEvent(const SDL_Event *ev)
    { return ImGui_ImplSDL3_ProcessEvent(ev) ? 1 : 0; }

// SDL_GPU3 rendering backend
int  IG_ImplSDLGPU3_Init(SDL_GPUDevice *device, SDL_GPUTextureFormat color_format)
{
    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device            = device;
    info.ColorTargetFormat = color_format;
    info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;
    return ImGui_ImplSDLGPU3_Init(&info) ? 1 : 0;
}
void IG_ImplSDLGPU3_Shutdown(void)  { ImGui_ImplSDLGPU3_Shutdown(); }
void IG_ImplSDLGPU3_NewFrame(void)  { ImGui_ImplSDLGPU3_NewFrame(); }
void IG_ImplSDLGPU3_PrepareDrawData(SDL_GPUCommandBuffer *cmd)
    { ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd); }
void IG_ImplSDLGPU3_RenderDrawData(SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass)
    { ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass); }

} // extern "C"

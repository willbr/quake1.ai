// imgui_bridge.cpp -- C++ glue between imgui and our C layer.
// Zero logic here: every function is a one-liner that delegates to imgui.

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
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
int  IG_IsItemActivated(void)            { return ImGui::IsItemActivated() ? 1 : 0; }
int  IG_IsItemDeactivatedAfterEdit(void) { return ImGui::IsItemDeactivatedAfterEdit() ? 1 : 0; }
int  IG_IsItemHovered(void)              { return ImGui::IsItemHovered() ? 1 : 0; }
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

// SDL3 input backend
int  IG_ImplSDL3_InitForSDLRenderer(SDL_Window *w, SDL_Renderer *r)
    { return ImGui_ImplSDL3_InitForSDLRenderer(w, r) ? 1 : 0; }
void IG_ImplSDL3_Shutdown(void)         { ImGui_ImplSDL3_Shutdown(); }
void IG_ImplSDL3_NewFrame(void)         { ImGui_ImplSDL3_NewFrame(); }
int  IG_ImplSDL3_ProcessEvent(const SDL_Event *ev)
    { return ImGui_ImplSDL3_ProcessEvent(ev) ? 1 : 0; }

// SDL_Renderer3 rendering backend
int  IG_ImplSDLRenderer3_Init(SDL_Renderer *r)
    { return ImGui_ImplSDLRenderer3_Init(r) ? 1 : 0; }
void IG_ImplSDLRenderer3_Shutdown(void) { ImGui_ImplSDLRenderer3_Shutdown(); }
void IG_ImplSDLRenderer3_NewFrame(void) { ImGui_ImplSDLRenderer3_NewFrame(); }
void IG_ImplSDLRenderer3_RenderDrawData(SDL_Renderer *r)
    { ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r); }

} // extern "C"

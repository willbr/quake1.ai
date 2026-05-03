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

// imgui_layer.c -- Dear ImGui dev overlay, pure C.
//
// Toggle with F12.  Renders after the game frame at native window resolution.
// Panels: Perf (FPS/ms), Cvars (filterable, editable), Entities (table).

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "imgui_bridge.h"
#include "imgui_layer.h"
#include "imgui_support.h"

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static int           s_open     = 0;
static int           s_inited   = 0;

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------

static void draw_perf(void)
{
    char buf[64];
    float fps = IG_GetFramerate();
    IG_SetNextWindowPos(10, 10, IG_Cond_Always);
    IG_SetNextWindowSize(180, 60, IG_Cond_Always);
    IG_Begin("Perf", NULL,
        IG_WF_NoTitleBar | IG_WF_NoResize | IG_WF_NoMove |
        IG_WF_NoCollapse  | IG_WF_NoSavedSettings);
    snprintf(buf, sizeof(buf), "%.1f fps  %.2f ms", fps, 1000.0f / fps);
    IG_TextUnformatted(buf);
    IG_End();
}

static void draw_cvars(void)
{
    static char filter[64];
    char value_buf[128];
    char id_buf[144];

    IG_SetNextWindowSize(400, 480, IG_Cond_Once);
    IG_SetNextWindowPos(200, 10, IG_Cond_Once);
    if (!IG_Begin("Cvars", NULL, IG_WF_None)) { IG_End(); return; }

    IG_SetNextItemWidth(200);
    IG_InputText("filter", filter, (int)sizeof(filter), IG_WF_None);

    if (IG_BeginTable("##cvt", 2,
            IG_TF_Borders | IG_TF_ScrollY | IG_TF_RowBg | IG_TF_Resizable,
            0, -1))
    {
        IG_TableSetupScrollFreeze(0, 1);
        IG_TableSetupColumn("Name",  IG_TCF_WidthStretch, 0);
        IG_TableSetupColumn("Value", IG_TCF_WidthStretch, 0);
        IG_TableHeadersRow();

        for (void *cv = ImguiSupport_GetCvarList(); cv; cv = ImguiSupport_CvarNext(cv))
        {
            const char *name = ImguiSupport_CvarName(cv);
            if (filter[0] && !strstr(name, filter)) continue;

            IG_TableNextRow();
            IG_TableSetColumnIndex(0);
            IG_TextUnformatted(name);
            IG_TableSetColumnIndex(1);

            snprintf(value_buf, sizeof(value_buf), "%s", ImguiSupport_CvarString(cv));
            snprintf(id_buf,    sizeof(id_buf),    "##v_%s", name);
            IG_SetNextItemWidth(-1);
            if (IG_InputText(id_buf, value_buf, (int)sizeof(value_buf),
                    IG_ITF_EnterReturnsTrue))
                ImguiSupport_CvarSet(name, value_buf);
        }
        IG_EndTable();
    }
    IG_End();
}

static void draw_console(void)
{
    static int auto_scroll = 1;

    IG_SetNextWindowSize(600, 300, IG_Cond_Once);
    IG_SetNextWindowPos(10, 500, IG_Cond_Once);
    if (!IG_Begin("Console", NULL, IG_WF_None)) { IG_End(); return; }

    IG_Checkbox("Auto-scroll", &auto_scroll);

    int total = ImguiSupport_GetNumConsoleLines();
    int show  = total < 200 ? total : 200;
    char buf[128];
    int i;
    for (i = show - 1; i >= 0; i--)
    {
        if (ImguiSupport_GetConsoleLine(i, buf, (int)sizeof(buf)) > 0)
            IG_TextUnformatted(buf);
    }

    if (auto_scroll)
        IG_SetScrollHereY(1.0f);

    IG_End();
}

static void draw_entities(void)
{
    char buf[64];
    int n = ImguiSupport_GetNumEdicts();

    IG_SetNextWindowSize(480, 280, IG_Cond_Once);
    IG_SetNextWindowPos(10, 200, IG_Cond_Once);
    if (!IG_Begin("Entities", NULL, IG_WF_None)) { IG_End(); return; }

    snprintf(buf, sizeof(buf), "%d edicts", n);
    IG_TextUnformatted(buf);

    if (IG_BeginTable("##ent", 5,
            IG_TF_Borders | IG_TF_ScrollY | IG_TF_RowBg | IG_TF_Resizable,
            0, -1))
    {
        IG_TableSetupScrollFreeze(0, 1);
        IG_TableSetupColumn("ID",    IG_TCF_WidthFixed, 36);
        IG_TableSetupColumn("Class", IG_TCF_WidthStretch, 0);
        IG_TableSetupColumn("X",     IG_TCF_WidthFixed, 56);
        IG_TableSetupColumn("Y",     IG_TCF_WidthFixed, 56);
        IG_TableSetupColumn("Z",     IG_TCF_WidthFixed, 56);
        IG_TableHeadersRow();

        for (int i = 1; i < n; i++)
        {
            const char *cls = NULL;
            float x = 0, y = 0, z = 0;
            ImguiSupport_GetEdict(i, &cls, &x, &y, &z);
            if (!cls) continue;

            IG_TableNextRow();
            IG_TableSetColumnIndex(0); snprintf(buf, sizeof(buf), "%d",   i);  IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(1); IG_TextUnformatted(cls);
            IG_TableSetColumnIndex(2); snprintf(buf, sizeof(buf), "%.0f", x);  IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(3); snprintf(buf, sizeof(buf), "%.0f", y);  IG_TextUnformatted(buf);
            IG_TableSetColumnIndex(4); snprintf(buf, sizeof(buf), "%.0f", z);  IG_TextUnformatted(buf);
        }
        IG_EndTable();
    }
    IG_End();
}

// ---------------------------------------------------------------------------
// Public interface (declared in imgui_layer.h)
// ---------------------------------------------------------------------------

void ImguiLayer_Init(SDL_Window *w, SDL_Renderer *r)
{
    s_window   = w;
    s_renderer = r;

    IG_CreateContext();
    IG_SetIniFilename(NULL);
    IG_StyleColorsDark();

    IG_ImplSDL3_InitForSDLRenderer(w, r);
    IG_ImplSDLRenderer3_Init(r);

    s_inited = 1;
}

void ImguiLayer_Shutdown(void)
{
    if (!s_inited) return;
    IG_ImplSDLRenderer3_Shutdown();
    IG_ImplSDL3_Shutdown();
    IG_DestroyContext();
    s_inited = 0;
}

int ImguiLayer_ProcessEvent(SDL_Event *ev)
{
    if (!s_inited || !s_open) return 0;
    return IG_ImplSDL3_ProcessEvent(ev);
}

void ImguiLayer_Toggle(void)
{
    if (!s_inited) return;
    s_open = !s_open;
    SDL_SetWindowRelativeMouseMode(s_window, !s_open);
}

int ImguiLayer_IsOpen(void)    { return s_open; }
int ImguiLayer_WantsMouse(void){ return s_open; }

void ImguiLayer_Render(void)
{
    if (!s_inited || !s_open) return;

    // Render at native window resolution, bypassing the 320x200 logical scale.
    SDL_SetRenderLogicalPresentation(s_renderer, 0, 0,
        SDL_LOGICAL_PRESENTATION_DISABLED);

    IG_ImplSDLRenderer3_NewFrame();
    IG_ImplSDL3_NewFrame();
    IG_NewFrame();

    draw_perf();
    draw_cvars();
    draw_entities();
    draw_console();

    IG_Render();
    IG_ImplSDLRenderer3_RenderDrawData(s_renderer);

    // Restore game's integer-scale presentation for the next frame.
    SDL_SetRenderLogicalPresentation(s_renderer, 320, 200,
        SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
}

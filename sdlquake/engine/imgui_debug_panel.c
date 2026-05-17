// imgui_debug_panel.c -- Debug Render panel for the F12 dev overlay.
//
// Curated UI over the debug-visualization cvars. Every widget is a thin
// reflection: read the cvar, render, write back on change. No local state.
// See docs/superpowers/specs/2026-05-15-f12-debug-render-panel-design.md.

#include <stdio.h>

#include "imgui_bridge.h"
#include "imgui_debug_panel.h"
#include "imgui_support.h"

// ---------------------------------------------------------------------------
// Row helpers - each one reads + writes a single cvar.
// ---------------------------------------------------------------------------

// Checkbox row. Reads cvar, converts to int, presents as boolean checkbox.
// On click writes back "1" or "0" via the numeric cvar setter.
static void cb_row(const char *label, const char *cvar)
{
    if (!ImguiSupport_CvarExists(cvar)) return;
    int on = ImguiSupport_CvarValue(cvar) != 0.0f;
    if (IG_Checkbox(label, &on))
        ImguiSupport_CvarSetValue(cvar, on ? 1.0f : 0.0f);
}

// Float slider row clamped to [vmin, vmax]. Reads cvar, clamps for display,
// writes back on edit. Values outside the slider's range survive in the cvar
// only until the slider is touched.
static void slider_row(const char *label, const char *cvar,
                       float vmin, float vmax, const char *fmt)
{
    if (!ImguiSupport_CvarExists(cvar)) return;
    float v = ImguiSupport_CvarValue(cvar);
    if (v < vmin) v = vmin;
    if (v > vmax) v = vmax;
    if (IG_SliderFloat(label, &v, vmin, vmax, fmt))
        ImguiSupport_CvarSetValue(cvar, v);
}

// Bit checkbox - toggles a single bit in a numeric cvar. Used by the two
// r_drawpaths_what halves.
static void bit_cb_row(const char *label, const char *cvar, int bit)
{
    if (!ImguiSupport_CvarExists(cvar)) return;
    int mask  = (int)ImguiSupport_CvarValue(cvar);
    int on    = (mask & bit) != 0;
    if (IG_Checkbox(label, &on)) {
        int new_mask = on ? (mask | bit) : (mask & ~bit);
        ImguiSupport_CvarSetValue(cvar, (float)new_mask);
    }
}

// 3-state radio: Off (0) / Ztested (1) / Xray (2). Matches the sim_*_debug
// convention enforced by Sim_DebugZTest in sim_main.c.
//
// IG_RadioButton returns 1 on click. We bind each option to a single value;
// clicking writes that exact value. Three radios share a row via IG_SameLine.
static void ztest_radio_row(const char *label, const char *cvar)
{
    if (!ImguiSupport_CvarExists(cvar)) return;
    int cur = (int)ImguiSupport_CvarValue(cvar);

    // Two-column visual layout: label on the left, three radios on the right.
    // Use the label as the section header so it lines up with the checkbox
    // labels above. ## prefix hides the radio's own label - we draw the
    // grouping text manually.
    char id_off[64], id_zt[64], id_xr[64];
    snprintf(id_off, sizeof(id_off), "Off##%s",     cvar);
    snprintf(id_zt,  sizeof(id_zt),  "Ztest##%s",   cvar);
    snprintf(id_xr,  sizeof(id_xr),  "Xray##%s",    cvar);

    IG_TextUnformatted(label);
    IG_SameLine(120, -1);
    if (IG_RadioButton(id_off, cur == 0)) ImguiSupport_CvarSetValue(cvar, 0.0f);
    IG_SameLine(0, -1);
    if (IG_RadioButton(id_zt,  cur == 1)) ImguiSupport_CvarSetValue(cvar, 1.0f);
    IG_SameLine(0, -1);
    if (IG_RadioButton(id_xr,  cur >= 2)) ImguiSupport_CvarSetValue(cvar, 2.0f);
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

static void draw_ai_section(void)
{
    // Default-open header. The Phase 8 sim overlays are why we built this
    // panel - this section gets pride of place.
    if (!IG_CollapsingHeader("AI overlays", (1<<5)))  // ImGuiTreeNodeFlags_DefaultOpen = 1<<5
        return;

    slider_row("Bboxes (density)",    "r_drawbboxes", 0.0f, 1.0f, "%.2f");
    slider_row("Patrol graph",        "r_drawpaths",  0.0f, 1.0f, "%.2f");

    // The two-bit r_drawpaths_what mask drives which halves of the graph
    // draw. Grey out when the master density is 0 - the bits are meaningless
    // then.
    int paths_on = ImguiSupport_CvarValue("r_drawpaths") > 0.0f;
    IG_BeginDisabled(!paths_on);
    IG_TextUnformatted("  graph layers:");
    IG_SameLine(120, -1);
    bit_cb_row("Static##paths", "r_drawpaths_what", 1);
    IG_SameLine(0, -1);
    bit_cb_row("Live##paths",   "r_drawpaths_what", 2);
    IG_EndDisabled();

    ztest_radio_row("Navmesh",    "sim_nav_debug");
    slider_row("  Nav range",     "sim_nav_debug_range", 0.0f, 4096.0f, "%.0f");
    ztest_radio_row("Patrol AI",  "sim_patrol_debug");
    ztest_radio_row("Senses",     "sim_sense_debug");
    ztest_radio_row("Wind",       "sim_wind_debug");
    ztest_radio_row("Light tier", "sim_light_debug");
}

static void draw_renderer_section(void)
{
    if (!IG_CollapsingHeader("Renderer", (1<<5)))
        return;

    cb_row("Fullbright",       "r_fullbright");
    cb_row("Lightmap only",    "r_lightmap");
    cb_row("Draw entities",    "r_drawentities");
    cb_row("Draw view-model",  "r_drawviewmodel");
    cb_row("Dynamic lights",   "r_dynamic");
    cb_row("No PVS culling",   "r_novis");
}

static void draw_decals_section(void)
{
    // Default-collapsed - decal tuning is less common than AI/render toggles.
    if (!IG_CollapsingHeader("Decals", 0))
        return;

    cb_row("Enable decals",   "r_decals");
    cb_row("Debug overlay",   "r_decals_debug");
    slider_row("Intensity",   "r_decals_intensity", 0.0f, 1.0f, "%.2f");
    slider_row("Max decals",  "r_decals_max",       0.0f, 4096.0f, "%.0f");
}

static void draw_hud_section(void)
{
    if (!IG_CollapsingHeader("HUD", 0))
        return;

    // The Perf window already shows FPS; this cvar drives the in-game HUD
    // counter that stays visible when the F12 layer is closed.
    cb_row("FPS counter",  "showfps");
    cb_row("Turtle icon",  "showturtle");
}

// ---------------------------------------------------------------------------
// Public entry point - called once per frame from imgui_layer.c.
// ---------------------------------------------------------------------------

void DebugPanel_Draw(int x, int y, int w, int h)
{
    // Rect comes from imgui_layer.c's responsive grid so the panel resizes
    // with the window. Cond_Always means a user drag snaps back — this is a
    // dev overlay, not a user-facing UI.
    IG_SetNextWindowPos ((float)x, (float)y, IG_Cond_Always);
    IG_SetNextWindowSize((float)w, (float)h, IG_Cond_Always);
    if (!IG_Begin("Debug Render", NULL, IG_WF_None)) { IG_End(); return; }

    draw_ai_section();
    draw_renderer_section();
    draw_decals_section();
    draw_hud_section();

    IG_End();
}

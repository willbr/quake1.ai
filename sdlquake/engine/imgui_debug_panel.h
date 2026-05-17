// imgui_debug_panel.h -- Debug Render panel for the F12 dev overlay.
// Pure cvar reflection; no local state. Drawn from imgui_layer.c.

#ifndef IMGUI_DEBUG_PANEL_H
#define IMGUI_DEBUG_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

// Caller passes the panel rect (computed by imgui_layer.c from the current
// display size) so the panel tracks window resizes.
void DebugPanel_Draw(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_DEBUG_PANEL_H

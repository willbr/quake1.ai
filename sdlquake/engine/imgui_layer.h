// imgui_layer.h -- C interface to the Dear ImGui dev overlay

#ifndef IMGUI_LAYER_H
#define IMGUI_LAYER_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call from VID_Init / VID_Shutdown.
void ImguiLayer_Init(SDL_Window *w, SDL_Renderer *r);
void ImguiLayer_Shutdown(void);

// Feed every SDL event before dispatching to Quake.
// Returns 1 if ImGui consumed the event (caller may skip Quake dispatch).
int  ImguiLayer_ProcessEvent(SDL_Event *ev);

// Call from VID_Update, after SDL_RenderTexture and before SDL_RenderPresent.
void ImguiLayer_Render(void);

// Toggle the overlay (F12).
void ImguiLayer_Toggle(void);

// Query state.
int  ImguiLayer_IsOpen(void);
int  ImguiLayer_WantsMouse(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_LAYER_H

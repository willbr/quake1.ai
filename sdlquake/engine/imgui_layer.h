// imgui_layer.h -- C interface to the Dear ImGui dev overlay

#ifndef IMGUI_LAYER_H
#define IMGUI_LAYER_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call from VID_Init / VID_Shutdown.
void ImguiLayer_Init(SDL_Window *w, SDL_GPUDevice *gpu, SDL_GPUTextureFormat swapchain_fmt);
void ImguiLayer_Shutdown(void);

// Feed every SDL event before dispatching to Quake.
// Returns 1 if ImGui consumed the event (caller may skip Quake dispatch).
int  ImguiLayer_ProcessEvent(SDL_Event *ev);

// Two-phase render under SDL_GPU. PrepareGPU must be called BEFORE opening
// the render pass (it uploads ImGui's vertex/index buffers). RenderGPU emits
// the draw commands inside an active render pass.
void ImguiLayer_PrepareGPU(SDL_GPUCommandBuffer *cmd);
void ImguiLayer_RenderGPU (SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *pass);

// Toggle the overlay (F3).
void ImguiLayer_Toggle(void);

// Query state.
int  ImguiLayer_IsOpen(void);
int  ImguiLayer_WantsMouse(void);

#ifdef __cplusplus
}
#endif

#endif // IMGUI_LAYER_H

// editor.h -- public C API for the Phase 7 in-game .map editor.
//
// Engine boundary calls live here. Everything else (scene state, brush compile,
// .map I/O, ImGui panels, wireframe render, gizmo) is editor-internal.

#ifndef EDITOR_H
#define EDITOR_H

#ifdef __cplusplus
extern "C" {
#endif

// Lifecycle. Call from Host_Init / Host_Shutdown.
void Editor_Init    (void);
void Editor_Shutdown(void);

// F2 toggle; mirrors ImguiLayer_Toggle but additionally opens the editor mode
// (UI panels, gizmo input, sim pause).
void Editor_Toggle  (void);

// Per-frame queries used by the engine to gate input/sim/render.
int  Editor_IsOpen  (void);
int  Editor_IsPaused(void); // OR'd into host.c sim-pause check

// Render hook: called from R_RenderView_ after R_DrawEntitiesOnList.
// Always runs (even when editor is closed) so saved edits stay visible
// during play. Cull-checks the camera against each brush's bbox.
void Editor_RenderScene(void);

// Pre-render hook: called from R_RenderView_ BEFORE R_SetupFrame so we can
// override r_refdef.vieworg / viewangles when in free-fly camera mode.
// Also reads keyboard / mouse delta to drive the editor camera.
void Editor_PreRender(void);

// 1 when the editor's RMB-look mode is active (cursor captured for
// mouse-look). in_sdl.c uses this to decide whether ImGui's keyboard /
// mouse capture should be bypassed and routed to the player input.
int  Editor_AllowGameInput(void);
int  Editor_LookmodeActive(void);

// Cycle editor_camera between 0 (free-fly) and 1 (FPS). Bound to Tab in
// in_sdl.c.
void Editor_CycleCameraMode(void);

// 1 when the engine should render the local player's model. Quake normally
// culls cl_entities[cl.viewentity] (you don't see your own legs), but in
// free-fly mode the camera has detached so we want it visible.
int  Editor_ShouldDrawPlayer(void);

// 1 when the editor is open in "map" view-mode — gameplay-transient FX
// (particles, rocket/fireball trails, explosions) should be suppressed so
// the authoring view shows only what's in the .map.
int  Editor_HideTransientFX(void);

// ImGui hook: called from ImguiLayer_Render between NewFrame and Render.
// Editor toolbar/inspector/brush list panels.
void Editor_DrawUI(void);

// SDL event hook: called from in_sdl.c IN_ProcessEvents, AFTER ImGui has had
// a chance to consume the event. The pointer is an SDL_Event*. Use void* in
// the signature so this header doesn't have to drag SDL3 in.
// Returns 1 if the editor consumed it.
int  Editor_ProcessEvent(void *sdl_event);

#ifdef __cplusplus
}
#endif

#endif // EDITOR_H

// editor_mode.h -- vtable describing one editor mode (Map, Particle, future
// Model/Texture). The shell (editor.c) holds a table of these and dispatches
// mode-specific behavior to the active one. Shared infrastructure (free-fly
// camera, open/close, look-mode) stays in the shell, NOT in the vtable.

#ifndef EDITOR_MODE_H
#define EDITOR_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct editor_mode_s {
    const char *name;                 // "Map", "Particle" -- shown in the switcher

    void (*enter)(void);              // optional; called when this mode becomes active
    void (*exit)(void);               // optional; called when leaving this mode

    void (*draw_ui)(void);            // ImGui panels for this mode (required)
    void (*render_scene)(void);       // 3D overlay for this mode (optional)
    int  (*process_event)(void *ev);  // SDL_Event*; return 1 if consumed (optional)

    // Per-mode policy queries. NULL => shell uses the documented default.
    int  (*hide_transient_fx)(void);  // Map=1 (hide), Particle=0 (show). default 1
    int  (*should_draw_player)(void); // default: shell's free-fly logic
} editor_mode_t;

#ifdef __cplusplus
}
#endif

#endif // EDITOR_MODE_H

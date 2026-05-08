// in_sdl.c -- SDL3 input driver replacing in_win.c

#include <SDL3/SDL.h>
#include "quakedef.h"
#include "winquake.h"
#include "imgui_layer.h"
#include "editor.h"

// Winsock lib is not used in our SDL net layer
qboolean winsock_lib_initialized = false;

// Accumulated mouse movement between frames
static int mouse_dx, mouse_dy;
static qboolean mouse_active = false;

cvar_t m_filter = {"m_filter", "0"};

// ---------------------------------------------------------------------------
// SDL scancode -> Quake key mapping
// ---------------------------------------------------------------------------

static int sdl_scancode_to_quake(SDL_Scancode sc)
{
    switch (sc)
    {
    case SDL_SCANCODE_TAB:          return K_TAB;
    case SDL_SCANCODE_RETURN:       return K_ENTER;
    case SDL_SCANCODE_ESCAPE:       return K_ESCAPE;
    case SDL_SCANCODE_SPACE:        return K_SPACE;
    case SDL_SCANCODE_BACKSPACE:    return K_BACKSPACE;
    case SDL_SCANCODE_UP:           return K_UPARROW;
    case SDL_SCANCODE_DOWN:         return K_DOWNARROW;
    case SDL_SCANCODE_LEFT:         return K_LEFTARROW;
    case SDL_SCANCODE_RIGHT:        return K_RIGHTARROW;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:         return K_ALT;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:        return K_CTRL;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:       return K_SHIFT;
    case SDL_SCANCODE_F1:           return K_F1;
    case SDL_SCANCODE_F2:           return K_F2;
    case SDL_SCANCODE_F3:           return K_F3;
    case SDL_SCANCODE_F4:           return K_F4;
    case SDL_SCANCODE_F5:           return K_F5;
    case SDL_SCANCODE_F6:           return K_F6;
    case SDL_SCANCODE_F7:           return K_F7;
    case SDL_SCANCODE_F8:           return K_F8;
    case SDL_SCANCODE_F9:           return K_F9;
    case SDL_SCANCODE_F10:          return K_F10;
    case SDL_SCANCODE_F11:          return K_F11;
    case SDL_SCANCODE_F12:          return K_F12;
    case SDL_SCANCODE_INSERT:       return K_INS;
    case SDL_SCANCODE_DELETE:       return K_DEL;
    case SDL_SCANCODE_PAGEDOWN:     return K_PGDN;
    case SDL_SCANCODE_PAGEUP:       return K_PGUP;
    case SDL_SCANCODE_HOME:         return K_HOME;
    case SDL_SCANCODE_END:          return K_END;
    case SDL_SCANCODE_PAUSE:        return K_PAUSE;
    default:
        // Map printable ASCII characters by keycode
        return -1;
    }
}

// ---------------------------------------------------------------------------
// Event processing (called from Sys_SendKeyEvents each frame)
// ---------------------------------------------------------------------------

void IN_ProcessEvents(void)
{
    extern SDL_Window *VID_GetWindow(void);

    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        // Feed every event to ImGui before Quake sees it.
        ImguiLayer_ProcessEvent(&ev);

        // Editor consumes mouse events for picking + gizmo while open. Lives
        // BEFORE the type switch so it can intercept clicks ImGui didn't
        // capture (i.e. clicks in the 3D viewport).
        if (Editor_ProcessEvent(&ev))
            continue;

        switch (ev.type)
        {
        case SDL_EVENT_QUIT:
            Sys_Quit();
            break;

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        {
            // F12 toggles the dev overlay; F2 toggles the in-game .map editor.
            // Neither key reaches the game.
            if (ev.key.scancode == SDL_SCANCODE_F12)
            {
                if (ev.key.type == SDL_EVENT_KEY_DOWN)
                    ImguiLayer_Toggle();
                break;
            }
            if (ev.key.scancode == SDL_SCANCODE_F2)
            {
                if (ev.key.type == SDL_EVENT_KEY_DOWN)
                    Editor_Toggle();
                break;
            }

            // Overlay open — all other keys go to ImGui only.
            if (ImguiLayer_IsOpen()) break;

            qboolean down = (ev.key.type == SDL_EVENT_KEY_DOWN);
            int qkey = sdl_scancode_to_quake(ev.key.scancode);
            if (qkey == -1)
            {
                // Fall back to keycode for ASCII printable
                int kc = ev.key.key;
                if (kc >= 32 && kc < 127)
                    qkey = kc;
                else if (kc == SDLK_GRAVE) // backtick = console toggle
                    qkey = '`';
            }
            if (qkey != -1)
                Key_Event(qkey, down);
            break;
        }

        case SDL_EVENT_MOUSE_MOTION:
            // Don't accumulate game mouse when overlay is open (mouse is free).
            if (mouse_active && !ImguiLayer_IsOpen())
            {
                mouse_dx += (int)ev.motion.xrel;
                mouse_dy += (int)ev.motion.yrel;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            if (ImguiLayer_IsOpen()) break;
            qboolean down = (ev.button.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            int btn = 0;
            switch (ev.button.button)
            {
            case SDL_BUTTON_LEFT:   btn = K_MOUSE1; break;
            case SDL_BUTTON_RIGHT:  btn = K_MOUSE2; break;
            case SDL_BUTTON_MIDDLE: btn = K_MOUSE3; break;
            }
            if (btn) Key_Event(btn, down);
            break;
        }

        case SDL_EVENT_MOUSE_WHEEL:
            if (ImguiLayer_IsOpen()) break;
            if (ev.wheel.y > 0)
            {
                Key_Event(K_MWHEELUP, true);
                Key_Event(K_MWHEELUP, false);
            }
            else if (ev.wheel.y < 0)
            {
                Key_Event(K_MWHEELDOWN, true);
                Key_Event(K_MWHEELDOWN, false);
            }
            break;

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        {
            SDL_Window *win = VID_GetWindow();
            if (win) SDL_SetWindowRelativeMouseMode(win, true);
            mouse_active = true;
            break;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        {
            SDL_Window *win = VID_GetWindow();
            if (win) SDL_SetWindowRelativeMouseMode(win, false);
            mouse_active = false;
            mouse_dx = mouse_dy = 0;
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// IN interface
// ---------------------------------------------------------------------------

void IN_Init(void)
{
    extern SDL_Window *VID_GetWindow(void);
    Cvar_RegisterVariable(&m_filter);
    SDL_Window *win = VID_GetWindow();
    if (win)
        SDL_SetWindowRelativeMouseMode(win, true);
    mouse_active = true;
}

void IN_Shutdown(void)
{
    extern SDL_Window *VID_GetWindow(void);
    SDL_Window *win = VID_GetWindow();
    if (win)
        SDL_SetWindowRelativeMouseMode(win, false);
    mouse_active = false;
}

void IN_Commands(void) {}

void IN_Move(usercmd_t *cmd)
{
    extern void V_StopPitchDrift(void);
    extern cvar_t sensitivity, m_pitch, m_yaw;

    if (!mouse_active)
        return;

    // Keep nodrift=true every frame so V_DriftPitch never overrides mouse pitch,
    // including the re-trigger that fires after walking forward for v_centermove seconds.
    V_StopPitchDrift();

    int dx = mouse_dx, dy = mouse_dy;
    mouse_dx = 0;
    mouse_dy = 0;

    cl.viewangles[YAW]   -= m_yaw.value  * dx * sensitivity.value;
    cl.viewangles[PITCH] += m_pitch.value * dy * sensitivity.value;

    if (cl.viewangles[PITCH] > 80)  cl.viewangles[PITCH] = 80;
    if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
}

void IN_ClearStates(void)
{
    mouse_dx = 0;
    mouse_dy = 0;
}

// IN_Accumulate is called from S_ExtraUpdate; SDL uses relative mouse so nothing to accumulate here.
void IN_Accumulate(void) {}

// Stubs for Win32 mouse-control functions not needed with SDL relative mode
void IN_ShowMouse(void)               {}
void IN_DeactivateMouse(void)         {}
void IN_HideMouse(void)               {}
void IN_ActivateMouse(void)           {}
void IN_RestoreOriginalMouseState(void) {}
void IN_SetQuakeMouseState(void)      {}
void IN_MouseEvent(int mstate)        { (void)mstate; }

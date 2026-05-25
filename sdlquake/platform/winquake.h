// winquake.h -- stub replacing sdlquake/engine_src/winquake.h
// Provides the same interface without DirectX/DirectDraw/DirectSound headers.

#pragma once

#ifdef _WIN32
#include <windows.h>
#else
// Non-Windows: provide tiny no-op equivalents of the Win32 typedefs that
// engine source still mentions through this header. The compiled engine
// .c files don't use these types directly (they go through Sys_/SDL_),
// so opaque void* is sufficient.
#include <stdint.h>

typedef void *HINSTANCE;
typedef void *HWND;
typedef void *HMODULE;
typedef void *HDC;
typedef void *HGLRC;
typedef void *HCURSOR;
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t  BYTE;
typedef int      BOOL;
typedef const char *LPCSTR;
typedef char       *LPSTR;
#endif

// ---------------------------------------------------------------------------
// Externs expected by engine files — defined in our platform .c files.
// DirectSound/DirectDraw bindings are gone (snd_sdl owns audio, vid_sdl owns
// the framebuffer); only the cross-platform glue the engine still touches
// stays here.
// ---------------------------------------------------------------------------

extern HINSTANCE global_hInstance;
extern int       global_nCmdShow;

#if defined(_WIN32) && !defined(WINDED)
void VID_LockBuffer   (void);
void VID_UnlockBuffer (void);
#endif

typedef enum { MS_WINDOWED, MS_FULLSCREEN, MS_FULLDIB, MS_UNINIT } modestate_t;
extern modestate_t modestate;

extern HWND     mainwindow;
extern qboolean ActiveApp, Minimized;
extern qboolean WinNT;

extern qboolean  winsock_lib_initialized;
extern cvar_t    _windowed_mouse;

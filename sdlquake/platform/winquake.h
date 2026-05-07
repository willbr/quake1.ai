// winquake.h -- stub replacing sdlquake/engine_src/winquake.h
// Provides the same interface without DirectX/DirectDraw/DirectSound headers.

#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// Minimal DirectSound/DirectDraw stubs (pDSBuf is always NULL at runtime)
// ---------------------------------------------------------------------------

#define DS_OK  0
#define DD_OK  0
#define DSBPLAY_LOOPING 1

typedef struct IDirectSoundBufferVtbl_s {
    // IUnknown (0-2)
    void *QueryInterface, *AddRef, *Release;
    // IDirectSoundBuffer (3-20)
    void *GetCaps;
    long (*GetCurrentPosition)(void *s, DWORD *cur, DWORD *write);
    void *GetFormat, *GetVolume, *GetPan, *GetFrequency;
    long (*GetStatus)(void *s, DWORD *status);
    void *Initialize;
    long (*Lock)(void *s, DWORD start, DWORD size, void **p1, DWORD *l1, void **p2, DWORD *l2, DWORD flags);
    long (*Play)(void *s, DWORD r, DWORD pri, DWORD flags);
    void *SetCurrentPosition, *SetFormat, *SetVolume, *SetPan, *SetFrequency, *Stop;
    long (*Unlock)(void *s, void *p1, DWORD l1, void *p2, DWORD l2);
    long (*Restore)(void *s);
} IDirectSoundBufferVtbl;

typedef struct IDirectSoundBuffer_s { IDirectSoundBufferVtbl *lpVtbl; } IDirectSoundBuffer;
typedef IDirectSoundBuffer *LPDIRECTSOUNDBUFFER;

typedef void *LPDIRECTDRAW;
typedef void *LPDIRECTDRAWSURFACE;
typedef void *LPDIRECTDRAWPALETTE;
typedef void *LPDIRECTSOUND;

// ---------------------------------------------------------------------------
// Externs expected by engine files — defined in our platform .c files
// ---------------------------------------------------------------------------

extern HINSTANCE global_hInstance;
extern int       global_nCmdShow;

extern LPDIRECTDRAW          lpDD;
extern qboolean              DDActive;
extern LPDIRECTDRAWSURFACE   lpPrimary;
extern LPDIRECTDRAWSURFACE   lpFrontBuffer;
extern LPDIRECTDRAWSURFACE   lpBackBuffer;
extern LPDIRECTDRAWPALETTE   lpDDPal;
extern LPDIRECTSOUND         pDS;
extern LPDIRECTSOUNDBUFFER   pDSBuf;
extern DWORD                 gSndBufSize;

void VID_LockBuffer   (void);
void VID_UnlockBuffer (void);

typedef enum { MS_WINDOWED, MS_FULLSCREEN, MS_FULLDIB, MS_UNINIT } modestate_t;
extern modestate_t modestate;

extern HWND     mainwindow;
extern qboolean ActiveApp, Minimized;
extern qboolean WinNT;

int  VID_ForceUnlockedAndReturnState (void);
void VID_ForceLockState (int lk);

void IN_ShowMouse              (void);
void IN_DeactivateMouse        (void);
void IN_HideMouse              (void);
void IN_ActivateMouse          (void);
void IN_RestoreOriginalMouseState (void);
void IN_SetQuakeMouseState     (void);
void IN_MouseEvent             (int mstate);

extern qboolean  winsock_lib_initialized;
extern cvar_t    _windowed_mouse;

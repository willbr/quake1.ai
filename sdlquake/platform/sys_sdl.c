// sys_sdl.c -- SDL3 system layer replacing sys_win.c

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(p,m) _mkdir(p)
#else
#include <sys/stat.h>
#endif

#include "quakedef.h"

#define MINIMUM_MEMORY  0x0880000
#define MAXIMUM_MEMORY  0x1000000

qboolean isDedicated;

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

#define MAX_HANDLES 10
static FILE *sys_handles[MAX_HANDLES];

static int findhandle(void)
{
    int i;
    for (i = 1; i < MAX_HANDLES; i++)
        if (!sys_handles[i])
            return i;
    Sys_Error("out of handles");
    return -1;
}

static int sys_filelength(FILE *f)
{
    int pos = ftell(f);
    fseek(f, 0, SEEK_END);
    int end = ftell(f);
    fseek(f, pos, SEEK_SET);
    return end;
}

int Sys_FileOpenRead(char *path, int *hndl)
{
    int i = findhandle();
    FILE *f = fopen(path, "rb");
    if (!f) { *hndl = -1; return -1; }
    sys_handles[i] = f;
    *hndl = i;
    return sys_filelength(f);
}

int Sys_FileOpenWrite(char *path)
{
    int i = findhandle();
    FILE *f = fopen(path, "wb");
    if (!f) Sys_Error("Error opening %s: %s", path, strerror(errno));
    sys_handles[i] = f;
    return i;
}

void Sys_FileClose(int handle)   { fclose(sys_handles[handle]); sys_handles[handle] = NULL; }
void Sys_FileSeek(int handle, int position) { fseek(sys_handles[handle], position, SEEK_SET); }
int  Sys_FileRead(int handle, void *dest, int count)  { return fread(dest, 1, count, sys_handles[handle]); }
int  Sys_FileWrite(int handle, void *data, int count) { return fwrite(data, 1, count, sys_handles[handle]); }

int Sys_FileTime(char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return -1;
}

void Sys_mkdir(char *path) { mkdir(path, 0755); }

// ---------------------------------------------------------------------------
// System I/O
// ---------------------------------------------------------------------------

void Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length) { (void)startaddr; (void)length; }
void Sys_DebugLog(char *file, char *fmt, ...) { (void)file; (void)fmt; }

void Sys_Error(char *error, ...)
{
    va_list argptr;
    char text[1024];
    va_start(argptr, error);
    vsnprintf(text, sizeof(text), error, argptr);
    va_end(argptr);
    fprintf(stderr, "Quake Error: %s\n", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Quake Error", text, NULL);
    Host_Shutdown();
    SDL_Quit();
    exit(1);
}

void Sys_Printf(char *fmt, ...)
{
    va_list argptr;
    char text[1024];
    va_start(argptr, fmt);
    vsnprintf(text, sizeof(text), fmt, argptr);
    va_end(argptr);
    fputs(text, stdout);
    fflush(stdout);
}

void Sys_Quit(void)
{
    Host_Shutdown();
    SDL_Quit();
    exit(0);
}

double Sys_FloatTime(void)
{
    static Uint64 start_ns = 0;
    Uint64 now = SDL_GetTicksNS();
    if (!start_ns) start_ns = now;
    return (double)(now - start_ns) * 1e-9;
}

char *Sys_ConsoleInput(void) { return NULL; }

void Sys_Sleep(void) { SDL_Delay(1); }

// Sys_SendKeyEvents is called from the main loop to pump SDL events.
// Actual key/mouse dispatch happens here; input module accumulates mouse.
void Sys_SendKeyEvents(void)
{
    extern void IN_ProcessEvents(void);
    IN_ProcessEvents();
}

void Sys_LowFPPrecision(void)  {}
void Sys_HighFPPrecision(void) {}
void Sys_SetFPCW(void)         {}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static char *argv_buf[MAX_NUM_ARGVS];

int main(int argc, char **argv)
{
    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    quakeparms_t parms;
    memset(&parms, 0, sizeof(parms));

    static char cwd[1024];
    if (!SDL_GetCurrentDirectory())
        parms.basedir = ".";
    else
    {
        // SDL_GetCurrentDirectory allocates; copy it
        const char *sdl_cwd = SDL_GetCurrentDirectory();
        strncpy(cwd, sdl_cwd, sizeof(cwd) - 1);
        // remove trailing slash
        int len = strlen(cwd);
        if (len > 1 && (cwd[len-1] == '/' || cwd[len-1] == '\\'))
            cwd[len-1] = '\0';
        parms.basedir = cwd;
        SDL_free((void*)sdl_cwd);
    }
    parms.cachedir = NULL;

    // copy argv
    int n = argc < MAX_NUM_ARGVS ? argc : MAX_NUM_ARGVS;
    for (int i = 0; i < n; i++) argv_buf[i] = argv[i];
    parms.argc = n;
    parms.argv = argv_buf;

    COM_InitArgv(parms.argc, parms.argv);
    parms.argc = com_argc;
    parms.argv = com_argv;

    isDedicated = (COM_CheckParm("-dedicated") != 0);

    parms.memsize = MINIMUM_MEMORY;
    {
        // give it 16 MB by default, or whatever -heapsize says
        parms.memsize = 16 * 1024 * 1024;
        int t = COM_CheckParm("-heapsize");
        if (t && t + 1 < com_argc)
            parms.memsize = Q_atoi(com_argv[t+1]) * 1024;
    }
    parms.membase = malloc(parms.memsize);
    if (!parms.membase)
        Sys_Error("Not enough memory: %d bytes requested", parms.memsize);

    Sys_Printf("Host_Init\n");
    Host_Init(&parms);

    double oldtime = Sys_FloatTime();
    while (1)
    {
        double newtime = Sys_FloatTime();
        double dt = newtime - oldtime;
        Host_Frame(dt);
        oldtime = newtime;
    }

    return 0;
}

// sys_sdl.c -- SDL3 system layer replacing sys_win.c

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <errno.h>

#include "mcp_server.h"
#include "hotreload.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir(p,m) _mkdir(p)
#else
#include <sys/stat.h>
#include <execinfo.h>
#include <unistd.h>
#endif

#include "quakedef.h"

#define MINIMUM_MEMORY  0x0880000
#define MAXIMUM_MEMORY  0x1000000

qboolean isDedicated;
qboolean sys_headless;  // set when running in --list-cvars mode (no window, no audio)

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

// Bridge SDL_EnumerateDirectory to the engine's Sys_EnumDir_cb_t. SDL invokes
// the callback once per entry (subdirs included); we filter to regular files
// by stat'ing each one — cheap for the dirs we walk (id1/maps/).
typedef struct
{
    Sys_EnumDir_cb_t cb;
    void            *userdata;
    const char      *root;
} sys_enum_ctx_t;

static SDL_EnumerationResult SDLCALL sys_enum_thunk(void *userdata, const char *dirname, const char *fname)
{
    sys_enum_ctx_t *ctx = (sys_enum_ctx_t *)userdata;
    SDL_PathInfo info;
    char fullpath[1024];

    (void)dirname;
    if (!fname || fname[0] == 0) return SDL_ENUM_CONTINUE;
    if (fname[0] == '.' && (fname[1] == 0 || (fname[1] == '.' && fname[2] == 0)))
        return SDL_ENUM_CONTINUE;

    snprintf(fullpath, sizeof(fullpath), "%s/%s", ctx->root, fname);
    if (SDL_GetPathInfo(fullpath, &info) && info.type != SDL_PATHTYPE_FILE)
        return SDL_ENUM_CONTINUE;

    ctx->cb(fname, ctx->userdata);
    return SDL_ENUM_CONTINUE;
}

void Sys_EnumerateDir(const char *path, Sys_EnumDir_cb_t cb, void *userdata)
{
    sys_enum_ctx_t ctx;
    if (!path || !cb) return;
    ctx.cb = cb;
    ctx.userdata = userdata;
    ctx.root = path;
    SDL_EnumerateDirectory(path, sys_enum_thunk, &ctx);
}

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
#ifndef _WIN32
    {
        void *frames[64];
        int n = backtrace(frames, 64);
        fprintf(stderr, "Backtrace (%d frames):\n", n);
        fflush(stderr);
        backtrace_symbols_fd(frames, n, fileno(stderr));
    }
#endif
    if (!mcp_active && !sys_headless)
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
    // In MCP / --list-cvars mode stdout is reserved for output; send logs to stderr.
    FILE *out = (mcp_active || sys_headless) ? stderr : stdout;
    fputs(text, out);
    fflush(out);
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

extern void Sys_InstallCrashHandler(void);

static int cvar_name_cmp(const void *a, const void *b)
{
    const cvar_t *ca = *(const cvar_t * const *)a;
    const cvar_t *cb = *(const cvar_t * const *)b;
    return strcmp(ca->name, cb->name);
}

static void dump_cvars_and_exit(void)
{
    int n = 0;
    for (cvar_t *v = cvar_vars; v; v = v->next) n++;

    cvar_t **all = (cvar_t **)malloc(n * sizeof(cvar_t *));
    if (!all) { fprintf(stderr, "out of memory\n"); exit(1); }
    int i = 0;
    for (cvar_t *v = cvar_vars; v; v = v->next) all[i++] = v;
    qsort(all, n, sizeof(cvar_t *), cvar_name_cmp);

    for (i = 0; i < n; i++)
    {
        cvar_t *v = all[i];
        const char *flags = v->archive && v->server ? " [archive,server]"
                          : v->archive              ? " [archive]"
                          : v->server               ? " [server]"
                          : "";
        printf("%-28s \"%s\"%s\n", v->name, v->string, flags);
    }
    fflush(stdout);
    free(all);
    exit(0);
}

static void print_usage(void)
{
    fputs(
        "Usage: quake [options] [+command ...]\n"
        "\n"
        "Platform options:\n"
        "  --mcp-stdio          Run MCP server on stdio (logs go to stderr)\n"
        "  --mcp-http <port>    Set the `mcp_http_port` cvar default; HTTP/SSE\n"
        "                       server starts at first frame on localhost:PORT\n"
        "  --hot-reload         Poll game.dll for changes and reload at runtime\n"
        "  --list-cvars         Print all registered cvars and their values, then exit\n"
        "  -dedicated           Run as dedicated server (no video/audio)\n"
        "  -heapsize <KB>       Engine heap size in KB (default 16384)\n"
        "  -basedir <path>      Override base directory (default: cwd)\n"
        "  --help, -h, -?       Show this help and exit\n"
        "\n"
        "Engine options (passed through to Quake):\n"
        "  -game <moddir>       Load mod from <moddir> (e.g. rogue, hipnotic)\n"
        "  -window              Force windowed mode\n"
        "  -fullscreen          Force fullscreen mode\n"
        "  -port <n>            Network port (default 26000)\n"
        "  -nosound             Disable audio\n"
        "  -nocdaudio           Disable CD music\n"
        "\n"
        "Console commands (start with '+'):\n"
        "  +map <name>          Load map at startup, e.g. +map e1m1\n"
        "  +connect <addr>      Connect to server at startup\n"
        "  +<cvar> <value>      Set a cvar at startup, e.g. +sv_gravity 400\n"
        "\n"
        "Examples:\n"
        "  quake +map e1m1\n"
        "  quake --hot-reload +map start\n"
        "  quake --mcp-stdio -dedicated +map e1m1\n"
        "  quake -game rogue +map ctf1\n",
        stdout);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-help") ||
            !strcmp(argv[i], "-h") || !strcmp(argv[i], "-?"))
        {
            print_usage();
            return 0;
        }
    }

    Sys_InstallCrashHandler();
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

    int list_cvars = COM_CheckParm("--list-cvars");
    if (list_cvars)
        sys_headless = true;

    // --mcp-http <port> just seeds the mcp_http_port cvar's default; the HTTP
    // listener is started lazily from MCP_Frame once Host_Init has registered
    // the cvar. --mcp-stdio still needs to bind stdin pre-Host_Init so
    // engine printf output is routed away from the response stream.
    if (!list_cvars)
    {
        int mcp_http_idx = COM_CheckParm("--mcp-http");
        if (mcp_http_idx && mcp_http_idx + 1 < com_argc)
            MCP_SetHttpPortDefault(atoi(com_argv[mcp_http_idx + 1]));
        else if (COM_CheckParm("--mcp-stdio"))
            MCP_Init(0);
    }

    parms.memsize = MINIMUM_MEMORY;
    {
        // 512 MB default. VID reserves ~20 MB (SSAA headroom). The stain
        // pool at STAIN_CELL_SHIFT==0 can reach ~243 MB at default r_decals_max=512
        // (1u cells × 288² × 3 × int16 × 512 slots). Override with -heapsize.
        parms.memsize = 512 * 1024 * 1024;
        int t = COM_CheckParm("-heapsize");
        if (t && t + 1 < com_argc)
            parms.memsize = Q_atoi(com_argv[t+1]) * 1024;
    }
    parms.membase = malloc(parms.memsize);
    if (!parms.membase)
        Sys_Error("Not enough memory: %d bytes requested", parms.memsize);

    Sys_Printf("Host_Init\n");
    Host_Init(&parms);
    MCP_RegisterCvars();
    HotReload_Init();
    if (list_cvars)
        dump_cvars_and_exit();
    if (COM_CheckParm("--hot-reload"))
        HotReload_EnablePolling();

    double oldtime = Sys_FloatTime();
    while (1)
    {
        double newtime = Sys_FloatTime();
        double dt = newtime - oldtime;
        Host_Frame(dt);
        HotReload_Frame((float)dt);
        MCP_Frame();   // unconditional: drives the mcp_http_port lazy-start
        oldtime = newtime;
    }

    return 0;
}

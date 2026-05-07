// sys_crash.c -- in-process unhandled-exception filter that dumps a
// symbolicated backtrace to stderr before the process dies.
//
// Quake's Sys_Error path is graceful (message + exit(1)). This handler
// only runs for *real* crashes: access violations, illegal instructions,
// FP/integer faults, stack overflow, etc. — anything that would otherwise
// kill the process silently on Windows.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    default:                                 return "UNKNOWN";
    }
}

// Build a search path that gives dbghelp every chance of finding our PDBs.
// dbghelp's default doesn't necessarily include the working directory, and
// game_loaded.dll has no game_loaded.pdb (it's a copy of game.dll, whose PDB
// is game.pdb) — so we point at zig-out/bin/ explicitly.
static void build_search_path(char *out, size_t cap)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (n == 0 || n >= sizeof(exe)) { out[0] = 0; return; }

    // strip filename
    for (DWORD i = n; i > 0; i--) {
        if (exe[i-1] == '\\' || exe[i-1] == '/') { exe[i-1] = 0; break; }
    }
    // exe dir + cwd + cwd's zig-out\bin (in case run from project root)
    snprintf(out, cap, "%s;.;zig-out\\bin", exe);
}

static BOOL CALLBACK module_load_cb(PCSTR name, DWORD64 base, ULONG size, PVOID ctx)
{
    HANDLE process = (HANDLE)ctx;
    // Force eager symbol load. SymLoadModuleEx returns 0 if already loaded;
    // the actual load result is read back via SymGetModuleInfo64 below.
    SymLoadModuleEx(process, NULL, name, NULL, base, size, NULL, 0);
    return TRUE;
}

struct mod_print_ctx {
    HANDLE process;
    int    skipped;
};

static BOOL CALLBACK module_print_cb(PCSTR name, DWORD64 base, ULONG size, PVOID ctx_v)
{
    struct mod_print_ctx *ctx = (struct mod_print_ctx *)ctx_v;
    IMAGEHLP_MODULE64 mi;
    mi.SizeOfStruct = sizeof(mi);
    if (!SymGetModuleInfo64(ctx->process, base, &mi)) {
        ctx->skipped++;
        return TRUE;
    }
    // Only print modules with real debug info — system DLLs are exports-only
    // and would just be noise (60+ entries on Windows).
    const char *sym_kind;
    switch (mi.SymType) {
    case SymPdb:        sym_kind = "PDB"; break;
    case SymCv:         sym_kind = "CodeView"; break;
    case SymCoff:       sym_kind = "COFF"; break;
    case SymDia:        sym_kind = "DIA"; break;
    case SymDeferred:   sym_kind = "deferred"; break;
    default:            ctx->skipped++; return TRUE;
    }
    fprintf(stderr, "  %-20s base=0x%llx size=0x%lx  syms=%s  pdb=%s\n",
            mi.ModuleName,
            (unsigned long long)base,
            (unsigned long)size,
            sym_kind,
            mi.LoadedPdbName[0] ? mi.LoadedPdbName : "(none)");
    return TRUE;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    HANDLE process = GetCurrentProcess();
    HANDLE thread  = GetCurrentThread();

    char search_path[MAX_PATH * 2];
    build_search_path(search_path, sizeof(search_path));

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(process, search_path[0] ? search_path : NULL, TRUE);
    SymRefreshModuleList(process);
    EnumerateLoadedModules64(process, module_load_cb, process);

    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    fprintf(stderr,
            "\n=== Unhandled exception: %s (0x%08lx) at %p ===\n",
            exception_name(er->ExceptionCode),
            (unsigned long)er->ExceptionCode,
            er->ExceptionAddress);

    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
    {
        const char *op = er->ExceptionInformation[0] == 0 ? "read"
                       : er->ExceptionInformation[0] == 1 ? "write"
                       : er->ExceptionInformation[0] == 8 ? "DEP"
                       : "?";
        fprintf(stderr, "  %s at 0x%p\n",
                op, (void*)er->ExceptionInformation[1]);
    }

    fputs("\nModules (with debug info):\n", stderr);
    fprintf(stderr, "  symbol search path: %s\n",
            search_path[0] ? search_path : "(default)");
    {
        struct mod_print_ctx mctx = { process, 0 };
        EnumerateLoadedModules64(process, module_print_cb, &mctx);
        if (mctx.skipped)
            fprintf(stderr, "  (+ %d system modules with exports-only/no debug info)\n",
                    mctx.skipped);
    }

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame;
    DWORD machine;

    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_IX86) || defined(__i386__)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#else
#  error "unsupported architecture for crash handler"
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    fputs("\nStack trace:\n", stderr);

    char sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    SYMBOL_INFO *sym = (SYMBOL_INFO*)sym_buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = MAX_SYM_NAME;

    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, process, thread, &frame, &ctx,
                         NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0)
            break;

        DWORD64 disp = 0;
        const char *name = "??";
        if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym))
            name = sym->Name;

        const char *modname = "";
        IMAGEHLP_MODULE64 mi;
        mi.SizeOfStruct = sizeof(mi);
        if (SymGetModuleInfo64(process, frame.AddrPC.Offset, &mi))
            modname = mi.ModuleName;

        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(line);
        DWORD line_disp = 0;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_disp, &line))
            fprintf(stderr, "  %2d  %s!%s+0x%llx  (%s:%lu)\n",
                    i, modname, name, (unsigned long long)disp,
                    line.FileName, (unsigned long)line.LineNumber);
        else
            fprintf(stderr, "  %2d  %s!%s+0x%llx  [0x%llx]\n",
                    i, modname, name, (unsigned long long)disp,
                    (unsigned long long)frame.AddrPC.Offset);
    }

    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;  // terminate; we've already reported.
}

void Sys_InstallCrashHandler(void)
{
    SetUnhandledExceptionFilter(crash_filter);
}

#else  // !_WIN32

void Sys_InstallCrashHandler(void) {}

#endif

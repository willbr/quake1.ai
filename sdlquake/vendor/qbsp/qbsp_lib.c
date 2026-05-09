/*
 * qbsp_lib.c -- entry point + glue for in-process qbsp compilation.
 *
 * Replaces the old `int main(argc, argv)` in qbsp.c. Sets the longjmp
 * recovery target Error() uses, calls into qbsp's existing ProcessFile()
 * pipeline, and harvests the .bsp bytes that bspfile.c's WriteBSPFile
 * (M1.4) writes to a memory buffer instead of disk.
 *
 * Until M1.4 lands, WriteBSPFile still writes to disk; this function
 * reads the resulting file back into a malloc'd buffer at the end. That
 * disk hop will go away when WriteBSPFile is buffer-aware.
 */

#include "cmdlib.h"
#include "bsp5.h"
#include "qbsp_lib.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Engine-side declaration; we resolve at link time. The qbsp namespace
 * header doesn't redirect these because they're not qbsp's. */
extern void Con_Printf(char *fmt, ...);

/* Set by qbsp_compile_to_memory's setjmp; cleared on exit. Error() in
 * cmdlib.c reads these to bail without exit(). */
jmp_buf *qbsp_err_jmp = NULL;
char     qbsp_err_msg[1024];

/* Globals qbsp.c declares; we touch them from here so the namespace
 * macros aren't needed (this TU includes qbsp_namespace.h via -include
 * just like the rest of the qbsp code). */
extern qboolean drawflag;
extern qboolean nofill;
extern qboolean notjunc;
extern qboolean noclip;
extern qboolean onlyents;
extern qboolean verbose;
extern qboolean allverbose;
extern qboolean usehulls;
extern int      subdivide_size;
extern int      hullnum;
extern char    *argv0;
extern void     ProcessFile(char *sourcename, char *destname);

/* Macro renames printf -> qbsp_con_print in qbsp_namespace.h; this is
 * the actual implementation that lands in the editor's console. */
#include <stdarg.h>
void qbsp_con_print(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* Con_Printf takes a char* fmt; passing a literal %s avoids
     * re-interpreting any % chars qbsp emitted. */
    Con_Printf("%s", buf);
}

static void reset_options_defaults(void)
{
    drawflag      = false;
    nofill        = false;
    notjunc       = false;
    noclip        = false;
    onlyents      = false;
    allverbose    = false;
    usehulls      = false;
    verbose       = true;          /* qbsp's main() default */
    hullnum       = 0;
    subdivide_size = 240;
    argv0         = "qbsp";        /* used by drawing code we'll never hit */
}

static void apply_options(const qbsp_options_t *opts)
{
    if (!opts) return;
    if (opts->notjunc)  notjunc = true;
    if (opts->nofill)   nofill  = true;
    if (opts->noclip)   noclip  = true;
    if (opts->onlyents) onlyents = true;
    if (opts->verbose)  allverbose = true;
    if (opts->subdivide > 0) subdivide_size = opts->subdivide;
}

/*
 * Read the freshly-written .bsp from disk into a malloc'd buffer. M1.4
 * replaces WriteBSPFile to skip the disk hop entirely; until then this
 * is the bridge.
 */
static int slurp_bsp(const char *path, void **out_bytes, int *out_size)
{
    FILE *f = fopen(path, "rb");
    long  size;
    void *buf;
    if (!f) {
        Con_Printf("qbsp: failed to read back %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)size);
    if (!buf) { fclose(f); return 1; }
    if ((long)fread(buf, 1, (size_t)size, f) != size) {
        fclose(f); free(buf); return 1;
    }
    fclose(f);
    *out_bytes = buf;
    *out_size  = (int)size;
    return 0;
}

int qbsp_compile_to_memory(const char *map_path,
                           void **out_bsp, int *out_size,
                           const qbsp_options_t *opts)
{
    char    sourcename[1024];
    char    destname[1024];
    jmp_buf err_buf;
    int     dot_idx;

    if (out_bsp)  *out_bsp = NULL;
    if (out_size) *out_size = 0;
    if (!map_path || !out_bsp || !out_size) return 1;

    reset_options_defaults();
    apply_options(opts);

    /* destname is used by ProcessFile for portfile/pointfile/hullfile
     * stems; it strips the extension and adds .bsp itself. We write the
     * .bsp to a sibling of the .map (same dir, .bsp extension) and slurp
     * it back into RAM at the end. M1.4 will eliminate this disk hop. */
    snprintf(sourcename, sizeof(sourcename), "%s", map_path);
    snprintf(destname,   sizeof(destname),   "%s", map_path);
    /* StripExtension/DefaultExtension are qbsp helpers but we want to
     * be defensive about the path string here. Find last '.' and chop. */
    {
        int i;
        for (i = (int)strlen(destname) - 1, dot_idx = -1; i >= 0; i--) {
            if (destname[i] == '.') { dot_idx = i; break; }
            if (destname[i] == '/' || destname[i] == '\\') break;
        }
        if (dot_idx >= 0) destname[dot_idx] = '\0';
        strncat(destname, ".bsp", sizeof(destname) - strlen(destname) - 1);
    }

    /* Set the longjmp target before any qbsp call. Error() in cmdlib.c
     * checks qbsp_err_jmp and longjmps here on any abort. */
    qbsp_err_jmp    = &err_buf;
    qbsp_err_msg[0] = '\0';
    if (setjmp(err_buf) != 0) {
        Con_Printf("qbsp: %s\n", qbsp_err_msg);
        qbsp_err_jmp = NULL;
        return 1;
    }

    ProcessFile(sourcename, destname);
    qbsp_err_jmp = NULL;

    if (slurp_bsp(destname, out_bsp, out_size) != 0) return 1;
    return 0;
}

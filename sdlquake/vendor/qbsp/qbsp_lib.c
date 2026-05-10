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

/* qbsp_lib.c IMPLEMENTS the tracked allocator and also calls real
 * malloc/free for its own buffers (slurp_bsp's output). #undef the
 * macros that cmdlib.h installed so the rest of this file sees real
 * libc symbols. */
#undef malloc
#undef free

/* Engine-side declaration; we resolve at link time. The qbsp namespace
 * header doesn't redirect these because they're not qbsp's. */
extern void Con_Printf(char *fmt, ...);

/* Set by qbsp_compile_to_memory's setjmp; cleared on exit. Error() in
 * cmdlib.c reads these to bail without exit(). */
jmp_buf *qbsp_err_jmp = NULL;
char     qbsp_err_msg[1024];

/* -------------------------------------------------------------------------
 * Tracked allocator
 *
 * Every qbsp source file gets the cmdlib.h `malloc -> qbsp_malloc` redirect
 * via the forced-include namespace path. This file is the implementation,
 * with its own #undef (above) so the implementation can use real libc.
 *
 * Doubly-linked list of headers prepended to each user allocation. Reset
 * walks the list and frees everything qbsp didn't free explicitly. Free
 * during a compile still works (unlinks in O(1)). */
typedef struct qhdr_s {
    struct qhdr_s *prev, *next;
} qhdr_t;
static qhdr_t qbsp_alloc_head = { &qbsp_alloc_head, &qbsp_alloc_head };

void *qbsp_malloc(size_t size)
{
    qhdr_t *h = (qhdr_t *)malloc(sizeof(qhdr_t) + size);
    if (!h) return NULL;
    h->next = qbsp_alloc_head.next;
    h->prev = &qbsp_alloc_head;
    qbsp_alloc_head.next->prev = h;
    qbsp_alloc_head.next = h;
    return (void *)(h + 1);
}

void qbsp_free(void *p)
{
    qhdr_t *h;
    if (!p) return;
    h = ((qhdr_t *)p) - 1;
    h->prev->next = h->next;
    h->next->prev = h->prev;
    free(h);
}

static void qbsp_free_all_tracked(void)
{
    qhdr_t *h = qbsp_alloc_head.next;
    qhdr_t *next;
    while (h != &qbsp_alloc_head)
    {
        next = h->next;
        free(h);
        h = next;
    }
    qbsp_alloc_head.next = qbsp_alloc_head.prev = &qbsp_alloc_head;
}

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
extern char     gamedir[1024];
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

/* qbsp keeps a fistful of file-scope counters and hash heads across all
 * .c files. Reset them between calls so a second qbsp_compile_to_memory
 * doesn't read stale data. The big arrays (dplanes[], dfaces[],
 * hvertex[], ...) don't need memset — qbsp only reads them up to the
 * corresponding count, which is what we zero. Hash buckets DO need
 * explicit clearing because qbsp walks them by next-pointer, not by
 * count. */

/* bsp5.h + bspfile.h declare most of these; the others are file-scope
 * to specific .c files and need fresh extern declarations here. */
extern int  nummapbrushes, num_entities, nummiptex;
extern int  unget, scriptline;
extern char *script_p;
extern int  num_visleafs, num_visportals;
extern int  headclipnode, firstface;
extern int  subdivides;
extern void *hvert_p;
extern int  leaffaces, nodefaces, splitnodes, c_solid, c_empty, c_water;
extern qboolean usemidsplit;
extern int  numwedges, numwverts, tjuncs, tjuncfaces;
extern int  numbrushfaces, num_hull_points, num_hull_edges;
extern int  c_activefaces, c_peakfaces;
extern int  c_activesurfaces, c_peaksurfaces;
extern int  c_activewindings, c_peakwindings;
extern int  c_activeportals, c_peakportals;
/* hashverts (surfaces.c) and wedge_hash (tjunc.c) are NUM_HASH-sized
 * pointer arrays of file-scope structs we don't have type access to.
 * Declare as opaque void* arrays — the linker resolves to the right
 * memory and we just zero pointers. */
extern void *hashverts[];
extern void *wedge_hash[];

static void qbsp_reset_state(void)
{
    int i;

    /* bspfile.h-declared counts */
    nummodels = 0;
    visdatasize = lightdatasize = texdatasize = entdatasize = 0;
    numleafs = numplanes = numvertexes = numnodes = numtexinfo = 0;
    numfaces = numclipnodes = numedges = nummarksurfaces = numsurfedges = 0;

    /* map.c — zero counters AND the arrays themselves. mapbrushes[]
     * and entities[] have linked-list pointers (b->faces, b->next,
     * e->epairs, e->brushes); the freelist walk above already released
     * the nodes those pointed at, but the array slots still hold
     * dangling pointers. ParseBrush's duplicate-plane check walks
     * b->faces; ParseEntity sets b->next from mapent->brushes; both
     * must start NULL or the walk dereferences freed memory.
     * map.h is already in scope via bsp5.h, so the typed arrays are
     * usable here directly. */
    memset(mapbrushes, 0, sizeof(mapbrushes));
    memset(entities,   0, sizeof(entities));
    memset(miptex,     0, sizeof(miptex));
    nummapbrushes = num_entities = nummiptex = 0;
    unget = scriptline = 0;
    script_p = NULL;

    /* portals.c. outside_node holds a `portals` linked-list head; the
     * list nodes themselves get freed by qbsp_free_all_tracked below,
     * but outside_node's own bookkeeping stays in BSS — needs memset. */
    {
        extern node_t outside_node;
        memset(&outside_node, 0, sizeof(outside_node));
    }
    num_visleafs = num_visportals = 0;

    /* writebsp.c */
    headclipnode = firstface = 0;

    /* surfaces.c — bsp5.h declares c_cornerverts, c_tryedges,
     * firstmodeledge, firstmodelface; rest are file-local. */
    subdivides = c_cornerverts = c_tryedges = 0;
    firstmodeledge = 1;          /* original initial value */
    firstmodelface = 0;
    hvert_p = NULL;
    for (i = 0; i < 64 /* NUM_HASH */; i++) hashverts[i] = NULL;

    /* solidbsp.c */
    leaffaces = nodefaces = splitnodes = 0;
    c_solid = c_empty = c_water = 0;
    usemidsplit = false;

    /* tjunc.c */
    numwedges = numwverts = tjuncs = tjuncfaces = 0;
    for (i = 0; i < 64 /* NUM_HASH */; i++) wedge_hash[i] = NULL;

    /* brush.c — bsp5.h declares numbrushplanes; rest are file-local. */
    numbrushplanes = numbrushfaces = num_hull_points = num_hull_edges = 0;

    /* qbsp.c — bsp5.h declares valid, worldmodel, brushset. */
    c_activefaces = c_peakfaces = 0;
    c_activesurfaces = c_peaksurfaces = 0;
    c_activewindings = c_peakwindings = 0;
    c_activeportals = c_peakportals = 0;
    valid = 0;
    worldmodel = false;
    brushset = NULL;

    /* Walk the malloc tracking list and free anything qbsp didn't
     * release on its own. Includes mface_t/winding_t/face_t/surface_t/
     * portal_t/node_t/brush_t plus the LoadFile + copystring buffers. */
    qbsp_free_all_tracked();
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
    gamedir[0] = '\0';     /* default: empty, WAD lookups fail relative */
    if (!opts) return;
    if (opts->notjunc)  notjunc = true;
    if (opts->nofill)   nofill  = true;
    if (opts->noclip)   noclip  = true;
    if (opts->onlyents) onlyents = true;
    if (opts->verbose)  allverbose = true;
    if (opts->subdivide > 0) subdivide_size = opts->subdivide;
    if (opts->gamedir && opts->gamedir[0])
    {
        size_t n = strlen(opts->gamedir);
        if (n >= sizeof(gamedir)) n = sizeof(gamedir) - 1;
        memcpy(gamedir, opts->gamedir, n);
        gamedir[n] = '\0';
    }
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

    qbsp_reset_state();           /* M2: clear globals + free leaks */
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

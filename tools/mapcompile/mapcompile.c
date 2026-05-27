/*
 * mapcompile.c -- standalone CLI driver for the vendored qbsp + vis + light
 * pipeline. Takes a .map source, writes .bsp + .lit to disk next to it.
 *
 * Mirrors what editor_compile_export does inside the running engine, but
 * without any of the engine's state (no SDL window, no editor scene, no
 * VFS handoff). Useful for batch-compiling test maps from shell scripts.
 *
 * Usage:
 *   mapcompile <basedir> <mapname>
 *
 * Reads:   <basedir>/maps/<mapname>.map
 *          <basedir>/gfx/base.wad        (must already exist; the editor
 *                                         synthesises one — copy from an
 *                                         existing editor session if you
 *                                         don't have it yet.)
 * Writes:  <basedir>/maps/<mapname>.bsp
 *          <basedir>/maps/<mapname>.lit
 *
 * Exit 0 on success, non-zero on any pipeline stage failure.
 */

#include "qbsp_lib.h"
#include "vis_lib.h"
#include "light_lib.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The vendored libs print diagnostics through Con_Printf. Engine builds
 * link this against the real Quake console; the CLI just forwards to
 * stdout so output lands in the shell. */
void Con_Printf(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static int write_file(const char *path, const void *data, int size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "mapcompile: can't open %s for write: %s\n",
                path, strerror(errno));
        return -1;
    }
    if ((int)fwrite(data, 1, (size_t)size, f) != size) {
        fclose(f);
        fprintf(stderr, "mapcompile: short write to %s\n", path);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "mapcompile: fclose %s failed: %s\n",
                path, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,
                "usage: mapcompile <basedir> <mapname>\n"
                "  Reads  <basedir>/maps/<mapname>.map and <basedir>/gfx/base.wad\n"
                "  Writes <basedir>/maps/<mapname>.bsp + .lit\n");
        return 2;
    }
    const char *basedir = argv[1];
    const char *mapname = argv[2];

    char map_path[1024];
    char bsp_path[1024];
    char lit_path[1024];
    snprintf(map_path, sizeof(map_path), "%s/maps/%s.map", basedir, mapname);
    snprintf(bsp_path, sizeof(bsp_path), "%s/maps/%s.bsp", basedir, mapname);
    snprintf(lit_path, sizeof(lit_path), "%s/maps/%s.lit", basedir, mapname);

    /* --- qbsp --- */
    qbsp_options_t qopts;
    memset(&qopts, 0, sizeof(qopts));
    qopts.gamedir = basedir;

    void *bsp_unlit = NULL;
    int   unlit_size = 0;
    int rc = qbsp_compile_to_memory(map_path, &bsp_unlit, &unlit_size, &qopts);
    if (rc != 0 || !bsp_unlit) {
        fprintf(stderr, "mapcompile: qbsp failed (rc=%d)\n", rc);
        return 1;
    }
    printf("mapcompile: qbsp produced %d bytes; running vis...\n", unlit_size);

    /* --- vis --- */
    vis_options_t vopts;
    memset(&vopts, 0, sizeof(vopts));
    vopts.level = 2;
    rc = vis_compile_in_place(&vopts);
    if (rc != 0) {
        fprintf(stderr, "mapcompile: vis failed (rc=%d)\n", rc);
        free(bsp_unlit);
        return 1;
    }
    printf("mapcompile: vis complete; running light...\n");

    /* --- light --- */
    light_options_t lopts;
    memset(&lopts, 0, sizeof(lopts));
    lopts.scaledist  = 1.0f;
    lopts.scalecos   = 0.5f;
    lopts.rangescale = 0.5f;

    void *bsp_lit       = NULL;
    int   lit_bsp_size  = 0;
    void *lit_bytes     = NULL;
    int   lit_bytes_size = 0;
    rc = light_compile_to_memory(&lopts,
                                 &bsp_lit, &lit_bsp_size,
                                 &lit_bytes, &lit_bytes_size);
    free(bsp_unlit);
    if (rc != 0 || !bsp_lit) {
        fprintf(stderr, "mapcompile: light failed (rc=%d)\n", rc);
        if (lit_bytes) free(lit_bytes);
        return 1;
    }

    /* --- write outputs --- */
    if (write_file(bsp_path, bsp_lit, lit_bsp_size) != 0) {
        free(bsp_lit);
        if (lit_bytes) free(lit_bytes);
        return 1;
    }
    printf("mapcompile: wrote %s (%d bytes)\n", bsp_path, lit_bsp_size);

    if (lit_bytes && lit_bytes_size > 0) {
        if (write_file(lit_path, lit_bytes, lit_bytes_size) != 0) {
            free(bsp_lit);
            free(lit_bytes);
            return 1;
        }
        printf("mapcompile: wrote %s (%d bytes)\n", lit_path, lit_bytes_size);
    }

    free(bsp_lit);
    if (lit_bytes) free(lit_bytes);
    return 0;
}

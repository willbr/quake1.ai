/*
r_lut.c -- precomputed RGB -> 8-bit palette lookup table for coloured lighting.

Generated once from host_basepal by exhaustive nearest-Euclidean-RGB match
(64^3 = 262144 entries, 256 KB). Cached to id1/rgbtable.lmp so the ~250 ms
bake only happens once per install. Cache header includes an 8-byte palette
hash; the table is rebuilt automatically if the palette ever changes.
*/

#include "quakedef.h"
#include "r_local.h"

byte rgbtable[64*64*64];
byte basepal_r[256];
byte basepal_g[256];
byte basepal_b[256];

#define RGBTABLE_MAGIC   "RGBT"
#define RGBTABLE_VERSION 1

typedef struct {
    char  magic[4];
    int   version;
    byte  pal_hash[8];
} rgbtable_header_t;

static void
R_ExtractBasepalChannels (void)
{
    int i;
    for (i = 0; i < 256; i++) {
        basepal_r[i] = host_basepal[i*3 + 0];
        basepal_g[i] = host_basepal[i*3 + 1];
        basepal_b[i] = host_basepal[i*3 + 2];
    }
}

/* Cheap 8-byte fingerprint of the 768-byte palette; only needs to detect
   change, not be cryptographically strong. */
static void
R_PaletteHash (byte out[8])
{
    int i, j;
    unsigned int h[2] = {0x9E3779B1u, 0x7F4A7C15u};
    for (i = 0; i < 768; i++) {
        unsigned int x = host_basepal[i];
        h[i & 1] = (h[i & 1] ^ x) * 16777619u + (h[i & 1] >> 13);
    }
    for (j = 0; j < 4; j++) out[j]     = (byte)(h[0] >> (j*8));
    for (j = 0; j < 4; j++) out[4 + j] = (byte)(h[1] >> (j*8));
}

static void
R_BuildRGBTable (void)
{
    int r, g, b, p;
    int best, best_d, d, dr, dg, db;

    Con_Printf ("Building rgbtable.lmp (256 KB, ~250 ms)...\n");

    for (r = 0; r < 64; r++)
        for (g = 0; g < 64; g++)
            for (b = 0; b < 64; b++) {
                best = 0; best_d = 0x7FFFFFFF;
                for (p = 0; p < 256; p++) {
                    dr = (int)basepal_r[p] - (r << 2);
                    dg = (int)basepal_g[p] - (g << 2);
                    db = (int)basepal_b[p] - (b << 2);
                    d = dr*dr + dg*dg + db*db;
                    if (d < best_d) { best_d = d; best = p; }
                }
                rgbtable[(r << 12) | (g << 6) | b] = (byte)best;
            }
}

static qboolean
R_LoadRGBTableCache (const char *path)
{
    FILE *f;
    rgbtable_header_t hdr;
    byte expected_hash[8];

    f = fopen (path, "rb");
    if (!f) return false;

    if (fread (&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }
    if (memcmp (hdr.magic, RGBTABLE_MAGIC, 4) != 0)  { fclose(f); return false; }
    if (hdr.version != RGBTABLE_VERSION)             { fclose(f); return false; }

    R_PaletteHash (expected_hash);
    if (memcmp (hdr.pal_hash, expected_hash, 8) != 0) { fclose(f); return false; }

    if (fread (rgbtable, sizeof(rgbtable), 1, f) != 1) { fclose(f); return false; }
    fclose (f);
    return true;
}

static void
R_SaveRGBTableCache (const char *path)
{
    FILE *f;
    rgbtable_header_t hdr;

    f = fopen (path, "wb");
    if (!f) { Con_Printf ("WARN: could not write %s\n", path); return; }

    memcpy (hdr.magic, RGBTABLE_MAGIC, 4);
    hdr.version = RGBTABLE_VERSION;
    R_PaletteHash (hdr.pal_hash);
    fwrite (&hdr, sizeof(hdr), 1, f);
    fwrite (rgbtable, sizeof(rgbtable), 1, f);
    fclose (f);
}

/* Console command: dump a few LUT entries for sanity-checking. */
static void
R_LUTInfo_f (void)
{
    Con_Printf ("rgbtable[0,0,0]    = %d (palette idx for black)\n",
                rgbtable[0]);
    Con_Printf ("rgbtable[63,63,63] = %d (palette idx for white)\n",
                rgbtable[(63<<12)|(63<<6)|63]);
    Con_Printf ("rgbtable[63,0,0]   = %d (palette idx for pure red)\n",
                rgbtable[(63<<12)]);
    Con_Printf ("rgbtable[0,63,0]   = %d (palette idx for pure green)\n",
                rgbtable[(63<<6)]);
    Con_Printf ("rgbtable[0,0,63]   = %d (palette idx for pure blue)\n",
                rgbtable[63]);
}

void
R_InitRGBTable (void)
{
    char path[MAX_OSPATH];

    R_ExtractBasepalChannels ();

    sprintf (path, "%s/rgbtable.lmp", com_gamedir);

    if (R_LoadRGBTableCache (path)) {
        Con_DPrintf ("Loaded %s\n", path);
    } else {
        R_BuildRGBTable ();
        R_SaveRGBTableCache (path);
    }

    Cmd_AddCommand ("r_lut_info", R_LUTInfo_f);
}

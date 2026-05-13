# Coloured Lighting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Spec:** `docs/superpowers/specs/2026-05-13-coloured-lighting-design.md`.

**Goal:** Add per-channel coloured static lighting via `.lit` sidecar files plus coloured dynamic lights, while keeping the 8-bit screen buffer and all downstream renderer paths untouched.

**Architecture:** A new `R_DrawSurfaceBlock8_mip*_rgb` writer variant produces 8-bit palette indices by multiplying each texel's base palette colour by per-channel interpolated light, quantising to 6 bits per channel, and looking up nearest palette via a precomputed 64³→byte LUT cached to `id1/rgbtable.lmp`. Selection between mono and RGB writers is gated on `surf->rgb_samples != NULL` (set by a new `.lit` loader) plus the `r_coloredlight` cvar.

**Tech Stack:** C (gnu89 `-fcommon`), Zig 0.16 build, SDL3 platform layer, software renderer.

**Verification model:** This codebase has no test suite (per `CLAUDE.md`: "Build success and visual/audio correctness in-game are the verification methods."). Each task ends with an explicit smoke-test step exercising the new code path.

**`.lit` test asset:** Before starting Task 2, download a known-good `e1m1.lit` from a community pack (e.g., the Quake Spasm "lit pack") and place it at `id1/maps/e1m1.lit`. The exact source URL goes in the Task 2 commit message. If no `.lit` is acquired, Tasks 2–5 can still be implemented but their smoke tests can only validate the mono-fallback path.

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `sdlquake/engine_src/r_lut.c` | 64³ RGB→palette LUT generator and `id1/rgbtable.lmp` cache. Owns `rgbtable[]`, `basepal_r/g/b[]`. |
| `sdlquake/engine_src/r_surf_rgb.c` | Four `R_DrawSurfaceBlock8_mip*_rgb` writers and the `R_BuildLightMap_RGB` / `R_AddDynamicLights_RGB` helpers. |
| `sdlquake/engine_src/cl_dlight_colors.h` | Named `vec3_t` constants for muzzle / rocket / explosion / lightning / torch / white. Pure header, no .c. |

**Modified files:**

| Path | Change |
|---|---|
| `build.zig` | Add `r_lut.c` and `r_surf_rgb.c` to the `engine_files` list. |
| `sdlquake/engine_src/model.h` | `msurface_t` gains `byte *rgb_samples`; `struct model_s` gains `byte *rgblightdata`. |
| `sdlquake/engine_src/model.c` | New `Mod_LoadLITFile`; `Mod_LoadLighting` calls it; `Mod_LoadFaces` wires `out->rgb_samples`. |
| `sdlquake/engine_src/r_local.h` | Forward declarations: `rgbtable`, `basepal_r/g/b`, `blocklights_rgb`, the four `*_rgb` writers, `R_BuildLightMap_RGB`, `R_AddDynamicLights_RGB`, `R_InitRGBTable`. |
| `sdlquake/engine_src/r_main.c` | Declare/register `r_coloredlight`, `r_colored_dlights` cvars; call `R_InitRGBTable` from `R_Init`. |
| `sdlquake/engine_src/r_surf.c` | Dispatch in `R_DrawSurface` between mono and RGB writers. (Build/AddDynamicLights helpers live in `r_surf_rgb.c`.) |
| `sdlquake/engine_src/client.h` | `dlight_t` gains `vec3_t color`. |
| `sdlquake/engine_src/cl_main.c` | `CL_AllocDlight` initialises `color={1,1,1}`. Each call site copies a named constant. |
| `sdlquake/engine_src/cl_tent.c` | Each dlight call site copies a named constant. |

No `engine_api_t` change. No `GAME_API_VERSION` bump. Game DLL untouched.

---

## Task 1: RGB→palette LUT and disk cache

**Files:**
- Create: `sdlquake/engine_src/r_lut.c`
- Modify: `sdlquake/engine_src/r_local.h`
- Modify: `sdlquake/engine_src/r_main.c`
- Modify: `build.zig`

- [ ] **Step 1.1: Add forward declarations to `r_local.h`**

Append to `r_local.h` (after the existing `extern unsigned blocklights[18*18];` declaration — find the equivalent location in the file):

```c
// ---- coloured-lighting LUT (r_lut.c) ----
extern byte rgbtable[64*64*64];     // (r<<12)|(g<<6)|b -> nearest palette index
extern byte basepal_r[256];          // host_basepal red   slice
extern byte basepal_g[256];          // host_basepal green slice
extern byte basepal_b[256];          // host_basepal blue  slice

void R_InitRGBTable (void);
```

- [ ] **Step 1.2: Create `r_lut.c`**

Create `sdlquake/engine_src/r_lut.c`:

```c
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
```

- [ ] **Step 1.3: Register the source file in `build.zig`**

Edit `build.zig`. Find the `engine_files` list (around line 30 — `r_aclip.c, r_alias.c, ...`). Add `"r_lut.c"` to the renderer group, e.g. right after `"r_main.c"`:

```zig
        "r_aclip.c", "r_alias.c", "r_bsp.c", "r_draw.c", "r_edge.c",
        "r_efrag.c", "r_light.c", "r_lut.c", "r_main.c", "r_misc.c", "r_part.c",
        "r_sky.c", "r_sprite.c", "r_surf.c", "r_vars.c",
```

- [ ] **Step 1.4: Wire `R_InitRGBTable` into `R_Init`**

Edit `sdlquake/engine_src/r_main.c`. Find `void R_Init (void)`. At the end of the function (before the closing brace), add:

```c
    R_InitRGBTable ();
```

- [ ] **Step 1.5: Build, expect clean**

```sh
zig build
```

Expected: no compile errors, no link errors. Linker resolves `R_InitRGBTable`, `rgbtable`, `basepal_r/g/b`.

- [ ] **Step 1.6: Smoke-test the LUT**

```sh
zig build run
```

In-game, drop the console (`~`) and run:

```
r_lut_info
```

Expected: five lines printed, e.g.:
```
rgbtable[0,0,0]    = 0 (palette idx for black)
rgbtable[63,63,63] = 15 (palette idx for white)
rgbtable[63,0,0]   = 251 (palette idx for pure red)
rgbtable[0,63,0]   = 184 (palette idx for pure green)
rgbtable[0,0,63]   = 35 (palette idx for pure blue)
```

(Exact indices depend on the Quake palette but black should be 0 and white should map to a near-white palette entry. Red/green/blue indices need only be plausible — they should *not* all be 0 or all the same.)

Also confirm `id1/rgbtable.lmp` exists on disk after the first run and is exactly **262168 bytes** (16-byte header + 262144-byte table).

- [ ] **Step 1.7: Re-run to verify cache load**

```sh
zig build run
```

In-game, the `r_lut_info` output should still be correct, and the **first-run "Building rgbtable.lmp..." message should NOT print** (it should silently load from disk instead).

- [ ] **Step 1.8: Commit**

```sh
git add sdlquake/engine_src/r_lut.c sdlquake/engine_src/r_local.h sdlquake/engine_src/r_main.c build.zig
git commit -m "feat(render): RGB->palette LUT with disk cache for coloured lighting"
```

---

## Task 2: `.lit` sidecar loader and surface plumbing

**Files:**
- Modify: `sdlquake/engine_src/model.h`
- Modify: `sdlquake/engine_src/model.c`

This task adds the data path but no rendering change. The rendering switch comes in Task 5.

**Prerequisite:** Place a known-good `e1m1.lit` at `id1/maps/e1m1.lit`. Suggested source: the QuakeSpasm "QRP lit pack" or any community pack. Note the URL in the commit message at Step 2.8.

- [ ] **Step 2.1: Extend `msurface_t` and `model_t` in `model.h`**

Edit `sdlquake/engine_src/model.h`.

Find `typedef struct msurface_s` (around line 102) and the line `byte *samples;`. Immediately below it, add:

```c
    byte        *rgb_samples;   // [numstyles*surfsize*3], NULL if no .lit loaded
```

Find `typedef struct model_s` (around line 300) and the `byte *lightdata;` line (around line 361). Immediately below it, add:

```c
    byte        *rgblightdata;  // parallel to lightdata, NULL if no .lit loaded
```

- [ ] **Step 2.2: Add `Mod_LoadLITFile` in `model.c`**

Edit `sdlquake/engine_src/model.c`. Above `Mod_LoadLighting` (around line 501), insert:

```c
/*
=================
Mod_LoadLITFile

Attempts to load maps/<name>.lit alongside the BSP's mono lightmap. The
.lit format (FitzQuake/QuakeSpasm standard) is:
    char magic[4] = "QLIT"
    int  version  = 1
    byte rgb[3 * mono_lightmap_byte_count]

On any mismatch or load failure we set rgblightdata to NULL and stay mono.
Bad sidecars never Sys_Error.
=================
*/
static void
Mod_LoadLITFile (int mono_size)
{
    char    litname[MAX_QPATH];
    byte   *raw;
    int     i;

    loadmodel->rgblightdata = NULL;

    if (mono_size <= 0)
        return;

    // loadmodel->name is "maps/e1m1.bsp" -> "maps/e1m1.lit"
    COM_StripExtension (loadmodel->name, litname);
    strcat (litname, ".lit");

    raw = (byte *)COM_LoadHunkFile (litname);
    if (!raw)
        return;

    // header: 4-byte magic + 4-byte version + RGB samples
    if (com_filesize < 8 || raw[0] != 'Q' || raw[1] != 'L' || raw[2] != 'I' || raw[3] != 'T') {
        Con_Printf ("ignoring %s: bad magic\n", litname);
        return;
    }
    if (LittleLong (*(int *)(raw + 4)) != 1) {
        Con_Printf ("ignoring %s: unsupported version %d\n",
                    litname, LittleLong (*(int *)(raw + 4)));
        return;
    }

    if (com_filesize - 8 != mono_size * 3) {
        Con_Printf ("ignoring %s: size mismatch (%d vs %d)\n",
                    litname, com_filesize - 8, mono_size * 3);
        return;
    }

    // raw points into a hunk allocation; the payload at raw+8 has the right
    // lifetime already, but the layout convention used by Mod_LoadFaces is
    // "offset from lightdata"; we keep a clean pointer to the rgb base.
    loadmodel->rgblightdata = raw + 8;
}
```

- [ ] **Step 2.3: Call `Mod_LoadLITFile` from `Mod_LoadLighting`**

Edit `Mod_LoadLighting` (around line 504) to pass the mono size to `Mod_LoadLITFile`. Replace the existing function body with:

```c
void Mod_LoadLighting (lump_t *l)
{
    if (!l->filelen)
    {
        loadmodel->lightdata = NULL;
        loadmodel->rgblightdata = NULL;
        return;
    }
    loadmodel->lightdata = Hunk_AllocName ( l->filelen, loadname);
    memcpy (loadmodel->lightdata, mod_base + l->fileofs, l->filelen);

    Mod_LoadLITFile (l->filelen);
}
```

- [ ] **Step 2.4: Wire `rgb_samples` in `Mod_LoadFaces`**

Edit `Mod_LoadFaces` (around line 800-810). Find the block:

```c
        i = LittleLong(in->lightofs);
        if (i == -1)
            out->samples = NULL;
        else
            out->samples = loadmodel->lightdata + i;
```

Extend it to:

```c
        i = LittleLong(in->lightofs);
        if (i == -1) {
            out->samples = NULL;
            out->rgb_samples = NULL;
        } else {
            out->samples = loadmodel->lightdata + i;
            out->rgb_samples = loadmodel->rgblightdata
                ? loadmodel->rgblightdata + i * 3
                : NULL;
        }
```

- [ ] **Step 2.5: Add a smoke-test console command**

Edit `sdlquake/engine_src/r_main.c`. Add a static function near `R_LUTInfo_f` (or, since this is model-side, you can put it in `model.c`). To keep model.c free of console glue, place this in `r_main.c`:

```c
static void
R_LitInfo_f (void)
{
    model_t *m = cl.worldmodel;
    if (!m) {
        Con_Printf ("no worldmodel\n");
        return;
    }
    if (!m->rgblightdata) {
        Con_Printf ("%s: no .lit loaded (mono)\n", m->name);
        return;
    }
    Con_Printf ("%s: .lit loaded\n", m->name);
    Con_Printf ("  first 3 RGB samples: (%d,%d,%d) (%d,%d,%d) (%d,%d,%d)\n",
        m->rgblightdata[0], m->rgblightdata[1], m->rgblightdata[2],
        m->rgblightdata[3], m->rgblightdata[4], m->rgblightdata[5],
        m->rgblightdata[6], m->rgblightdata[7], m->rgblightdata[8]);
}
```

In `R_Init`, after `R_InitRGBTable ();`, add:

```c
    Cmd_AddCommand ("r_lit_info", R_LitInfo_f);
```

- [ ] **Step 2.6: Build**

```sh
zig build
```

Expected: clean compile and link.

- [ ] **Step 2.7: Smoke-test (no `.lit`)**

Temporarily move `id1/maps/e1m1.lit` aside (`mv id1/maps/e1m1.lit id1/maps/e1m1.lit.bak`). Then:

```sh
zig build run -- +map e1m1
```

In-game, console:

```
r_lit_info
```

Expected: `maps/e1m1.bsp: no .lit loaded (mono)`.

Visual: pixel-identical to before this task (since nothing in the renderer consumes `rgb_samples` yet).

- [ ] **Step 2.8: Smoke-test (with `.lit`)**

Restore `id1/maps/e1m1.lit`:

```sh
mv id1/maps/e1m1.lit.bak id1/maps/e1m1.lit
zig build run -- +map e1m1
```

In-game console:

```
r_lit_info
```

Expected: `maps/e1m1.bsp: .lit loaded` plus a line showing nine non-identical bytes (the first three RGB triples). Visual: still pixel-identical to before — renderer hasn't switched paths yet.

- [ ] **Step 2.9: Commit**

```sh
git add sdlquake/engine_src/model.h sdlquake/engine_src/model.c sdlquake/engine_src/r_main.c
git commit -m "$(cat <<'EOF'
feat(render): .lit sidecar loader and rgb_samples plumbing

Loads maps/<name>.lit if present (FitzQuake/QuakeSpasm format) and
wires per-surface rgb_samples pointers parallel to the existing
samples pointers. No rendering change yet.

Test .lit: <URL of community pack used>
EOF
)"
```

---

## Task 3: `R_BuildLightMap_RGB` and `r_coloredlight` cvar

**Files:**
- Modify: `sdlquake/engine_src/r_local.h`
- Modify: `sdlquake/engine_src/r_main.c`
- Modify: `sdlquake/engine_src/r_surf.c`

This task adds the build-side of the RGB pipeline. Nothing consumes the new `blocklights_rgb[]` array yet (the writers come in Task 4 and the dispatch in Task 5).

- [ ] **Step 3.1: Forward-declarations in `r_local.h`**

Append to `r_local.h` after the LUT declarations from Task 1:

```c
// ---- coloured-lighting blocklights (r_surf_rgb.c, R_BuildLightMap_RGB lives in r_surf.c) ----
extern unsigned blocklights_rgb[18*18*3];   // R0,G0,B0, R1,G1,B1, ...

void R_BuildLightMap_RGB (void);
void R_AddDynamicLights_RGB (void);
```

- [ ] **Step 3.2: Add `r_coloredlight` cvar in `r_main.c`**

Edit `sdlquake/engine_src/r_main.c`. After the existing `cvar_t r_fullbright = ...;` line (around line 124), add:

```c
cvar_t  r_coloredlight    = {"r_coloredlight",    "1", true};   // archived
cvar_t  r_colored_dlights = {"r_colored_dlights", "1", true};   // archived
```

Edit `sdlquake/engine_src/r_local.h`. The existing pattern declares each renderer cvar as `extern cvar_t r_*;` around lines 56-80. Add (next to the `extern cvar_t r_fullbright;` line at line 62):

```c
extern cvar_t   r_coloredlight;
extern cvar_t   r_colored_dlights;
```

Find the `R_Init` registration block where other `Cvar_RegisterVariable` calls live (search for `Cvar_RegisterVariable (&r_fullbright);`). Add two lines:

```c
    Cvar_RegisterVariable (&r_coloredlight);
    Cvar_RegisterVariable (&r_colored_dlights);
```

- [ ] **Step 3.3: Add the RGB buildlightmap to `r_surf.c`**

Edit `sdlquake/engine_src/r_surf.c`. After the existing `unsigned blocklights[18*18];` declaration (around line 54), add:

```c
unsigned blocklights_rgb[18*18*3];
```

After `R_AddDynamicLights` (around line 140), add `R_AddDynamicLights_RGB`:

```c
/*
===============
R_AddDynamicLights_RGB

Mono R_AddDynamicLights, three channels. The dlight->color modulates each
channel's contribution; default {1,1,1} reproduces mono behaviour.
===============
*/
void R_AddDynamicLights_RGB (void)
{
    msurface_t *surf;
    int         lnum;
    int         sd, td;
    float       dist, rad, minlight;
    vec3_t      impact, local;
    int         s, t;
    int         i;
    int         smax, tmax;
    mtexinfo_t *tex;

    surf = r_drawsurf.surf;
    smax = (surf->extents[0]>>4)+1;
    tmax = (surf->extents[1]>>4)+1;
    tex  = surf->texinfo;

    for (lnum = 0; lnum < MAX_DLIGHTS; lnum++)
    {
        if (!(surf->dlightbits & (1 << lnum)))
            continue;

        rad      = cl_dlights[lnum].radius;
        dist     = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
                   surf->plane->dist;
        rad     -= fabs(dist);
        minlight = cl_dlights[lnum].minlight;
        if (rad < minlight) continue;
        minlight = rad - minlight;

        for (i = 0; i < 3; i++)
            impact[i] = cl_dlights[lnum].origin[i] - surf->plane->normal[i]*dist;

        local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
        local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];
        local[0] -= surf->texturemins[0];
        local[1] -= surf->texturemins[1];

        for (t = 0; t < tmax; t++)
        {
            td = local[1] - t*16;
            if (td < 0) td = -td;
            for (s = 0; s < smax; s++)
            {
                sd = local[0] - s*16;
                if (sd < 0) sd = -sd;
                dist = (sd > td) ? sd + (td>>1) : td + (sd>>1);
                if (dist < minlight)
                {
                    unsigned add = (unsigned)((rad - dist) * 256);
                    int      idx = (t*smax + s) * 3;
                    blocklights_rgb[idx + 0] += (unsigned)(add * cl_dlights[lnum].color[0]);
                    blocklights_rgb[idx + 1] += (unsigned)(add * cl_dlights[lnum].color[1]);
                    blocklights_rgb[idx + 2] += (unsigned)(add * cl_dlights[lnum].color[2]);
                }
            }
        }
    }
}
```

(Note: `cl_dlights[lnum].color` will not exist until Task 6. To keep this task buildable in isolation, write the body referring to `color` and accept that the build will fail at link/compile only if `color` is referenced. Since dlight_t doesn't yet have a color field, the compile will fail in step 3.5. **Workaround for this task:** temporarily replace the three `cl_dlights[lnum].color[N]` expressions with `1.0f` placeholders, and revisit in Task 6 to wire the real colour. To make this explicit, write:)

```c
                    /* TASK6: replace 1.0f with cl_dlights[lnum].color[N] */
                    blocklights_rgb[idx + 0] += (unsigned)(add * 1.0f);
                    blocklights_rgb[idx + 1] += (unsigned)(add * 1.0f);
                    blocklights_rgb[idx + 2] += (unsigned)(add * 1.0f);
```

After `R_BuildLightMap` (around line 203), add `R_BuildLightMap_RGB`:

```c
/*
===============
R_BuildLightMap_RGB

Three-channel sibling of R_BuildLightMap. Reads from surf->rgb_samples
(must be non-NULL; caller's responsibility), writes blocklights_rgb in
the same 8.8 fixed range as the mono path (64..16320 per channel).
===============
*/
void R_BuildLightMap_RGB (void)
{
    int         smax, tmax;
    int         t;
    int         i, size;
    byte       *lightmap;
    unsigned    scale;
    int         maps;
    msurface_t *surf;
    unsigned    amb;

    surf = r_drawsurf.surf;
    smax = (surf->extents[0]>>4)+1;
    tmax = (surf->extents[1]>>4)+1;
    size = smax * tmax;
    lightmap = surf->rgb_samples;

    if (r_fullbright.value || !cl.worldmodel->lightdata) {
        for (i = 0; i < size * 3; i++) blocklights_rgb[i] = 0;
        return;
    }

    amb = r_refdef.ambientlight << 8;
    for (i = 0; i < size; i++) {
        blocklights_rgb[i*3 + 0] = amb;
        blocklights_rgb[i*3 + 1] = amb;
        blocklights_rgb[i*3 + 2] = amb;
    }

    if (lightmap)
        for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255; maps++)
        {
            scale = r_drawsurf.lightadj[maps];  // 8.8 fixed
            for (i = 0; i < size; i++) {
                blocklights_rgb[i*3 + 0] += lightmap[i*3 + 0] * scale;
                blocklights_rgb[i*3 + 1] += lightmap[i*3 + 1] * scale;
                blocklights_rgb[i*3 + 2] += lightmap[i*3 + 2] * scale;
            }
            lightmap += size * 3;
        }

    if (surf->dlightframe == r_framecount)
        R_AddDynamicLights_RGB ();

    for (i = 0; i < size * 3; i++) {
        t = (255*256 - (int)blocklights_rgb[i]) >> (8 - VID_CBITS);
        if (t < (1 << 6)) t = (1 << 6);
        blocklights_rgb[i] = t;
    }
}
```

- [ ] **Step 3.4: Build**

```sh
zig build
```

Expected: clean compile. `R_BuildLightMap_RGB` and `R_AddDynamicLights_RGB` are defined but currently unused. The `-w` flag in `engine_c_flags` suppresses unused-function warnings.

- [ ] **Step 3.5: Smoke-test (no behaviour change yet)**

```sh
zig build run -- +map e1m1
```

In-game, the world should look identical to current master. Confirm cvars exist:

```
r_coloredlight
```

Expected: `"r_coloredlight" is "1"`.

```
r_colored_dlights
```

Expected: `"r_colored_dlights" is "1"`.

- [ ] **Step 3.6: Commit**

```sh
git add sdlquake/engine_src/r_local.h sdlquake/engine_src/r_main.c sdlquake/engine_src/r_surf.c
git commit -m "feat(render): R_BuildLightMap_RGB and r_coloredlight cvar (no dispatch yet)"
```

---

## Task 4: RGB block writers in `r_surf_rgb.c`

**Files:**
- Create: `sdlquake/engine_src/r_surf_rgb.c`
- Modify: `sdlquake/engine_src/r_local.h`
- Modify: `build.zig`

- [ ] **Step 4.1: Forward declarations in `r_local.h`**

Append to the existing coloured-lighting declarations block. The four `*_rgb` writers live in a separate TU, so they need both the function prototypes and externs for the file-scope globals from `r_surf.c` that they consume:

```c
void R_DrawSurfaceBlock8_mip0_rgb (void);
void R_DrawSurfaceBlock8_mip1_rgb (void);
void R_DrawSurfaceBlock8_mip2_rgb (void);
void R_DrawSurfaceBlock8_mip3_rgb (void);

// File-scope globals defined in r_surf.c, consumed by r_surf_rgb.c.
// `-fcommon` merges these as tentative definitions, but explicit externs
// make the dependency visible and let the compiler type-check.
extern unsigned char  *pbasesource;
extern void           *prowdestbase;
extern unsigned       *r_lightptr;
extern unsigned        blocklights[18*18];
extern int             r_lightwidth;
extern int             r_numvblocks;
extern int             sourcetstep;
extern unsigned char  *r_sourcemax;
extern int             r_stepback;
extern int             surfrowbytes;
```

(If any of those `extern`s collide with an existing declaration elsewhere in the engine — say `r_shared.h` — drop the duplicate. The build is the arbiter.)

- [ ] **Step 4.2: Create `r_surf_rgb.c`**

Create `sdlquake/engine_src/r_surf_rgb.c`:

```c
/*
r_surf_rgb.c -- coloured-light surface-cache block writers.

These are RGB siblings of R_DrawSurfaceBlock8_mip* in r_surf.c. Each writer
reads three corner light values per channel from blocklights_rgb (in the
same 8.8 fixed range as mono blocklights[]), bilinearly interpolates per
texel, computes basepal[texel] * light_RGB, quantises to 6 bits per channel,
and looks up the nearest palette index in rgbtable[].

The lightR_ptr/lightG_ptr/lightB_ptr triples below mirror r_lightptr in
the mono writers; the inner-loop variable naming (bx for column, by for row,
r6/g6/b6 for quantised channels) avoids shadowing.
*/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

/* These mono-side globals are shared (declared in r_local.h via r_surf.c):
   r_numvblocks, r_lightwidth, sourcetstep, r_sourcemax, r_stepback,
   surfrowbytes, prowdestbase, pbasesource. We use them read-only. */

#define RGB_LIGHT_INTEGER(L) ((L) >> 8)
#define RGB_SHIFT 8

/* Helper: interpolate the three RGB channel pairs from one row of light data,
   given the starting block-corner pointer rptr (pointing at the R channel of
   the top-left block corner) and stride r_lightwidth (in *triples*, i.e.
   stride in unsigned-ints is r_lightwidth*3). */

void R_DrawSurfaceBlock8_mip0_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;   // 3-stride: r,g,b per corner

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 4;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 4;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 4;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 4;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 4;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 4;

        for (by = 0; by < 16; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 4;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 4;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 4;

            lightR = lightR_r;
            lightG = lightG_r;
            lightB = lightB_r;

            for (bx = 15; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

void R_DrawSurfaceBlock8_mip1_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 3;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 3;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 3;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 3;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 3;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 3;

        for (by = 0; by < 8; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 3;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 3;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 3;

            lightR = lightR_r; lightG = lightG_r; lightB = lightB_r;

            for (bx = 7; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

void R_DrawSurfaceBlock8_mip2_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 2;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 2;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 2;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 2;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 2;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 2;

        for (by = 0; by < 4; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 2;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 2;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 2;

            lightR = lightR_r; lightG = lightG_r; lightB = lightB_r;

            for (bx = 3; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}

void R_DrawSurfaceBlock8_mip3_rgb (void)
{
    int             v, by, bx;
    unsigned        lightR_l, lightR_r, lightR_lstep, lightR_rstep;
    unsigned        lightG_l, lightG_r, lightG_lstep, lightG_rstep;
    unsigned        lightB_l, lightB_r, lightB_lstep, lightB_rstep;
    unsigned        lightR, lightG, lightB;
    unsigned        lightR_step, lightG_step, lightB_step;
    unsigned        tex, r6, g6, b6;
    unsigned char  *psource, *prowdest;
    unsigned       *rptr;

    psource = pbasesource;
    prowdest = prowdestbase;
    rptr = blocklights_rgb + (r_lightptr - blocklights) * 3;

    for (v = 0; v < r_numvblocks; v++)
    {
        lightR_l = rptr[0*3+0]; lightG_l = rptr[0*3+1]; lightB_l = rptr[0*3+2];
        lightR_r = rptr[1*3+0]; lightG_r = rptr[1*3+1]; lightB_r = rptr[1*3+2];
        rptr += r_lightwidth * 3;
        lightR_lstep = ((int)rptr[0*3+0] - (int)lightR_l) >> 1;
        lightG_lstep = ((int)rptr[0*3+1] - (int)lightG_l) >> 1;
        lightB_lstep = ((int)rptr[0*3+2] - (int)lightB_l) >> 1;
        lightR_rstep = ((int)rptr[1*3+0] - (int)lightR_r) >> 1;
        lightG_rstep = ((int)rptr[1*3+1] - (int)lightG_r) >> 1;
        lightB_rstep = ((int)rptr[1*3+2] - (int)lightB_r) >> 1;

        for (by = 0; by < 2; by++)
        {
            lightR_step = ((int)lightR_l - (int)lightR_r) >> 1;
            lightG_step = ((int)lightG_l - (int)lightG_r) >> 1;
            lightB_step = ((int)lightB_l - (int)lightB_r) >> 1;

            lightR = lightR_r; lightG = lightG_r; lightB = lightB_r;

            for (bx = 1; bx >= 0; bx--)
            {
                tex = psource[bx];
                r6 = (RGB_LIGHT_INTEGER(lightR) * basepal_r[tex]) >> RGB_SHIFT;
                g6 = (RGB_LIGHT_INTEGER(lightG) * basepal_g[tex]) >> RGB_SHIFT;
                b6 = (RGB_LIGHT_INTEGER(lightB) * basepal_b[tex]) >> RGB_SHIFT;
                if (r6 > 63) r6 = 63;
                if (g6 > 63) g6 = 63;
                if (b6 > 63) b6 = 63;
                prowdest[bx] = rgbtable[(r6 << 12) | (g6 << 6) | b6];
                lightR += lightR_step;
                lightG += lightG_step;
                lightB += lightB_step;
            }

            psource += sourcetstep;
            lightR_r += lightR_rstep; lightG_r += lightG_rstep; lightB_r += lightB_rstep;
            lightR_l += lightR_lstep; lightG_l += lightG_lstep; lightB_l += lightB_lstep;
            prowdest += surfrowbytes;
        }

        if (psource >= r_sourcemax) psource -= r_stepback;
    }
}
```

- [ ] **Step 4.3: Register the source file in `build.zig`**

Edit `build.zig`'s `engine_files` list. Add `"r_surf_rgb.c"` adjacent to `"r_surf.c"`:

```zig
        "r_sky.c", "r_sprite.c", "r_surf.c", "r_surf_rgb.c", "r_vars.c",
```

- [ ] **Step 4.4: Build**

```sh
zig build
```

Expected: clean compile. Four new symbols are exported but currently unreferenced.

- [ ] **Step 4.5: Smoke-test (no behaviour change)**

```sh
zig build run -- +map e1m1
```

Expected: identical pixels to current master, both with and without `id1/maps/e1m1.lit` present.

- [ ] **Step 4.6: Commit**

```sh
git add sdlquake/engine_src/r_surf_rgb.c sdlquake/engine_src/r_local.h build.zig
git commit -m "feat(render): RGB surface-block writers (unwired)"
```

---

## Task 5: Dispatch in `R_DrawSurface` — first visible coloured lighting

**Files:**
- Modify: `sdlquake/engine_src/r_surf.c`

- [ ] **Step 5.1: Insert dispatch logic in `R_DrawSurface`**

Edit `sdlquake/engine_src/r_surf.c`. Find `void R_DrawSurface (void)` (around line 248). Replace the line:

```c
// calculate the lightings
    R_BuildLightMap ();
```

with:

```c
    qboolean use_rgb =
        r_pixbytes == 1 &&
        r_coloredlight.value &&
        r_drawsurf.surf->rgb_samples != NULL;

    if (use_rgb) R_BuildLightMap_RGB ();
    else         R_BuildLightMap ();
```

Then find the `if (r_pixbytes == 1)` dispatch block (around line 284). Replace:

```c
    if (r_pixbytes == 1)
    {
        pblockdrawer = surfmiptable[r_drawsurf.surfmip];
    // TODO: only needs to be set when there is a display settings change
        horzblockstep = blocksize;
    }
    else
    {
        pblockdrawer = R_DrawSurfaceBlock16;
    // TODO: only needs to be set when there is a display settings change
        horzblockstep = blocksize << 1;
    }
```

with:

```c
    static void (*surfmiptable_rgb[4])(void) = {
        R_DrawSurfaceBlock8_mip0_rgb,
        R_DrawSurfaceBlock8_mip1_rgb,
        R_DrawSurfaceBlock8_mip2_rgb,
        R_DrawSurfaceBlock8_mip3_rgb
    };

    if (r_pixbytes == 1)
    {
        pblockdrawer = use_rgb
            ? surfmiptable_rgb[r_drawsurf.surfmip]
            : surfmiptable[r_drawsurf.surfmip];
    // TODO: only needs to be set when there is a display settings change
        horzblockstep = blocksize;
    }
    else
    {
        pblockdrawer = R_DrawSurfaceBlock16;
    // TODO: only needs to be set when there is a display settings change
        horzblockstep = blocksize << 1;
    }
```

Note: the `static` array inside the function relies on C89 declarations being at the top of a block. Move the declaration to the top of `R_DrawSurface` if your compiler complains; gnu89 allows mid-block declarations.

`r_coloredlight` is already declared `extern` in `r_local.h` from Task 3.2, which `r_surf.c` includes; no further declaration needed.

- [ ] **Step 5.2: Build**

```sh
zig build
```

Expected: clean compile.

- [ ] **Step 5.3: Smoke-test (no `.lit`)**

Ensure no `.lit` is loaded:

```sh
mv id1/maps/e1m1.lit id1/maps/e1m1.lit.bak
zig build run -- +map e1m1
```

Expected: pixel-identical to current master (the `rgb_samples == NULL` check routes everything through the mono path).

Restore:

```sh
mv id1/maps/e1m1.lit.bak id1/maps/e1m1.lit
```

- [ ] **Step 5.4: Smoke-test (with `.lit` — first colour!)**

```sh
zig build run -- +map e1m1
```

Walk through the start area, slipgate room, and first lava pool. Expected:

- Lava surface throws warm orange tint on adjacent walls.
- Slipgate area has cool/blue light.
- Metal corridors are duller, neutral.
- Overall brightness is consistent with the mono version — toggle `r_coloredlight 0/1` mid-game and confirm scenes are similarly bright (not significantly dimmer/brighter). If brightness mismatch is severe, adjust `RGB_SHIFT` in `r_surf_rgb.c` from 8 to 7 (brighter) or 9 (dimmer) and rebuild.
- `r_coloredlight 0` mid-game → after walking around 1–2 seconds (surface-cache turnover), classic mono Quake.
- `r_coloredlight 1` again → coloured.

- [ ] **Step 5.5: Smoke-test `r_fullbright`**

In-game:

```
r_fullbright 1
```

Expected: fullbright works (palette mostly visible), no crash. Then:

```
r_fullbright 0
```

Expected: lighting restored.

- [ ] **Step 5.6: Commit**

```sh
git add sdlquake/engine_src/r_surf.c
git commit -m "feat(render): wire RGB surface writers; coloured lighting visible"
```

---

## Task 6: Coloured dynamic lights

**Files:**
- Modify: `sdlquake/engine_src/client.h`
- Modify: `sdlquake/engine_src/cl_main.c`
- Modify: `sdlquake/engine_src/cl_tent.c`
- Modify: `sdlquake/engine_src/r_surf.c`
- Create: `sdlquake/engine_src/cl_dlight_colors.h`

- [ ] **Step 6.1: Extend `dlight_t`**

Edit `sdlquake/engine_src/client.h`. In `typedef struct { ... } dlight_t;` (line 72-83), add a `color` field right before `#ifdef QUAKE2`:

```c
typedef struct
{
    vec3_t  origin;
    float   radius;
    float   die;
    float   decay;
    float   minlight;
    int     key;
    vec3_t  color;          // NEW: per-channel multiplier, default {1,1,1}
#ifdef QUAKE2
    qboolean    dark;
#endif
} dlight_t;
```

- [ ] **Step 6.2: Initialise `color` in `CL_AllocDlight`**

Edit `sdlquake/engine_src/cl_main.c`. In `CL_AllocDlight` (line 317), there are three `memset (dl, 0, sizeof(*dl));` calls followed by `dl->key = key;`. After **each** of those `dl->key = key;` lines, add:

```c
        dl->color[0] = dl->color[1] = dl->color[2] = 1.0f;
```

This guarantees any caller that doesn't set `color` gets a white dlight, identical to current behaviour.

- [ ] **Step 6.3: Create `cl_dlight_colors.h`**

Create `sdlquake/engine_src/cl_dlight_colors.h`:

```c
/*
cl_dlight_colors.h -- named per-event dynamic-light colours.

These are vec3_t multipliers applied to each channel in
R_AddDynamicLights_RGB. {1,1,1} is white (the historical mono behaviour).
Tune values here without touching call sites.
*/

#ifndef CL_DLIGHT_COLORS_H
#define CL_DLIGHT_COLORS_H

static const vec3_t DLIGHT_COLOR_WHITE      = {1.00f, 1.00f, 1.00f};
static const vec3_t DLIGHT_COLOR_MUZZLE     = {1.00f, 0.85f, 0.45f};  // warm yellow
static const vec3_t DLIGHT_COLOR_ROCKET     = {1.00f, 0.60f, 0.20f};  // orange
static const vec3_t DLIGHT_COLOR_EXPLOSION  = {1.00f, 0.50f, 0.20f};  // orange-red
static const vec3_t DLIGHT_COLOR_LIGHTNING  = {0.60f, 0.70f, 1.00f};  // pale blue
static const vec3_t DLIGHT_COLOR_BRIGHTLIGHT = {1.00f, 0.90f, 0.70f}; // warm
static const vec3_t DLIGHT_COLOR_DIMLIGHT   = {1.00f, 0.85f, 0.60f};  // warm

#endif
```

- [ ] **Step 6.4: Wire colours at `cl_main.c` call sites**

Edit `sdlquake/engine_src/cl_main.c`. Near the top of the file, add:

```c
#include "cl_dlight_colors.h"
```

Each `dl = CL_AllocDlight (...)` in this file is followed by a `VectorCopy` setting `dl->origin`. Immediately after each origin assignment block, before the `dl->radius = ...`, add a `VectorCopy` for the appropriate colour.

Specifically (line numbers from current master):

- Line 544 (EF_MUZZLEFLASH): after `VectorMA (dl->origin, 18, fv, dl->origin);`, add
  ```c
  VectorCopy (DLIGHT_COLOR_MUZZLE, dl->color);
  ```
- Line 556 (EF_BRIGHTLIGHT): after `dl->origin[2] += 16;`, add
  ```c
  VectorCopy (DLIGHT_COLOR_BRIGHTLIGHT, dl->color);
  ```
- Line 564 (EF_DIMLIGHT): after the `VectorCopy (ent->origin, dl->origin);`, add
  ```c
  VectorCopy (DLIGHT_COLOR_DIMLIGHT, dl->color);
  ```
- Line 572 (EF_DARKLIGHT, QUAKE2-only): leave as `DLIGHT_COLOR_WHITE` (skip — it's `dark`-mode and Quake1 doesn't reach this branch).
- Line 580 (EF_LIGHT, QUAKE2-only): same — skip.
- Line 598 (EF_ROCKET): after `VectorCopy (ent->origin, dl->origin);`, add
  ```c
  VectorCopy (DLIGHT_COLOR_ROCKET, dl->color);
  ```

- [ ] **Step 6.5: Wire colours at `cl_tent.c` call sites**

Edit `sdlquake/engine_src/cl_tent.c`. Near the top of the file, add:

```c
#include "cl_dlight_colors.h"
```

- Line 199 (TE_EXPLOSION): after `VectorCopy (pos, dl->origin);`, add
  ```c
  VectorCopy (DLIGHT_COLOR_EXPLOSION, dl->color);
  ```
- Line 255 (TE_EXPLOSION2, QUAKE2): after `VectorCopy (pos, dl->origin);`, add
  ```c
  VectorCopy (DLIGHT_COLOR_EXPLOSION, dl->color);
  ```
- Line 282 (TE_RAILTRAIL, QUAKE2): after `VectorCopy (endpos, dl->origin);`, add
  ```c
  VectorCopy (DLIGHT_COLOR_LIGHTNING, dl->color);
  ```

(There are no `TE_LIGHTNING*` dlight allocations in stock cl_tent.c; lightning beams are sprite trails. Skip.)

- [ ] **Step 6.6: Replace placeholders in `R_AddDynamicLights_RGB`**

Edit `sdlquake/engine_src/r_surf.c`. In `R_AddDynamicLights_RGB` (added in Task 3), find the three `1.0f` placeholders marked `/* TASK6: ... */` and replace:

```c
                    /* TASK6: replace 1.0f with cl_dlights[lnum].color[N] */
                    blocklights_rgb[idx + 0] += (unsigned)(add * 1.0f);
                    blocklights_rgb[idx + 1] += (unsigned)(add * 1.0f);
                    blocklights_rgb[idx + 2] += (unsigned)(add * 1.0f);
```

with:

```c
                    if (r_colored_dlights.value) {
                        blocklights_rgb[idx + 0] += (unsigned)(add * cl_dlights[lnum].color[0]);
                        blocklights_rgb[idx + 1] += (unsigned)(add * cl_dlights[lnum].color[1]);
                        blocklights_rgb[idx + 2] += (unsigned)(add * cl_dlights[lnum].color[2]);
                    } else {
                        blocklights_rgb[idx + 0] += add;
                        blocklights_rgb[idx + 1] += add;
                        blocklights_rgb[idx + 2] += add;
                    }
```

`r_colored_dlights` is already declared `extern` in `r_local.h` from Task 3.2; no further declaration needed.

- [ ] **Step 6.7: Build**

```sh
zig build
```

Expected: clean compile.

- [ ] **Step 6.8: Smoke-test colours**

```sh
zig build run -- +map e1m1
```

In-game:

1. Find a darkened room. Fire the shotgun (`impulse 2; +attack`). Expected: muzzle flash visibly **yellow** on the walls for one frame.
2. Fire grenades in open space (`impulse 6`). Expected: explosion flash visibly **orange-red**.
3. Fire rockets (`impulse 7`). Expected: orange rocket-trail light on walls and the same orange-red on explosion.
4. Toggle `r_colored_dlights 0`. Repeat 1–3: dlights are visibly **white** again (static `.lit` colour still works).
5. Toggle `r_colored_dlights 1`. Colours restored.

- [ ] **Step 6.9: Commit**

```sh
git add sdlquake/engine_src/client.h sdlquake/engine_src/cl_main.c sdlquake/engine_src/cl_tent.c sdlquake/engine_src/cl_dlight_colors.h sdlquake/engine_src/r_surf.c
git commit -m "feat(cl): per-channel dlight colours (muzzle/rocket/explosion/lightning)"
```

---

## Task 7: Smoke-test pass and perf baselines

**Files:** (no code changes — measurement and documentation)

- [ ] **Step 7.1: Full smoke-test pass per spec §Smoke test plan**

Each of the seven items from the spec, top to bottom:

1. **No `.lit`** — `mv id1/maps/e1m1.lit aside; zig build run -- +map e1m1` — expect pixel-identical to current master. Restore the `.lit` afterwards.

2. **With `.lit` — walk and observe** — start, slipgate, first lava pool. Note any places where the colour seems wrong (over-saturated, hue-shifted). Adjust `RGB_SHIFT` if mid-tones are obviously off; otherwise leave.

3. **Dlight colours visible** — confirm muzzle (yellow), explosion (orange-red), rocket trail (orange) on walls in a darkened room.

4. **`r_coloredlight 0` mid-game** — within 1–2s of camera motion, classic mono lighting. Set back to 1 — colour returns.

5. **`r_coloredlight 1; r_colored_dlights 0`** — static colour intact, dlights white.

6. **`r_fullbright 1`** — fullbright works, no crash. `r_fullbright 0` — restored.

7. **`host_speeds 1` baselines** — at the start of `e1m1`, with the camera in the same position both times, record the average of three frames'-worth of "tot brk" (total brokerage time) from `host_speeds`:
   - Native 320×200, `r_coloredlight 0`: ___ ms
   - Native 320×200, `r_coloredlight 1`: ___ ms
   - 2× scaled, `r_coloredlight 0`: ___ ms
   - 2× scaled, `r_coloredlight 1`: ___ ms

   Expected delta: <10% at native, <15% at 2×. If the delta is significantly larger, investigate (most likely cause: a cache-thrashing access pattern in the block writers).

- [ ] **Step 7.2: Append perf table to commit message and the design doc's perf section**

Edit `docs/superpowers/specs/2026-05-13-coloured-lighting-design.md` and append the measured numbers under the **Performance** heading.

- [ ] **Step 7.3: Final commit**

```sh
git add docs/superpowers/specs/2026-05-13-coloured-lighting-design.md
git commit -m "$(cat <<'EOF'
docs(spec): record coloured-lighting perf baselines

  Native, r_coloredlight 0: __.__ ms
  Native, r_coloredlight 1: __.__ ms
  2x,     r_coloredlight 0: __.__ ms
  2x,     r_coloredlight 1: __.__ ms
EOF
)"
```

---

## Implementation notes (engineer-facing)

- **gnu89 / -fcommon / -w**: the engine compiles with `-std=gnu89 -fcommon -w`. Mid-block declarations are allowed (gnu89, not strict c89). Unused functions and unused variables do not warn (`-w`). New file requires no special build flag — just append to the `engine_files` list in `build.zig`.
- **`extern cvar_t` placement**: existing engine `cvar_t` declarations are sometimes in headers, sometimes redeclared `extern` at the top of consuming `.c` files. If in doubt, add an `extern cvar_t r_coloredlight;` line near the top of any consuming file. The linker will resolve.
- **`COM_LoadHunkFile` lifetime**: returns a hunk allocation that persists for the lifetime of the model. Safe to keep the `byte *` and offset into it.
- **`MAX_OSPATH` / `MAX_QPATH`**: see `quakedef.h`. Usually 128 / 64.
- **`com_filesize`**: a global set by `COM_LoadHunkFile` to the size of the most recently loaded file.
- **`LittleLong`**: byte-swap helper. Use whenever reading an `int` from disk.
- **`com_gamedir`**: the working game directory ("id1" by default); use for cache file paths.
- **Hot-reload note**: this work is entirely in `engine_src/` (no `game.dll` files). Restart `zig build run` for each rebuild; the hot-reload pipeline doesn't apply.
- **Existing `samples` pointer arithmetic**: each surface's `samples` is `loadmodel->lightdata + lightofs`, where `lightofs` is per-surface in the BSP. The parallel `rgb_samples` uses `lightofs * 3` because RGB data is three bytes per mono byte.

## Out of scope (do not implement; deferred to v2+)

- In-engine `.lit` baker from `_color` light entities.
- Dithering in the block writer.
- Coloured liquid/sky surfaces (`D_DrawTurbulent8Span`, sky path).
- Coloured alias/sprite/particle lighting.
- `Sample_Lightmap` API exposure for Phase 8 (the field is wired and ready, but no consumer exists yet).

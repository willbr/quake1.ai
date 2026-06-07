# File Formats

Every file Quake reads or writes, in one place. Sizes are in bytes;
everything is little-endian; "string" means a fixed-size, NUL-terminated
char array unless noted otherwise.

## Asset bundles

### `.pak` — PAK archive

The container for almost everything ID shipped. Defined in
`engine_src/common.c`.

```c
struct dpackheader_t {        // file header
    char  id[4];              // "PACK"
    int   dirofs;             // offset to directory
    int   dirlen;             // size of directory in bytes
};

struct dpackfile_t {          // directory entry; dirlen / 64 of them
    char  name[56];           // forward-slash path, NUL-padded
    int   filepos;
    int   filelen;            // uncompressed (no compression in v1)
};
```

`numpackfiles = dirlen / sizeof(dpackfile_t)`. The engine mounts every
PAK in `id1/` (and any `-game <mod>` dir) into a single virtual
filesystem, search-path ordered. `COM_LoadHunkFile` first tries each
mounted PAK, then loose files on disk.

Max files per pack is `MAX_FILES_IN_PACK = 2048`. Shareware ships
`pak0.pak` (~18 MB); the full game adds `pak1.pak`.

### `.wad` — WAD2 texture archive

Used by the editor and by old maps that reference textures by name
without bundling them in the BSP. The BSP can also embed a mini-WAD
of its own textures in `LUMP_TEXTURES` (lump 2) — same layout.

```c
struct wadinfo_t {            // header
    char  identification[4];  // "WAD2"
    int   numlumps;
    int   infotableofs;
};

struct lumpinfo_t {           // numlumps of these at infotableofs
    int   filepos;
    int   disksize;
    int   size;               // uncompressed
    char  type;               // TYP_* (PALETTE, QTEX, QPIC, SOUND, MIPTEX)
    char  compression;        // 0 (none) or 1 (LZSS — Quake never used this)
    char  pad1, pad2;
    char  name[16];           // NUL-terminated
};
```

Lump types:

| Type | Value | Payload |
|---|---|---|
| `TYP_PALETTE` | 64 | 768-byte RGB palette |
| `TYP_QTEX` | 65 | qtex_t (unused in Quake 1) |
| `TYP_QPIC` | 66 | 8-bit RGBA-indexed picture (status-bar, menu chars). 8-byte header (w, h) then `w*h` palette indices. |
| `TYP_SOUND` | 67 | (legacy; Quake uses .wav instead) |
| `TYP_MIPTEX` | 68 | Texture with 4 mip levels; same layout as BSP's miptex |

## Levels

### `.bsp` — Brush-Solid Polytope, version 29

Compiled by qbsp. Lighting is baked in by `light` and PVS by `vis`.
Headers in `engine_src/bspfile.h`.

```c
struct dheader_t {
    int    version;          // 29
    lump_t lumps[15];        // table of (offset, length) for each lump
};
```

15 lumps:

| # | Lump | Holds |
|---|---|---|
| 0 | ENTITIES | Newline-delimited `{ "key" "value" ... }` blocks |
| 1 | PLANES | `dplane_t[]` — normal, dist, axis-type |
| 2 | TEXTURES | Embedded mini-WAD: `dmiptexlump_t` + `miptex_t[]` with 4 mip levels |
| 3 | VERTEXES | `dvertex_t[]` — `float point[3]` |
| 4 | VISIBILITY | PVS bitfield, RLE-compressed, indexed by `dleaf_t.visofs` |
| 5 | NODES | `dnode_t[]` — BSP interior nodes |
| 6 | TEXINFO | `texinfo_t[]` — s/t projection + texture index + flags |
| 7 | FACES | `dface_t[]` — plane + edge range + texinfo + lightmap offset |
| 8 | LIGHTING | One byte per luxel (sometimes ×4 for animated styles). RGB in `.lit` sidecar. |
| 9 | CLIPNODES | `dclipnode_t[]` — collision-hull BSP (3 hulls: point, player, monster) |
| 10 | LEAFS | `dleaf_t[]` — contents flag, visofs, marksurface range, ambient levels |
| 11 | MARKSURFACES | `unsigned short[]` — per-leaf face refs |
| 12 | EDGES | `dedge_t[]` — two `unsigned short` vertex indices |
| 13 | SURFEDGES | `int[]` — signed edge ref (negative = use in reverse) |
| 14 | MODELS | `dmodel_t[]` — submodels (worldspawn = 0, brush ents = 1+) |

Engine caps (`MAX_MAP_*` in `bspfile.h`): 32K planes, 32K nodes/clipnodes,
8K leafs, 65K verts/faces, 256K edges, 1 MB visibility, 1 MB lighting, 2 MB
miptex. The `world.c` loader will Sys_Error on overrun.

Contents flags (negative) live in `dleaf_t.contents` and node-children
pointers: `EMPTY = -1`, `SOLID = -2`, `WATER = -3`, `SLIME = -4`,
`LAVA = -5`, `SKY = -6`. The collision hull uses the same encoding.

### `.lit` — Coloured lighting sidecar

Lives next to the BSP as `maps/<name>.lit`. FitzQuake / QuakeSpasm
standard, parsed by `Mod_LoadLITFile`:

```
char  magic[4]  = "QLIT"
int   version   = 1
byte  rgb[3 * (bsp lump-8 byte count)]
```

The RGB array is one triple per mono luxel, in lump-order. If the
file is missing or size-mismatched the engine silently falls back to
mono lighting. See [rendering.md](rendering.md#coloured-lighting-lit-sidecars)
for the gotchas.

### `.map` — Editor source

Worldcraft / TrenchBroom-compatible text. Parsed and written by
`engine/editor/map_io.c`. Each entity is a `{ }` block with key/value
pairs and optional brushes; each brush is a `{ }` block with face
lines:

```
{
"classname" "worldspawn"
{
( -64 -64 0 ) ( -64  64 0 ) ( 64 64 0 ) WALL07 0 0 0 1 1
... (≥4 face planes; a 6-face AABB has 6)
}
}
{
"classname" "info_player_start"
"origin" "0 0 24"
"angle" "0"
}
```

Each face line is `( P1 ) ( P2 ) ( P3 ) <texname> <xshift> <yshift> <rotate> <xscale> <yscale>`.
The three points are in winding order — that determines the plane normal
direction. qbsp clips the planes into a convex polytope.

### `.prt` — Portal file (vis intermediate)

Written by qbsp, consumed by vis. Plain text; portals between leafs.
The engine never reads `.prt`; only the editor's in-process vis call
touches them.

## Models

### `.mdl` — Alias model

Player view-weapon, monsters, items. `modelgen.h`:

```c
struct mdl_t {                // file header (ident = "IDPO")
    int     ident;            // 'OPDI' little-endian magic
    int     version;          // 6
    vec3_t  scale;
    vec3_t  scale_origin;
    float   boundingradius;
    vec3_t  eyeposition;
    int     numskins;
    int     skinwidth;
    int     skinheight;
    int     numverts;
    int     numtris;
    int     numframes;
    synctype_t synctype;      // ST_SYNC or ST_RAND
    int     flags;
    float   size;
};
```

After the header come (in order, none length-prefixed because every
size is known from the header):

1. **Skins** — `numskins` of either a single skin or a group:
   `aliasskintype_t` discriminator, then `skinwidth * skinheight`
   palette indices.
2. **stverts** — `numverts × stvert_t`: u, v plus an `onseam` flag.
3. **Triangles** — `numtris × dtriangle_t`: vertex indices + which side
   of the seam the texture wraps from.
4. **Frames** — `numframes` of either a single frame or a group:
   bounding box + 16-char name + `numverts × trivertx_t` (3 bytes
   position + 1 byte light-normal index).

Vertex positions are compressed to bytes via the per-model
`scale`/`scale_origin`. Light normal indices reference the global
`anorms.h` table of 162 unit vectors. Frame names like `stand1`,
`run5`, `attack3` are how QuakeC (and now `game/*.c`) drive
animation by string match.

### `.spr` — Sprite

Billboards for particles, flames, explosions. (Phase 6 also rendered the
Doom/Wolf3D weapon viewmodels as `.spr`s; those guns were removed 2026-06-07.)
`spritegn.h`:

```c
struct dsprite_t {            // ident = "IDSP"
    int     ident;            // 'PSDI' little-endian magic
    int     version;          // 1
    int     type;             // 0=VP_PARALLEL_UPRIGHT, 1=FACING_UPRIGHT,
                              // 2=VP_PARALLEL, 3=ORIENTED, 4=VP_PARALLEL_ORIENTED
    float   boundingradius;
    int     width, height;
    int     numframes;
    float   beamlength;
    synctype_t synctype;
};
```

Each frame is either a single `dspriteframe_t { origin[2], width, height }`
followed by `width * height` palette indices, or a group of frames with
intervals (for animated sprites like flames).

## Audio

### `.wav` — Microsoft WAVE

Stock RIFF format, decoded by `snd_mem.c`. Quake supports 8/16-bit
mono PCM, mostly at 11025 Hz. The engine handles the `LIST INFO`
chunk only for the loop point — sounds with a `cue` chunk loop back to
the cue's sample offset for ambient sounds (`ambient_*`).

## State files

### `.dem` — Demo recording

Recorded with `record <name> <map> [skill]`. Plain stream — no
hierarchical structure, no random access. `cl_demo.c`:

```
ASCII forced-cdtrack number + "\n"
repeat until EOF:
    int32  message length (LE)
    float  yaw, pitch, roll   // recorded viewangles
    byte   message_data[length]   // raw svc_* stream
```

Playback (`playdemo <name>`) replays the svc messages into the client
parser with no server side — the recorded bytes drive the entire
renderer.

### `.sav` — Save game

Plain text Quake script. Written by `Host_Savegame_f`:

- Version line (`5\n`).
- Comment string.
- Sixteen `parm[]` values (one per line).
- Map name.
- Server time.
- 64 light-style strings.
- Every edict, printed via `ED_Print` — same key/value-pair format as
  `.map` entities, round-tripped via `pr_edict.c`'s field tables.

Adding a new edict field requires it to appear in the `NF_*` table or
it won't persist. `autosave.sav` is written at level start by the
host.

### `.cfg` — Console script

A stream of console commands typed line-by-line. `default.cfg` (in
`pak0.pak`) seeds the engine's keybinds; `config.cfg` is overwritten
at shutdown with the current binds, cvar values, and aliases.
`exec foo.cfg` from the console runs an arbitrary one.

## Images

### `.lmp` — Generic lump

Anything not in a WAD that's still palettised. `gfx/palette.lmp` is
the 768-byte palette; `gfx/colormap.lmp` is the 16K-entry colormap.
HUD and menu pictures (`gfx/pause.lmp`, etc.) are `qpic_t` payloads —
two int dimensions then `w*h` palette indices.

### `.pcx` — PCX image

`screen.c`'s screenshot writer uses PCX (1990s tradition). Modern code
mostly uses PNG via `stb_image_write` from
`VID_SaveScreenshotPNG` — the MCP `screenshot` tool always returns
PNG.

## Legacy / unused

### `progs.dat` — QuakeC bytecode

Compiled by `qcc` from `.qc` source. The original engine loaded this
to drive game logic via the `pr_exec.c` VM. With `-Dnative_game=true`
(the default) we never load `progs.dat` — our `game.dll` replaces it
entirely. Build with `-Dnative_game=false` to fall back to the QC
interpreter, which still works as a reference.

### `gfx.wad`, `id1/gfx/*`

Shipped in `pak0.pak`. Status bar artwork, HUD font, intermission
graphics. Still loaded as-is — none of these formats changed.

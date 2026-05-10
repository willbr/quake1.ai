// sim_nav.c -- BSP walkable-surface extraction, navmesh bake, A* pathfinding.

#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern engine_api_t   *eng;
extern game_globals_t *g;

// ---------------------------------------------------------------------------
// BSP file structures (from Quake WinQuake bspfile.h).
// Re-declared locally to avoid including engine headers.
// ---------------------------------------------------------------------------
#define BSPVERSION 29

typedef struct { int fileofs, filelen; } bsp_lump_t;
typedef struct {
    int        version;
    bsp_lump_t entities, planes, miptex, vertexes, visibility, nodes,
               texinfo, faces, lighting, clipnodes, leafs, marksurfaces,
               edges, surfedges, models;
} bsp_header_t;

typedef struct { float point[3]; } bsp_vertex_t;
typedef struct { unsigned short v[2]; } bsp_edge_t;
typedef struct {
    short  planenum;
    short  side;
    int    firstedge;
    short  numedges;
    short  texinfo;
    unsigned char styles[4];
    int    lightofs;
} bsp_face_t;

typedef struct {
    float normal[3];
    float dist;
    int   type;
} bsp_plane_t;

// ---------------------------------------------------------------------------
// Navmesh types
// ---------------------------------------------------------------------------
typedef struct {
    vec3_t pos;
} nav_point_t;

typedef struct {
    int   from, to;
    float weight;
} nav_edge_t;

struct sim_navmesh_s {
    nav_point_t *points;
    int          point_count;
    nav_edge_t  *edges;
    int          edge_count;
    int         *adj_offsets;   // CSR: offsets into adj[] per point
    int         *adj;           // CSR: edge indices
};

static sim_navmesh_t *s_mesh;
static int            s_ready;

// ---------------------------------------------------------------------------
// File I/O helper
// ---------------------------------------------------------------------------
static int read_file(const char *path, void **out_data, int *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    *out_data = buf;
    *out_size = (int)sz;
    return 1;
}

// ---------------------------------------------------------------------------
// Step 1: read the BSP, extract walkable face centroids.
// ---------------------------------------------------------------------------
static int extract_walkable_points(const char *bsp_path,
                                   nav_point_t **out_points,
                                   int *out_count)
{
    void *raw = 0;
    int   size = 0;
    if (!read_file(bsp_path, &raw, &size)) return 0;

    bsp_header_t *h = (bsp_header_t *)raw;
    if (h->version != BSPVERSION) { free(raw); return 0; }

    bsp_vertex_t *verts     = (bsp_vertex_t *)((char *)raw + h->vertexes.fileofs);
    int           nverts     = h->vertexes.filelen / (int)sizeof(bsp_vertex_t);

    bsp_edge_t   *edges      = (bsp_edge_t  *)((char *)raw + h->edges.fileofs);
    int          *surfedges   = (int         *)((char *)raw + h->surfedges.fileofs);

    bsp_face_t   *faces      = (bsp_face_t  *)((char *)raw + h->faces.fileofs);
    int           nfaces      = h->faces.filelen / (int)sizeof(bsp_face_t);

    bsp_plane_t  *planes     = (bsp_plane_t *)((char *)raw + h->planes.fileofs);

    int cap = 1024, n = 0;
    nav_point_t *pts = (nav_point_t *)malloc(sizeof(nav_point_t) * cap);
    if (!pts) { free(raw); return 0; }

    for (int fi = 0; fi < nfaces; fi++) {
        const bsp_face_t  *f = &faces[fi];
        const bsp_plane_t *p = &planes[f->planenum];
        // Plane normal flips with side flag.
        float nx = p->normal[0], ny = p->normal[1], nz = p->normal[2];
        if (f->side) { nx = -nx; ny = -ny; nz = -nz; }
        if (nz < 0.7f) continue;   // not walkable

        // Compute face centroid from edge vertices.
        float cx = 0, cy = 0, cz = 0;
        int   nv = 0;
        for (int e = 0; e < f->numedges; e++) {
            int   se   = surfedges[f->firstedge + e];
            int   vidx = (se >= 0) ? (int)edges[se].v[0] : (int)edges[-se].v[1];
            if (vidx < 0 || vidx >= nverts) continue;
            cx += verts[vidx].point[0];
            cy += verts[vidx].point[1];
            cz += verts[vidx].point[2];
            nv++;
        }
        if (nv == 0) continue;
        cx /= nv; cy /= nv; cz /= nv;

        if (n == cap) {
            cap *= 2;
            nav_point_t *tmp = (nav_point_t *)realloc(pts, sizeof(nav_point_t) * cap);
            if (!tmp) break;
            pts = tmp;
        }
        pts[n].pos[0] = cx;
        pts[n].pos[1] = cy;
        pts[n].pos[2] = cz + 16.0f;   // lift to standing height
        n++;
    }

    free(raw);
    *out_points = pts;
    *out_count  = n;
    return 1;
}

// ---------------------------------------------------------------------------
// Lifecycle stubs (edge-build, bake, A* wired in Tasks 17-19)
// ---------------------------------------------------------------------------
void Sim_Nav_Init(void) {
    (void)extract_walkable_points;
    s_mesh = 0;
    s_ready = 0;
}

int            Sim_Nav_IsReady(void) { return s_ready; }
sim_navmesh_t *Sim_Nav_Get(void)     { return s_mesh; }

void Sim_Nav_LevelInit(const char *mapname) {
    (void)mapname;
    // Full bake wired in Task 18.
}

int Sim_Nav_PathTo(const vec3_t a, const vec3_t b, vec3_t *o, int n) {
    (void)a; (void)b; (void)o; (void)n;
    return 0;
}

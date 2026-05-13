// sim_nav.c -- BSP walkable-surface extraction, navmesh bake, A* pathfinding.

#include "sim.h"
#include "../game_defs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <direct.h>

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
// Step 1: extract walkable face centroids from an in-memory BSP buffer.
// ---------------------------------------------------------------------------
static int extract_walkable_points(void *raw, int raw_size,
                                   nav_point_t **out_points,
                                   int *out_count)
{
    (void)raw_size;
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
    if (!pts) { return 0; }

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

    *out_points = pts;
    *out_count  = n;
    return 1;
}

// ---------------------------------------------------------------------------
// Step 2: allocate a probe entity for walkmove testing.
// ---------------------------------------------------------------------------

// Need an entity to probe with. Allocate once, reuse, free at end.
static edict_t *alloc_probe(void) {
    edict_t *e = eng->ED_Alloc();
    e->v.movetype = MOVETYPE_STEP;
    e->v.solid    = SOLID_SLIDEBOX;
    eng->SV_SetSize(e, (vec3_t){-16, -16, -24}, (vec3_t){16, 16, 32});
    return e;
}

static int build_edges(sim_navmesh_t *m) {
    int   cap = 4096, n = 0;
    nav_edge_t *e = malloc(sizeof(nav_edge_t) * cap);

    edict_t *probe = alloc_probe();
    if (!e) { eng->ED_Free(probe); return 0; }

    for (int i = 0; i < m->point_count; i++) {
        eng->SV_SetOrigin(probe, m->points[i].pos);
        if (!eng->SV_DropToFloor(probe)) continue;

        for (int j = 0; j < m->point_count; j++) {
            if (i == j) continue;
            float dx = m->points[i].pos[0] - m->points[j].pos[0];
            float dy = m->points[i].pos[1] - m->points[j].pos[1];
            float dz = m->points[i].pos[2] - m->points[j].pos[2];
            float d  = (float)sqrt(dx*dx + dy*dy + dz*dz);
            if (d > 96.0f) continue;
            // Probe walkmove from i to j.
            eng->SV_SetOrigin(probe, m->points[i].pos);
            float yaw = (float)(atan2(-dy, -dx) * 180.0 / 3.14159265358979);
            int ok = eng->SV_WalkMove(probe, yaw, d);
            if (!ok) continue;

            if (n == cap) {
                cap *= 2;
                nav_edge_t *tmp = realloc(e, sizeof(nav_edge_t) * cap);
                if (!tmp) { free(e); eng->ED_Free(probe); return 0; }
                e = tmp;
            }
            e[n].from   = i;
            e[n].to     = j;
            e[n].weight = d;
            n++;
        }
    }

    eng->ED_Free(probe);
    m->edges      = e;
    m->edge_count = n;
    return 1;
}

static void build_adjacency(sim_navmesh_t *m) {
    m->adj_offsets = calloc(m->point_count + 1, sizeof(int));
    if (!m->adj_offsets) return;
    for (int i = 0; i < m->edge_count; i++) m->adj_offsets[m->edges[i].from + 1]++;
    for (int i = 1; i <= m->point_count; i++) m->adj_offsets[i] += m->adj_offsets[i-1];

    int *cursor = calloc(m->point_count, sizeof(int));
    if (!cursor) { free(m->adj_offsets); m->adj_offsets = NULL; return; }
    m->adj = malloc(sizeof(int) * m->edge_count);
    if (!m->adj) { free(m->adj_offsets); m->adj_offsets = NULL; free(cursor); return; }
    for (int i = 0; i < m->edge_count; i++) {
        int from = m->edges[i].from;
        m->adj[m->adj_offsets[from] + cursor[from]++] = i;
    }
    free(cursor);
}

// ---------------------------------------------------------------------------
// Mesh helpers: free, save, load
// ---------------------------------------------------------------------------
static void free_mesh(sim_navmesh_t *m) {
    if (!m) return;
    free(m->points);
    free(m->edges);
    free(m->adj_offsets);
    free(m->adj);
    free(m);
}

static int save_mesh(const char *path, const sim_navmesh_t *m) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int magic = 0x4E41564D;     // 'NAVM'
    int ver   = 1;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver,   4, 1, f);
    fwrite(&m->point_count, 4, 1, f);
    fwrite(&m->edge_count,  4, 1, f);
    fwrite(m->points, sizeof(nav_point_t), m->point_count, f);
    fwrite(m->edges,  sizeof(nav_edge_t),  m->edge_count, f);
    fclose(f);
    return 1;
}

static sim_navmesh_t *load_mesh(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int magic, ver, np, ne;
    fread(&magic, 4, 1, f); fread(&ver, 4, 1, f);
    fread(&np, 4, 1, f);    fread(&ne, 4, 1, f);
    if (magic != 0x4E41564D || ver != 1) { fclose(f); return 0; }
    sim_navmesh_t *m = calloc(1, sizeof(*m));
    if (!m) { fclose(f); return 0; }
    m->point_count = np;
    m->edge_count  = ne;
    m->points = malloc(sizeof(nav_point_t) * np);
    m->edges  = malloc(sizeof(nav_edge_t)  * ne);
    if (!m->points || !m->edges) { fclose(f); free_mesh(m); return 0; }
    fread(m->points, sizeof(nav_point_t), np, f);
    fread(m->edges,  sizeof(nav_edge_t),  ne, f);
    fclose(f);
    build_adjacency(m);
    return m;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Sim_Nav_Init(void) {
    s_mesh = 0;
    s_ready = 0;
    eng->Cvar_Register("sim_nav_debug",  "0");
    eng->Cvar_Register("sim_nav_ztest",  "0");
}

int            Sim_Nav_IsReady(void) { return s_ready; }
sim_navmesh_t *Sim_Nav_Get(void)     { return s_mesh; }

void Sim_Nav_Frame(void) {
    int i;
    int ztest;
    if (!s_ready || !s_mesh) return;
    if (eng->Cvar_VariableValue("sim_nav_debug") <= 0.0f) return;

    ztest = eng->Cvar_VariableValue("sim_nav_ztest") > 0.0f;
    for (i = 0; i < s_mesh->edge_count; i++) {
        nav_edge_t *e = &s_mesh->edges[i];
        eng->SV_DebugLine(s_mesh->points[e->from].pos,
                          s_mesh->points[e->to].pos,
                          244,    // sky blue
                          ztest);
    }
}

void Sim_Nav_LevelInit(const char *mapname) {
    if (s_mesh) { free_mesh(s_mesh); s_mesh = 0; }
    s_ready = 0;
    if (!mapname || !*mapname) return;

    char bsp_vfs_path[256];
    char nav_path[256];
    snprintf(bsp_vfs_path, sizeof(bsp_vfs_path), "maps/%s.bsp", mapname);

    int bsp_sz = 0;
    void *bsp_raw = eng->LoadFile(bsp_vfs_path, &bsp_sz);
    if (!bsp_raw) {
        eng->Con_Print("sim_nav: bsp not found\n");
        return;
    }
    snprintf(nav_path, sizeof(nav_path),
             "id1/cache/navmesh/%s-%d.nav", mapname, bsp_sz);

    // Try cache.
    s_mesh = load_mesh(nav_path);
    if (s_mesh) {
        char buf[160];
        snprintf(buf, sizeof(buf), "sim_nav: loaded %d pts %d edges from cache\n",
                 s_mesh->point_count, s_mesh->edge_count);
        eng->Con_Print(buf);
        {
            // Push to imgui as 2D xy-only.
            int np = s_mesh->point_count;
            if (np > 4096) np = 4096;
            float *xy = malloc(sizeof(float) * 2 * np);
            if (xy) {
                for (int i = 0; i < np; i++) {
                    xy[2*i+0] = s_mesh->points[i].pos[0];
                    xy[2*i+1] = s_mesh->points[i].pos[1];
                }
                int ne = 0;
                unsigned short *eds = malloc(sizeof(unsigned short) * 2 * s_mesh->edge_count);
                if (eds) {
                    for (int i = 0; i < s_mesh->edge_count; i++) {
                        int a = s_mesh->edges[i].from;
                        int b = s_mesh->edges[i].to;
                        if (a < 65536 && b < 65536) {
                            eds[2*ne+0] = (unsigned short)a;
                            eds[2*ne+1] = (unsigned short)b;
                            ne++;
                        }
                    }
                    eng->ImguiNav_Set(xy, np, eds, ne);
                    free(eds);
                }
                free(xy);
            }
        }
        s_ready = 1;
        free(bsp_raw);
        return;
    }

    // Bake from scratch.
    eng->Con_Print("sim_nav: baking...\n");
    s_mesh = calloc(1, sizeof(*s_mesh));
    if (!s_mesh) { free(bsp_raw); return; }
    if (!extract_walkable_points(bsp_raw, bsp_sz, &s_mesh->points, &s_mesh->point_count)) {
        eng->Con_Print("sim_nav: extract failed\n");
        free_mesh(s_mesh); s_mesh = 0; free(bsp_raw); return;
    }
    free(bsp_raw);
    if (!build_edges(s_mesh)) {
        eng->Con_Print("sim_nav: edge build failed\n");
        free_mesh(s_mesh); s_mesh = 0; return;
    }
    build_adjacency(s_mesh);

    // Make cache directory and save (best-effort, Windows-compatible).
    _mkdir("id1\\cache");
    _mkdir("id1\\cache\\navmesh");
    save_mesh(nav_path, s_mesh);

    {
        int np = s_mesh->point_count;
        if (np > 4096) np = 4096;
        float *xy = malloc(sizeof(float) * 2 * np);
        if (xy) {
            for (int i = 0; i < np; i++) {
                xy[2*i+0] = s_mesh->points[i].pos[0];
                xy[2*i+1] = s_mesh->points[i].pos[1];
            }
            int ne = 0;
            unsigned short *eds = malloc(sizeof(unsigned short) * 2 * s_mesh->edge_count);
            if (eds) {
                for (int i = 0; i < s_mesh->edge_count; i++) {
                    int a = s_mesh->edges[i].from;
                    int b = s_mesh->edges[i].to;
                    if (a < 65536 && b < 65536) {
                        eds[2*ne+0] = (unsigned short)a;
                        eds[2*ne+1] = (unsigned short)b;
                        ne++;
                    }
                }
                eng->ImguiNav_Set(xy, np, eds, ne);
                free(eds);
            }
            free(xy);
        }
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "sim_nav: baked %d pts %d edges\n",
             s_mesh->point_count, s_mesh->edge_count);
    eng->Con_Print(buf);
    s_ready = 1;
}

// ---------------------------------------------------------------------------
// A* — small open-set, no priority queue. O(N²) for v1; fine for ~1k nodes.
// ---------------------------------------------------------------------------
static int nearest_point(const sim_navmesh_t *m, const vec3_t pos) {
    int   best = -1;
    float best_d2 = 1e18f;
    for (int i = 0; i < m->point_count; i++) {
        float dx = m->points[i].pos[0] - pos[0];
        float dy = m->points[i].pos[1] - pos[1];
        float dz = m->points[i].pos[2] - pos[2];
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 < best_d2) { best_d2 = d2; best = i; }
    }
    return best;
}

static float dist3(const vec3_t a, const vec3_t b) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)sqrt(dx*dx + dy*dy + dz*dz);
}

int Sim_Nav_PathTo(const vec3_t from, const vec3_t to, vec3_t *out, int max_out) {
    if (!s_mesh || !s_ready) return 0;
    int start = nearest_point(s_mesh, from);
    int goal  = nearest_point(s_mesh, to);
    if (start < 0 || goal < 0) return 0;
    if (start == goal) {
        if (max_out >= 1) {
            out[0][0] = s_mesh->points[goal].pos[0];
            out[0][1] = s_mesh->points[goal].pos[1];
            out[0][2] = s_mesh->points[goal].pos[2];
        }
        return 1;
    }

    int N = s_mesh->point_count;
    float *gscore = malloc(sizeof(float) * N);
    float *fscore = malloc(sizeof(float) * N);
    int   *came   = malloc(sizeof(int)   * N);
    char  *open   = calloc(N, 1);
    char  *closed = calloc(N, 1);
    if (!gscore || !fscore || !came || !open || !closed) {
        free(gscore); free(fscore); free(came); free(open); free(closed);
        return 0;
    }
    for (int i = 0; i < N; i++) { gscore[i] = 1e18f; fscore[i] = 1e18f; came[i] = -1; }

    gscore[start] = 0;
    fscore[start] = dist3(s_mesh->points[start].pos, s_mesh->points[goal].pos);
    open[start] = 1;

    int found = 0;
    while (1) {
        int   cur  = -1;
        float bf = 1e18f;
        for (int i = 0; i < N; i++) {
            if (!open[i]) continue;
            if (fscore[i] < bf) { bf = fscore[i]; cur = i; }
        }
        if (cur < 0) break;
        if (cur == goal) { found = 1; break; }
        open[cur]   = 0;
        closed[cur] = 1;

        int o0 = s_mesh->adj_offsets[cur];
        int o1 = s_mesh->adj_offsets[cur + 1];
        for (int k = o0; k < o1; k++) {
            const nav_edge_t *e = &s_mesh->edges[s_mesh->adj[k]];
            int nb = e->to;
            if (closed[nb]) continue;
            float tentative = gscore[cur] + e->weight;
            if (tentative < gscore[nb]) {
                came[nb]   = cur;
                gscore[nb] = tentative;
                fscore[nb] = tentative + dist3(s_mesh->points[nb].pos, s_mesh->points[goal].pos);
                open[nb]   = 1;
            }
        }
    }

    int written = 0;
    if (found) {
        // Reconstruct path: walk came[] backwards into a temp buffer, reverse.
        int  tmp[1024];
        int  tn = 0;
        int  c  = goal;
        while (c != -1 && tn < 1024) { tmp[tn++] = c; c = came[c]; }
        // Reverse and copy to out.
        for (int i = tn - 1; i >= 0 && written < max_out; i--) {
            out[written][0] = s_mesh->points[tmp[i]].pos[0];
            out[written][1] = s_mesh->points[tmp[i]].pos[1];
            out[written][2] = s_mesh->points[tmp[i]].pos[2];
            written++;
        }
    }

    free(gscore); free(fscore); free(came); free(open); free(closed);
    return written;
}

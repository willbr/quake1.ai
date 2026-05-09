// map_io.c -- .map text parse + write.
//
// Tolerant: malformed brushes are skipped with a Con_Printf, but the rest of
// the file still loads. Uses Quake's COM_Parse tokenizer (handles quoted
// strings, {/} as single-char tokens, // comments).
//
// Quake .map grammar (per ref/quake_map_source-master/e1m1.map):
//
//     {                              ; entity start
//         "key" "value"              ; key/value pair
//         {                          ; brush start
//             ( x y z ) ( x y z ) ( x y z ) TEXNAME s_shift t_shift rot s_scale t_scale
//             ...                    ; one such line per face plane
//         }                          ; brush end
//     }                              ; entity end

#include "quakedef.h"
#include "edit_scene.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *COM_Parse(char *data);
extern char com_token[1024];

// Read whole file into a malloc'd buffer (NUL-terminated). Returns NULL on
// error.
static char *slurp(const char *path, long *out_len)
{
    FILE *f;
    long n;
    char *buf;
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if ((long)fread(buf, 1, (size_t)n, f) != n) { free(buf); fclose(f); return NULL; }
    buf[n] = 0;
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

// Token helpers.
static char *next_tok(char *p)
{
    return COM_Parse(p);
}

static char *expect_tok(char *p, const char *want, int *ok)
{
    p = next_tok(p);
    if (!p || strcmp(com_token, want) != 0) { *ok = 0; return p; }
    return p;
}

static char *parse_vec3(char *p, vec3_t out, int *ok)
{
    int i;
    p = expect_tok(p, "(", ok); if (!*ok) return p;
    for (i = 0; i < 3; i++)
    {
        p = next_tok(p);
        if (!p) { *ok = 0; return p; }
        out[i] = (vec_t)atof(com_token);
    }
    p = expect_tok(p, ")", ok);
    return p;
}

static char *parse_brush(char *p, edit_brush_t *b, int *ok)
{
    b->numplanes = 0;
    b->faces = NULL;
    b->numfaces = 0;
    b->valid = 0;

    while (1)
    {
        p = next_tok(p);
        if (!p) { *ok = 0; return p; }
        if (com_token[0] == '}') return p;          // end of brush
        if (com_token[0] != '(') { *ok = 0; return p; }
        // We just consumed the '('. Rewind by treating the rest as a plane line.
        // parse_vec3 expects to parse the leading '(' itself, so we manually
        // grab the three coords first since '(' is already gone.
        if (b->numplanes >= EDIT_MAX_PLANES_PER_BRUSH)
        {
            // Skip the rest of this brush — too many planes.
            int depth = 1;
            while (depth > 0)
            {
                p = next_tok(p);
                if (!p) { *ok = 0; return p; }
                if (com_token[0] == '{') depth++;
                else if (com_token[0] == '}') depth--;
            }
            return p;
        }
        {
            edit_plane_t *pl = &b->planes[b->numplanes++];
            int i;
            // first vec3 — '(' already consumed
            for (i = 0; i < 3; i++)
            {
                p = next_tok(p); if (!p) { *ok = 0; return p; }
                pl->points[0][i] = (vec_t)atof(com_token);
            }
            p = expect_tok(p, ")", ok); if (!*ok) return p;

            p = parse_vec3(p, pl->points[1], ok); if (!*ok) return p;
            p = parse_vec3(p, pl->points[2], ok); if (!*ok) return p;

            // texture name (one token)
            p = next_tok(p); if (!p) { *ok = 0; return p; }
            {
                size_t n = strlen(com_token);
                if (n >= sizeof(pl->texname)) n = sizeof(pl->texname) - 1;
                memcpy(pl->texname, com_token, n);
                pl->texname[n] = 0;
            }

            // 5 texture params: s_shift t_shift rotation s_scale t_scale
            p = next_tok(p); if (!p) { *ok = 0; return p; } pl->s_shift  = (float)atof(com_token);
            p = next_tok(p); if (!p) { *ok = 0; return p; } pl->t_shift  = (float)atof(com_token);
            p = next_tok(p); if (!p) { *ok = 0; return p; } pl->rotation = (float)atof(com_token);
            p = next_tok(p); if (!p) { *ok = 0; return p; } pl->s_scale  = (float)atof(com_token);
            p = next_tok(p); if (!p) { *ok = 0; return p; } pl->t_scale  = (float)atof(com_token);
        }
    }
}

static char *parse_entity(char *p, edit_entity_t *e, int *ok)
{
    int kv_cap = 4, brush_cap = 4;

    e->numkv     = 0;
    e->kv        = (edit_kv_t *)calloc(kv_cap, sizeof(edit_kv_t));
    e->numbrushes = 0;
    e->brushes   = (edit_brush_t *)calloc(brush_cap, sizeof(edit_brush_t));
    e->classname_idx = -1;
    e->origin_idx    = -1;

    while (1)
    {
        p = next_tok(p);
        if (!p) { *ok = 0; return p; }
        if (com_token[0] == '}') return p;          // end of entity
        if (com_token[0] == '{')                   // brush starts
        {
            edit_brush_t b;
            memset(&b, 0, sizeof(b));
            int brush_ok = 1;
            p = parse_brush(p, &b, &brush_ok);
            if (!brush_ok)
            {
                Con_Printf("editor: bad brush in entity, skipped\n");
                continue;
            }
            if (e->numbrushes >= brush_cap)
            {
                brush_cap *= 2;
                e->brushes = (edit_brush_t *)realloc(e->brushes, brush_cap * sizeof(edit_brush_t));
            }
            e->brushes[e->numbrushes++] = b;
            continue;
        }
        // key/value pair
        {
            edit_kv_t kv;
            memset(&kv, 0, sizeof(kv));
            {
                size_t n = strlen(com_token);
                if (n >= sizeof(kv.key)) n = sizeof(kv.key) - 1;
                memcpy(kv.key, com_token, n);
                kv.key[n] = 0;
            }
            p = next_tok(p);
            if (!p) { *ok = 0; return p; }
            {
                size_t n = strlen(com_token);
                if (n >= sizeof(kv.value)) n = sizeof(kv.value) - 1;
                memcpy(kv.value, com_token, n);
                kv.value[n] = 0;
            }
            if (e->numkv >= kv_cap)
            {
                kv_cap *= 2;
                e->kv = (edit_kv_t *)realloc(e->kv, kv_cap * sizeof(edit_kv_t));
            }
            if (!strcmp(kv.key, "classname")) e->classname_idx = e->numkv;
            else if (!strcmp(kv.key, "origin")) e->origin_idx  = e->numkv;
            e->kv[e->numkv++] = kv;
        }
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

// In-memory snapshot of the .map text from the most recent Scene_Load.
// Owned by map_io.c; freed in Scene_Shutdown or replaced on the next load.
// Scene_Revert re-parses this buffer so it gives back the *load-time* state
// even after the user has saved over the file on disk. COM_Parse only walks
// the input (no mutation), so re-parsing is safe.
static char *s_snapshot_text = NULL;

void MapIO_FreeSnapshot(void)
{
    if (s_snapshot_text) { free(s_snapshot_text); s_snapshot_text = NULL; }
}

// Parse a NUL-terminated .map text buffer into edit_scene. Returns 1 on
// success. Replaces the existing scene contents.
static int parse_scene_text(char *src)
{
    char *p;
    int ok = 1;
    int cap = 8;

    Scene_Clear();

    edit_scene.entities = (edit_entity_t *)calloc(cap, sizeof(edit_entity_t));
    edit_scene.numentities = 0;

    p = src;
    while (1)
    {
        p = next_tok(p);
        if (!p) break;
        if (com_token[0] != '{')
        {
            Con_Printf("editor: expected '{' at top level, got '%s'\n", com_token);
            ok = 0;
            break;
        }
        if (edit_scene.numentities >= cap)
        {
            cap *= 2;
            edit_scene.entities = (edit_entity_t *)realloc(edit_scene.entities,
                                                           cap * sizeof(edit_entity_t));
        }
        {
            edit_entity_t e;
            int e_ok = 1;
            memset(&e, 0, sizeof(e));
            p = parse_entity(p, &e, &e_ok);
            if (!e_ok)
            {
                Con_Printf("editor: bad entity, skipped\n");
                if (e.kv) free(e.kv);
                if (e.brushes) free(e.brushes);
                continue;
            }
            edit_scene.entities[edit_scene.numentities++] = e;
        }
    }

    {
        int i, j;
        for (i = 0; i < edit_scene.numentities; i++)
        {
            edit_entity_t *e = &edit_scene.entities[i];
            for (j = 0; j < e->numbrushes; j++)
                Brush_Compile(&e->brushes[j]);
            // Anything in a freshly-parsed scene was either spawned by the
            // engine's load-from-file flow (because the .bsp/.map text we
            // just parsed matches what the server already knows about) or
            // is brush-only and never had a server edict. Either way, the
            // editor shouldn't attempt to spawn it again on close.
            e->spawned = 1;
        }
    }
    return ok;
}

int Scene_Load(const char *path)
{
    char *src = slurp(path, NULL);
    int ok;
    if (!src) return 0;

    ok = parse_scene_text(src);

    // Stash the source buffer as the load-time snapshot for Scene_Revert.
    // Replaces any prior snapshot.
    MapIO_FreeSnapshot();
    s_snapshot_text = src;

    {
        size_t n = strlen(path);
        if (n >= sizeof(edit_scene.filename)) n = sizeof(edit_scene.filename) - 1;
        memcpy(edit_scene.filename, path, n);
        edit_scene.filename[n] = 0;
    }
    return ok;
}

// Restore from the in-memory load-time snapshot. Disk is NOT touched — if the
// user wants their reverted state on disk they have to save again. Returns 0
// if no snapshot exists (no .map has been loaded this session).
int Scene_Revert(void)
{
    if (!s_snapshot_text) return 0;
    return parse_scene_text(s_snapshot_text);
}

// -----------------------------------------------------------------------------
// Serialize / deserialize
// -----------------------------------------------------------------------------

// Growing byte buffer used to assemble the .map text in memory. Doubles on
// overflow; the caller takes ownership of `buf` and frees it.
typedef struct {
    char  *buf;
    int    len;
    int    cap;
} sbuf_t;

static void sbuf_reserve(sbuf_t *s, int extra)
{
    int need = s->len + extra + 1;          // +1 for terminator
    if (need <= s->cap) return;
    if (s->cap == 0) s->cap = 4096;
    while (s->cap < need) s->cap *= 2;
    s->buf = (char *)realloc(s->buf, s->cap);
}

static void sbuf_printf(sbuf_t *s, const char *fmt, ...)
{
    va_list ap;
    int written;
    for (;;)
    {
        int avail = s->cap - s->len;
        va_start(ap, fmt);
        written = vsnprintf(s->buf + s->len, avail, fmt, ap);
        va_end(ap);
        if (written >= 0 && written < avail)
        {
            s->len += written;
            return;
        }
        // Truncated — grow and retry.
        sbuf_reserve(s, written > 0 ? written : 1024);
    }
}

int Scene_Serialize(char **out_text, int *out_len)
{
    sbuf_t s = {0};
    int i, j, k;

    sbuf_reserve(&s, 4096);
    for (i = 0; i < edit_scene.numentities; i++)
    {
        edit_entity_t *e = &edit_scene.entities[i];
        sbuf_printf(&s, "{\n");
        for (j = 0; j < e->numkv; j++)
            sbuf_printf(&s, "\"%s\" \"%s\"\n", e->kv[j].key, e->kv[j].value);
        for (j = 0; j < e->numbrushes; j++)
        {
            edit_brush_t *b = &e->brushes[j];
            sbuf_printf(&s, "{\n");
            for (k = 0; k < b->numplanes; k++)
            {
                edit_plane_t *p = &b->planes[k];
                sbuf_printf(&s,
                    "( %g %g %g ) ( %g %g %g ) ( %g %g %g ) %s %g %g %g %g %g\n",
                    p->points[0][0], p->points[0][1], p->points[0][2],
                    p->points[1][0], p->points[1][1], p->points[1][2],
                    p->points[2][0], p->points[2][1], p->points[2][2],
                    p->texname[0] ? p->texname : "editor/grid",
                    p->s_shift, p->t_shift, p->rotation, p->s_scale, p->t_scale);
            }
            sbuf_printf(&s, "}\n");
        }
        sbuf_printf(&s, "}\n");
    }
    if (!s.buf)
    {
        // Empty scene — return a valid empty string.
        s.buf = (char *)malloc(1);
        s.cap = 1;
    }
    s.buf[s.len] = 0;
    *out_text = s.buf;
    if (out_len) *out_len = s.len;
    return 1;
}

int Scene_DeserializeText(const char *text)
{
    // parse_scene_text takes a mutable buffer because COM_Parse walks it
    // with a saved char*; copy so the caller's const text isn't disturbed.
    char *copy = (char *)malloc(strlen(text) + 1);
    int ok;
    if (!copy) return 0;
    strcpy(copy, text);
    ok = parse_scene_text(copy);
    free(copy);
    return ok;
}

int Scene_Save(const char *path)
{
    FILE *f;
    char *text;
    int   len, ok;

    if (!Scene_Serialize(&text, &len)) return 0;
    f = fopen(path, "wb");
    if (!f) { free(text); return 0; }
    ok = ((int)fwrite(text, 1, len, f) == len);
    fclose(f);
    free(text);

    if (!ok) return 0;
    {
        size_t n = strlen(path);
        if (n >= sizeof(edit_scene.filename)) n = sizeof(edit_scene.filename) - 1;
        memcpy(edit_scene.filename, path, n);
        edit_scene.filename[n] = 0;
    }
    return 1;
}

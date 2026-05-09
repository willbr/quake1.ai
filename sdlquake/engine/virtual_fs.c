/*
 * virtual_fs.c -- tiny linear-array file registry for the editor's
 * compile-and-reload flow. Only ever holds 1-2 entries (the freshly
 * compiled .bsp, maybe a .lit later), so a hash table is overkill.
 */

#include "virtual_fs.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char  *path;     /* malloc'd, NUL-terminated */
    void  *bytes;    /* malloc'd; ownership held here */
    int    size;
} vfs_entry_t;

static vfs_entry_t *s_entries = NULL;
static int          s_count   = 0;
static int          s_cap     = 0;

/* Case-insensitive ASCII compare — file paths in Quake's loader are
 * lowercased / inconsistent across PAK lookups, so we play it safe. */
static int paths_equal(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + ('a' - 'A')) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + ('a' - 'A')) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int find_index(const char *path)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (paths_equal(s_entries[i].path, path)) return i;
    return -1;
}

void Editor_VFS_Register(const char *path, void *bytes, int size)
{
    int idx;
    if (!path || !bytes || size <= 0) return;
    idx = find_index(path);
    if (idx >= 0) {
        free(s_entries[idx].bytes);
        s_entries[idx].bytes = bytes;
        s_entries[idx].size  = size;
        return;
    }
    if (s_count >= s_cap) {
        int new_cap = s_cap ? s_cap * 2 : 4;
        s_entries = (vfs_entry_t *)realloc(s_entries,
            new_cap * sizeof(vfs_entry_t));
        s_cap = new_cap;
    }
    {
        size_t n = strlen(path);
        char *p = (char *)malloc(n + 1);
        memcpy(p, path, n + 1);
        s_entries[s_count].path  = p;
        s_entries[s_count].bytes = bytes;
        s_entries[s_count].size  = size;
        s_count++;
    }
}

void Editor_VFS_Unregister(const char *path)
{
    int idx = find_index(path);
    if (idx < 0) return;
    free(s_entries[idx].path);
    free(s_entries[idx].bytes);
    /* compact: move tail down */
    {
        int j;
        for (j = idx + 1; j < s_count; j++)
            s_entries[j - 1] = s_entries[j];
    }
    s_count--;
}

int Editor_VFS_Find(const char *path, void **out_bytes, int *out_size)
{
    int idx;
    if (!path) return 0;
    idx = find_index(path);
    if (idx < 0) return 0;
    if (out_bytes) *out_bytes = s_entries[idx].bytes;
    if (out_size)  *out_size  = s_entries[idx].size;
    return 1;
}

void Editor_VFS_Shutdown(void)
{
    int i;
    for (i = 0; i < s_count; i++) {
        free(s_entries[i].path);
        free(s_entries[i].bytes);
    }
    free(s_entries);
    s_entries = NULL;
    s_count   = 0;
    s_cap     = 0;
}

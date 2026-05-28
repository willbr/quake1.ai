#include "quakedef.h"
#include "qalloc_hunk.h"
#include <string.h>

static void *qa_hunk_alloc(void *ctx, size_t size) {
    return Hunk_AllocName((int)size, (char *)ctx);
}
static void *qa_hunk_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    void *p = Hunk_AllocName((int)new_size, (char *)ctx);
    if (p && ptr && old_size)
        memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    return p;
}
static void qa_hunk_free(void *ctx, void *ptr) {
    (void)ctx; (void)ptr; /* reclaimed via Hunk_FreeToLowMark */
}

qalloc_t qalloc_hunk(const char *name) {
    qalloc_t a;
    a.alloc   = qa_hunk_alloc;
    a.realloc = qa_hunk_realloc;
    a.free    = qa_hunk_free;
    a.ctx     = (void *)name;
    return a;
}

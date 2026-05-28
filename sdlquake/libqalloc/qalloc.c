#include "qalloc.h"
#include <stdlib.h>
#include <string.h>

/* ---- malloc backend ---- */

static void *qa_malloc_alloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}
static void *qa_malloc_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx; (void)old_size;
    return realloc(ptr, new_size);
}
static void qa_malloc_free(void *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

qalloc_t qalloc_malloc(void) {
    qalloc_t a;
    a.alloc   = qa_malloc_alloc;
    a.realloc = qa_malloc_realloc;
    a.free    = qa_malloc_free;
    a.ctx     = NULL;
    return a;
}

/* ---- arena (bump) backend ---- */

#define QA_ALIGN 16

static size_t qa_round_up(size_t n) {
    return (n + (QA_ALIGN - 1)) & ~((size_t)QA_ALIGN - 1);
}

static void *qa_arena_alloc(void *ctx, size_t size) {
    qalloc_arena_t *a = (qalloc_arena_t *)ctx;
    size_t need = qa_round_up(size);
    if (a->used + need > a->size)
        return NULL;
    {
        void *p = a->base + a->used;
        a->used += need;
        return p;
    }
}
static void *qa_arena_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    void *p = qa_arena_alloc(ctx, new_size);
    if (p && ptr && old_size)
        memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    return p;
}
static void qa_arena_free(void *ctx, void *ptr) {
    (void)ctx; (void)ptr; /* no-op */
}

qalloc_t qalloc_arena_init(qalloc_arena_t *a, void *buf, size_t size) {
    qalloc_t out;
    a->base = (unsigned char *)buf;
    a->size = size;
    a->used = 0;
    out.alloc   = qa_arena_alloc;
    out.realloc = qa_arena_realloc;
    out.free    = qa_arena_free;
    out.ctx     = a;
    return out;
}
void qalloc_arena_reset(qalloc_arena_t *a) {
    a->used = 0;
}

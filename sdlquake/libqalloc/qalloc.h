#ifndef QALLOC_H
#define QALLOC_H

#include <stddef.h>

/* Shared allocator interface. Backends provide alloc/realloc/free over an
   opaque ctx. All allocations are suitably aligned for any scalar type. */
typedef struct qalloc_s {
    void *(*alloc)  (void *ctx, size_t size);
    void *(*realloc)(void *ctx, void *ptr, size_t old_size, size_t new_size);
    void  (*free)   (void *ctx, void *ptr);
    void  *ctx;
} qalloc_t;

#define QALLOC(a, T)        ((T *)(a)->alloc((a)->ctx, sizeof(T)))
#define QALLOC_ARR(a, T, n) ((T *)(a)->alloc((a)->ctx, sizeof(T) * (size_t)(n)))
#define QFREE(a, p)         ((a)->free((a)->ctx, (p)))

/* libc malloc-backed allocator. ctx is NULL. */
qalloc_t qalloc_malloc(void);

/* Bump allocator over a caller-owned buffer. free is a no-op; realloc bumps a
   fresh block and copies old_size bytes. Out-of-space returns NULL. */
typedef struct qalloc_arena_s {
    unsigned char *base;
    size_t         size;
    size_t         used;
} qalloc_arena_t;

qalloc_t qalloc_arena_init(qalloc_arena_t *a, void *buf, size_t size);
void     qalloc_arena_reset(qalloc_arena_t *a);

#endif /* QALLOC_H */

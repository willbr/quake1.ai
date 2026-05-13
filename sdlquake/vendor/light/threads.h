/*
 * threads.h -- stub for the LIGHT tool's threading layer. The vanilla
 * id-LIGHT used DEC alpha pthreads for face-parallel baking; we ship a
 * single-threaded path because the editor's bake button is rare and
 * the in-process port doesn't have a stable threading story yet.
 * LOCK/UNLOCK collapse to no-ops; RunThreadsOn just invokes the
 * worker callback once on the calling thread.
 */

#ifndef LIGHT_THREADS_H
#define LIGHT_THREADS_H

#define LOCK
#define UNLOCK

extern int numthreads;

typedef void (threadfunc_t) (void *);

void InitThreads (void);
void RunThreadsOn (threadfunc_t func);

#endif

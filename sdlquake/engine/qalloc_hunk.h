#ifndef QALLOC_HUNK_H
#define QALLOC_HUNK_H

#include "qalloc.h"

/* A qalloc_t that allocates from the Quake Hunk via Hunk_AllocName(name).
   free is a no-op — the caller reclaims everything with Hunk_FreeToLowMark.
   Intended for transient parse scratch that is translated then discarded. */
qalloc_t qalloc_hunk(const char *name);

#endif /* QALLOC_HUNK_H */

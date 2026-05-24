#ifndef SDLQUAKE_SCREENSHOT_PATH_H
#define SDLQUAKE_SCREENSHOT_PATH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ensures screenshots/ exists, writes the next free
   screenshots/shot_NNNN.png into `out` (size `outsz`).
   Returns 1 on success, 0 if all 10000 slots are taken or
   the path won't fit. */
int Screenshot_NextPath(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif

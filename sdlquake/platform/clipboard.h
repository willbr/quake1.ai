#ifndef SDLQUAKE_CLIPBOARD_H
#define SDLQUAKE_CLIPBOARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy `size` bytes of PNG-encoded image data onto the system
   clipboard with MIME type "image/png". The bytes are duplicated
   internally — the caller may free `png_bytes` immediately.
   Returns 1 on success, 0 if SDL rejected the request. */
int Clipboard_SetPNG(const void *png_bytes, size_t size);

#ifdef __cplusplus
}
#endif

#endif
